// src/sbcli_check.cpp —— `sb check` 的检查引擎
//
// 站在 src/sbcli_parser 的 AST 上做语义检查。分工：
//   解析器 → 结构类诊断（词法/括号/缩进/else）
//   本文件 → 语义类诊断（opcode 收录、参数名与必填、ARG 数量、重复定义、
//                        reporter 深度、对 meta 的引用）
// 两者拼起来正好覆盖 docs/format.md §5 的四类检查项。
//
// 关于该 guard 的范围：解析器已经把非法结构尽可能修成一个"能继续遍历"的 AST，
// 所以这里对 opcode 为空的行会直接跳过，避免把同一个错误报两遍。

#include "sbcli_check.hpp"
#include "sbcli_parser.hpp"
#include "common.hpp"
#include "sb3_tables.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace sb {
namespace {

// ==========================================================================
// 目录与文件工具
// ==========================================================================

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

std::string normalizeSlashes(std::string s) {
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
    if (ec) return normalizeSlashes(abs);
    return normalizeSlashes(rp.u8string());
}

std::vector<std::string> readLines(const std::string& absUtf8) {
    std::vector<std::string> out;
    std::ifstream f(fs::u8path(absUtf8), std::ios::binary);
    if (!f) return out;
    std::string all((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    if (all.size() >= 3 && (unsigned char)all[0] == 0xEF &&
        (unsigned char)all[1] == 0xBB && (unsigned char)all[2] == 0xBF)
        all.erase(0, 3);
    std::istringstream is(all);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

// ==========================================================================
// meta.sbcli 解析
// ==========================================================================

struct MetaDoc {
    std::set<std::string> variables, lists, broadcasts;
    std::map<std::string, std::string> costumes;  // 名字 → 素材相对路径
    std::map<std::string, std::string> sounds;
};

// 去掉值两侧的引号
std::string unquote(std::string s) {
    s = trimStr(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

// 去掉包裹符并把内容按逗号切开（不处理引号内的逗号——meta 里足够用）
std::vector<std::string> splitListBody(const std::string& raw) {
    std::string body = trimStr(raw);
    if (body.size() >= 2) {
        char a = body.front(), b = body.back();
        if ((a == '[' && b == ']') || (a == '{' && b == '}') ||
            (a == '(' && b == ')'))
            body = body.substr(1, body.size() - 2);
    }
    std::vector<std::string> out;
    std::string cur;
    bool inStr = false;
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c == '"') { inStr = !inStr; cur += c; continue; }
        if (c == ',' && !inStr) { out.push_back(trimStr(cur)); cur.clear(); continue; }
        cur += c;
    }
    if (!trimStr(cur).empty()) out.push_back(trimStr(cur));
    return out;
}

MetaDoc loadMetaDoc(const std::string& absPath, bool isRoot) {
    MetaDoc m;
    std::string section;
    for (const std::string& raw : readLines(absPath)) {
        // 剥注释（引号内的 # 不算）
        std::string s;
        bool inStr = false;
        for (char c : raw) {
            if (c == '"') { inStr = !inStr; s += c; continue; }
            if (c == '#' && !inStr) break;
            s += c;
        }
        s = trimStr(s);
        if (s.empty()) continue;

        if (s.front() == '[' && s.back() == ']') {
            std::string sec = trimStr(s.substr(1, s.size() - 2));
            if (sec == "variables") section = "variables";
            else if (sec == "lists") section = "lists";
            else if (sec == "broadcasts") section = "broadcasts";
            else section.clear();
            continue;
        }

        if (isRoot) {
            // 项目级：[variables]/[lists] 段里 `名 = 值`；[broadcasts] 段里只有名字
            if (section.empty()) continue;
            size_t eq = s.find('=');
            std::string nm;
            if (section == "broadcasts") nm = s;
            else nm = (eq == std::string::npos) ? s : s.substr(0, eq);
            nm = unquote(nm);
            if (nm.empty()) continue;
            if (section == "variables") m.variables.insert(nm);
            else if (section == "lists") m.lists.insert(nm);
            else m.broadcasts.insert(nm);
            continue;
        }

        // 角色级：`key: value`
        size_t colon = s.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trimStr(s.substr(0, colon));
        std::string val = trimStr(s.substr(colon + 1));

        if (key == "variables" || key == "lists") {
            // {名 = 初值, ...}
            std::string body = val;
            size_t lb = body.find('{');
            if (lb != std::string::npos) {
                size_t rb = body.rfind('}');
                if (rb != std::string::npos) body = body.substr(lb + 1, rb - lb - 1);
            }
            for (const auto& item : splitListBody(body)) {
                size_t eq = item.find('=');
                std::string nm = unquote(eq == std::string::npos ? item
                                                                 : item.substr(0, eq));
                if (nm.empty()) continue;
                (key == "variables" ? m.variables : m.lists).insert(nm);
            }
        } else if (key == "broadcasts") {
            for (const auto& item : splitListBody(val)) {
                std::string nm = unquote(item);
                if (!nm.empty()) m.broadcasts.insert(nm);
            }
        } else if (key == "costumes" || key == "sounds") {
            // [名: assets/xxx, ...]
            for (const auto& item : splitListBody(val)) {
                size_t colon2 = item.find(':');
                if (colon2 == std::string::npos) continue;
                std::string nm = unquote(item.substr(0, colon2));
                std::string file = unquote(item.substr(colon2 + 1));
                if (nm.empty()) continue;
                (key == "costumes" ? m.costumes : m.sounds)[nm] = file;
            }
        }
    }
    return m;
}

// ==========================================================================
// 必填参数表
// ==========================================================================

// format.md §5.2：必填参数缺失要报错。
// 这里列的是"缺了就完全不成块"的那些；可选参数（如 control_for_each 的 VALUE）
// 留给 pack 时的默认值处理。键统一用 SB3 真实字段名（解析器已规范化简写）。
const std::map<std::string, std::set<std::string>>& requiredParams() {
    static const std::map<std::string, std::set<std::string>> req = {
        {"motion_movesteps", {"STEPS"}},
        {"motion_turnright", {"DEGREES"}},
        {"motion_turnleft",  {"DEGREES"}},
        {"motion_gotoxy",    {"X", "Y"}},
        {"motion_glidesecstoxy", {"SECS", "X", "Y"}},
        {"motion_pointindirection", {"DIRECTION"}},
        {"motion_changexby", {"DX"}}, {"motion_setx", {"X"}},
        {"motion_changeyby", {"DY"}}, {"motion_sety", {"Y"}},
        {"motion_setrotationstyle", {"STYLE"}},
        {"looks_sayforsecs", {"MESSAGE", "SECS"}},
        {"looks_say",        {"MESSAGE"}},
        {"looks_thinkforsecs", {"MESSAGE", "SECS"}},
        {"looks_think",      {"MESSAGE"}},
        {"looks_switchcostumeto",  {"COSTUME"}},
        {"looks_switchbackdropto", {"BACKDROP"}},
        {"looks_changesizeby", {"CHANGE"}},
        {"looks_setsizeto",   {"SIZE"}},
        {"looks_changeeffectby", {"EFFECT", "CHANGE"}},
        {"looks_seteffectto",   {"EFFECT", "VALUE"}},
        {"looks_gotofrontback", {"FRONT_BACK"}},
        {"looks_goforwardbackwardlayers", {"FORWARD_BACKWARD", "NUM"}},
        {"looks_costumenumbername",   {"NUMBER_NAME"}},
        {"looks_backdropnumbername",  {"NUMBER_NAME"}},
        {"sound_play",          {"SOUND_MENU"}},
        {"sound_playuntildone", {"SOUND_MENU"}},
        {"sound_changeeffectby", {"EFFECT", "VALUE"}},
        {"sound_seteffectto",   {"EFFECT", "VALUE"}},
        {"sound_changevolumeby", {"VOLUME"}},
        {"sound_setvolumeto",   {"VOLUME"}},
        {"event_whenkeypressed", {"KEY_OPTION"}},
        {"event_whenbackdropswitchesto", {"BACKDROP"}},
        {"event_whengreaterthan", {"WHENGREATERTHANMENU", "VALUE"}},
        {"event_whenbroadcastreceived", {"BROADCAST_OPTION"}},
        {"event_broadcast",        {"BROADCAST_INPUT"}},
        {"event_broadcastandwait", {"BROADCAST_INPUT"}},
        {"control_wait", {"DURATION"}},
        {"control_repeat", {"TIMES"}},
        {"control_if",         {"CONDITION"}},
        {"control_if_else",    {"CONDITION"}},
        {"control_wait_until",  {"CONDITION"}},
        {"control_repeat_until", {"CONDITION"}},
        {"control_while",        {"CONDITION"}},
        {"control_for_each", {"VARIABLE", "VALUE"}},
        {"control_stop", {"STOP_OPTION"}},
        {"control_create_clone_of", {"CLONE_OPTION"}},
        {"sensing_touchingobject", {"TOUCHINGOBJECTMENU"}},
        {"sensing_touchingcolor",  {"COLOR"}},
        {"sensing_coloristouchingcolor", {"COLOR", "COLOR2"}},
        {"sensing_distanceto", {"DISTANCETOMENU"}},
        {"sensing_askandwait", {"QUESTION"}},
        {"sensing_keypressed", {"KEY_OPTION"}},
        {"sensing_of",      {"OBJECT", "PROPERTY"}},
        {"sensing_current", {"CURRENTMENU"}},
        {"sensing_setdragmode", {"DRAG_MODE"}},
        {"operator_add",      {"NUM1", "NUM2"}},
        {"operator_subtract", {"NUM1", "NUM2"}},
        {"operator_multiply", {"NUM1", "NUM2"}},
        {"operator_divide",   {"NUM1", "NUM2"}},
        {"operator_mod",      {"NUM1", "NUM2"}},
        {"operator_random", {"FROM", "TO"}},
        {"operator_gt", {"OPERAND1", "OPERAND2"}},
        {"operator_lt", {"OPERAND1", "OPERAND2"}},
        {"operator_equals", {"OPERAND1", "OPERAND2"}},
        {"operator_and", {"OPERAND1", "OPERAND2"}},
        {"operator_or",  {"OPERAND1", "OPERAND2"}},
        {"operator_not", {"OPERAND"}},
        {"operator_join",      {"STRING1", "STRING2"}},
        {"operator_letter_of", {"LETTER", "STRING"}},
        {"operator_length",    {"STRING"}},
        {"operator_contains",  {"STRING1", "STRING2"}},
        {"operator_round",  {"NUM"}},
        {"operator_mathop", {"NUM", "OPERATOR"}},
        {"data_variable", {"VARIABLE"}},
        {"data_setvariableto",    {"VARIABLE", "VALUE"}},
        {"data_changevariableby", {"VARIABLE", "VALUE"}},
        {"data_showvariable", {"VARIABLE"}},
        {"data_hidevariable", {"VARIABLE"}},
        {"data_listcontents", {"LIST"}},
        {"data_addtolist",  {"ITEM", "LIST"}},
        {"data_deleteoflist", {"LIST", "INDEX"}},
        {"data_deletealloflist", {"LIST"}},
        {"data_insertatlist", {"ITEM", "LIST", "INDEX"}},
        {"data_replaceitemoflist", {"LIST", "INDEX", "ITEM"}},
        {"data_itemoflist", {"LIST", "INDEX"}},
        {"data_itemnumoflist", {"ITEM", "LIST"}},
        {"data_lengthoflist", {"LIST"}},
        {"data_listcontainsitem", {"LIST", "ITEM"}},
        {"data_showlist", {"LIST"}},
        {"data_hidelist", {"LIST"}},
        {"argument_reporter_string_number", {"VALUE"}},
        {"argument_reporter_boolean", {"VALUE"}},
        {"procedures_definition", {"PROCCODE"}},
        {"procedures_call", {"PROCCODE"}},
    };
    return req;
}

// opcode 收录判断统一走 core-parser 的 sbcIsKnownOpcode（单一权威，已覆盖
// SB3_T / SB2_T / procedures_* 等保留 opcode）。别名也由解析器归一化成
// SbcParam::canon，这里不再自行查表或重复报别名提示。

std::string joinWords(const std::set<std::string>& s, const std::string& sep) {
    std::string out;
    for (const auto& x : s) { if (!out.empty()) out += sep; out += x; }
    return out;
}

// PROCCODE 里的占位符个数（%s / %n / %b）
int countPlaceholders(const std::string& code) {
    int n = 0;
    for (size_t i = 0; i + 1 < code.size(); ++i)
        if (code[i] == '%' && (code[i + 1] == 's' || code[i + 1] == 'n' ||
                               code[i + 1] == 'b')) {
            ++n;
            ++i;
        }
    return n;
}

// ARG3 这样的名字 → true（ARG 后全是数字）
bool isArgKey(const std::string& k) {
    if (k.size() < 4 || k.compare(0, 3, "ARG") != 0) return false;
    return k.find_first_not_of("0123456789", 3) == std::string::npos;
}

} // namespace

// ==========================================================================
// 主流程
// ==========================================================================

namespace {
// 项目级诊断（不属于某个具体文件时用 "<project>" 作为归属）
void emitProjectDiag(CheckReport& rep, CheckLevel lv, const std::string& cat,
                     const std::string& code, const std::string& msg) {
    CheckDiag d;
    d.file = "<project>"; d.line = 0; d.col = 0; d.level = lv;
    d.category = cat; d.code = code; d.message = msg;
    rep.diags.push_back(std::move(d));
    if (lv == CheckLevel::Error) ++rep.errors; else ++rep.warnings;
}
} // namespace

CheckReport sbcliCheck(const std::string& rootOrDir) {
    CheckReport rep;

    // ---- 定位项目根目录 ----
    std::string root;
    {
        std::error_code ec;
        fs::path input = fs::u8path(rootOrDir);
        if (dirExistsU8(rootOrDir)) {
            if (dirExistsU8(joinRel(normalizeSlashes(rootOrDir), "character")) ||
                fileExistsU8(joinRel(normalizeSlashes(rootOrDir), "meta.sbcli")))
                root = normalizeSlashes(rootOrDir);
        } else if (fileExistsU8(rootOrDir)) {
            fs::path p = input.parent_path();
            for (;;) {
                std::string ps = normalizeSlashes(p.u8string());
                if (fileExistsU8(joinRel(ps, "meta.sbcli")) ||
                    dirExistsU8(joinRel(ps, "character"))) { root = ps; break; }
                if (p == p.parent_path()) break;
                p = p.parent_path();
            }
        }
        if (root.empty()) {
            // 也可能是指向了 character/ 或 character/1/ 本身
            fs::path p = input;
            bool found = false;
            while (!found) {
                std::string ps = normalizeSlashes(p.u8string());
                if (fileExistsU8(joinRel(ps, "meta.sbcli")) ||
                    dirExistsU8(joinRel(ps, "character"))) { root = ps; found = true; break; }
                if (p == p.parent_path()) break;
                p = p.parent_path();
            }
        }
    }
    if (root.empty()) {
        rep.error = "找不到项目根目录（需含 meta.sbcli 或 character/ 子目录）：" + rootOrDir;
        return rep;
    }
    while (root.size() > 3 && root.back() == '/') root.pop_back();
    rep.root = root;

    // ---- 收集 block.sbcli ----
    std::vector<std::string> absFiles;
    if (fileExistsU8(rootOrDir)) {
        absFiles.push_back(normalizeSlashes(rootOrDir));
    } else {
        std::set<std::string> dirs;
        std::error_code ec;
        for (fs::path base : {fs::u8path(root) / "character", fs::u8path(root)}) {
            if (!dirExistsU8(normalizeSlashes(base.u8string()))) continue;
            // recursive_directory_iterator 不含起点自身，两者都要试一遍，
            // 否则 root 自己就是角色目录（sb check character/1）时会漏掉
            std::vector<fs::path> candidates{base};
            for (const auto& de : fs::recursive_directory_iterator(base, ec)) {
                if (ec) break;
                if (de.is_directory()) candidates.push_back(de.path());
            }
            for (const auto& d : candidates)
                if (fileExistsU8(normalizeSlashes((d / "block.sbcli").u8string())))
                    dirs.insert(normalizeSlashes(d.u8string()));
        }
        for (const auto& d : dirs) absFiles.push_back(joinRel(d, "block.sbcli"));
    }
    if (absFiles.empty()) {
        rep.error = "项目里没有找到任何 block.sbcli（应在 character/stage/ 或 character/{id}/ 下）";
        return rep;
    }
    std::sort(absFiles.begin(), absFiles.end());

    // ---- 根 meta ----
    MetaDoc global;
    std::string rootMeta = joinRel(root, "meta.sbcli");
    if (fileExistsU8(rootMeta)) {
        global = loadMetaDoc(rootMeta, true);
        rep.rootMetaFound = true;
    }

    size_t fileSeq = 0;
    // ---- 逐个文件检查 ----
    // 广播孤儿判定要跨角色，所以发送/接收先汇总到项目级，最后统一裁决
    std::map<std::string, std::string> projectSent, projectRecv;  // 广播名 → "文件:行"
    for (const auto& abs : absFiles) {
        (void)fileSeq++;
        std::string rel = relToRoot(root, abs);
        rep.files.push_back(rel);

        // 角色级 meta
        MetaDoc local;
        std::string dir = normalizeSlashes(fs::u8path(abs).parent_path().u8string());
        std::string localMeta = joinRel(dir, "meta.sbcli");
        bool hasLocalMeta = false;
        if (fileExistsU8(localMeta) && normalizeSlashes(localMeta) != rootMeta) {
            local = loadMetaDoc(localMeta, false);
            hasLocalMeta = true;
        }

        SbcFile sf = sbcParseFile(abs);

        auto emit = [&](int line, int col, CheckLevel lv, const std::string& cat,
                        const std::string& code, const std::string& msg) {
            CheckDiag d;
            d.file = rel; d.line = line; d.col = col; d.level = lv;
            d.category = cat; d.code = code; d.message = msg;
            rep.diags.push_back(std::move(d));
            if (lv == CheckLevel::Error) ++rep.errors; else ++rep.warnings;
        };

        // ---- 解析器给的结构类诊断（词法 / 语法）----
        for (const auto& sd : sf.diags) {
            const char* cat = "lexical";
            std::string code = sd.code;
            std::string c = code.substr(0, 2);
            if (code.size() > 2 && (code[0] == 'E' || code[0] == 'W') && code[1] == '_')
                c = code.substr(2, 2);
            bool isErr = sd.isError();
            // 按短码把诊断分到 §5 的四类里
            if (code.find("UNCLOSED") != std::string::npos ||
                code.find("STRING") != std::string::npos ||
                code.find("ESCAPE") != std::string::npos ||
                code.find("INDENT") != std::string::npos ||
                code.find("TAB") != std::string::npos ||
                code.find("COMMENT") != std::string::npos)
                cat = "lexical";
            else if (code.find("ELSE") != std::string::npos ||
                     code.find("SCRIPT") != std::string::npos ||
                     code.find("HAT") != std::string::npos ||
                     code.find("PARAM") != std::string::npos ||
                     code.find("OPCODE") != std::string::npos ||
                     code.find("TOKEN") != std::string::npos ||
                     code.find("UNEXPECTED") != std::string::npos ||
                     code.find("SYNTAX") != std::string::npos)
                cat = "syntax";
            else if (code.find("DEEP") != std::string::npos ||
                     code.find("NEST") != std::string::npos)
                cat = "structure";
            else
                cat = "lexical";
            (void)c;
            emit(sd.line, sd.col, isErr ? CheckLevel::Error : CheckLevel::Warning,
                 cat, code, sd.message);
        }

        // ---- 语义检查：遍历 AST ----
        std::set<std::string> localProc;  // 本文件所有 procedures_definition（预扫描）
        std::set<std::string> seenDef;    // 遍历过程中已见过的定义（查重用）

        auto checkVarName = [&](const std::string& nm, int line) {
            if (nm.empty()) return;
            if (!global.variables.count(nm) && !local.variables.count(nm))
                emit(line, 0, CheckLevel::Warning, "reference", "undeclared-variable",
                     "变量「" + nm + "」未在 meta 声明（可用 `sb fix` 自动登记）");
        };
        auto checkListName = [&](const std::string& nm, int line) {
            if (nm.empty()) return;
            if (!global.lists.count(nm) && !local.lists.count(nm))
                emit(line, 0, CheckLevel::Warning, "reference", "undeclared-list",
                     "列表「" + nm + "」未在 meta 声明（可用 `sb fix` 自动登记）");
        };
        auto checkBroadcast = [&](const std::string& nm, int line) {
            if (nm.empty()) return;
            if (!global.broadcasts.count(nm) && !local.broadcasts.count(nm))
                emit(line, 0, CheckLevel::Warning, "reference", "undeclared-broadcast",
                     "广播「" + nm + "」未在 meta 的 broadcasts 声明"
                     "（可用 `sb fix` 自动登记）");
        };
        auto checkAsset = [&](const std::string& kind, const std::string& nm,
                              int line) {
            const bool isCostume = (kind == "造型");
            const auto& table = isCostume ? local.costumes : local.sounds;
            if (nm.empty()) return;
            auto it = table.find(nm);
            if (it == table.end()) {
                emit(line, 0, CheckLevel::Warning, "reference",
                     isCostume ? "undeclared-costume" : "undeclared-sound",
                     kind + "「" + nm + "」未在角色 meta 的 " +
                     (isCostume ? "costumes" : "sounds") +
                     " 声明（可用 `sb fix` 自动登记）");
                return;
            }
            if (it->second.empty()) {
                emit(line, 0, CheckLevel::Error, "reference", "asset-missing",
                     kind + "「" + nm + "」在 meta 里没有对应的素材文件");
                return;
            }
            std::string absAsset = joinRel(root, it->second);
            if (!fileExistsU8(absAsset))
                emit(line, 0, CheckLevel::Error, "reference", "asset-missing",
                     kind + "「" + nm + "」的素材文件不存在：" + it->second);
        };

        // 递归检查一个值（reporter 可嵌套 reporter）
        std::function<void(const SbcValue&, int, int)> walkValue;
        // 解析器在超过 100 层时就停止下钻并逐层报 E_DEEP_REPORTER，
        // 这里不再重复计数，避免同一处问题刷十几行。
        bool parserDepthReported = false;
        // 解析器在某行结构失败后，该行剩下的是"尽力恢复"出来的残缺 AST，
        // 再对它做语义检查只会刷出一串无意义的连带错误（未知参数/重复参数…）。
        // 结构错本身已经报过了，这里按行号屏蔽派生的语义诊断。
        std::set<int> parserErrorLines;
        for (const auto& sd : sf.diags) {
            if (sd.code.find("DEEP") != std::string::npos) parserDepthReported = true;
            if (sd.isError() && sd.line > 0) parserErrorLines.insert(sd.line);
        }
        bool depthReported = false;
        walkValue = [&](const SbcValue& v, int line, int depth) {
            if (v.kind != SbcValue::Kind::Reporter) return;  // 标量/列表无需下钻
            // 解析器超过上限就放弃下钻，剩下的是恢复出来的残缺结构；
            // 再去逐个判 opcode/参数只会刷出几十行无意义的连带错误，直接停。
            if (parserDepthReported) return;
            std::string vop = v.scalar.text;
            // 恢复产物的 opcode 可能是 "(" 或空 —— 已由 E_PARAM_EXPECTED 报过
            if (vop.empty() || !std::isalnum((unsigned char)vop[0])) return;
            if (parserErrorLines.count(line)) return;  // 该行结构已错，不叠加派生诊断
            bool known = sbcIsKnownOpcode(vop);
            if (!known) {
                emit(line, 0, CheckLevel::Error, "syntax", "unknown-opcode",
                     "未收录的 opcode「" + vop + "」（用 `sb search <关键词>` 查准确名字）");
                return;
            }
            auto fields = sbcOpcodeFields(vop);
            std::set<std::string> allowed(fields.begin(), fields.end());
            std::set<std::string> seen;
            for (const auto& p : v.args) {
                const std::string& key = p.canon.empty() ? p.key : p.canon;
                // obscured shadow 双键约定（unpack 输出 `KEY=<主块> __shadow_KEY=<桩>`）：
                // __shadow_ 前缀是保留标记，不是真实参数名，跳过白名单校验。
                if (p.key.compare(0, 9, "__shadow_") == 0) continue;
                if (allowed.count(key) == 0 && !fields.empty())
                    emit(line, 0, CheckLevel::Error, "syntax", "unknown-param",
                         "未知参数名「" + p.key + "」（" + vop + " 的参数：" +
                         joinWords(allowed, "、") + "）");
                else if (!fields.empty()) {
                    if (!seen.insert(key).second)
                        emit(line, 0, CheckLevel::Warning, "syntax", "duplicate-param",
                             "参数「" + p.key + "」重复出现，后者覆盖前者");
                }
                // 引用回收
                if (!p.value) continue;
                const SbcValue& pv = *p.value;
                // obscured shadow 双键（__shadow_KEY）是保留标记，不参与引用校验
                if (p.key.compare(0, 9, "__shadow_") == 0) continue;
                if (key == "VARIABLE")                      checkVarName(pv.text(), line);
                else if (key == "LIST")                     checkListName(pv.text(), line);
                else if (key == "BROADCAST_INPUT" ||
                         key == "BROADCAST_OPTION")         checkBroadcast(pv.text(), line);
                // 造型/声音名只对**字面量**校验；Reporter（如 (operator_join …)、
                // (looks_costume COSTUME="x")）是动态值或菜单桩，不是资产名。
                else if (key == "COSTUME" && pv.kind == SbcValue::Kind::Scalar)
                    checkAsset("造型", pv.text(), line);
                else if (key == "SOUND_MENU" && pv.kind == SbcValue::Kind::Scalar)
                    checkAsset("声音", pv.text(), line);
                if (pv.kind == SbcValue::Kind::Reporter)
                    walkValue(pv, line, depth + 1);
            }
            // 嵌套深度（§5.3 防爆）：解析器已报过就不再重复
            if (depth >= 100 && !depthReported) {
                if (!parserDepthReported)
                    emit(line, 0, CheckLevel::Error, "structure", "reporter-too-deep",
                         "reporter 嵌套超过 100 层上限（可能是括号没闭合或表达式失控）");
                depthReported = true;
            }
        };

        std::function<void(const SbcBlock&, int)> walkBlock;
        walkBlock = [&](const SbcBlock& b, int depth) {
            if (b.opcode.empty()) return;  // 非法行，解析器已报过
            // 该行结构已经错了：后续语义诊断多半是恢复产物，屏蔽掉避免刷屏
            if (b.line > 0 && parserErrorLines.count(b.line)) {
                for (const auto& sub : b.substacks)
                    for (const auto& cb : sub) walkBlock(cb, depth + 1);
                return;
            }

            int line = b.line;

            // ---- 语法：opcode 收录 ----
            if (!sbcIsKnownOpcode(b.opcode)) {
                std::string msg = "未收录的 opcode「" + b.opcode + "」";
                std::vector<std::string> near;
                for (const auto& kv : SB3_T)
                    if (kv.first.find(b.opcode) != std::string::npos ||
                        b.opcode.find(kv.first) != std::string::npos)
                        near.push_back(kv.first);
                if (!near.empty()) {
                    std::sort(near.begin(), near.end());
                    if (near.size() > 3) near.resize(3);
                    std::string s;
                    for (const auto& x : near) { if (!s.empty()) s += " / "; s += x; }
                    msg += "（是否想写 " + s + "？）";
                } else {
                    msg += "（不在 SB3_T/SB2_T 表中；用 `sb search <关键词>` 查准确名字）";
                }
                emit(line, 0, CheckLevel::Error, "syntax", "unknown-opcode", msg);
                return;  // opcode 都不认识，参数无从校验
            }

            // ---- 语法：参数名 / 必填 ----
            auto fields = sbcOpcodeFields(b.opcode);
            std::set<std::string> allowed(fields.begin(), fields.end());
            std::set<std::string> givenCanon;
            std::set<std::string> seen;

            for (const auto& p : b.params) {
                const std::string& key = p.canon.empty() ? p.key : p.canon;
                givenCanon.insert(key);

                if (b.opcode == "procedures_call" && (key == "ARGS" || isArgKey(key)))
                    continue;  // ARG1..N 由结构检查单独校验
                if (b.opcode == "procedures_definition" && key == "ARGS")
                    continue;
                // obscured shadow 双键约定：__shadow_ 前缀是保留标记，跳过白名单校验
                if (p.key.compare(0, 9, "__shadow_") == 0)
                    continue;

                if (!fields.empty() && allowed.count(key) == 0) {
                    // 欲 literal "值给了不存在的参数" —— 会被丢弃，报 warning 而非 error，
                    // 并提示正确的写法（format.md §2 的 looks_say ... SECS 是文档笔误）
                    if (p.key == "SECS" && b.opcode == "looks_say")
                        emit(line, 0, CheckLevel::Warning, "syntax", "value-discarded",
                             "looks_say 没有 SECS 参数，这个值会被丢弃"
                             "（要定时请用 looks_sayforsecs）");
                    else
                        emit(line, 0, CheckLevel::Error, "syntax", "unknown-param",
                             "未知参数名「" + p.key + "」（" + b.opcode + " 的参数：" +
                             joinWords(allowed, "、") + "）");
                } else if (!fields.empty()) {
                    if (!seen.insert(key).second)
                        emit(line, 0, CheckLevel::Warning, "syntax", "duplicate-param",
                             "参数「" + p.key + "」重复出现，后者覆盖前者");
                }

                // 引用回收（含 reporter 里的）
                if (p.value && p.key.compare(0, 9, "__shadow_") != 0) {
                    const SbcValue& pv = *p.value;
                    if (key == "VARIABLE")            checkVarName(pv.text(), line);
                    else if (key == "LIST")           checkListName(pv.text(), line);
                    else if (key == "BROADCAST_INPUT" ||
                             key == "BROADCAST_OPTION") checkBroadcast(pv.text(), line);
                    // 造型/声音只对字面量校验（Reporter 是动态值/菜单桩）
                    else if (key == "COSTUME" && pv.kind == SbcValue::Kind::Scalar)
                        checkAsset("造型", pv.text(), line);
                    else if (key == "SOUND_MENU" && pv.kind == SbcValue::Kind::Scalar)
                        checkAsset("声音", pv.text(), line);
                    if (pv.kind == SbcValue::Kind::Reporter)
                        walkValue(pv, line, 1);
                }
            }

            // 必填缺失（SB3_T 里的块）
            if (!fields.empty()) {
                auto rit = requiredParams().find(b.opcode);
                if (rit != requiredParams().end()) {
                    std::set<std::string> missing;
                    for (const auto& rn : rit->second)
                        if (!givenCanon.count(rn)) missing.insert(rn);
                    if (!missing.empty()) {
                        std::string fmt = SB3_T.count(b.opcode) ? SB3_T.at(b.opcode)
                                                                : b.opcode;
                        emit(line, 0, CheckLevel::Error, "syntax", "missing-param",
                             "缺必填参数 " + joinWords(missing, "、") + "（" + fmt + "）");
                    }
                }
            }

            // ---- 结构：procedures ----
            if (b.opcode == "procedures_definition" || b.opcode == "procedures_call") {
                std::string proccode;
                if (const SbcParam* p = b.find("PROCCODE"))
                    if (p->value && p->value->kind == SbcValue::Kind::Scalar)
                        proccode = p->value->text();
                if (proccode.empty())
                    emit(line, 0, CheckLevel::Error, "structure", "proccode-missing",
                         "缺 PROCCODE（自定义积木必须有 PROCCODE）");

                // 非法占位符（%% 之外只允许 %s/%n/%b）
                for (size_t i = 0; i + 1 < proccode.size(); ++i) {
                    if (proccode[i] != '%') continue;
                    char t = proccode[i + 1];
                    if (t == 's' || t == 'n' || t == 'b') { ++i; continue; }
                    if (t == '%') { ++i; continue; }
                    emit(line, 0, CheckLevel::Error, "structure", "bad-placeholder",
                         "PROCCODE 里有非法占位符 %" + std::string(1, t) +
                         "（只允许 %s / %n / %b）");
                }

                int nph = countPlaceholders(proccode);
                if (b.opcode == "procedures_definition") {
                    int nArgs = 0;
                    if (const SbcParam* p = b.find("ARGS"))
                        if (p->value && p->value->kind == SbcValue::Kind::List)
                            nArgs = (int)p->value->items.size();
                    if (nph != nArgs)
                        emit(line, 0, CheckLevel::Error, "structure", "arg-count-mismatch",
                             "ARGS 给了 " + std::to_string(nArgs) + " 个参数名，但 PROCCODE 有 " +
                             std::to_string(nph) + " 个占位符（应一致）");
                    if (!proccode.empty()) {
                        // 重复定义检测用单独的集合：localProc 是预扫描得到的全集，
                        // 若在这里再 insert 会把每个定义都判成"第二次出现"
                        if (!seenDef.insert(proccode).second)
                            emit(line, 0, CheckLevel::Error, "structure",
                                 "duplicate-definition",
                                 "procedures_definition 重复定义「" + proccode + "」");
                    }
                } else {
                    int n = 0;
                    for (const auto& p : b.params)
                        if (isArgKey(p.canon.empty() ? p.key : p.canon)) ++n;
                    if (nph != n)
                        emit(line, 0, CheckLevel::Error, "structure", "call-arg-count",
                             "procedures_call 给了 " + std::to_string(n) +
                             " 个 ARG，但 PROCCODE 有 " + std::to_string(nph) +
                             " 个占位符（应一致）");
                    // 调用找不到对应定义（definition 可能在本文件的其它 @script 里）
                    if (!proccode.empty() && !localProc.count(proccode))
                        emit(line, 0, CheckLevel::Error, "reference", "proc-undefined",
                             "procedures_call 调用了未定义的自定义积木「" + proccode +
                             "」（本文件里找不到对应的 procedures_definition）");
                }
            }

            // ---- 递归子栈 ----
            for (const auto& sub : b.substacks)
                for (const auto& cb : sub) walkBlock(cb, depth + 1);
        };

        // 定义先收齐再遍历，保证 call 能查到同文件里靠后的 definition
        std::set<std::string> allDefined;
        std::function<void(const SbcBlock&)> collectProc;
        collectProc = [&](const SbcBlock& b) {
            if (b.opcode == "procedures_definition")
                if (const SbcParam* p = b.find("PROCCODE"))
                    if (p->value && p->value->kind == SbcValue::Kind::Scalar)
                        allDefined.insert(p->value->text());
            for (const auto& sub : b.substacks)
                for (const auto& cb : sub) collectProc(cb);
        };
        for (const auto& sc : sf.scripts)
            for (const auto& b : sc.blocks) collectProc(b);
        localProc = std::move(allDefined);

        // ---- @script 帽子合法性（§5.2）----
        // 解析器已经在成套化地把非法帽子/缺参报过一层（E_HAT_UNKNOWN /
        // E_HAT_ARG_MISSING 等），这里只补它管不到的语义部分（广播名是否在 meta
        // 声明），避免同一处问题在输出里出现两行。
        std::set<int> parserHatLines;
        for (const auto& sd : sf.diags)
            if (sd.code.find("HAT") != std::string::npos ||
                sd.code.find("SCRIPT") != std::string::npos)
                parserHatLines.insert(sd.line);

        for (const auto& sc : sf.scripts) {
            if (sc.hat == "broadcast" && !sc.hatArg.empty())
                checkBroadcast(sc.hatArg, sc.line);
            else if (parserHatLines.count(sc.line) == 0) {
                const std::set<std::string> hats = {"flag", "broadcast", "key",
                                                    "clone", "click"};
                if (!hats.count(sc.hat))
                    emit(sc.line, 0, CheckLevel::Error, "syntax", "bad-hat",
                         "非法 @script 帽子类型「" + sc.hat +
                         "」（允许 flag / broadcast / key / clone / click）");
                else if ((sc.hat == "broadcast" || sc.hat == "key") && sc.hatArg.empty())
                    emit(sc.line, 0, CheckLevel::Error, "syntax", "hat-arg-missing",
                         "@script " + sc.hat + " 缺少参数（广播名/键名不能为空）");
            }
        }

        // ---- 遍历 ----
        for (const auto& sc : sf.scripts)
            for (const auto& b : sc.blocks) walkBlock(b, 0);

        // ---- 收集本文件的广播发送/接收，汇总到项目级 ----
        // 孤儿判定必须等所有文件都扫完再做（见函数末尾），
        // 否则跨角色收发（角色 A 发、角色 B 收）会被两边各自误判成孤儿。
        auto note = [&](std::map<std::string, std::string>& dst, const std::string& nm,
                        int line) {
            if (nm.empty()) return;
            dst.emplace(nm, rel + ":" + std::to_string(line));
        };
        std::function<void(const SbcBlock&)> scanBcast;
        scanBcast = [&](const SbcBlock& b) {
            for (const auto& p : b.params) {
                const std::string& key = p.canon.empty() ? p.key : p.canon;
                if ((key == "BROADCAST_INPUT" || key == "BROADCAST_OPTION") &&
                    p.value && p.value->kind == SbcValue::Kind::Scalar)
                    note(key == "BROADCAST_INPUT" ? projectSent : projectRecv,
                         p.value->text(), b.line);
                if (p.value && p.value->kind == SbcValue::Kind::Reporter)
                    for (const auto& ap : p.value->args) {
                        const std::string& ak = ap.canon.empty() ? ap.key : ap.canon;
                        if ((ak == "BROADCAST_INPUT" || ak == "BROADCAST_OPTION") &&
                            ap.value && ap.value->kind == SbcValue::Kind::Scalar)
                            note(ak == "BROADCAST_INPUT" ? projectSent : projectRecv,
                                 ap.value->text(), b.line);
                    }
            }
            for (const auto& sub : b.substacks)
                for (const auto& cb : sub) scanBcast(cb);
        };
        for (const auto& sc : sf.scripts) {
            if (sc.hat == "broadcast" && !sc.hatArg.empty())
                projectRecv[sc.hatArg] = rel + ":" + std::to_string(sc.line);
            for (const auto& b : sc.blocks) scanBcast(b);
        }
    }

    // ---- 孤儿广播（§5.4）：所有角色都汇总完再裁决 ----
    // 跨角色收发（角色 A 发、角色 B 收）是真实作品的常见结构，不能算孤儿。
    for (const auto& kv : projectSent) {
        if (!projectRecv.count(kv.first))
            emitProjectDiag(rep, CheckLevel::Warning, "reference", "broadcast-no-recv",
                            "广播「" + kv.first + "」只有发送没有接收脚本"
                            "（孤儿广播，发自 " + kv.second + "）");
    }
    for (const auto& kv : projectRecv) {
        if (!projectSent.count(kv.first))
            emitProjectDiag(rep, CheckLevel::Warning, "reference", "broadcast-no-send",
                            "广播「" + kv.first + "」只有接收没有发送脚本"
                            "（孤儿广播，接收于 " + kv.second + "）");
    }

    return rep;
}

} // namespace sb
