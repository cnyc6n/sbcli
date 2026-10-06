// src/sbcli_view.cpp —— `sb view`：sbcli 项目的浏览视图
//
// 输入是**文本 AST**（src/sbcli_parser），不是 .sb3 的 DOM：
//    · 角色清单来自目录结构 + meta.sbcli
//    · 脚本中文来自 "opcode → SB3_T/SB2_T 模板 → 填参"
// Renderer（src/sb3_render.cpp）吃的是 simdjson 的 project.json 视图，
// 对这里不适用，所以翻译在本文件自己走一遍模板替换。

#include "sbcli_view.hpp"
#include "sbcli_parser.hpp"
#include "common.hpp"
#include "sb3_tables.hpp"
#include "ext_loader.hpp"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace sb {
namespace {

std::string trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool fileExistsU8(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(p), ec);
}

bool dirExistsU8(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(fs::u8path(p), ec);
}

std::string normPath(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string joinRel(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

std::string relToRoot(const std::string& root, const std::string& abs) {
    std::error_code ec;
    fs::path rp = fs::relative(fs::u8path(abs), fs::u8path(root), ec);
    if (ec) return normPath(abs);
    return normPath(rp.u8string());
}

std::string unquote(std::string s) {
    s = trimStr(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

// 角色 meta 里我们关心的几个字段
struct SpriteMeta {
    std::string name;
    int  costumes = 0, sounds = 0;
    bool has = false;
};

SpriteMeta loadSpriteMeta(const std::string& absPath) {
    SpriteMeta m;
    std::ifstream f(fs::u8path(absPath), std::ios::binary);
    if (!f) return m;
    m.has = true;
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::istringstream is(all);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 剥注释（引号内的 # 不算）
        std::string s;
        bool inStr = false;
        for (char c : line) {
            if (c == '"') { inStr = !inStr; s += c; continue; }
            if (c == '#' && !inStr) break;
            s += c;
        }
        s = trimStr(s);
        size_t colon = s.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trimStr(s.substr(0, colon));
        std::string val = trimStr(s.substr(colon + 1));
        if (key == "name") { m.name = unquote(val); continue; }
        if (key != "costumes" && key != "sounds") continue;
        // 数一数 [ ... ] 里的顶层元素个数
        std::string body = val;
        size_t lb = body.find('[');
        size_t rb = body.rfind(']');
        if (lb == std::string::npos || rb == std::string::npos || rb < lb) continue;
        body = body.substr(lb + 1, rb - lb - 1);
        int n = 0;
        bool any = false;
        bool inS = false;
        for (char c : body) {
            if (c == '"') { inS = !inS; any = true; continue; }
            if (c == ',' && !inS) { ++n; continue; }
            if (!std::isspace((unsigned char)c)) any = true;
        }
        if (any) ++n;
        (key == "costumes" ? m.costumes : m.sounds) = n;
    }
    return m;
}

std::string loadProjectName(const std::string& absPath) {
    std::ifstream f(fs::u8path(absPath), std::ios::binary);
    if (!f) return "";
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::istringstream is(all);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string s = trimStr(line);
        if (s.empty() || s[0] == '#' || s[0] == '[') continue;
        size_t colon = s.find(':');
        if (colon == std::string::npos) continue;
        if (trimStr(s.substr(0, colon)) == "name") return unquote(s.substr(colon + 1));
    }
    return "";
}

// ==========================================================================
// 翻译：AST → 中文
// ==========================================================================

// 取一个值的显示文本（reporter 会递归翻译成内联表达式）
std::string valueText(const SbcValue& v, int depth);

// 菜单值的本地化（ FIELD_MAP 里的英文名 → 中文）
std::string menuLocalize(const std::string& key, const std::string& val) {
    auto it = FIELD_MAP.find(key);
    if (it == FIELD_MAP.end()) return val;
    auto jt = it->second.find(val);
    if (jt == it->second.end()) return val;
    return jt->second;
}

// SB2 菜单值（mouse-pointer / all around …）
std::string sb2MenuLocalize(const std::string& val) {
    auto it = SB2_MENU.find(val);
    return (it == SB2_MENU.end()) ? val : it->second;
}

// 按 SB3 模板填参。tpl 里的 {KEY} 用 params 里对应 canon 的值替换。
std::string fillTemplate(const std::string& tpl, const std::string& opcode,
                         const std::vector<SbcParam>& params, int depth) {
    std::string out;
    size_t p = 0;
    while (true) {
        size_t open = tpl.find('{', p);
        if (open == std::string::npos) { out.append(tpl, p, std::string::npos); break; }
        out.append(tpl, p, open - p);
        size_t close = tpl.find('}', open);
        if (close == std::string::npos) { out.append(tpl, open, std::string::npos); break; }
        std::string key = tpl.substr(open + 1, close - open - 1);

        // 找到对应参数（canon 优先）
        const SbcParam* found = nullptr;
        for (const auto& prm : params) {
            const std::string& k = prm.canon.empty() ? prm.key : prm.canon;
            if (k == key) { found = &prm; break; }
        }
        if (!found) {
            // 兜底：按 SB3 模板占位符顺序取第 N 个参数（SB2 的 {__1} 也走这里）
            out += "?";
        } else if (!found->value) {
            out += "?";
        } else {
            const SbcValue& v = *found->value;
            std::string rendered;
            if (v.kind == SbcValue::Kind::Scalar) {
                rendered = v.text();
                // 菜单类字段做本地化
                if (FIELD_MAP.count(key)) rendered = menuLocalize(key, rendered);
                else if (opcode.rfind("sensing_", 0) == 0 ||
                         opcode.rfind("looks_", 0) == 0 ||
                         opcode.rfind("motion_", 0) == 0 ||
                         opcode == "control_stop" || opcode == "event_whenkeypressed" ||
                         opcode == "sensing_keypressed")
                    rendered = sb2MenuLocalize(rendered);
            } else if (v.kind == SbcValue::Kind::List) {
                rendered = "[...]";
            } else {
                rendered = valueText(v, depth + 1);
            }
            out += rendered.empty() ? "?" : rendered;
        }
        p = close + 1;
    }
    return out;
}

// SB2 模板：占位符是 {__1} {__2}，按出现顺序对应参数的书写顺序
std::string fillTemplateSB2(const std::string& tpl, const std::string& opcode,
                            const std::vector<SbcParam>& params, int depth) {
    std::string out;
    std::vector<std::string> vals;
    for (const auto& prm : params) {
        if (!prm.value) { vals.push_back("?"); continue; }
        const SbcValue& v = *prm.value;
        if (v.kind == SbcValue::Kind::Scalar) vals.push_back(v.text().empty() ? "?" : v.text());
        else if (v.kind == SbcValue::Kind::List) vals.push_back("[...]");
        else vals.push_back(valueText(v, depth + 1));
    }
    // SB2 的第几个输入是菜单
    const std::set<int>* menuPos = nullptr;
    auto mp = SB2_MENU_POS_MAP.find(opcode);
    if (mp != SB2_MENU_POS_MAP.end()) menuPos = &mp->second;

    size_t p = 0, k = 0;
    while (true) {
        size_t open = tpl.find('{', p);
        if (open == std::string::npos) { out.append(tpl, p, std::string::npos); break; }
        out.append(tpl, p, open - p);
        size_t close = tpl.find('}', open);
        if (close == std::string::npos) { out.append(tpl, open, std::string::npos); break; }
        std::string key = tpl.substr(open + 1, close - open - 1);
        std::string rendered = (k < vals.size()) ? vals[k] : "?";
        if (menuPos && key.size() > 2 && key[0] == '_' && key[1] == '_') {
            try {
                int idx = std::stoi(key.substr(2));
                if (menuPos->count(idx)) rendered = sb2MenuLocalize(rendered);
            } catch (...) {}
        }
        out += rendered;
        ++k;
        p = close + 1;
    }
    return out;
}

// 扩展积木的渲染上下文：项目根 + 已解析的扩展表。
// 只在 sbcliView() 里加载一次（每个扩展文件要 spawn 一次 node，不能逐块加载）。
struct ExtCtx {
    bool    loaded = false;
    ExtInfo info;
};
ExtCtx g_ext;

std::string valueText(const SbcValue& v, int depth);   // 下面定义（extBlockText 要回调它）
std::string extBlockText(const std::string& fullOp, int type,
                         const std::vector<SbcParam>& params, int depth);

std::string valueText(const SbcValue& v, int depth) {
    if (depth > 40) return "?";  // 防御：AST 异常深时不递归到栈溢出
    if (v.kind == SbcValue::Kind::Scalar) return v.text();
    if (v.kind == SbcValue::Kind::List) return "[...]";
    // Reporter：内层块inline 渲染
    const std::string& op = v.scalar.text;
    if (op.empty()) return "?";
    auto t3 = SB3_T.find(op);
    if (t3 != SB3_T.end()) return fillTemplate(t3->second, op, v.args, depth);
    auto t2 = SB2_T.find(op);
    if (t2 != SB2_T.end()) return fillTemplateSB2(t2->second, op, v.args, depth);
    if (g_ext.loaded) {
        auto et = g_ext.info.blockTypes.find(op);
        if (et != g_ext.info.blockTypes.end())
            return extBlockText(op, et->second, v.args, depth);
    }
    if (op == "argument_reporter_string_number" || op == "argument_reporter_boolean") {
        for (const auto& prm : v.args)
            if (prm.canon == "VALUE" || prm.key == "VALUE")
                if (prm.value && prm.value->kind == SbcValue::Kind::Scalar)
                    return "(" + prm.value->text() + ")";
        return "?";
    }
    return "?";
}

// 常见扩展积木参数名 → 中文（与 sb3_render.cpp 的 PARAM_ZH 一致）
const std::map<std::string, std::string>& paramZh() {
    static const std::map<std::string, std::string> m = {
        {"FONT", "字体"}, {"COLOR", "颜色"}, {"COLOR2", "颜色2"}, {"COLOR3", "颜色3"},
        {"TEXT", "文字"}, {"TEXT1", "文字1"}, {"TEXT2", "文字2"}, {"MESSAGE", "消息"},
        {"MUSIC", "音乐"}, {"FILE", "文件"}, {"PATH", "路径"}, {"NAME", "名称"},
        {"VOLUME", "音量"}, {"SPEED", "速度"}, {"PITCH", "音调"}, {"RATE", "速率"},
        {"LANG", "语言"}, {"LANGUAGE", "语言"}, {"URL", "地址"}, {"ID", "编号"},
        {"X", "x"}, {"Y", "y"}, {"SIZE", "大小"}, {"SCALE", "缩放"}, {"TIMES", "次数"},
        {"SECS", "秒数"}, {"START", "起点"}, {"END", "终点"}, {"INDEX", "序号"},
        {"DELAY", "延迟"}, {"DURATION", "时长"}, {"TIMES2", "次数2"}, {"ANGLE", "角度"},
        {"RADIUS", "半径"}, {"TARGET", "目标"}, {"OPTION", "选项"}, {"VALUE", "值"},
        {"ANSWER", "回答"}, {"HEIGHT", "高度"}, {"WIDTH", "宽度"}, {"COSTUME", "造型"},
        {"BACKDROP", "背景"}, {"SOUND", "声音"}, {"STYLE", "样式"}, {"TYPE", "类型"},
        {"MATRIX", "矩阵"}, {"BRIGHTNESS", "亮度"}, {"SATURATION", "饱和度"},
        {"HUE", "色相"}, {"SHADE", "色相"}, {"PERCENT", "百分比"}, {"RATIO", "比例"},
        {"ENABLED", "启用"}, {"COLOR1", "颜色1"},
        {"SX", "起点x"}, {"SY", "起点y"}, {"EX", "终点x"}, {"EY", "终点y"},
        {"ITEM", "项"}, {"LIST", "列表"},
    };
    return m;
}

std::string paramNameZh(const std::string& k) {
    auto it = paramZh().find(k);
    return (it == paramZh().end()) ? k : it->second;
}

// 扩展积木：`<extId>_<blockName>`。
// 与 sb3_render.cpp 的 extBlockRender() 对齐：
//   REPORTER(1) → (扩展名·块名 参数…)   BOOLEAN(2) → <…>
//   HAT(3)      → 扩展名·块名             COMMAND(0) → 扩展名·块名(参数…)
// 参数顺序按扩展声明的 blockParams（getInfo 里的顺序），与 sb3 侧的
// fields+inputs 排序不同但同样稳定。
std::string extBlockText(const std::string& fullOp, int type,
                         const std::vector<SbcParam>& params, int depth) {
    std::string extId, blockName;
    size_t us = fullOp.find('_');
    if (us != std::string::npos) {
        extId = fullOp.substr(0, us);
        blockName = fullOp.substr(us + 1);
    } else {
        extId = fullOp;
    }
    std::string extName = extId;
    auto nit = g_ext.info.extNames.find(extId);
    if (nit != g_ext.info.extNames.end()) extName = nit->second;

    // 参数：优先按扩展声明顺序，其次按脚本里的书写顺序
    std::vector<std::pair<std::string, std::string>> bits;   // (显示名, 值)
    std::set<std::string> used;
    auto pit = g_ext.info.blockParams.find(fullOp);
    if (pit != g_ext.info.blockParams.end()) {
        for (const auto& nm : pit->second) {
            const SbcParam* found = nullptr;
            for (const auto& prm : params) {
                const std::string& k = prm.canon.empty() ? prm.key : prm.canon;
                if (k == nm) { found = &prm; break; }
            }
            if (!found) continue;
            used.insert(nm);
            std::string val = found->value ? valueText(*found->value, depth + 1) : "?";
            bits.emplace_back(paramNameZh(nm), val);
        }
    }
    for (const auto& prm : params) {
        const std::string& k = prm.canon.empty() ? prm.key : prm.canon;
        if (used.count(k)) continue;
        std::string val = prm.value ? valueText(*prm.value, depth + 1) : "?";
        bits.emplace_back(paramNameZh(k), val);
    }

    std::string body;
    for (size_t i = 0; i < bits.size(); ++i) {
        if (i) body += ", ";
        body += bits[i].first + "=" + bits[i].second;
    }
    std::string text = extName + "·" + blockName;
    if (type == 1) return "(" + text + (body.empty() ? "" : " " + body) + ")";
    if (type == 2) return "<" + text + (body.empty() ? "" : " " + body) + ">";
    if (type == 3) return text;
    return text + (body.empty() ? "" : "(" + body + ")");
}

// 翻译一个积木
std::string blockText(const SbcBlock& b, int depth) {
    const std::string& op = b.opcode;
    if (op.empty()) return "?";

    // 自定义积木：定义 / 调用
    if (op == "procedures_definition" || op == "procedures_call") {
        std::string code;
        if (const SbcParam* p = b.find("PROCCODE"))
            if (p->value && p->value->kind == SbcValue::Kind::Scalar)
                code = p->value->text();
        if (code.empty()) return (op == "procedures_call") ? "调用自定义积木" : "定义自定义积木";
        // 把 %s/%n/%b 换成实参名
        std::vector<std::string> names;
        if (op == "procedures_definition") {
            if (const SbcParam* p = b.find("ARGS"))
                if (p->value && p->value->kind == SbcValue::Kind::List)
                    for (const auto& it : p->value->items) names.push_back(it.text);
        } else {
            for (const auto& prm : b.params) {
                const std::string& k = prm.canon.empty() ? prm.key : prm.canon;
                if (k.size() >= 4 && k.compare(0, 3, "ARG") == 0 &&
                    k.find_first_not_of("0123456789", 3) == std::string::npos)
                    if (prm.value && prm.value->kind == SbcValue::Kind::Scalar)
                        names.push_back(prm.value->text());
            }
        }
        // 自定义积木参数在 Scratch 里是椭圆占位符：
        //   · 定义处 ARGS 是**参数名** → 加括号便于阅读「移动 (步数) 步」
        //   · 调用处 ARG1..N 是**实参值** → 直接嵌入，加括号会变成「移动 (10) 步」
        const bool isDef = (op == "procedures_definition");
        std::string out;
        size_t i = 0, k = 0;
        while (i < code.size()) {
            if (code[i] == '%' && i + 1 < code.size() &&
                (code[i + 1] == 'n' || code[i + 1] == 's' || code[i + 1] == 'b')) {
                if (k < names.size()) {
                    out += isDef ? ("(" + names[k] + ")") : names[k];
                } else {
                    out += "(?)";
                }
                ++k;
                i += 2;
            } else {
                out += code[i++];
            }
        }
        for (; k < names.size(); ++k) out += isDef ? (" (" + names[k] + ")")
                                                   : (" " + names[k]);
        return isDef ? ("定义 " + out) : out;
    }

    if (op == "argument_reporter_string_number" || op == "argument_reporter_boolean") {
        if (const SbcParam* p = b.find("VALUE"))
            if (p->value && p->value->kind == SbcValue::Kind::Scalar)
                return "(" + p->value->text() + ")";
        return "?";
    }

    auto t3 = SB3_T.find(op);
    if (t3 != SB3_T.end()) return fillTemplate(t3->second, op, b.params, depth);
    auto t2 = SB2_T.find(op);
    if (t2 != SB2_T.end()) return fillTemplateSB2(t2->second, op, b.params, depth);
    // 扩展积木（extensions/*.js 里声明的）—— 与 sb script 的渲染一致
    if (g_ext.loaded) {
        auto et = g_ext.info.blockTypes.find(op);
        if (et != g_ext.info.blockTypes.end())
            return extBlockText(op, et->second, b.params, depth);
    }
    return op;  // 未收录：原样显示 opcode
}

// 帽子的中文
std::string hatText(const SbcScript& sc) {
    if (sc.hat == "flag")      return "当绿旗被点击";
    if (sc.hat == "clone")     return "当作为克隆体启动时";
    if (sc.hat == "click")     return "当角色被点击";
    if (sc.hat == "key")       return "当按下 " + sc.hatArg + " 键";
    if (sc.hat == "broadcast") return "当接收到 " + sc.hatArg;
    return "@script " + sc.hat + (sc.hatArg.empty() ? "" : " " + sc.hatArg);
}

void emitBlocks(const std::vector<SbcBlock>& blocks, int indent,
                std::vector<ViewLine>& out, int& count) {
    for (const auto& b : blocks) {
        ViewLine vl;
        vl.indent = indent;
        vl.text = blockText(b, 0);
        vl.opcode = b.opcode;
        vl.srcLine = b.line;
        out.push_back(std::move(vl));
        ++count;
        // 第一个子栈 = 那么/主体；其余（else 分支）同样展开，
        // else 本身插一层提示行便于阅读
        for (size_t si = 0; si < b.substacks.size(); ++si) {
            if (si == 1 && b.hasElse()) {
                ViewLine el;
                el.indent = indent;
                el.text = "否则";
                el.opcode = "else";
                el.srcLine = b.elseLine;
                out.push_back(std::move(el));
            } else if (si == 1) {
                ViewLine el;
                el.indent = indent;
                el.text = "否则";
                el.opcode = "else";
                el.srcLine = 0;
                out.push_back(std::move(el));
            }
            emitBlocks(b.substacks[si], indent + 1, out, count);
        }
    }
}

} // namespace

// ==========================================================================
// 主流程
// ==========================================================================

ViewReport sbcliView(const std::string& rootOrDir) {
    ViewReport rep;

    // ---- 定位项目根 ----
    std::string root;
    {
        fs::path input = fs::u8path(rootOrDir);
        fs::path p = fileExistsU8(rootOrDir) ? input.parent_path() : input;
        for (;;) {
            std::string ps = normPath(p.u8string());
            if (fileExistsU8(joinRel(ps, "meta.sbcli")) ||
                dirExistsU8(joinRel(ps, "character"))) { root = ps; break; }
            if (p == p.parent_path()) break;
            p = p.parent_path();
        }
    }
    if (root.empty()) {
        rep.error = "找不到项目根目录（需含 meta.sbcli 或 character/ 子目录）：" + rootOrDir;
        return rep;
    }
    while (root.size() > 3 && root.back() == '/') root.pop_back();
    rep.root = root;

    std::string rootMeta = joinRel(root, "meta.sbcli");
    if (fileExistsU8(rootMeta)) rep.projectName = loadProjectName(rootMeta);

    // 加载项目扩展（extensions/*.js）→ 扩展积木才能渲染成中文而不是裸 opcode。
    // 只在有 extensions/ 目录时才 spawn node：无扩展的项目完全不触发，
    // 行为与改动前一致（也不产生额外开销）。
    {
        std::error_code ecExt;
        if (fs::is_directory(fs::u8path(joinRel(root, "extensions")), ecExt)) {
            if (loadExtensionsFromDir(root, g_ext.info) > 0) g_ext.loaded = true;
        }
    }

    // ---- 列出角色目录 ----
    // 与 check 的发现逻辑保持一致：
    //   · 优先 character/ 下的 stage 与各数字 id 目录
    //   · character/ 不存在时（有些项目直接把角色放在根下）退化为递归找
    //     所有含 block.sbcli 的目录
    std::vector<std::string> spriteDirs;
    std::set<std::string> seen;
    std::error_code ec;
    fs::path ch = fs::u8path(root) / "character";
    if (dirExistsU8(normPath(ch.u8string()))) {
        // stage 固定排最前，其余按数字 id 升序
        fs::path stage = ch / "stage";
        if (dirExistsU8(normPath(stage.u8string())))
            spriteDirs.push_back(normPath(stage.u8string()));
        std::vector<std::pair<long long, std::string>> numbered;
        for (const auto& de : fs::directory_iterator(ch, ec)) {
            if (ec) break;
            if (!de.is_directory()) continue;
            std::string id = de.path().filename().u8string();
            if (id == "stage") continue;
            try { numbered.emplace_back(std::stoll(id), normPath(de.path().u8string())); }
            catch (...) { numbered.emplace_back(LLONG_MAX, normPath(de.path().u8string())); }
        }
        std::sort(numbered.begin(), numbered.end());
        for (auto& kv : numbered) spriteDirs.push_back(kv.second);
        for (const auto& d : spriteDirs) seen.insert(d);
    }
    // 兜底：character/ 缺失，或 character/ 下没有 block.sbcli（比如直接 hencoding
    // 把脚本放在根目录下）——递归找所有含 block.sbcli 的目录并补进来
    for (const auto& de : fs::recursive_directory_iterator(fs::u8path(root), ec)) {
        if (ec) break;
        if (!de.is_directory()) continue;
        std::string d = normPath(de.path().u8string());
        if (d.find("/.git") != std::string::npos) continue;
        if (!fileExistsU8(joinRel(d, "block.sbcli"))) continue;
        if (seen.insert(d).second) spriteDirs.push_back(d);
    }
    // 根目录自己也算（项目结构扁平，根下直接放 block.sbcli 的情况）
    if (fileExistsU8(joinRel(root, "block.sbcli")) && seen.insert(root).second)
        spriteDirs.push_back(root);

    for (const auto& dir : spriteDirs) {
        ViewSprite vs;
        vs.path = relToRoot(root, dir);
        vs.id = trimStr(fs::u8path(dir).filename().u8string());
        vs.isStage = (vs.id == "stage");

        std::string metaPath = joinRel(dir, "meta.sbcli");
        if (fileExistsU8(metaPath)) {
            SpriteMeta sm = loadSpriteMeta(metaPath);
            vs.hasMeta = true;
            vs.name = sm.name;
            vs.costumeCount = sm.costumes;
            vs.soundCount = sm.sounds;
        }

        std::string blockPath = joinRel(dir, "block.sbcli");
        if (!fileExistsU8(blockPath)) {
            if (vs.name.empty()) vs.name = vs.id;
            rep.sprites.push_back(std::move(vs));
            continue;
        }
        vs.hasBlock = true;
        SbcFile sf = sbcParseFile(blockPath);
        vs.scriptCount = (int)sf.scripts.size();
        int total = 0;
        for (const auto& sc : sf.scripts) {
            ViewScript vsc;
            vsc.hat = sc.hat;
            vsc.hatArg = sc.hatArg;
            vsc.line = sc.line;
            vsc.hatText = hatText(sc);
            emitBlocks(sc.blocks, 0, vsc.lines, total);
            vs.scripts.push_back(std::move(vsc));
        }
        vs.blockCount = total;
        if (vs.name.empty()) vs.name = vs.id;
        rep.sprites.push_back(std::move(vs));
    }

    return rep;
}

} // namespace sb
