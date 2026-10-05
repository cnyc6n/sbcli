// src/sbcli_unpack.cpp
// `sb unpack <作品.sb3> <输出目录>` —— 把 Scratch 3 作品拆成 sbcli 项目目录。
//
// 这是 src/sbcli_pack.cpp 的逆操作，两边必须**严格对称**，
// 否则 `sb unpack X F:\tmp\p && sb pack F:\tmp\p Y` 之后中文翻译会漂移。
// 对称点逐条对照（左边是 pack 的写入端，右边是本文件的读取端）：
//
//   pack: 块引用输入 [2, id]              unpack: kind 2/3 + 字符串 → 递归还原 reporter
//   pack: 内联影子 [1,[4,n]] / [1,[10,s]] unpack: kind 1 → [4,..]数字 / [10,..]带引号文本
//   pack: VARIABLE/LIST 存 fields 带 id   unpack: field 只取名字（id 由 sbcVarId 重算，一致）
//   pack: 菜单键存 fields（单元素数组）    unpack: field → KEY=值
//   pack: BROADCAST_INPUT 存 input        unpack: input → BROADCAST_INPUT=名
//   pack: @script 帽子 → 帽子字段          unpack: 帽子字段 → @script flag/broadcast/key/clone/click
//   pack: substack[0]→SUBSTACK [1]→SUBSTACK2 unpack: 两个子栈之间插 else 行
//   pack: procedures_definition + prototype 子块  unpack: 只出 procedures_definition 一行
//                                          （定义体挂在 prototype.next 上，随 prototype 一起跳过）
//
// 未收录的 opcode（不在 SB3_T/SB2_T、也不是保留 opcode）**不丢弃**：
// 照原样输出全部 fields/inputs 键值，并在行前加 `# 未知积木: <opcode>` 注释。
#include "sbcli_unpack.hpp"

#include "sbcli_parser.hpp"
#include "sbcli_meta.hpp"
#include "sb3.hpp"
#include "sapi.hpp"
#include "zip.hpp"
#include "common.hpp"
#include "sb3_tables.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

// ==========================================================================
// 小工具
// ==========================================================================

// 数值 → 文本：整数不带小数点，浮点用 %g（pack 侧用 stod 读回，往返无损）
std::string numToText(const Elem& e) {
    if (e.is_integer()) return std::to_string(e.i64());
    if (e.is_float()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", e.f64());
        return buf;
    }
    return std::string();
}

// 剥掉首尾成对引号（多字节安全，不动 UTF-8 续字节）
std::string unquoteStr(const std::string& s) {
    if (s.size() < 2) return s;
    char f = s.front(), b = s.back();
    if (!((f == '"' && b == '"') || (f == '\'' && b == '\''))) return s;
    size_t end = s.size() - 1;
    while (end > 0 && ((unsigned char)s[end] & 0xC0) == 0x80) --end;
    if (end <= 1) return std::string();
    return s.substr(1, end - 1);
}

// 长 alnum 串（Scratch 的变量/广播 id）—— 用来区分"名字"和"id"
bool looksLikeId(const std::string& s) {
    if (s.size() < 8 || s.size() > 64) return false;
    for (char c : s) {
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') return false;
    }
    // 纯数字不算 id（那是"变量名叫 2024"之类）
    bool anyAlpha = false;
    for (char c : s) if (std::isalpha((unsigned char)c)) { anyAlpha = true; break; }
    return anyAlpha;
}

// 裸标量文本 → sbcli 字面量（quoted=true 表示原文在 SB3 里是字符串型影子）
std::string literalText(const std::string& raw, bool quoted) {
    if (raw.empty()) return "\"\"";
    bool numeric = true, seenDot = false, seenDigit = false;
    for (size_t i = 0; i < raw.size(); ++i) {
        char ch = raw[i];
        if (ch == '-' && i == 0) continue;
        if (ch == '.') {
            if (seenDot) { numeric = false; break; }
            seenDot = true;
            continue;
        }
        if (!std::isdigit((unsigned char)ch)) { numeric = false; break; }
        seenDigit = true;
    }
    if (numeric && seenDigit) {
        bool leadingZero = raw.size() > 1 && raw[0] == '0' && raw[1] != '.';
        if (!leadingZero) return raw;          // 数字裸写（pack 侧会写成 [4,n]）
    }
    if (!quoted && (raw == "true" || raw == "false")) return raw;
    return "\"" + sbcEscape(raw) + "\"";
}
std::string boolText(bool v) { return v ? "true" : "false"; }

std::string rawJsonOf(const Elem& e) { return e.ok() ? compactJson(e.raw()) : "null"; }

// ==========================================================================
// 上下文
// ==========================================================================

struct UnpackCtx {
    const Elem* t = nullptr;
    std::map<std::string, Elem> blocks;
    std::map<std::string, std::string> varNames, listNames, bcastNames;  // id → 名
    // 作用域消歧用：当前 target 声明的变量/列表（按 id 与名字）以及全局的
    std::set<std::string> currentVarIds, currentListIds;
    std::set<std::string> currentVarNames, currentListNames;
    std::set<std::string> globalVarIds, globalListIds;
    std::set<std::string> globalVarNames, globalListNames;
    std::vector<std::string> lines;
    int unknown = 0;
    int depth   = 0;
    std::set<std::string> onStack;      // 正在展开的块（防成环）
};

void buildMaps(UnpackCtx& c) {
    const Elem& t = *c.t;
    Elem bl = t.at("blocks");
    if (bl.is_object())
        for (auto f : bl.obj()) c.blocks[std::string(f.key)] = Elem(f.value);

    auto collectPair = [](const Elem& obj, std::map<std::string, std::string>& out) {
        if (!obj.is_object()) return;
        for (auto f : obj.obj()) {
            Elem v(f.value);
            if (!v.is_array() || v.empty()) continue;
            Elem n0 = v.op(0);
            out[std::string(f.key)] = n0.is_string() ? std::string(n0.sv()) : rawJsonOf(n0);
        }
    };
    collectPair(t.at("variables"),  c.varNames);
    collectPair(t.at("lists"),      c.listNames);
    collectPair(t.at("broadcasts"), c.bcastNames);

    // 记录"当前 target 自己声明"的变量/列表（id + 名字），供同名消歧判断
    auto collectScope = [](const Elem& obj, std::set<std::string>& ids,
                           std::set<std::string>& names) {
        if (!obj.is_object()) return;
        for (auto f : obj.obj()) {
            Elem v(f.value);
            if (!v.is_array() || v.empty()) continue;
            Elem n0 = v.op(0);
            ids.insert(std::string(f.key));
            if (n0.is_string()) names.insert(std::string(n0.sv()));
        }
    };
    collectScope(t.at("variables"), c.currentVarIds,  c.currentVarNames);
    collectScope(t.at("lists"),     c.currentListIds, c.currentListNames);
}

const Elem* blockOf(const UnpackCtx& c, const std::string& id) {
    auto it = c.blocks.find(id);
    return it == c.blocks.end() ? nullptr : &it->second;
}
std::string opcodeOf(const Elem& b) {
    Elem op = b.at("opcode");
    return op.is_string() ? std::string(op.sv()) : std::string();
}

// ==========================================================================
// inputs / fields → sbcli 参数文本
// ==========================================================================

std::string shadowText(UnpackCtx& c, const Elem& x);
std::string inputText(UnpackCtx& c, const Elem& arr);
std::string blockExpr(UnpackCtx& c, const std::string& id, bool* ok);
std::string paramsText(UnpackCtx& c, const Elem& b, const std::string& op, bool skipSubstack);

// 影子数组 [type, value] → 标量文本
std::string shadowText(UnpackCtx& c, const Elem& x) {
    if (!x.is_array() || x.empty()) return x.ok() ? rawJsonOf(x) : "?";
    int t = x.op(0).is_number() ? (int)x.op(0).i64() : 0;
    Elem v = x.op(1);
    switch (t) {
        case 4: case 5: case 6: case 7: {
            // 数字类影子（6=整数 7=正角度）；v 也可能是变量 id（sb2 形态）
            if (v.is_number()) return literalText(numToText(v), false);
            if (v.is_string()) {
                std::string s(v.sv());
                auto vit = c.varNames.find(s);
                if (vit != c.varNames.end()) return literalText(vit->second, false);
                auto lit = c.listNames.find(s);
                if (lit != c.listNames.end()) return literalText(lit->second, false);
                if (s.empty()) return "";          // 数字空槽 → 裸空（pack 重建 [4,""]）
                return literalText(s, false);
            }
            return "?";
        }
        case 8:     // 角度
            if (v.is_number()) return literalText(numToText(v), false);
            return v.is_string() ? literalText(std::string(v.sv()), false) : "?";
        case 9:     // 颜色字面量 "#rrggbb"
            return v.is_string() ? literalText(std::string(v.sv()), true) : "?";
        case 10: case 11:   // 文本 / 广播
            if (v.is_string()) return literalText(std::string(v.sv()), true);
            if (v.is_number()) return literalText(numToText(v), true);
            return "?";
        case 12: case 13: { // 事件广播：value = [名, id]
            if (v.is_array() && !v.empty()) {
                Elem n0 = v.op(0);
                if (n0.is_string()) {
                    std::string nm(n0.sv());
                    if (nm.empty() && v.size() > 1 && v.op(1).is_string()) {
                        auto it = c.bcastNames.find(std::string(v.op(1).sv()));
                        if (it != c.bcastNames.end()) nm = it->second;
                    }
                    return literalText(nm, false);
                }
            }
            if (v.is_string()) {
                std::string id(v.sv());
                if (looksLikeId(id)) {
                    auto it = c.bcastNames.find(id);
                    if (it != c.bcastNames.end()) return literalText(it->second, false);
                }
                return literalText(id, false);
            }
            return "?";
        }
        default:
            return v.is_string() ? literalText(std::string(v.sv()), true) : rawJsonOf(x);
    }
}

// 一个 input 槽 → 参数值文本
std::string inputText(UnpackCtx& c, const Elem& arr) {
    if (!arr.is_array() || arr.empty()) return arr.ok() ? rawJsonOf(arr) : "?";
    int kind = arr.op(0).is_number() ? (int)arr.op(0).i64() : 0;
    Elem v = arr.op(1);
    switch (kind) {
        case 1:
            // 可能是内联 shadow 数组，也可能是块 id 字符串（引用 menu 块，
            // 如 looks_switchcostumeto 的 COSTUME → looks_costume_menu 块）。
            // 后者要沿引用取出 menu 块的字段值，否则会把 block id 当值写出去。
            if (v.is_string()) {
                std::string s(v.sv());
                auto bit = c.blocks.find(s);
                if (bit != c.blocks.end()) {
                    Elem mb = bit->second;
                    Elem mf = mb.at("fields");
                    if (mf.is_object()) {
                        // menu 块通常只有一个 field（COSTUME / CLONE_OPTION / TOUCHINGOBJECTMENU…）
                        auto keys = sortedKeysOf(mf.obj());
                        if (!keys.empty()) {
                            Elem fv = mf.at(keys[0]);
                            // 双元素 [名, id] 取名字；纯值直接取
                            Elem first = (fv.is_array() && !fv.empty()) ? fv.op(0) : fv;
                            if (first.is_string()) {
                                std::string nm(first.sv());
                                if (nm.empty() && fv.is_array() && fv.size() > 1 &&
                                    fv.op(1).is_string()) {
                                    // 名字空时用 id 反查（变量/广播表）
                                    std::string id(fv.op(1).sv());
                                    auto vit = c.varNames.find(id);
                                    if (vit != c.varNames.end()) nm = vit->second;
                                    else {
                                        auto bt2 = c.bcastNames.find(id);
                                        if (bt2 != c.bcastNames.end()) nm = bt2->second;
                                    }
                                }
                                return literalText(nm, false);
                            }
                            if (first.is_number()) return literalText(numToText(first), false);
                        }
                    }
                }
                return shadowText(c, v);
            }
            return shadowText(c, v);
        case 2:
            if (v.is_string()) {
                bool ok = false;
                std::string s = blockExpr(c, std::string(v.sv()), &ok);
                if (ok) return s;
                return literalText(std::string(v.sv()), true);   // 悬垂引用：保留文本
            }
            if (v.is_array()) return shadowText(c, v);
            return "?";
        case 3:
            if (v.is_string()) {
                bool ok = false;
                std::string s = blockExpr(c, std::string(v.sv()), &ok);
                if (ok) return s;
            }
            if (v.is_array()) return shadowText(c, v);
            return "?";
        default:
            if (v.is_string()) return literalText(std::string(v.sv()), true);
            return shadowText(c, v);
    }
}

// field（[值, id] 或裸值）→ 参数值文本
std::string fieldText(UnpackCtx& c, const Elem& f) {
    Elem v = (f.is_array() && !f.empty()) ? f.op(0) : f;
    if (v.is_string()) {
        std::string s(v.sv());
        // 不做 looksLikeId 前置过滤：id 不一定是纯 hex（sb2 遗留 id 形如 `@v@xxx`、
        // 或含 `|`/`]` 等字符）。过滤会漏掉映射，把 id 当名字输出。
        {
            auto vit = c.varNames.find(s);
            if (vit != c.varNames.end()) return literalText(vit->second, false);
            auto lit = c.listNames.find(s);
            if (lit != c.listNames.end()) return literalText(lit->second, false);
            auto bit = c.bcastNames.find(s);
            if (bit != c.bcastNames.end()) return literalText(bit->second, false);
        }
        return literalText(s, false);
    }
    if (v.is_number()) return literalText(numToText(v), false);
    if (v.is_bool())   return boolText(v.b());
    return v.ok() ? rawJsonOf(v) : "?";
}

// 变量/列表 field 的 [名, id]：默认只留名字（id 由 sbcVarId 重算，与 pack 天然一致）。
// 例外：当同名变量在**全局与当前角色都存在**时（Scratch 允许），名字不足以消歧，
// 必须按 id 输出 `@local:名` / `@global:名`，否则重打包会指向同一个变量 → 数据串位。
std::string varFieldText(UnpackCtx& c, const std::string& value) {
    // 不做 looksLikeId 前置过滤：变量 id 不一定是纯 hex（sb2 用 @v@xxx 形式），
    // 过滤会漏掉映射、把 id 当名字输出（曾造成 round-trip 回归）。
    {
        auto vit = c.varNames.find(value);
        if (vit != c.varNames.end()) {
            const std::string& nm = vit->second;
            // 消歧前缀 + 名字：名字部分按 literalText 规则（简单标识符裸写，否则引号），
            // 避免含空格/冒号的名字（如 "Stage: ii"）被词法按空格切分截断。
            auto scoped = [&](const std::string& prefix) -> std::string {
                std::string inner = literalText(nm, false);
                return prefix + inner;
            };
            if (c.currentVarIds.count(value)) {
                if (c.globalVarNames.count(nm)) return scoped("@local:");   // 全局也有同名 → 消歧
            } else if (c.globalVarIds.count(value)) {
                if (c.currentVarNames.count(nm)) return scoped("@global:");
            }
            return nm;
        }
        auto lit = c.listNames.find(value);
        if (lit != c.listNames.end()) {
            const std::string& nm = lit->second;
            auto scoped = [&](const std::string& prefix) -> std::string {
                std::string inner = literalText(nm, false);
                return prefix + inner;
            };
            if (c.currentListIds.count(value)) {
                if (c.globalListNames.count(nm)) return scoped("@local:");
            } else if (c.globalListIds.count(value)) {
                if (c.currentListNames.count(nm)) return scoped("@global:");
            }
            return nm;
        }
    }
    return value;
}

// mutation 里 argumentids 是「字符串化 JSON 数组」，取出来
std::vector<std::string> mutationArgIds(const Elem& b) {
    std::vector<std::string> ids;
    Elem mut = b.at("mutation");
    if (!mut.is_object()) return ids;
    Elem ai = mut.at("argumentids");
    if (!ai.is_string()) return ids;
    try {
        Json arr = Json::parse(std::string(ai.sv()));
        if (arr.is_array())
            for (auto& x : arr)
                if (x.is_string()) ids.push_back(x.get<std::string>());
    } catch (...) {}
    return ids;
}
std::string mutationProccode(const Elem& b) {
    Elem mut = b.at("mutation");
    if (!mut.is_object()) return std::string();
    Elem pc = mut.at("proccode");
    return pc.is_string() ? std::string(pc.sv()) : std::string();
}

// fields + inputs → " KEY=val KEY2=val2"（键排序，输出稳定）
std::string paramsText(UnpackCtx& c, const Elem& b, const std::string& op, bool skipSubstack) {
    std::string out;
    Elem fields = b.at("fields");
    if (fields.is_object()) {
        for (auto& k : sortedKeysOf(fields.obj())) {
            std::string val = fieldText(c, fields.at(k));
            if (k == "VARIABLE" || k == "LIST") {
                // 字段结构是 [名, id]：把 **id** 交给 varFieldText 做作用域消歧
                Elem fv = fields.at(k);
                std::string id;
                if (fv.is_array() && fv.size() > 1 && fv.op(1).is_string())
                    id = std::string(fv.op(1).sv());
                std::string nm = val;
                if (nm.size() >= 2 && nm.front() == '"' && nm.back() == '"') nm = unquoteStr(nm);
                std::string res = varFieldText(c, id.empty() ? nm : id);
                // `@local:名` / `@global:名` 是解析器认识的语法标记，不加引号输出；
                // 其余含特殊字符的名字仍走 literalText 的引号规则。
                if (res.rfind("@local:", 0) == 0 || res.rfind("@global:", 0) == 0)
                    val = res;
                else
                    val = literalText(res, false);
            }
            out += " " + k + "=" + val;
        }
    }
    // procedures_call：inputs 的键是 mutation 里的 argumentid（a0/a1…），
    // block.sbcli 侧要写成 ARG1..N（与 pack 的读法对称）
    std::vector<std::string> argIds;
    if (op == "procedures_call") argIds = mutationArgIds(b);

    Elem inputs = b.at("inputs");
    if (inputs.is_object()) {
        for (auto& k : sortedKeysOf(inputs.obj())) {
            if (k == "custom_block") continue;                 // 自定义积木的 prototype 引用
            if (skipSubstack && (k == "SUBSTACK" || k == "SUBSTACK2")) continue;
            std::string key = k;
            if (!argIds.empty()) {
                auto it = std::find(argIds.begin(), argIds.end(), k);
                if (it != argIds.end())
                    key = "ARG" + std::to_string((int)(it - argIds.begin()) + 1);
            }
            out += " " + key + "=" + inputText(c, inputs.at(k));
        }
    }
    return out;
}

// ==========================================================================
// 块 → 表达式 / 行
// ==========================================================================

std::string indentOf(int depth) { return std::string((size_t)depth * 2, ' '); }

// procedures_call → (procedures_call PROCCODE="x" ARG1=.. ARG2=..)
std::string proceduresCallExpr(UnpackCtx& c, const Elem& b) {
    std::string proccode = mutationProccode(b);
    std::vector<std::string> argIds = mutationArgIds(b);
    if (proccode.empty()) {
        Elem fields = b.at("fields");
        if (fields.is_object() && fields.contains("PROCCODE")) {
            std::string t = fieldText(c, fields.at("PROCCODE"));
            proccode = (t.size() >= 2 && t.front() == '"') ? unquoteStr(t) : t;
        }
    }
    std::string out = "(procedures_call PROCCODE=" + literalText(proccode, true);
    int n = 1;
    for (const auto& aid : argIds) {
        Elem inputs = b.at("inputs");
        if (!inputs.is_object() || !inputs.contains(aid)) continue;
        out += " ARG" + std::to_string(n++) + "=" + inputText(c, inputs.at(aid));
    }
    out += ")";
    return out;
}

// procedures_definition → PROCCODE="…" ARGS=[名, 名]
std::string definitionParams(UnpackCtx& c, const Elem& b, bool* hasArgs) {
    std::string proccode;
    std::vector<std::string> names;
    // mutation 优先（pack 写在 definition 上）；没有就找 prototype 子块
    auto readMutation = [&](const Elem& m) {
        if (!m.is_object()) return;
        Elem pc = m.at("proccode");
        if (pc.is_string() && proccode.empty()) proccode = std::string(pc.sv());
        Elem an = m.at("argumentnames");
        if (an.is_string() && names.empty()) {
            try {
                Json arr = Json::parse(std::string(an.sv()));
                if (arr.is_array())
                    for (auto& x : arr)
                        if (x.is_string()) names.push_back(x.get<std::string>());
            } catch (...) {}
        }
    };
    readMutation(b.at("mutation"));
    if (proccode.empty()) {
        Elem inputs = b.at("inputs");
        if (inputs.is_object() && inputs.contains("custom_block")) {
            Elem arr = inputs.at("custom_block");
            if (arr.is_array() && arr.size() > 1 && arr.op(1).is_string()) {
                const Elem* proto = blockOf(c, std::string(arr.op(1).sv()));
                if (proto) readMutation(proto->at("mutation"));
            }
        }
    }
    *hasArgs = !names.empty();
    // ARGS 元素：简单标识符裸写；含空格/逗号/引号的名字用双引号包（parseList 支持）
    std::string args;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) args += ", ";
        args += literalText(names[i], false);
    }
    return "PROCCODE=" + literalText(proccode, true) + " ARGS=[" + args + "]";
}

// 判断该 opcode 在 reporter 位置是否用尖括号（布尔）
bool isBoolOpcode(const std::string& op) {
    static const std::set<std::string> BOOL_OPS = {
        "operator_gt", "operator_lt", "operator_equals", "operator_and",
        "operator_or", "operator_not", "operator_contains",
        "sensing_touchingobject", "sensing_touchingcolor",
        "sensing_coloristouchingcolor", "sensing_keypressed", "sensing_mousedown",
    };
    return BOOL_OPS.count(op) != 0;
}

// 块 → 表达式文本（reporter 位置）。ok=false 表示找不到块。
std::string blockExpr(UnpackCtx& c, const std::string& id, bool* ok) {
    if (ok) *ok = false;
    const Elem* bp = blockOf(c, id);
    if (!bp) return "?";
    const Elem& b = *bp;
    std::string op = opcodeOf(b);
    if (op.empty()) return "?";
    if (c.depth > 60 || c.onStack.count(id)) return "?";     // 防御成环
    ++c.depth;
    c.onStack.insert(id);

    std::string out;
    // menu 影子块（*_menu 等）：本身不渲染成 reporter，直接输出它的菜单字段值
    //（如 sensing_of_object_menu → OBJECT=功能块）。否则会把 (sensing_of_object_menu)
    // 当作 reporter 包出来，导致空槽/悬垂（round-trip 差异的根源）。
    if (op.size() > 5 && op.compare(op.size() - 5, 5, "_menu") == 0) {
        Elem flds = b.at("fields");
        if (flds.is_object()) {
            for (auto& k : sortedKeysOf(flds.obj())) {
                Elem fv = flds.at(k);
                Elem n0 = (fv.is_array() && !fv.empty()) ? fv.op(0) : fv;
                if (n0.is_string()) {
                    std::string nm(n0.sv());
                    // [名, id] 取名字；名字空时用 id 反查（变量/广播）
                    if (nm.empty() && fv.is_array() && fv.size() > 1 &&
                        fv.op(1).is_string()) {
                        std::string id(fv.op(1).sv());
                        auto vit = c.varNames.find(id);
                        if (vit != c.varNames.end()) nm = vit->second;
                        else {
                            auto bit = c.bcastNames.find(id);
                            if (bit != c.bcastNames.end()) nm = bit->second;
                        }
                    }
                    out = literalText(nm, false);
                }
                break;
            }
        }
        if (out.empty()) out = "?";
    } else if (op == "procedures_call") {
        out = proceduresCallExpr(c, b);
    } else if (op == "argument_reporter_string_number" ||
               op == "argument_reporter_boolean") {
        // ARG 引用在 block.sbcli 里写成 reporter 形式（自定义积木定义体内）
        out = "(" + op + paramsText(c, b, op, false) + ")";
    } else {
        bool isBool = isBoolOpcode(op);
        out = (isBool ? "<" : "(") + op + paramsText(c, b, op, false) + (isBool ? ">" : ")");
    }

    c.onStack.erase(id);
    --c.depth;
    if (ok) *ok = true;
    return out;
}

// ==========================================================================
// 脚本（顶层块链 + 子栈）
// ==========================================================================

// 帽子 → @script 行
std::string hatDirective(UnpackCtx& c, const Elem& b, const std::string& op) {
    if (op == "event_whenflagclicked")           return "flag";
    if (op == "event_whencloned" || op == "control_start_as_clone")
                                                 return "clone";
    if (op == "event_whenthisspriteclicked" ||
        op == "event_whenstageclicked")          return "click";
    if (op == "event_whenbroadcastreceived") {
        Elem f = b.at("fields");
        std::string nm;
        if (f.is_object() && f.contains("BROADCAST_OPTION"))
            nm = fieldText(c, f.at("BROADCAST_OPTION"));
        if (nm.size() >= 2 && nm.front() == '"' && nm.back() == '"') nm = unquoteStr(nm);
        return "broadcast " + nm;
    }
    if (op == "event_whenkeypressed") {
        Elem f = b.at("fields");
        std::string nm;
        if (f.is_object() && f.contains("KEY_OPTION"))
            nm = fieldText(c, f.at("KEY_OPTION"));
        if (nm.size() >= 2 && nm.front() == '"' && nm.back() == '"') nm = unquoteStr(nm);
        return "key " + nm;
    }
    return std::string();   // 不是帽子
}

// 一个块（含 next 链与子栈）→ 行
void emitChain(UnpackCtx& c, const std::string& firstId, int depth);

void emitOne(UnpackCtx& c, const std::string& id, int depth, const std::string& op) {
    const Elem* bp = blockOf(c, id);
    if (!bp) return;
    const Elem& b = *bp;
    const std::string ind = indentOf(depth);

    // --- procedures_definition：先出行，再把定义体（挂在 prototype.next 上）缩进展开
    if (op == "procedures_definition") {
        bool hasArgs = false;
        std::string ps = definitionParams(c, b, &hasArgs);
        (void)hasArgs;
        c.lines.push_back(ind + "procedures_definition " + ps);
        // 定义体在 prototype 的 next 链上（Scratch 标准结构）：
        // pack 侧也是把定义体挂到 prototype.next，这里必须对称地认出来。
        Elem inputs = b.at("inputs");
        if (inputs.is_object() && inputs.contains("custom_block")) {
            Elem arr = inputs.at("custom_block");
            if (arr.is_array() && arr.size() > 1 && arr.op(1).is_string()) {
                std::string pid = std::string(arr.op(1).sv());
                const Elem* proto = blockOf(c, pid);
                if (proto) {
                    Elem nx = proto->at("next");
                    if (nx.is_string()) emitChain(c, std::string(nx.sv()), depth + 1);
                }
            }
        }
        return;
    }
    if (op == "procedures_prototype") return;   // 定义体锚点，不单独出行

    if (!sbcIsKnownOpcode(op)) {
        ++c.unknown;
        c.lines.push_back(ind + "# 未知积木: " + op);
        // 未知积木：结构化的 SUBSTACK 照常展开（内容尽量保留），
        // 其余 fields/inputs 原样键值输出
        c.lines.push_back(ind + op + paramsText(c, b, op, true));
        // 未知积木的 SUBSTACK 仍展开，避免丢脚本
        Elem inputs = b.at("inputs");
        if (inputs.is_object()) {
            for (const char* key : {"SUBSTACK", "SUBSTACK2"}) {
                Elem arr = inputs.at(key);
                if (!arr.is_array() || arr.size() < 2) continue;
                Elem child = arr.op(1);
                if (child.is_string()) {
                    if (std::string(key) == "SUBSTACK2")
                        c.lines.push_back(ind + "else");
                    emitChain(c, std::string(child.sv()), depth + 1);
                }
            }
        }
        return;
    }

    // procedures_call：实参键在 SB3 里是 mutation 的 argumentid（a0/arg1…），
    // 必须借 mutation 才能还原成 PROCCODE=… ARG1=…（栈位置与 reporter 位置同一套逻辑）
    if (op == "procedures_call") {
        std::string expr = proceduresCallExpr(c, b);
        if (expr.size() >= 2 && expr.front() == '(' && expr.back() == ')')
            expr = expr.substr(1, expr.size() - 2);      // 栈位置去掉外层括号
        c.lines.push_back(ind + expr);
        return;
    }

    c.lines.push_back(ind + op + paramsText(c, b, op, true));

    // 子栈：SUBSTACK → 缩进；SUBSTACK2 → 先出 else 行再缩进
    Elem inputs = b.at("inputs");
    if (!inputs.is_object()) return;
    for (const char* key : {"SUBSTACK", "SUBSTACK2"}) {
        Elem arr = inputs.at(key);
        if (!arr.is_array() || arr.size() < 2) continue;
        Elem child = arr.op(1);
        if (!child.is_string()) continue;
        if (std::string(key) == "SUBSTACK2") c.lines.push_back(ind + "else");
        emitChain(c, std::string(child.sv()), depth + 1);
    }
}

void emitChain(UnpackCtx& c, const std::string& firstId, int depth) {
    std::string cur = firstId;
    int guard = 0;
    while (!cur.empty() && guard++ < 20000) {
        if (c.onStack.count(cur)) return;        // 成环保护
        const Elem* bp = blockOf(c, cur);
        if (!bp) return;
        const Elem& b = *bp;
        std::string op = opcodeOf(b);

        // 定义体内：procedures_definition.body 走 prototype.next，
        // 这里遇到 prototype 就沿它的 next 继续（就是定义体）
        if (op == "procedures_prototype") {
            Elem nx = b.at("next");
            cur = nx.is_string() ? std::string(nx.sv()) : std::string();
            continue;
        }
        c.onStack.insert(cur);
        emitOne(c, cur, depth, op);
        c.onStack.erase(cur);

        Elem nx = b.at("next");
        cur = nx.is_string() ? std::string(nx.sv()) : std::string();
    }
}

// 一条脚本：从帽子开始
void emitScript(UnpackCtx& c, const std::string& hatId) {
    const Elem* bp = blockOf(c, hatId);
    if (!bp) return;
    const std::string& b = opcodeOf(*bp);
    std::string dir = hatDirective(c, *bp, b);
    if (!dir.empty()) {
        c.lines.push_back("@script " + dir);
        // 帽子自己的字段已经进了 @script 行，块行里不再重复（与 pack 的
        // "@script 行 → hatArg" 对称）
        emitChain(c, hatId, 0);
    } else {
        // 顶层但不是已知帽子（含 procedures_definition / 未知积木）：
        // 归到最有意义的 @script（定义 → flag，其余 → flag 便于手写时调）
        c.lines.push_back("@script flag");
        emitChain(c, hatId, 0);
    }
}

// ==========================================================================
// 顶层脚本排序（与 sb3 的 sb3TopScripts / Renderer::scripts 同口径）
// ==========================================================================

std::vector<std::string> topLevelIds(const Elem& target) {
    std::vector<std::pair<std::string, Elem>> tops;
    Elem bl = target.at("blocks");
    if (bl.is_object()) {
        for (auto f : bl.obj()) {
            Elem b(f.value);
            if (!b.is_object()) continue;
            if (b.at("topLevel").b() && !b.at("shadow").b())
                tops.emplace_back(std::string(f.key), b);
        }
    }
    std::sort(tops.begin(), tops.end(), [](const auto& a, const auto& b) {
        long long ya = a.second.i64At("y", 0), yb = b.second.i64At("y", 0);
        if (ya != yb) return ya > yb;
        long long xa = a.second.i64At("x", 0), xb = b.second.i64At("x", 0);
        if (xa != xb) return xa < xb;
        return a.first < b.first;
    });
    std::vector<std::string> out;
    for (auto& p : tops) out.push_back(p.first);
    return out;
}

// ==========================================================================
// 变量 / 列表 / 广播 / 素材
// ==========================================================================

// 变量初始值 → sbcli 字面量
std::string varInitText(const Elem& v) {
    if (!v.is_array() || v.size() < 2) return "0";
    Elem n0 = v.op(0);     // 名字
    Elem val = v.op(1);    // 值
    std::string nm = n0.is_string() ? std::string(n0.sv()) : std::string();
    (void)nm;
    if (val.is_number()) return numToText(val);
    if (val.is_bool())   return boolText(val.b());
    if (val.is_string()) {
        std::string s(val.sv());
        // pack 侧：变量初始值写回时只做"是不是数字"的判断（无引号 → [4,n]），
        // 所以裸数字字符串要保持裸写，否则重打包会变成字符串型。
        return literalText(s, false);
    }
    return "0";
}

// 列表初始内容 → [a, "b", c]
std::string listInitText(const Elem& v) {
    if (!v.is_array() || v.size() < 2) return "[]";
    Elem arr = v.op(1);
    if (!arr.is_array() || arr.empty()) return "[]";
    std::string out = "[";
    bool first = true;
    for (auto x : arr.arr()) {
        Elem e(x);
        if (!first) out += ", ";
        first = false;
        if (e.is_number())      out += numToText(e);
        else if (e.is_bool())   out += boolText(e.b());
        else if (e.is_string()) out += literalText(std::string(e.sv()), false);
        else                    out += literalText(rawJsonOf(e), false);
    }
    out += "]";
    return out;
}

// 素材条目名：md5ext 优先，其次 assetId+dataFormat。
// 注意 sb2 的素材在 zip 里以 **baseLayerID 数字**命名（如 "13.png"），
// 而桥接后的 md5ext 是 md5 名（zip 里根本没有）——所以 sb2 要用 baseLayerID
// 优先，md5ext 作为备选（返回多个候选，由 resolveZipEntry 逐个试）。
std::vector<std::string> assetEntryCandidates(const Elem& obj) {
    std::vector<std::string> out;
    Elem df = obj.at("dataFormat");
    std::string fmt = df.is_string() ? std::string(df.sv()) : std::string();

    // sb2：baseLayerID（造型数字名）或 baseLayerMD5；声音用 soundID
    Elem blid = obj.at("baseLayerID");
    Elem blmd5 = obj.at("baseLayerMD5");
    Elem sid = obj.at("soundID");
    if (blid.is_number()) {
        std::string s = std::to_string(blid.i64());
        if (!fmt.empty()) out.push_back(s + "." + fmt);
        out.push_back(s);
    } else if (sid.is_number()) {
        std::string s = std::to_string(sid.i64());
        if (!fmt.empty()) out.push_back(s + "." + fmt);
        out.push_back(s);
    } else if (blmd5.is_string() && !std::string(blmd5.sv()).empty()) {
        std::string s(blmd5.sv());
        out.push_back(s);
        if (!fmt.empty() && s.find('.') == std::string::npos) out.push_back(s + "." + fmt);
    }
    // sb3：md5ext
    Elem md5 = obj.at("md5ext");
    if (md5.is_string() && !std::string(md5.sv()).empty()) out.push_back(std::string(md5.sv()));
    // 通用：assetId + dataFormat
    Elem id = obj.at("assetId");
    if (id.is_string() && !std::string(id.sv()).empty()) {
        std::string s(id.sv());
        out.push_back(s);
        if (!fmt.empty() && s.find('.') == std::string::npos) out.push_back(s + "." + fmt);
    }
    // 去重保序
    std::vector<std::string> uniq;
    for (auto& c : out)
        if (!c.empty() && std::find(uniq.begin(), uniq.end(), c) == uniq.end())
            uniq.push_back(c);
    return uniq;
}

// 兼容旧接口：返回首选候选（首个）
std::string assetEntryName(const Elem& obj) {
    auto v = assetEntryCandidates(obj);
    return v.empty() ? std::string() : v[0];
}

// 在 zip 里找条目（大小写不敏感 + 兜底 assets/ 前缀）
std::string resolveZipEntry(const mzip::Reader& zip, const std::string& want) {
    if (want.empty()) return std::string();
    auto names = zip.names();
    auto lower = [](std::string s) {
        for (auto& ch : s) ch = (char)::tolower((unsigned char)ch);
        return s;
    };
    std::string w = lower(want);
    for (auto& n : names) if (lower(n) == w) return n;
    std::string w2 = lower("assets/" + want);
    for (auto& n : names) if (lower(n) == w2) return n;
    // 最后按 basename 比
    std::string base = want;
    size_t sl = base.find_last_of('/');
    if (sl != std::string::npos) base = base.substr(sl + 1);
    for (auto& n : names) {
        std::string nb = n;
        size_t sl2 = nb.find_last_of('/');
        if (sl2 != std::string::npos) nb = nb.substr(sl2 + 1);
        if (lower(nb) == lower(base)) return n;
    }
    return std::string();
}

struct AssetPlan {
    std::map<std::string, std::string> costumes;   // 名 → assets/<file>
    std::map<std::string, std::string> sounds;
    std::vector<std::string> zipEntries;           // 待导出的 zip 条目
};

AssetPlan planAssets(UnpackCtx& c, const Elem& target, std::set<std::string>& wanted,
                     const mzip::Reader* zip) {
    AssetPlan plan;
    auto handle = [&](const char* key, std::map<std::string, std::string>& out) {
        Elem arr = target.at(key);
        if (!arr.is_array()) return;
        for (auto ce : arr.arr()) {
            Elem o(ce);
            if (!o.is_object()) continue;
            Elem nm = o.at("name");
            std::string name = nm.is_string() ? std::string(nm.sv()) : std::string();
            // 候选条目名（sb2 的 baseLayerID / sb3 的 md5ext / assetId+format）；
            // 第一个能在 zip 里找到的即采用（sb2 用数字名、sb3 用 md5 名）。
            std::vector<std::string> cands = assetEntryCandidates(o);
            std::string entry;
            if (zip) {
                for (auto& cand : cands) {
                    if (!resolveZipEntry(*zip, cand).empty()) { entry = cand; break; }
                }
            }
            if (entry.empty() && !cands.empty()) entry = cands[0];   // 都找不到：留首个候选
            if (name.empty()) name = entry;
            std::string rel;
            if (!entry.empty()) {
                rel = "assets/" + entry;
                if (wanted.insert(entry).second) plan.zipEntries.push_back(entry);
            }
            out[name] = rel;
        }
    };
    handle("costumes", plan.costumes);
    handle("sounds",   plan.sounds);
    return plan;
}

} // namespace

// ==========================================================================
// 主入口
// ==========================================================================

UnpackResult sbcliUnpack(const std::string& sb3Path, const std::string& outDir,
                         bool overwrite) {
    UnpackResult r;
    std::ostringstream log;

    if (sb3Path.empty()) { r.error = "缺少输入文件。用法：sb unpack <作品.sb3> <输出目录>"; return r; }
    if (outDir.empty())  { r.error = "缺少输出目录。用法：sb unpack <作品.sb3> <输出目录>"; return r; }

    // 格式检查：unpack 只支持 zip 型作品（.sb3 / .sb2）。
    // Scratch 1.4 的 .sb 是 Squeak 二进制对象树，不是 zip，无法拆成项目目录。
    {
        std::string ext = sb::basename(sb3Path);
        size_t dot = ext.find_last_of('.');
        std::string e = (dot == std::string::npos) ? "" : ext.substr(dot);
        for (auto& ch : e) ch = (char)::tolower((unsigned char)ch);
        if (e == ".sb" || e == ".sprite") {
            r.error = "不支持 Scratch 1.4 格式（" + e + "）：它是 Squeak 二进制，不是 zip 包。\n"
                      "       请先用 Scratch 2/3 打开并另存为 .sb2/.sb3，再 unpack。";
            return r;
        }
    }

    // ---- 读作品 ----
    Sb3File f;
    try {
        f = sb3LoadAny(sb3Path);
    } catch (const std::exception& e) {
        r.error = e.what();
        return r;
    }
    if (!f.zip) { r.error = "无法读取作品：" + sb3Path; return r; }
    if (!f.targets.is_array() || f.targets.size() == 0) {
        r.error = "作品里没有 target（project.json 结构异常）";
        return r;
    }

    // ---- 输出目录 ----
    fs::path out = fs::u8path(outDir);
    std::error_code ec;
    if (fs::exists(out, ec)) {
        bool empty = true;
        for (auto it = fs::directory_iterator(out, ec); !ec && it != fs::directory_iterator(); ++it) {
            std::string fn = it->path().filename().u8string();
            if (fn == "." || fn == "..") continue;
            empty = false;
            break;
        }
        if (!empty && !overwrite) {
            r.error = "输出目录已存在且非空：" + outDir + "（加 --force 覆盖）";
            return r;
        }
    }
    if (!cm::makeDirs(cm::norm(outDir))) {
        r.error = "无法创建输出目录：" + outDir;
        return r;
    }
    r.outDir = cm::norm(outDir);

    // ---- 分出舞台与角色 ----
    std::vector<Elem> stages, sprites;
    std::vector<std::string> spriteNames;
    for (auto te : f.targets.arr()) {
        Elem t(te);
        if (!t.is_object()) continue;
        if (t.at("isStage").b()) stages.push_back(t);
        else                     sprites.push_back(t);
    }
    // 舞台永远存在（format.md §0：character/stage 固定）
    Elem stage = stages.empty() ? Elem() : stages.front();

    // ---- 根 meta ----
    cm::RootMeta root;
    root.exists = true;
    {
        std::string base = sb::basename(sb3Path);
        size_t dot = base.find_last_of('.');
        root.name = (dot == std::string::npos) ? base : base.substr(0, dot);
        fs::path stem = fs::u8path(sb3Path).stem();
        if (!stem.empty()) root.name = stem.u8string();
    }
    if (!root.name.empty()) root.name = std::string(root.name);

    // 全局变量/列表/广播 = 舞台 target 的对应表（Scratch 约定）
    std::set<std::string> globalVars, globalLists;
    std::set<std::string> globalVarIds, globalListIds;   // 按 id 判定"同一个变量"
    if (stage.ok()) {
        Elem vars = stage.at("variables");
        if (vars.is_object()) {
            for (auto vf : vars.obj()) {
                Elem v(vf.value);
                if (!v.is_array() || v.empty()) continue;
                Elem n0 = v.op(0);
                std::string nm = n0.is_string() ? std::string(n0.sv()) : rawJsonOf(n0);
                root.variables[nm] = varInitText(v);
                globalVars.insert(nm);
                globalVarIds.insert(std::string(vf.key));
            }
        }
        Elem lists = stage.at("lists");
        if (lists.is_object()) {
            for (auto lf : lists.obj()) {
                Elem l(lf.value);
                if (!l.is_array() || l.empty()) continue;
                Elem n0 = l.op(0);
                std::string nm = n0.is_string() ? std::string(n0.sv()) : rawJsonOf(n0);
                root.lists[nm] = listInitText(l);
                globalLists.insert(nm);
                globalListIds.insert(std::string(lf.key));
            }
        }
        Elem bc = stage.at("broadcasts");
        if (bc.is_object()) {
            for (auto bf : bc.obj()) {
                Elem b(bf.value);
                if (!b.is_array() || b.empty()) continue;
                Elem n0 = b.op(0);
                std::string nm = n0.is_string() ? std::string(n0.sv()) : rawJsonOf(n0);
                if (!nm.empty()) root.broadcasts.insert(nm);
            }
        }
    }

    // ---- 目标 → 目录 ----
    struct Plan {
        Elem        target;
        std::string dirId;      // "stage" / "1" / "2"…
        std::string name;
        bool        isStage = false;
        UnpackCtx   ctx;
        AssetPlan   assets;
    };
    std::vector<Plan> plans;

    auto makePlan = [&](const Elem& t, const std::string& dirId, bool isStage) {
        Plan p;
        p.target  = t;
        p.dirId   = dirId;
        p.isStage = isStage;
        Elem nm = t.at("name");
        p.name = nm.is_string() && !std::string(nm.sv()).empty()
                 ? std::string(nm.sv())
                 : (isStage ? "Stage" : ("角色" + dirId));
        p.ctx.t = &p.target;
        buildMaps(p.ctx);
        // 传入全局作用域信息（同名消歧用）
        p.ctx.globalVarIds    = globalVarIds;
        p.ctx.globalListIds   = globalListIds;
        p.ctx.globalVarNames  = globalVars;
        p.ctx.globalListNames = globalLists;
        return p;
    };

    plans.push_back(makePlan(stage.ok() ? stage : Elem(), "stage", true));
    {
        int id = 1;
        for (auto& t : sprites) {
            std::string d = std::to_string(id++);
            plans.push_back(makePlan(t, d, false));
            spriteNames.push_back(plans.back().name);
        }
    }

    // 全局变量/列表的 id→名映射合并进每个角色的 ctx：
    // 全局变量声明在舞台，但**任何角色的脚本都能引用**它；不合并的话角色里
    // 引用全局变量时查不到名字，会输出原始 id（round-trip 回归）。
    for (auto& p : plans) {
        if (p.isStage) continue;                 // 舞台本身就是全局声明处
        Elem sv = stage.ok() ? stage.at("variables") : Elem();
        if (sv.is_object())
            for (auto vf : sv.obj()) {
                Elem v(vf.value);
                if (!v.is_array() || v.empty()) continue;
                Elem n0 = v.op(0);
                if (n0.is_string())
                    p.ctx.varNames[std::string(vf.key)] = std::string(n0.sv());
            }
        Elem sl = stage.ok() ? stage.at("lists") : Elem();
        if (sl.is_object())
            for (auto lf : sl.obj()) {
                Elem l(lf.value);
                if (!l.is_array() || l.empty()) continue;
                Elem n0 = l.op(0);
                if (n0.is_string())
                    p.ctx.listNames[std::string(lf.key)] = std::string(n0.sv());
            }
        Elem sb = stage.ok() ? stage.at("broadcasts") : Elem();
        if (sb.is_object())
            for (auto bf : sb.obj()) {
                Elem b(bf.value);
                if (!b.is_array() || b.empty()) continue;
                Elem n0 = b.op(0);
                if (n0.is_string())
                    p.ctx.bcastNames[std::string(bf.key)] = std::string(n0.sv());
            }
    }

    // 目录里提前登记所有广播（含角色级 broadcasts 表里的）
    std::set<std::string> allBcasts = root.broadcasts;
    for (auto& p : plans) {
        Elem bc = p.target.at("broadcasts");
        if (!bc.is_object()) continue;
        for (auto bf : bc.obj()) {
            Elem b(bf.value);
            if (!b.is_array() || b.empty()) continue;
            Elem n0 = b.op(0);
            if (n0.is_string()) {
                std::string nm(n0.sv());
                if (!nm.empty()) allBcasts.insert(nm);
            }
        }
    }

    std::set<std::string> wantedEntries;      // zip 条目（去重）
    std::vector<std::string> allEntries;
    for (auto& p : plans) {
        p.assets = planAssets(p.ctx, p.target, wantedEntries, f.zip.get());
        for (const auto& e : p.assets.zipEntries)
            if (std::find(allEntries.begin(), allEntries.end(), e) == allEntries.end())
                allEntries.push_back(e);
    }

    // ---- 写每个角色目录 ----
    for (auto& p : plans) {
        std::string dir = cm::joinRel(cm::norm(outDir),
                                      cm::joinRel("character", p.dirId));
        if (!cm::makeDirs(dir)) {
            r.error = "无法创建目录：" + dir;
            return r;
        }
        const Elem& t = p.target;

        // block.sbcli
        std::vector<std::string> ids = t.ok() ? topLevelIds(t) : std::vector<std::string>();
        if (!ids.empty()) {
            std::vector<std::string> lines;
            lines.push_back("# 由 sb unpack 从 " + sb::basename(sb3Path) + " 生成");
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i) lines.push_back("");
                size_t before = p.ctx.lines.size();
                emitScript(p.ctx, ids[i]);
                for (size_t k = before; k < p.ctx.lines.size(); ++k)
                    lines.push_back(p.ctx.lines[k]);
            }
            std::string bp = cm::joinRel(dir, "block.sbcli");
            if (!cm::writeLines(bp, lines)) {
                r.error = "无法写入：" + bp;
                return r;
            }
            log << "  写 " << cm::joinRel(cm::joinRel("character", p.dirId), "block.sbcli")
                << "（脚本 " << ids.size() << "）\n";
            // 记录导出的素材条目
        }

        // meta.sbcli
        cm::CharMeta meta;
        meta.exists = false;
        meta.costumes = p.assets.costumes;
        meta.sounds   = p.assets.sounds;
        std::map<std::string, std::string> kv;
        auto defs = cm::defaultCharMeta(p.name, p.isStage);
        for (const auto& d : defs) kv[d.first] = d.second;
        kv["name"]     = p.name;
        kv["is_stage"] = p.isStage ? "true" : "false";
        if (p.isStage) {
            kv["x"] = "0"; kv["y"] = "0"; kv["size"] = "100"; kv["direction"] = "90";
            kv["visible"] = "true";
            kv["current_costume"] = std::to_string(t.ok() ? t.i64At("currentCostume", 0) : 0);
            kv["rotation_style"]  = "all_around";
            kv["layer_order"]     = "0";
            kv["draggable"]       = "false";
        } else {
            kv["x"] = std::to_string(t.i64At("x", 0));
            kv["y"] = std::to_string(t.i64At("y", 0));
            kv["size"] = std::to_string(t.i64At("size", 100));
            kv["direction"] = std::to_string(t.i64At("direction", 90));
            kv["visible"] = boolText(t.bAt("visible", true));
            kv["current_costume"] = std::to_string(t.i64At("currentCostume", 0));
            Elem rs = t.at("rotationStyle");
            kv["rotation_style"] = rs.is_string() ? std::string(rs.sv()) : "all_around";
            kv["layer_order"] = std::to_string(t.i64At("layerOrder", 0));
            kv["draggable"] = boolText(t.bAt("draggable", false));
        }
        meta.kv = kv;

        // 角色级变量/列表（全局的留在根 meta）
        // 注意：Scratch 允许全局与角色**同名**变量 —— 必须按 **id** 区分，不能按名字。
        // 全局变量的 id 与根 meta 的相同 → 跳过；id 不同（即使同名）是该角色的私有变量。
        std::map<std::string, std::string> localVars, localLists;
        Elem vars = t.at("variables");
        if (vars.is_object()) {
            for (auto vf : vars.obj()) {
                Elem v(vf.value);
                if (!v.is_array() || v.empty()) continue;
                std::string id(vf.key);
                if (globalVarIds.count(id)) continue;      // 就是那个全局变量
                Elem n0 = v.op(0);
                std::string nm = n0.is_string() ? std::string(n0.sv()) : rawJsonOf(n0);
                if (nm.empty()) continue;
                localVars[nm] = varInitText(v);
                meta.variables.insert(nm);
            }
        }
        Elem lists = t.at("lists");
        if (lists.is_object()) {
            for (auto lf : lists.obj()) {
                Elem l(lf.value);
                if (!l.is_array() || l.empty()) continue;
                std::string id(lf.key);
                if (globalListIds.count(id)) continue;
                Elem n0 = l.op(0);
                std::string nm = n0.is_string() ? std::string(n0.sv()) : rawJsonOf(n0);
                if (nm.empty()) continue;
                localLists[nm] = listInitText(l);
                meta.lists.insert(nm);
            }
        }

        std::vector<std::string> ml;
        ml.push_back("name: " + (p.name.empty() ? std::string("Sprite") : p.name));
        ml.push_back("is_stage: " + std::string(p.isStage ? "true" : "false"));
        ml.push_back("x: " + kv["x"]);
        ml.push_back("y: " + kv["y"]);
        ml.push_back("size: " + kv["size"]);
        ml.push_back("direction: " + kv["direction"]);
        ml.push_back("visible: " + kv["visible"]);
        ml.push_back("current_costume: " + kv["current_costume"]);
        ml.push_back("rotation_style: " + kv["rotation_style"]);
        ml.push_back("layer_order: " + kv["layer_order"]);
        ml.push_back("draggable: " + kv["draggable"]);
        ml.push_back("costumes: "   + cm::renderAssets(meta.costumes));
        ml.push_back("sounds: "     + cm::renderAssets(meta.sounds));
        ml.push_back("variables: "  + cm::renderVariables(localVars));
        ml.push_back("lists: "      + cm::renderLists(localLists));
        ml.push_back("broadcasts: " + cm::renderNames(meta.broadcasts, false));

        std::string mp = cm::joinRel(dir, "meta.sbcli");
        if (!cm::writeLines(mp, ml)) {
            r.error = "无法写入：" + mp;
            return r;
        }
        r.scriptCount += (int)ids.size();
    }

    // ---- assets/ ----
    // 无论有没有素材都建出来：与 `sb project init` 的产物一致
    // （add-costume 要往这里拷，check 也可能按它定位）。
    {
        std::string adir = cm::joinRel(cm::norm(outDir), "assets");
        if (!cm::makeDirs(adir)) {
            r.error = "无法创建素材目录：" + adir;
            return r;
        }
        for (const auto& want : allEntries) {
            std::string entry = resolveZipEntry(*f.zip, want);
            if (entry.empty()) {
                log << "  警告：压缩包里没有素材 " << want << "\n";
                continue;
            }
            std::vector<unsigned char> data;
            try { data = f.zip->readBinary(entry); }
            catch (const std::exception& e) {
                log << "  警告：素材解压失败 " << entry << "：" << e.what() << "\n";
                continue;
            }
            std::string dst = cm::joinRel(adir, sb::basename(entry));
            sb::writeFileBinary(dst, data.data(), data.size());
            if (!cm::fileExists(dst)) {
                log << "  警告：素材写入失败 " << dst << "\n";
                continue;
            }
            ++r.assetCount;
        }
    }

    // ---- 根 meta.sbcli ----
    if (stage.ok()) {
        Elem bc = stage.at("broadcasts");
        if (bc.is_object()) {
            for (auto bf : bc.obj()) {
                Elem b(bf.value);
                if (!b.is_array() || b.empty()) continue;
                Elem n0 = b.op(0);
                if (n0.is_string() && !std::string(n0.sv()).empty())
                    root.broadcasts.insert(std::string(n0.sv()));
            }
        }
    }
    for (const auto& b : allBcasts) root.broadcasts.insert(b);
    cm::writeRootMeta(cm::joinRel(cm::norm(outDir), "meta.sbcli"), root);

    r.ok = true;
    r.projectName = root.name;
    r.spriteCount = (int)sprites.size();
    r.unknownBlocks = 0;
    for (auto& p : plans) r.unknownBlocks += p.ctx.unknown;
    r.log = log.str();
    return r;
}

} // namespace sb
