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
    } else if (s.empty() && !t.quoted) {
        // 空槽（无引号的裸空值）：Scratch 的空 input 默认是**数字槽** [4,""]。
        // 带引号的 ""（quoted=true）是明确空文本，保持 [10,""]（渲染 ∅）。
        inner.push_back(4);
        inner.push_back("");
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
            // 运动/侦测的菜单槽：motion_goto.TO → motion_goto_menu、
            // motion_pointtowards.TOWARDS → motion_pointtowards_menu（实测键名）
            "TO", "TOWARDS",
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
    std::set<std::string> localVars;         // 本角色私有变量名（局部优先解析用）
    std::set<std::string> localLists;        // 本角色私有列表名
    std::set<std::string>* missing = nullptr;// 未知 opcode 统计（可为空）
    // 当前正在生成的块 id：emitReporterBlock 生成子 reporter 时要回填 parent，
    // 否则 reporter 的 parent 为 null（Scratch 打开时输入挂不上 → 参数丢失）。
    std::string parentId;
    long long counter = 0;
    std::string newId() { return "b" + std::to_string(++counter); }
};

// 前向声明
Json encodeValue(const SbcValue& val, PackCtx& ctx);
std::string emitReporterBlock(const SbcValue& val, PackCtx& ctx);
void routeParam(Json& b, const SbcParam& p, PackCtx& ctx,
                const std::string& opcode = "",
                const std::string& blockId = "");

// 帽子块（hat）里，有些 opcode 的值就存在**块自身 fields**（不是菜单桩）：
//   event_whenkeypressed.KEY_OPTION        —— 真实作品实测 523/523 全在 fields
//   event_whenbroadcastreceived.BROADCAST_OPTION —— 12434/12434 全在 fields
//   control_stop.STOP_OPTION               —— 4956/4956 全在 fields
// 这些块**不能**做桩转换（否则会同时写出 fields 和 inputs 两个同名键，
// 出现重复冗余、偏离 Scratch 标准结构）。
bool isHatWithOwnField(const std::string& op) {
    static const std::set<std::string> s = {
        "event_whenkeypressed",
        "event_whenbroadcastreceived",
        "event_whenbackdropswitchesto",
        "event_whengreaterthan",
        "control_stop",
    };
    return s.count(op) != 0;
}

// 桩 opcode 判定（与 unpack 侧 isMenuStubOpcode 对称）：用于识别 unpack 输出的
// reporter 形态 `(pen_menu_colorParam colorParam="color")` 是不是一个菜单桩。
bool isMenuStubOpcodeName(const std::string& op) {
    if (op.empty()) return false;
    static const std::set<std::string> kNative = {
        "looks_costume", "looks_backdrops", "sound_sounds_menu",
        "sensing_touchingobjectmenu", "sensing_distancetomenu", "sensing_keyoptions",
        "control_create_clone_of_menu", "motion_goto_menu", "motion_pointtowards_menu",
        "sensing_of_object_menu", "pen_menu_colorParam",
    };
    if (kNative.count(op)) return true;
    if (op.find("_menu_") != std::string::npos) return true;
    if (op.rfind("menu_", 0) == 0) return true;
    return false;
}

// 从 reporter 形态的菜单桩值里取出「值」与「桩字段键」。
// 形态：SbcValue::Reporter，其 params 里只有一项（字段键 → 值）。
std::string stubValueOf(const SbcValue& v, std::string* fieldKeyOut) {
    if (!v.args.empty() && v.args[0].value &&
        v.args[0].value->kind == SbcValue::Kind::Scalar) {
        if (fieldKeyOut) {
            const std::string& k = v.args[0].canon.empty() ? v.args[0].key
                                                           : v.args[0].canon;
            *fieldKeyOut = k;
        }
        return v.args[0].value->scalar.text;
    }
    return std::string();
}

// 菜单字段 → shadow 桩块 opcode。返回空表示"不生成桩"（用内联 fields 即可）。
// 映射依据：真实作品实测（桩块 parent / 槽键 / 桩 fields 键）
//   sensing_keypressed.KEY_OPTION        → sensing_keyoptions       (fields KEY_OPTION)
//   motion_goto.TO                       → motion_goto_menu         (fields TO)
//   motion_pointtowards.TOWARDS          → motion_pointtowards_menu (fields TOWARDS)
//   sensing_of.OBJECT                    → sensing_of_object_menu   (fields OBJECT)
//   looks_switchcostumeto.COSTUME        → looks_costume
//   sound_play.SOUND_MENU                → sound_sounds_menu
//   control_create_clone_of.CLONE_OPTION → control_create_clone_of_menu
//   pen_*.COLOR_PARAM                    → pen_menu_colorParam      (桩 fields 键为 colorParam)
std::string menuStubOpcode(const std::string& parentOp, const std::string& fieldKey) {
    // 先查"opcode+key 联合"的桩槽白名单（真实作品矩阵）——
    // 这决定了"这个槽在 Scratch 里是不是菜单桩引用"。
    // 不在白名单里的（如 operator_random.TO）绝不是桩，返回空 → 内联 fields。
    static const std::map<std::string, std::string> native = {
        {"COSTUME",           "looks_costume"},
        {"BACKDROP",          "looks_backdrops"},
        {"SOUND_MENU",        "sound_sounds_menu"},
        {"TOUCHINGOBJECTMENU","sensing_touchingobjectmenu"},
        {"DISTANCETOMENU",    "sensing_distancetomenu"},
        {"KEY_OPTION",        "sensing_keyoptions"},
        {"CLONE_OPTION",      "control_create_clone_of_menu"},
        {"TO",                "motion_goto_menu"},
        {"TOWARDS",           "motion_pointtowards_menu"},
        {"OBJECT",            "sensing_of_object_menu"},
    };
    // opcode 约束：同一个 key 在不同 opcode 上语义完全不同
    // （motion_goto.TO = 菜单；operator_random.TO = 普通数字入参）
    static const std::map<std::string, std::set<std::string>> kOpSlots = {
        {"looks_switchcostumeto",  {"COSTUME"}},
        {"looks_costume",          {"COSTUME"}},
        {"looks_switchbackdropto", {"BACKDROP"}},
        {"looks_backdrops",        {"BACKDROP"}},
        {"sound_play",             {"SOUND_MENU"}},
        {"sound_playuntildone",    {"SOUND_MENU"}},
        {"motion_goto",            {"TO"}},
        {"motion_glideto",         {"TO"}},
        {"motion_pointtowards",    {"TOWARDS"}},
        {"sensing_touchingobject", {"TOUCHINGOBJECTMENU"}},
        {"sensing_distanceto",     {"DISTANCETOMENU"}},
        {"sensing_keypressed",     {"KEY_OPTION"}},
        {"sensing_of",             {"OBJECT"}},
        {"control_create_clone_of",{"CLONE_OPTION"}},
        {"pen_setPenColorParamTo",    {"COLOR_PARAM"}},
        {"pen_changePenColorParamBy", {"COLOR_PARAM"}},
    };
    auto osIt = kOpSlots.find(parentOp);
    if (osIt == kOpSlots.end() || !osIt->second.count(fieldKey)) {
        // 不是桩槽（原生内联菜单如 operator_mathop.OPERATOR / 普通入参如 operator_random.TO）
        // 注意：**不给未知扩展块自动造桩**——那会凭空生成原文件里不存在的桩
        // （如 witCat.dollyPro_menu_PROPERTY ×19，真实作品里没有）。
        // 扩展块有桩的，unpack 会输出 reporter 形态，routeParam 用 reporter 的 opcode
        // 直接当桩名，不需要这里的命名约定。
        return std::string();
    }
    // pen 扩展的 COLOR_PARAM → pen_menu_colorParam（桩字段键为小写 colorParam）
    if (fieldKey == "COLOR_PARAM" && parentOp.rfind("pen_", 0) == 0)
        return "pen_menu_colorParam";
    auto it = native.find(fieldKey);
    if (it != native.end()) return it->second;
    return std::string();
}

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
    // parent 回填：reporter 被哪个块引用（Scratch 靠 parent 把输入挂到父块上）
    b["parent"]   = ctx.parentId.empty() ? Json() : Json(ctx.parentId);
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
    } else if (op == "ccw_hat_parameter") {
        // CCW 扩展的「帽子参数定义块」：shadow=true + fields.VALUE（与 argument_reporter
        // 同类）。unpack 输出 reporter 形态 (ccw_hat_parameter VALUE="senderID")，
        // 这里还原成 shadow 参数块，否则扩展块的自定义参数在 Scratch 里显示异常。
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
            routeParam(b, p, ctx, op, bid);
        }
    }

    ctx.blocks[bid] = std::move(b);
    return bid;
}

// 把单个命名参数归位到 block b 的 inputs / fields
void routeParam(Json& b, const SbcParam& p, PackCtx& ctx,
                const std::string& opcode, const std::string& blockId) {
    if (!p.value) return;
    const std::string& key = p.canon.empty() ? p.key : p.canon;
    const SbcValue& pv = *p.value;

    if (key == "VARIABLE") {
        const std::string& nm = pv.scalar.text;
        // 作用域解析（显式提示优先，其次 Scratch 的局部优先规则）：
        //   1. @local:名   → 强制用当前角色的私有 id
        //   2. @global:名  → 强制用 stage 的全局 id
        //   3. 无提示      → 局部优先（角色声明过则用角色 id），否则全局，再否则兜底当前 scope
        std::string id;
        if (pv.scope == SbcValue::Scope::Local)
            id = sbcVarId(ctx.scope, nm);
        else if (pv.scope == SbcValue::Scope::Global)
            id = sbcVarId("stage", nm);
        else if (ctx.localVars.count(nm))
            id = sbcVarId(ctx.scope, nm);
        else if (ctx.globalVars.count(nm))
            id = sbcVarId("stage", nm);
        else
            id = sbcVarId(ctx.scope, nm);
        b["fields"]["VARIABLE"] = Json::array();
        b["fields"]["VARIABLE"].push_back(nm);
        b["fields"]["VARIABLE"].push_back(id);
    } else if (key == "LIST") {
        const std::string& nm = pv.scalar.text;
        std::string id;
        if (pv.scope == SbcValue::Scope::Local)
            id = sbcVarId(ctx.scope, nm);
        else if (pv.scope == SbcValue::Scope::Global)
            id = sbcVarId("stage", nm);
        else if (ctx.localLists.count(nm))
            id = sbcVarId(ctx.scope, nm);
        else if (ctx.globalLists.count(nm))
            id = sbcVarId("stage", nm);
        else
            id = sbcVarId(ctx.scope, nm);
        b["fields"]["LIST"] = Json::array();
        b["fields"]["LIST"].push_back(nm);
        b["fields"]["LIST"].push_back(id);
    } else if (key == "BROADCAST_INPUT") {
        b["inputs"]["BROADCAST_INPUT"] = broadcastInput(pv.scalar.text);
    } else if (menuFieldKeys().count(key) && isHatWithOwnField(opcode)) {
        // 帽子块自己的字段（KEY_OPTION / BROADCAST_OPTION / STOP_OPTION…）：
        // 直接写进**块自身 fields**（真实作品：event_whenkeypressed 523/523、
        // control_stop 4956/4956 都是 fields），**不生成菜单桩、不写 inputs**。
        // 注意：hatArg 路径（@script key space）也会写同一个 field，这里重复写安全
        // （同键同值覆盖）；但若脚本里显式写了 KEY_OPTION=space，这里才是唯一来源。
        b["fields"][key] = Json::array();
        b["fields"][key].push_back(pv.scalar.text);
    } else if (menuFieldKeys().count(key) &&
               (pv.kind == SbcValue::Kind::Scalar ||
                (pv.kind == SbcValue::Kind::Reporter &&
                 isMenuStubOpcodeName(pv.scalar.text)))) {
        // 菜单字段。行为分两种：
        //  a) 原文件是「菜单桩引用」的槽（motion_goto.TO、sensing_touchingobject…）
        //     → 还原成「桩块 + input 引用」（menuStubOpcode 返回桩名）
        //  b) 原文件是「内联 fields」的原生菜单（operator_mathop.OPERATOR、
        //     looks_switchcostumeto 的旧形态…）→ 直接写 fields（menuStubOpcode 返回空）
        // 判断依据在 menuStubOpcode 内部（opcode+key 联合 + 桩名映射表），
        // 不在这里提前用白名单挡掉 —— 否则 OPERATOR/EFFECT 这类原生菜单会漏进
        // inputs 分支（真实作品里它们在 fields，见 69 部全量统计）。
        std::string stubOp = blockId.empty() ? std::string()
                                             : menuStubOpcode(opcode, key);
        // reporter 形态：直接用 reporter 的 opcode 当桩名（已是准确名字）
        std::string stubVal;
        std::string stubFieldKey;
        if (pv.kind == SbcValue::Kind::Reporter) {
            stubOp = pv.scalar.text;
            stubVal = stubValueOf(pv, &stubFieldKey);
        } else {
            stubVal = pv.scalar.text;
        }
        if (!stubOp.empty()) {
            std::string stubId = ctx.newId();
            Json stub = Json::object();
            stub["opcode"]   = stubOp;
            stub["next"]     = Json();
            stub["parent"]   = Json(blockId);
            stub["inputs"]   = Json::object();
            Json sf = Json::object();
            Json sv = Json::array();
            sv.push_back(stubVal);
            // 桩自己的字段键可能与槽键不同（pen 的槽是 COLOR_PARAM、桩字段是 colorParam），
            // reporter 形态时用 unpack 给出的实际键，标量形态时用槽键。
            sf[stubFieldKey.empty() ? key : stubFieldKey] = sv;
            stub["fields"]   = sf;
            stub["shadow"]   = true;
            stub["topLevel"] = false;
            ctx.blocks[stubId] = std::move(stub);
            Json slot = Json::array();
            slot.push_back(1);          // kind 1 = 未遮挡的 shadow
            slot.push_back(stubId);
            b["inputs"][key] = slot;
        } else {
            b["fields"][key] = Json::array();
            b["fields"][key].push_back(stubVal);
        }
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

    // 本块生成期间，ctx.parentId 指向它：emitReporterBlock 生成子 reporter 时
    // 要把 parent 回填成引用它的块（否则 reporter 的 parent=null，Scratch 挂不上）。
    struct ParentGuard {
        PackCtx& c; std::string prev;
        ParentGuard(PackCtx& c_, const std::string& id) : c(c_), prev(c_.parentId) {
            c.parentId = id;
        }
        ~ParentGuard() { c.parentId = prev; }
    } parentGuard(ctx, bid);

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
                routeParam(b, p, ctx, op, bid);
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
            Json protoInputs = Json::object();
            proto["fields"]   = Json::object();
            proto["shadow"]   = true;
            proto["topLevel"] = false;

            // ★ prototype 的参数占位块：每个 argumentid 对应一个
            // argument_reporter_string_number（shadow=true），挂在 prototype.inputs 下。
            // 这是 Scratch 标准结构（scratch-vm sb3.js：parent 为 procedures_prototype
            // 的 argument_reporter 必须 shadow=true）；缺了会丢参数占位，
            // 导致 Scratch/Gandi 打开时自定义积木参数显示异常、块数与原作品不一致。
            for (size_t ai = 0; ai < argIds.size(); ++ai) {
                const std::string& acid = argIds[ai];
                std::string repId = ctx.newId();
                Json rep = Json::object();
                rep["opcode"]   = "argument_reporter_string_number";
                rep["next"]     = Json();
                rep["parent"]   = protoId;
                rep["inputs"]   = Json::object();
                Json rfields = Json::object();
                Json varr = Json::array();
                varr.push_back(ai < argNames.size() ? argNames[ai] : std::string());
                rfields["VALUE"] = varr;
                rep["fields"]   = rfields;
                rep["shadow"]   = true;
                rep["topLevel"] = false;
                ctx.blocks[repId] = std::move(rep);
                Json slot = Json::array();
                slot.push_back(1);
                slot.push_back(repId);
                protoInputs[acid] = slot;
            }
            proto["inputs"]   = protoInputs;
            proto["mutation"] = mutation;
            ctx.blocks[protoId] = std::move(proto);
            b["inputs"]["custom_block"] = refInput(protoId);
            // 子栈（定义体）正常递归，由后续统一处理
        }
    } else {
        for (const auto& p : blk.params) {
            if (!p.value) continue;
            routeParam(b, p, ctx, op, bid);
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
// localVars/localLists：本角色 meta 里声明的私有变量/列表名（局部优先解析用）。
Json buildTargetBlocks(const std::vector<SbcScript>& scripts, const std::string& scope,
                       const std::set<std::string>& globalVars,
                       const std::set<std::string>& globalLists,
                       std::set<std::string>* missing,
                       const std::set<std::string>& localVars = {},
                       const std::set<std::string>& localLists = {}) {
    PackCtx ctx;
    ctx.scope = scope;
    ctx.globalVars = globalVars;
    ctx.globalLists = globalLists;
    ctx.localVars = localVars;
    ctx.localLists = localLists;
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

        // 作用域分析（与 Scratch 一致：局部优先）
        //  · 角色 meta 里**显式声明**的名字 = 该角色的私有变量/列表（即使全局也有同名）
        //  · 仅被脚本引用、且未在任何地方声明为私有的名字 → 归全局（若全局有）
        Refs refs;
        collectRefs(scripts, refs);
        const std::set<std::string>& declaredVars  = sp.meta.variables;
        const std::set<std::string>& declaredLists = sp.meta.lists;

        auto isLocalVar = [&](const std::string& n) {
            if (declaredVars.count(n)) return true;                 // 显式私有（局部优先）
            return refs.vars.count(n) && !globalVars.count(n);      // 未全局声明 → 私有
        };
        auto isLocalList = [&](const std::string& n) {
            if (declaredLists.count(n)) return true;
            return refs.lists.count(n) && !globalLists.count(n);
        };

        std::set<std::string> localVars, localLists;
        for (const auto& n : refs.vars)    if (isLocalVar(n))  localVars.insert(n);
        for (const auto& n : declaredVars) localVars.insert(n);
        for (const auto& n : refs.lists)    if (isLocalList(n)) localLists.insert(n);
        for (const auto& n : declaredLists) localLists.insert(n);

        // 私有变量的初值：CharMeta 只存名字集合（无初值），默认 0。
        // 初值的权威来源是根 meta（全局）与 add-variable 写入的行；这里保守用 0，
        // 与 Scratch 打开后未初始化的默认表现一致。
        Json vars = Json::object();
        for (const auto& v : localVars) {
            const std::string id = sbcVarId(sp.id, v);
            vars[id] = Json::array();
            vars[id].push_back(v);
            vars[id].push_back(0);
        }
        Json lists = Json::object();
        for (const auto& l : localLists) {
            const std::string id = sbcVarId(sp.id, l);
            lists[id] = Json::array();
            lists[id].push_back(l);
            lists[id].push_back(Json::array());
        }
        Json bcasts = Json::object();

        Json blocks = buildTargetBlocks(scripts, sp.id, globalVars, globalLists, &missing,
                                        sp.meta.variables, sp.meta.lists);

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
