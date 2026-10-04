// src/sbcli_pack.cpp
// `sb pack <项目目录> <输出.sb3>` —— 把 sbcli 项目目录重新打包成 Scratch 3 (.sb3)。
//
// 映射要点（与 sb3_render / sb2.cpp 的读取端严格对称，确保 info/script 能读回）：
//  · 每个角色目录 <id>/block.sbcli → 一个 target（sprites）。
//  · 根 meta.sbcli 的 [variables]/[lists] 归舞台（带初始值）；[broadcasts] 全局。
//  · 缺省舞台：若根目录下没有 is_stage 的角色，则生成默认舞台 target（名字 "Stage"）。
//  · 积木：block.sbcli 的 AST（SbcBlock 树）逐块翻译成 SB3 block JSON；
//    inputs/fields 依据「键语义」划分（见 routeParam）：
//      VARIABLE / LIST  → field（带 id，按全局/角色 scope 用 sbcVarId 生成）
//      菜单类键（KEY_OPTION / BROADCAST_OPTION / STOP_OPTION / TOUCHINGOBJECTMENU …）
//                      → field（单元素 [值]）
//      BROADCAST_INPUT → input（[2,[10,"名"]]）
//      其余            → input（标量→内联影子 [1,[...]]，reporter→新块引用 [2,id]）
//  · 资源（造型/声音）：meta 里的 assets/xxx 路径；文件缺失时仍写入条目但告警。
//
// 变量/列表/广播的 id 用 sbcVarId / sbcBroadcastId（纯函数，与 fix/parser 一致）。
#include "sbcli_pack.hpp"

#include "sbcli_parser.hpp"
#include "sbcli_meta.hpp"
#include "sb3_tables.hpp"
#include "zip.hpp"
#include "jdoc.hpp"
#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace cm = sb::meta;

namespace sb {

namespace {

// ----------------------------------------------------------------- 基础编码

// 去掉包裹的引号（按 UTF-8 字符剥离首尾，避免误伤多字节中文）。
// 仅当文本确实以引号首尾包裹时才剥离；否则原样返回。
static std::string unquoteStr(const std::string& s) {
    if (s.size() < 2) return s;
    char f = s.front(), b = s.back();
    if (!((f == '"' && b == '"') || (f == '\'' && b == '\''))) return s;
    // 逐字符定位最后一个起始字符之后的位置（多字节安全）
    size_t start = 1;
    size_t end = s.size() - 1;
    // 从后往前找到 end 这个字符的起始字节
    while (end > 0 && (unsigned char)s[end] >= 0x80 && ((unsigned char)s[end] & 0xC0) == 0x80)
        --end;
    if (end <= start) return std::string();
    return s.substr(start, end - start);
}

// 裸标量 → SB3 内联影子值 [1,[<type>,<val>]]
//  type: 4=数字，10=文本/布尔字符串
Json primShadow(const SbToken& t) {
    Json v = Json::array();
    v.push_back(1);
    Json inner = Json::array();
    std::string s = t.text;
    // 去掉可能残留的引号（压平 key="x" 时会带引号，但 Scalar.text 通常已是裸值）
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);

    bool isNum = !s.empty();
    bool seenDot = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '-' && i == 0) continue;
        if (c == '.') {
            if (seenDot) { isNum = false; break; }
            seenDot = true;
            continue;
        }
        if (!std::isdigit((unsigned char)c)) { isNum = false; break; }
    }
    if (isNum) {
        inner.push_back(4);
        try {
            if (seenDot) inner.push_back(std::stod(s));
            else         inner.push_back((long long)std::stoll(s));
        } catch (...) { inner.push_back(s); }
    } else if (s == "true" || s == "false") {
        inner.push_back(10);
        inner.push_back(s);
    } else {
        inner.push_back(10);
        inner.push_back(s);
    }
    v.push_back(inner);
    return v;
}

// 块引用输入：Scratch 3 里 reporter 的引用是 [3, blockId, fallbackShadow]，
// fallback 是「引用为空时的默认值」——数字槽 [4,""]、文本槽 [10,""]。
// （早期实现用 [2,blockId]，能跑但重打包后与原始结构不一致，渲染会出现 ∅ 差异。）
Json refInput(const std::string& blockId, int shadowKind = 4) {
    Json v = Json::array();
    v.push_back(3);
    v.push_back(blockId);
    Json fb = Json::array();
    fb.push_back(shadowKind);
    fb.push_back("");
    v.push_back(fb);
    return v;
}

// 广播引用输入：BROADCAST_INPUT = [2,[10,"名"]]
Json broadcastInput(const std::string& name) {
    Json v = Json::array();
    v.push_back(2);
    Json inner = Json::array();
    inner.push_back(10);
    inner.push_back(name);
    v.push_back(inner);
    return v;
}

// 菜单类字段键（dropdowns / 选择器）。这些在 SB3 里存进 fields 而非 inputs。
const std::set<std::string>& menuFieldKeys() {
    // 权威来源 = sb3_tables.hpp 的 FIELD_MAP（菜单字段 → 值域的中文映射）。
    // 任何在 FIELD_MAP 里的键都是"菜单型字段"，应写入 sb3 的 fields 而非 inputs。
    // 这样新增菜单不用两处维护，也不会因手写清单遗漏导致归类漂移。
    static const std::set<std::string> s = [] {
        std::set<std::string> keys;
        for (const auto& kv : FIELD_MAP) keys.insert(kv.first);
        // 手工补充：FIELD_MAP 未收录、但确实属于 fields 的键（含扩展/特殊结构）
        static const char* extra[] = {
            "KEY_OPTION", "BROADCAST_OPTION", "STOP_OPTION", "PROCCODE",
            "TOUCHINGOBJECTMENU", "OBJECT", "PROPERTY", "VIDEO_STATE_MENU",
            "VIDEOOPTION", "GOTO", "LANGUAGE", "DRUM", "INSTRUMENT", "EFFECT",
            "CLONE_OPTION", "BACKDROPMENU", "COSTUMEMENU", "WHENGREATERTHANMENU",
            "ROTATION_STYLE", "DIRECTIONMENU", "GROUNDMENU", "STOPEVENT", "OUTCOME",
            "OPERATOR", "SET_OPTION", "TARGET_OPTION", "CLONEOPTION", "SOUND_MENU",
            "COSTUME", "BACKDROP", "TYPE", "COLOR_PARAM", "DISTANCETOMENU",
            "SENSING_OF", "PEN_COLOR", "PROPERTY_MENU", "TOUCHING_COLOR", "PITCH",
            "FRONT_BACK", "FORWARD_BACKWARD", "POSITION", "DIRECTION", "STOP_OPTION2",
            "SCALE", "CURRENTMENU", "SUBSECOND", "CLICK_OPTION", "BOOLEAN",
            "MOTION_OPTION",
        };
        for (auto k : extra) keys.insert(k);
        return keys;
    }();
    return s;
}

// ----------------------------------------------------------------- 上下文

struct PackCtx {
    std::string scope;                       // 当前 target 的 scope："1" 或 "stage"
    std::map<std::string, Json> blocks;      // id → block
    std::set<std::string> globalVars;        // 全局变量名（来自根 meta）
    std::set<std::string> globalLists;       // 全局列表名（来自根 meta）
    std::set<std::string>* missing = nullptr;// 未知 opcode 统计（可为空）
    long long counter = 0;
    std::string newId() { return "b" + std::to_string(++counter); }
};

// 前向声明
Json encodeValue(const SbcValue& val, PackCtx& ctx);
std::string emitReporterBlock(const SbcValue& val, PackCtx& ctx);
void routeParam(Json& b, const SbcParam& p, PackCtx& ctx,
                const std::string& opcode = "");

// 把一个值翻译成「输入编码」（标量→内联影子；reporter→新块引用；列表→文本退化）
Json encodeValue(const SbcValue& val, PackCtx& ctx) {
    switch (val.kind) {
        case SbcValue::Kind::Scalar:
            return primShadow(val.scalar);
        case SbcValue::Kind::Reporter: {
            std::string id = emitReporterBlock(val, ctx);
            return refInput(id);
        }
        case SbcValue::Kind::List: {
            Json arr = Json::array();
            for (const auto& it : val.items) arr.push_back(it.text);
            Json v = Json::array();
            v.push_back(1);
            Json inner = Json::array();
            inner.push_back(10);
            inner.push_back(arr.dump());
            v.push_back(inner);
            return v;
        }
    }
    return Json::array();
}

// 发射一个 reporter 块（可能递归产出子块），返回其 blockId 并写入 ctx.blocks
std::string emitReporterBlock(const SbcValue& val, PackCtx& ctx) {
    const std::string& op = val.scalar.text;
    const std::string& bid = ctx.newId();

    Json b = Json::object();
    b["opcode"]   = op;
    b["next"]     = Json();
    b["parent"]   = Json();
    b["inputs"]   = Json::object();
    b["fields"]   = Json::object();
    b["shadow"]   = false;
    b["topLevel"] = false;

    if (!sbcIsKnownOpcode(op) && ctx.missing) ctx.missing->insert(op);

    if (op == "argument_reporter_string_number" || op == "argument_reporter_boolean") {
        const std::string* nm = nullptr;
        if (!val.args.empty() && val.args[0].value &&
            val.args[0].value->kind == SbcValue::Kind::Scalar)
            nm = &val.args[0].value->scalar.text;
        b["fields"]["VALUE"] = Json::array();
        b["fields"]["VALUE"].push_back(nm ? *nm : std::string());
        b["shadow"] = true;
    } else {
        for (const auto& p : val.args) {
            if (!p.value) continue;
            routeParam(b, p, ctx);
        }
    }

    ctx.blocks[bid] = std::move(b);
    return bid;
}

// 把单个命名参数归位到 block b 的 inputs / fields
void routeParam(Json& b, const SbcParam& p, PackCtx& ctx,
                const std::string& opcode) {
    if (!p.value) return;
    const std::string& key = p.canon.empty() ? p.key : p.canon;
    const SbcValue& pv = *p.value;

    if (key == "VARIABLE") {
        const std::string& nm = pv.scalar.text;
        const std::string& id = ctx.globalVars.count(nm)
                                 ? sbcVarId("stage", nm) : sbcVarId(ctx.scope, nm);
        b["fields"]["VARIABLE"] = Json::array();
        b["fields"]["VARIABLE"].push_back(nm);
        b["fields"]["VARIABLE"].push_back(id);
    } else if (key == "LIST") {
        const std::string& nm = pv.scalar.text;
        const std::string& id = ctx.globalLists.count(nm)
                                 ? sbcVarId("stage", nm) : sbcVarId(ctx.scope, nm);
        b["fields"]["LIST"] = Json::array();
        b["fields"]["LIST"].push_back(nm);
        b["fields"]["LIST"].push_back(id);
    } else if (key == "BROADCAST_INPUT") {
        b["inputs"]["BROADCAST_INPUT"] = broadcastInput(pv.scalar.text);
    } else if (menuFieldKeys().count(key) && pv.kind == SbcValue::Kind::Scalar) {
        // 菜单字段：只有「标量值」才写进 fields。
        // 若值是 reporter（如 KEY_OPTION=(argument_reporter_string_number VALUE=键)），
        // 必须走 inputs 生成块引用，否则会把 reporter 的 opcode 当字面菜单值写坏。
        b["fields"][key] = Json::array();
        b["fields"][key].push_back(pv.scalar.text);
    } else if (!opcode.empty() && !sbcIsKnownOpcode(opcode)) {
        // 未知/扩展 opcode：文本层无法区分 field 与 input（unpack 都写成 KEY=value），
        // 用启发式避免重打包时分类漂移。
        // 规则：键名全大写（含下划线/数字）且值是不含空白的标量 → field；否则 input。
        // 仅对未知 opcode 生效；已知 opcode 走上面的精确表，行为不变。
        bool allUpper = !key.empty();
        for (char c : key)
            if (!(::isupper((unsigned char)c) || c == '_' || ::isdigit((unsigned char)c))) {
                allUpper = false; break;
            }
        bool simpleScalar = (pv.kind == SbcValue::Kind::Scalar);
        if (simpleScalar) {
            const std::string& t = pv.scalar.text;
            if (t.find(' ') != std::string::npos || t.find('\t') != std::string::npos)
                simpleScalar = false;
        }
        if (allUpper && simpleScalar) {
            b["fields"][key] = Json::array();
            b["fields"][key].push_back(pv.scalar.text);
        } else {
            b["inputs"][key] = encodeValue(pv, ctx);
        }
    } else {
        b["inputs"][key] = encodeValue(pv, ctx);
    }
}

// 发射一个普通块；返回该块 id。parentId 为该块的父（顶层时为空）。
// hatArg：@script 行剩余参数（仅顶层帽子块用，如广播名/按键名）。
std::string emitBlock(const SbcBlock& blk, PackCtx& ctx,
                      const std::string& parentId, bool topLevel,
                      const std::string& hatArg = "");

std::string emitBlock(const SbcBlock& blk, PackCtx& ctx,
                      const std::string& parentId, bool topLevel,
                      const std::string& hatArg) {
    const std::string& op = blk.opcode;
    const std::string& bid = ctx.newId();

    Json b = Json::object();
    b["opcode"]   = op;
    b["next"]     = Json();
    b["parent"]   = parentId.empty() ? Json() : Json(parentId);
    b["inputs"]   = Json::object();
    b["fields"]   = Json::object();
    b["shadow"]   = false;
    b["topLevel"] = topLevel;

    if (!sbcIsKnownOpcode(op) && ctx.missing) ctx.missing->insert(op);

    // 顶层帽子：把 @script 行的 hatArg 落到对应字段
    if (topLevel) {
        if (op == "event_whenbroadcastreceived") {
            b["fields"]["BROADCAST_OPTION"] = Json::array();
            b["fields"]["BROADCAST_OPTION"].push_back(hatArg);
        } else if (op == "event_whenkeypressed") {
            b["fields"]["KEY_OPTION"] = Json::array();
            b["fields"]["KEY_OPTION"].push_back(hatArg);
        }
        // event_whenflagclicked / event_whencloned / event_whenthisspriteclicked /
        // event_whenstageclicked / event_whenbackdropswitchesto 等无参数
    }

    bool isDef = (op == "procedures_definition");
    std::string protoId;
    if (op == "procedures_call" || isDef) {
        // 提取 proccode
        std::string proccode;
        for (const auto& p : blk.params) {
            if (!p.value) continue;
            std::string k = p.canon.empty() ? p.key : p.canon;
            if (k == "PROCCODE" && p.value->kind == SbcValue::Kind::Scalar)
                proccode = p.value->scalar.text;
        }
        if (proccode.front() == '"' || proccode.front() == '\'')
            proccode = unquoteStr(proccode);

        // 参数个数 = proccode 里的 %s/%n/%b 占位符个数（与 Scratch 一致）
        int nph = 0;
        for (size_t i = 0; i + 1 < proccode.size(); ++i)
            if (proccode[i] == '%' && std::strchr("snb", proccode[i + 1])) { ++nph; ++i; }

        // 参数名：definition 用 ARGS / ARGx；call 仅需 argumentids（与占位符位置对应）
        std::vector<std::string> argNames;   // 仅 definition 需要
        for (const auto& p : blk.params) {
            if (!p.value) continue;
            std::string k = p.canon.empty() ? p.key : p.canon;
            if (k == "ARGS" && p.value->kind == SbcValue::Kind::List) {
                for (const auto& it : p.value->items) argNames.push_back(it.text);
            }
        }
        if (argNames.empty()) {
            for (const auto& p : blk.params) {
                if (!p.value) continue;
                std::string k = p.canon.empty() ? p.key : p.canon;
                if (k.rfind("ARG", 0) == 0 && k != "ARGS")
                    argNames.push_back(p.value->scalar.text);
            }
        }
        // 用占位符数对齐 argumentnames 长度（不足补空、超出截断）
        while ((int)argNames.size() < nph) argNames.push_back("");
        argNames.resize(nph);

        // argumentids：与 Scratch 一致的无连字符 24 位十六进制（这里用 a0,a1… 足够解析）
        std::vector<std::string> argIds;
        for (int i = 0; i < nph; ++i) argIds.push_back("a" + std::to_string(i));

        Json mutation = Json::object();
        mutation["tagName"]        = "mutation";
        mutation["proccode"]       = proccode;
        // Scratch 3 的 mutation 里 argumentids/argumentnames/argumentdefaults
        // 都是「字符串化的 JSON 数组」（如 "[\"a0\"]"），不是真数组。
        Json idsArr = Json::array();
        for (const auto& id : argIds) idsArr.push_back(id);
        Json namesArr = Json::array();
        for (const auto& nm : argNames) namesArr.push_back(nm);
        Json defsArr = Json::array();
        for (size_t i = 0; i < argNames.size(); ++i) defsArr.push_back("");
        mutation["argumentids"]       = idsArr.dump();
        mutation["argumentnames"]     = namesArr.dump();
        mutation["argumentdefaults"]  = defsArr.dump();
        mutation["warp"]              = "false";

        b["mutation"] = mutation;

        if (op == "procedures_call") {
            // 实参：ARG1/ARG2…（或 ARGS 列表）按位置填到 argumentids
            int idx = 0;
            for (const auto& p : blk.params) {
                if (!p.value) continue;
                std::string k = p.canon.empty() ? p.key : p.canon;
                if (k == "ARGS" && p.value->kind == SbcValue::Kind::List) {
                    for (size_t j = 0; j < p.value->items.size() && idx < (int)argIds.size(); ++j)
                        b["inputs"][argIds[idx++]] = encodeValue(
                            SbcValue{SbcValue::Kind::Scalar, p.value->items[j]}, ctx);
                    continue;
                }
                if (k.rfind("ARG", 0) == 0 && k != "ARGS") {
                    if (idx < (int)argIds.size()) {
                        b["inputs"][argIds[idx]] = encodeValue(*p.value, ctx);
                        ++idx;
                    }
                    continue;
                }
                // 其余参数（理论上不会有）按名归位
                routeParam(b, p, ctx);
            }
            ctx.blocks[bid] = std::move(b);
            return bid;
        } else {
            // procedures_definition：需要一个 procedures_prototype 子块，
            // 其 mutation 与定义一致；definition 的 custom_block 输入引用它。
            protoId = ctx.newId();
            Json proto = Json::object();
            proto["opcode"]   = "procedures_prototype";
            proto["next"]     = Json();
            proto["parent"]   = bid;
            proto["inputs"]   = Json::object();
            proto["fields"]   = Json::object();
            proto["shadow"]   = true;
            proto["topLevel"] = false;
            proto["mutation"] = mutation;
            ctx.blocks[protoId] = std::move(proto);
            b["inputs"]["custom_block"] = refInput(protoId);
            // 子栈（定义体）正常递归，由后续统一处理
        }
    } else {
        for (const auto& p : blk.params) {
            if (!p.value) continue;
            routeParam(b, p, ctx, op);
        }
    }

    ctx.blocks[bid] = std::move(b);

    // 子栈：if / else / repeat / forever …（[0]=then，[1]=else）
    // procedures_definition 的定义体要挂在 prototype 的 next 链上（parent=prototype），
    // 而不是 SUBSTACK（Scratch 的渲染/读取都沿 prototype.next 走定义体）。
    std::string bodyAnchor = isDef ? protoId : bid;
    for (size_t si = 0; si < blk.substacks.size(); ++si) {
        const auto& sub = blk.substacks[si];
        if (sub.empty()) continue;
        std::string firstChild, prevInSub;
        for (size_t j = 0; j < sub.size(); ++j) {
            bool isFirst = (j == 0);
            std::string cid = emitBlock(sub[j], ctx, isFirst ? bodyAnchor : prevInSub, false);
            if (isFirst) firstChild = cid;
            else if (!prevInSub.empty()) ctx.blocks[prevInSub]["next"] = cid;
            prevInSub = cid;
        }
        if (isDef) {
            // 定义体直接链到 prototype.next（[0]=then）；忽略 else（定义体无分支）
            ctx.blocks[protoId]["next"] = firstChild;
        } else if (si == 0) {
            ctx.blocks[bid]["inputs"]["SUBSTACK"]  = refInput(firstChild);
        } else if (si == 1) {
            ctx.blocks[bid]["inputs"]["SUBSTACK2"] = refInput(firstChild);
        }
    }

    return bid;
}

// 发射一条脚本（从 hat 开始），返回 hat 块 id
std::string emitScript(const SbcScript& sc, PackCtx& ctx, int scriptIndex) {
    if (sc.blocks.empty()) return std::string();
    std::string headId, prevId;
    for (size_t i = 0; i < sc.blocks.size(); ++i) {
        bool top = (i == 0);
        std::string id = emitBlock(sc.blocks[i], ctx, top ? std::string() : prevId, top,
                                   top ? sc.hatArg : std::string());
        if (top) headId = id;
        else if (!prevId.empty()) ctx.blocks[prevId]["next"] = id;
        prevId = id;
    }
    if (!headId.empty()) {
        ctx.blocks[headId]["x"] = 20;
        ctx.blocks[headId]["y"] = -(long long)(scriptIndex * 150);
    }
    return headId;
}

// 引用收集（与 fix 同口径）：把脚本里用到的变量/列表/广播名归到一起
struct Refs { std::set<std::string> vars, lists, bcasts; };

void takeRef(Refs& r, const std::string& canonKey, const std::string& nm) {
    if (nm.empty()) return;
    if      (canonKey == "VARIABLE")                       r.vars.insert(nm);
    else if (canonKey == "LIST")                           r.lists.insert(nm);
    else if (canonKey == "BROADCAST_INPUT" ||
             canonKey == "BROADCAST_OPTION")               r.bcasts.insert(nm);
}

void collectBlockRefs(const SbcBlock& b, Refs& r) {
    std::function<void(const SbcValue&)> walk = [&](const SbcValue& v) {
        if (v.kind != SbcValue::Kind::Reporter) return;
        for (const auto& p : v.args) {
            if (!p.value) continue;
            const std::string& k = p.canon.empty() ? p.key : p.canon;
            const SbcValue& pv = *p.value;
            if (pv.kind == SbcValue::Kind::Scalar)      takeRef(r, k, pv.text());
            else if (pv.kind == SbcValue::Kind::Reporter) walk(pv);
        }
    };
    for (const auto& p : b.params) {
        if (!p.value) continue;
        const std::string& k = p.canon.empty() ? p.key : p.canon;
        const SbcValue& pv = *p.value;
        if (pv.kind == SbcValue::Kind::Scalar)        takeRef(r, k, pv.text());
        else if (pv.kind == SbcValue::Kind::Reporter) walk(pv);
    }
    for (const auto& sub : b.substacks)
        for (const auto& cb : sub) collectBlockRefs(cb, r);
}

void collectRefs(const std::vector<SbcScript>& scripts, Refs& r) {
    for (const auto& sc : scripts)
        for (const auto& b : sc.blocks) collectBlockRefs(b, r);
}

// 翻译整组脚本为一个 target 的 blocks 表（id→block）
Json buildTargetBlocks(const std::vector<SbcScript>& scripts, const std::string& scope,
                       const std::set<std::string>& globalVars,
                       const std::set<std::string>& globalLists,
                       std::set<std::string>* missing) {
    PackCtx ctx;
    ctx.scope = scope;
    ctx.globalVars = globalVars;
    ctx.globalLists = globalLists;
    ctx.missing = missing;
    for (size_t i = 0; i < scripts.size(); ++i) emitScript(scripts[i], ctx, (int)i);
    Json out = Json::object();
    for (auto& kv : ctx.blocks) out[kv.first] = std::move(kv.second);
    return out;
}

// ----------------------------------------------------------------- meta 取值

int metaInt(const cm::CharMeta& m, const std::string& k, int def) {
    auto it = m.kv.find(k);
    if (it == m.kv.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}
double metaDouble(const cm::CharMeta& m, const std::string& k, double def) {
    auto it = m.kv.find(k);
    if (it == m.kv.end()) return def;
    try { return std::stod(it->second); } catch (...) { return def; }
}
bool metaBool(const cm::CharMeta& m, const std::string& k, bool def) {
    auto it = m.kv.find(k);
    if (it == m.kv.end()) return def;
    return it->second == "true";
}
std::string metaStr(const cm::CharMeta& m, const std::string& k, const std::string& def) {
    auto it = m.kv.find(k);
    return it == m.kv.end() ? def : it->second;
}

// 造型/声音 → SB3 的 costumes/sounds 数组
Json jsonCostumes(const std::map<std::string, std::string>& m) {
    Json arr = Json::array();
    for (const auto& kv : m) {
        std::string path = kv.second;
        size_t dot = path.find_last_of('.');
        std::string ext  = (dot != std::string::npos) ? path.substr(dot + 1) : "";
        std::string base = sb::basename(path);
        std::string assetId = base;
        if (!ext.empty() && base.size() > ext.size() + 1)
            assetId = base.substr(0, base.size() - ext.size() - 1);
        Json c = Json::object();
        c["assetId"]         = assetId;
        c["name"]            = kv.first;
        c["md5ext"]          = base;
        c["dataFormat"]      = ext;
        c["rotationCenterX"] = 0;
        c["rotationCenterY"] = 0;
        arr.push_back(std::move(c));
    }
    return arr;
}
Json jsonSounds(const std::map<std::string, std::string>& m) {
    Json arr = Json::array();
    for (const auto& kv : m) {
        std::string path = kv.second;
        size_t dot = path.find_last_of('.');
        std::string ext  = (dot != std::string::npos) ? path.substr(dot + 1) : "";
        std::string base = sb::basename(path);
        std::string assetId = base;
        if (!ext.empty() && base.size() > ext.size() + 1)
            assetId = base.substr(0, base.size() - ext.size() - 1);
        Json s = Json::object();
        s["assetId"]     = assetId;
        s["name"]        = kv.first;
        s["dataFormat"]  = ext;
        s["rate"]        = 44100;
        s["sampleCount"] = 0;
        arr.push_back(std::move(s));
    }
    return arr;
}

} // namespace

// --------------------------------------------------------------- 命令入口
int cmd_pack(Args& a) {
    // 参数：a.file = 项目目录；a.extra[0] = 输出 .sb3 路径
    std::string dir = a.file;
    if (dir.empty()) dir = ".";
    std::string outPath;
    if (!a.extra.empty()) outPath = a.extra[0];
    if (outPath.empty()) {
        std::cerr << "错误：缺少输出文件名。用法：sb pack <项目目录> <输出.sb3>\n";
        return 2;
    }

    fs::path root(dir);
    if (!fs::exists(root) || !fs::is_directory(root)) {
        std::cerr << "错误：项目目录不存在：" << dir << "\n";
        return 2;
    }

    // 解析项目根
    fs::path base = cm::findProjectRoot(root.string());
    if (base.empty()) base = root;
    fs::path u8base = fs::u8path(base.string());

    // 根 meta
    cm::RootMeta rootMeta;
    fs::path rootMetaPath = u8base / "meta.sbcli";
    if (fs::exists(rootMetaPath))
        rootMeta = cm::loadRootMeta(rootMetaPath.string());

    std::string projectName = rootMeta.name.empty() ? "未命名作品" : rootMeta.name;

    std::set<std::string> globalVars, globalLists;
    for (const auto& kv : rootMeta.variables) globalVars.insert(kv.first);
    for (const auto& kv : rootMeta.lists)     globalLists.insert(kv.first);

    // 发现角色目录：扫描 base/character 或 base 自身下的子目录（含 meta.sbcli/block.sbcli 的
    // 即角色目录；与 sbcli_check/view 的判定一致：根是含 meta.sbcli 的目录，角色在子目录里）。
    struct SpriteInfo {
        std::string id;
        std::string name;
        bool        isStage = false;
        cm::CharMeta meta;
    };
    std::vector<SpriteInfo> sprites;
    bool hasStage = false;
    std::set<std::string> seenIds;
    auto scanDir = [&](const fs::path& top) {
        std::error_code ec;
        if (!fs::is_directory(top, ec)) return;
        for (auto& e : fs::directory_iterator(top, ec)) {
            if (!e.is_directory(ec)) continue;
            std::string id = e.path().filename().u8string();
            if (id == "meta.sbcli" || id == "character" || id == "assets") continue;
            fs::path cmPath = e.path() / "meta.sbcli";
            fs::path bkPath = e.path() / "block.sbcli";
            if (!fs::exists(cmPath, ec) && !fs::exists(bkPath, ec)) continue;
            if (seenIds.count(id)) continue;
            seenIds.insert(id);
            cm::CharMeta cmeta;
            if (fs::exists(cmPath, ec)) cmeta = cm::loadCharMeta(cmPath.string());
            SpriteInfo info;
            info.id = id;
            info.name = metaStr(cmeta, "name", "Sprite" + id);
            info.isStage = metaStr(cmeta, "is_stage", "false") == "true" || id == "stage";
            info.meta = std::move(cmeta);
            if (info.isStage) hasStage = true;
            sprites.push_back(std::move(info));
        }
    };
    scanDir(fs::u8path(base.string()) / "character");
    scanDir(fs::u8path(base.string()));
    std::sort(sprites.begin(), sprites.end(),
              [](const SpriteInfo& a, const SpriteInfo& b) {
                  // stage 排最前（Scratch 要求 Stage 是 targets[0]），其余按 id
                  if (a.isStage != b.isStage) return a.isStage;
                  return a.id < b.id;
              });

    // 收集未知 opcode
    std::set<std::string> missing;

    // 组装 targets
    Json targets = Json::array();

    // 舞台
    Json stage = Json::object();
    stage["isStage"] = true;
    stage["name"]    = "Stage";
    if (hasStage) {
        // 用 is_stage 角色充当舞台内容
        for (const auto& sp : sprites) {
            if (!sp.isStage) continue;
            fs::path blockPath = u8base / "character" / sp.id / "block.sbcli";
            if (!fs::exists(blockPath)) blockPath = u8base / sp.id / "block.sbcli";
            std::vector<SbcScript> scripts;
            if (fs::exists(blockPath))
                scripts = std::move(sbcParseFile(blockPath.string()).scripts);
            Json blocks = buildTargetBlocks(scripts, "stage", globalVars, globalLists, &missing);
            // 舞台变量 = 全局（带初始值）+ 该角色本地
            Json vars = Json::object();
            for (const auto& kv : rootMeta.variables) {
                std::string s = kv.second;
                if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
                    s = s.substr(1, s.size() - 2);
                bool isNum = !s.empty();
                for (char c : s) if (!std::isdigit((unsigned char)c) && c != '-' && c != '.') { isNum = false; break; }
                vars[sbcVarId("stage", kv.first)] = Json::array();
                vars[sbcVarId("stage", kv.first)].push_back(kv.first);
                if (isNum) { try { vars[sbcVarId("stage", kv.first)].push_back(std::stod(s)); }
                             catch (...) { vars[sbcVarId("stage", kv.first)].push_back(s); } }
                else        vars[sbcVarId("stage", kv.first)].push_back(s);
            }
            for (const auto& v : sp.meta.variables) {
                if (vars.contains(sbcVarId("stage", v))) continue;
                vars[sbcVarId("stage", v)] = Json::array();
                vars[sbcVarId("stage", v)].push_back(v);
                vars[sbcVarId("stage", v)].push_back(0);
            }
            Json lists = Json::object();
            for (const auto& kv : rootMeta.lists) {
                lists[sbcVarId("stage", kv.first)] = Json::array();
                lists[sbcVarId("stage", kv.first)].push_back(kv.first);
                lists[sbcVarId("stage", kv.first)].push_back(Json::array());
            }
            for (const auto& l : sp.meta.lists) {
                if (lists.contains(sbcVarId("stage", l))) continue;
                lists[sbcVarId("stage", l)] = Json::array();
                lists[sbcVarId("stage", l)].push_back(l);
                lists[sbcVarId("stage", l)].push_back(Json::array());
            }
            Json bcasts = Json::object();
            for (const auto& bc : rootMeta.broadcasts) {
                bcasts[sbcBroadcastId(bc)] = Json::array();
                bcasts[sbcBroadcastId(bc)].push_back(bc);
            }
            for (const auto& bc : sp.meta.broadcasts) {
                if (bcasts.contains(sbcBroadcastId(bc))) continue;
                bcasts[sbcBroadcastId(bc)] = Json::array();
                bcasts[sbcBroadcastId(bc)].push_back(bc);
            }
            stage["variables"]      = vars;
            stage["lists"]          = lists;
            stage["broadcasts"]     = bcasts;
            stage["blocks"]         = blocks;
            stage["comments"]       = Json::object();
            stage["currentCostume"] = metaInt(sp.meta, "current_costume", 0);
            stage["costumes"]       = jsonCostumes(sp.meta.costumes);
            stage["sounds"]         = jsonSounds(sp.meta.sounds);
            stage["volume"]         = 100;
            stage["layerOrder"]     = 0;
            stage["tempo"]          = 60;
            stage["videoTransparency"] = 50;
            stage["videoState"]     = "on";
            stage["textToSpeechLanguage"] = Json();
            break;
        }
    } else {
        stage["variables"]      = Json::object();
        stage["lists"]          = Json::object();
        stage["broadcasts"]     = Json::object();
        // 全局变量（带初始值）
        for (const auto& kv : rootMeta.variables) {
            std::string s = kv.second;
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
                s = s.substr(1, s.size() - 2);
            bool isNum = !s.empty();
            for (char c : s) if (!std::isdigit((unsigned char)c) && c != '-' && c != '.') { isNum = false; break; }
            stage["variables"][sbcVarId("stage", kv.first)] = Json::array();
            stage["variables"][sbcVarId("stage", kv.first)].push_back(kv.first);
            if (isNum) { try { stage["variables"][sbcVarId("stage", kv.first)].push_back(std::stod(s)); }
                         catch (...) { stage["variables"][sbcVarId("stage", kv.first)].push_back(s); } }
            else        stage["variables"][sbcVarId("stage", kv.first)].push_back(s);
        }
        for (const auto& kv : rootMeta.lists) {
            stage["lists"][sbcVarId("stage", kv.first)] = Json::array();
            stage["lists"][sbcVarId("stage", kv.first)].push_back(kv.first);
            stage["lists"][sbcVarId("stage", kv.first)].push_back(Json::array());
        }
        for (const auto& bc : rootMeta.broadcasts) {
            stage["broadcasts"][sbcBroadcastId(bc)] = Json::array();
            stage["broadcasts"][sbcBroadcastId(bc)].push_back(bc);
        }
        stage["blocks"]         = Json::object();
        stage["comments"]       = Json::object();
        stage["currentCostume"] = 0;
        stage["costumes"]       = Json::array();
        stage["sounds"]         = Json::array();
        stage["volume"]         = 100;
        stage["layerOrder"]     = 0;
        stage["tempo"]          = 60;
        stage["videoTransparency"] = 50;
        stage["videoState"]     = "on";
        stage["textToSpeechLanguage"] = Json();
    }
    targets.push_back(std::move(stage));

    // 角色
    int layer = 1;
    for (const auto& sp : sprites) {
        if (sp.isStage) continue;
        fs::path blockPath = u8base / "character" / sp.id / "block.sbcli";
        if (!fs::exists(blockPath)) blockPath = u8base / sp.id / "block.sbcli";
        std::vector<SbcScript> scripts;
        if (fs::exists(blockPath))
            scripts = std::move(sbcParseFile(blockPath.string()).scripts);

        // 收集引用，保证变量/列表都被登记（全局的归舞台，本地的归角色）
        Refs refs;
        collectRefs(scripts, refs);
        std::set<std::string> localset;
        auto addLocal = [&](const std::set<std::string>& src) {
            for (const auto& n : src) if (!globalVars.count(n) && !globalLists.count(n))
                localset.insert(n);
        };
        addLocal(refs.vars);   addLocal(refs.lists);
        addLocal(sp.meta.variables); addLocal(sp.meta.lists);

        // 注意：上面把变量和列表混在一个 localset 里；下面分别归位
        std::set<std::string> localVars, localLists;
        for (const auto& n : refs.vars)        if (!globalVars.count(n))  localVars.insert(n);
        for (const auto& n : sp.meta.variables) if (!globalVars.count(n)) localVars.insert(n);
        for (const auto& n : refs.lists)        if (!globalLists.count(n)) localLists.insert(n);
        for (const auto& n : sp.meta.lists)     if (!globalLists.count(n)) localLists.insert(n);
        (void)localset;

        Json vars = Json::object();
        for (const auto& v : localVars) {
            vars[sbcVarId(sp.id, v)] = Json::array();
            vars[sbcVarId(sp.id, v)].push_back(v);
            vars[sbcVarId(sp.id, v)].push_back(0);
        }
        Json lists = Json::object();
        for (const auto& l : localLists) {
            lists[sbcVarId(sp.id, l)] = Json::array();
            lists[sbcVarId(sp.id, l)].push_back(l);
            lists[sbcVarId(sp.id, l)].push_back(Json::array());
        }
        Json bcasts = Json::object();

        Json blocks = buildTargetBlocks(scripts, sp.id, globalVars, globalLists, &missing);

        Json sprite = Json::object();
        sprite["isStage"]        = false;
        sprite["name"]           = sp.name;
        sprite["variables"]      = vars;
        sprite["lists"]          = lists;
        sprite["broadcasts"]     = bcasts;
        sprite["blocks"]         = blocks;
        sprite["comments"]       = Json::object();
        sprite["currentCostume"] = metaInt(sp.meta, "current_costume", 0);
        sprite["costumes"]       = jsonCostumes(sp.meta.costumes);
        sprite["sounds"]         = jsonSounds(sp.meta.sounds);
        sprite["volume"]         = 100;
        sprite["layerOrder"]     = layer++;
        sprite["visible"]        = metaBool(sp.meta, "visible", true);
        sprite["x"]              = metaInt(sp.meta, "x", 0);
        sprite["y"]              = metaInt(sp.meta, "y", 0);
        sprite["size"]           = metaInt(sp.meta, "size", 100);
        sprite["direction"]      = metaInt(sp.meta, "direction", 90);
        sprite["rotationStyle"]  = metaStr(sp.meta, "rotation_style", "all_around");
        sprite["draggable"]      = metaBool(sp.meta, "draggable", false);
        targets.push_back(std::move(sprite));
    }

    // project.json
    Json project = Json::object();
    project["targets"]   = targets;
    project["monitors"]  = Json::array();
    project["extensions"]= Json::array();
    Json meta = Json::object();
    meta["semver"]           = "3.0.0";
    meta["vm"]               = "https://github.com/scratchfoundation/scratch-vm";
    meta["agent"]            = "https://github.com/scratchfoundation/scratch-www";
    meta["loader"]           = "https://github.com/scratchfoundation/scratch-loader";
    meta["renderer"]         = "https://github.com/scratchfoundation/scratch-render";
    meta["stage"]            = "https://github.com/scratchfoundation/scratch-stage";
    meta["isNativelyCreated"]= false;
    meta["hasCloudData"]     = false;
    project["meta"] = meta;

    std::string projectJson = project.dump(2);

    // 写 zip
    mzip::Writer w;
    if (!w.ok()) {
        std::cerr << "错误：无法初始化 zip 写入器\n";
        return 1;
    }
    if (!w.add("project.json", projectJson)) {
        std::cerr << "错误：写入 project.json 失败\n";
        return 1;
    }

    // 资源：遍历所有角色 + 舞台的 costumes/sounds
    // meta 里存的是**项目根相对路径**（如 assets/造型1.svg，由 sb project add-costume 写入），
    // 因此优先按项目根解析；同时兼容写成角色目录相对路径（spriteId/xxx）的旧数据。
    auto resolveAsset = [&](const std::string& spriteId,
                            const std::string& rel) -> fs::path {
        fs::path byRoot  = u8base / rel;                 // 项目根相对（标准）
        if (fs::exists(byRoot)) return byRoot;
        fs::path bySprite = u8base / spriteId / rel;     // 角色目录相对（兼容）
        if (fs::exists(bySprite)) return bySprite;
        fs::path byName = u8base / "assets" / sb::basename(rel);  // 兜底：assets 下按文件名
        if (fs::exists(byName)) return byName;
        return byRoot;                                   // 都不存在：报原始的根相对路径
    };
    auto addAssets = [&](const cm::CharMeta& m, const std::string& spriteId) {
        for (const auto& kv : m.costumes) {
            fs::path p = resolveAsset(spriteId, kv.second);
            if (fs::exists(p)) {
                auto data = sb::readFileBinary(p.string());
                w.add("assets/" + sb::basename(kv.second), data.data(), data.size());
            } else {
                std::cerr << "警告：造型资源缺失，跳过：" << p.string() << "\n";
            }
        }
        for (const auto& kv : m.sounds) {
            fs::path p = resolveAsset(spriteId, kv.second);
            if (fs::exists(p)) {
                auto data = sb::readFileBinary(p.string());
                w.add("assets/" + sb::basename(kv.second), data.data(), data.size());
            } else {
                std::cerr << "警告：声音资源缺失，跳过：" << p.string() << "\n";
            }
        }
    };
    for (const auto& sp : sprites) addAssets(sp.meta, sp.id);

    if (!w.finalize(outPath)) {
        std::cerr << "错误：生成 " << outPath << " 失败\n";
        return 1;
    }

    if (!missing.empty()) {
        std::cerr << "警告：遇到未知 opcode（已照原样写入，Scratch 打开时可能显示为灰色块）：\n";
        for (const auto& op : missing) std::cerr << "  - " << op << "\n";
    }

    std::cout << "已打包：" << outPath << "\n";
    std::cout << "  目标数（含舞台）：" << targets.size() << "\n";
    std::cout << "  项目名：" << projectName << "\n";
    return 0;
}

} // namespace sb
