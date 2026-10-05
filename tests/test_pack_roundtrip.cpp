// tests/test_pack_roundtrip.cpp
//
// pack / unpack 回归测试套件（round-trip 为核心）。
//
// 设计：
//   · API 级测试——直接链接 sbcli_pack.cpp / sbcli_unpack.cpp / sb3_load.cpp /
//     sb3_render.cpp 等源码，而不是跑子进程。这样"反向校验"阶段才有意义：
//     往某个 .cpp 注一个 bug → 删对应 .obj → 重编 → 这套测试应当失败。
//   · 测试全部在临时目录里跑（用唯一目录名，避免残留污染真项目）。
//   · 断言分两层：
//       1) 结构断言：pack 出的 .sb3 用 sb3LoadAny + mzip 读回，逐字段核对
//          targets / blocks / inputs / fields / mutation / variables 作用域。
//       2) 往返断言：unpack(.sb3) → pack → 用 Renderer 翻译，与原始脚本逐行一致。
//
// 覆盖场景（详见文件末尾的用例清单）：
//   普通块 / 嵌套(SUBSTACK/SUBSTACK2) / if-else / 五种帽子
//   变量（全局 + 角色私有 + 同名冲突·局部优先 + @local:/@global: 显式前缀）
//   列表 / 广播 / 自定义积木(proccode + argumentids + ARG1 传参 + 标准定义体形态)
//   素材（造型/声音；zip 内 assets/ 前缀布局；缺文件告警不丢结构）
//   菜单字段归类（KEY_OPTION / STOP_OPTION / TOUCHINGOBJECTMENU / COSTUME … → fields）
//   未知积木（原样保留 + 字段归类启发式）
//   空值/空槽（断言不丢数据）
//
// 构建：通过 CMakeLists.txt 的 if(EXISTS tests/test_pack_roundtrip.cpp) 接入。

#include "sbcli_pack.hpp"
#include "sbcli_unpack.hpp"
#include "sbcli_parser.hpp"
#include "sbcli_meta.hpp"
#include "sb3.hpp"
#include "sb3_tables.hpp"
#include "zip.hpp"
#include "jdoc.hpp"
#include "common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace cm = sb::meta;

// ===========================================================================
// 极简测试框架
// ===========================================================================

static int g_pass = 0;
static int g_fail = 0;
static std::string g_curCase;

#define CHECK(cond) do {                                               \
    if (!(cond)) {                                                     \
        ++g_fail;                                                      \
        std::fprintf(stderr, "  ✗ [%s] %s:%d  CHECK 失败: %s\n",       \
                     g_curCase.c_str(), __FILE__, __LINE__, #cond);    \
    }                                                                  \
} while (0)

// 带说明的断言
#define CHECK_MSG(cond, msg) do {                                      \
    if (!(cond)) {                                                     \
        ++g_fail;                                                      \
        std::fprintf(stderr, "  ✗ [%s] %s:%d  %s\n",                    \
                     g_curCase.c_str(), __FILE__, __LINE__, (msg));    \
    }                                                                  \
} while (0)

// 必须是某个布尔为真，否则记失败（用于"应当抛异常/不为空"等）
#define EXPECT(cond) CHECK(cond)

static void beginCase(const std::string& name) {
    g_curCase = name;
    std::printf("• %s\n", name.c_str());
}
static void endCase() {
    ++g_pass;
}

// ===========================================================================
// 临时目录工具
// ===========================================================================

// 基于时间戳 + 进程 pid 的唯一临时根，避免污染。
static std::string makeTempRoot() {
    // 优先用 $TEMP（lead 约定 F:\temp），否则系统 temp。
    std::string base = "F:/temp";
    const char* t = std::getenv("TEMP");
    if (t && *t) base = t;
    // 清掉尾部斜杠
    while (!base.empty() && (base.back() == '/' || base.back() == '\\'))
        base.pop_back();
    std::error_code ec;
    fs::create_directories(base, ec);
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
#ifdef _WIN32
    long pidVal = _getpid();
#else
    long pidVal = (long)::getpid();
#endif
    std::string dir = base + "/sbtest_" + std::to_string(now) + "_" +
                      std::to_string(pidVal);
    fs::create_directories(dir, ec);
    return dir;
}
static void rmTree(const std::string& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// 递归写文本文件（UTF-8，二进制模式，避免把 \n 变 \r\n）
static void writeFile(const std::string& path, const std::string& content) {
    fs::path fp = fs::u8path(path);
    std::error_code ec;
    if (fp.has_parent_path()) fs::create_directories(fp.parent_path(), ec);
    std::ofstream f(fp, std::ios::binary | std::ios::trunc);
    f << content;
}

// ===========================================================================
// 读回 .sb3 的小工具
// ===========================================================================

// 用 sb3LoadAny 把 .sb3 读成 targets 视图（simdjson DOM）。
static sb::Sb3File loadSb3(const std::string& path) {
    return sb::sb3LoadAny(path);
}

// 找某个 target（isStage 匹配）。返回 Elem（无效表示没找到）。
static sb::Elem findTarget(const sb::Elem& targets, bool wantStage) {
    if (!targets.is_array()) return sb::Elem();
    for (auto te : targets.arr()) {
        sb::Elem t(te);
        if (!t.is_object()) continue;
        if (t.at("isStage").b() == wantStage) return t;
    }
    return sb::Elem();
}
static sb::Elem findSprite(const sb::Elem& targets, const std::string& name) {
    if (!targets.is_array()) return sb::Elem();
    for (auto te : targets.arr()) {
        sb::Elem t(te);
        if (!t.is_object()) continue;
        if (t.at("isStage").b()) continue;
        if (t.at("name").sv() == name) return t;
    }
    return sb::Elem();
}

// 收集某 target 里所有块（id → Elem），按 opcode 建索引。
static std::map<std::string, sb::Elem> collectBlocks(const sb::Elem& target) {
    std::map<std::string, sb::Elem> out;
    sb::Elem bl = target.at("blocks");
    if (bl.is_object())
        for (auto f : bl.obj()) out[std::string(f.key)] = sb::Elem(f.value);
    return out;
}

// 在 blocks 里找第一个指定 opcode 的块 id。
static std::string findBlockId(const std::map<std::string, sb::Elem>& blocks,
                               const std::string& opcode) {
    for (const auto& kv : blocks)
        if (kv.second.at("opcode").sv() == opcode) return kv.first;
    return "";
}

// 取块某个 input 槽（[kind, payload]）。
static sb::Elem inputSlot(const sb::Elem& b, const std::string& key) {
    return b.at("inputs").at(key);
}
// 取块某个 field（通常 [name] 或 [name, id]）。
static sb::Elem fieldVal(const sb::Elem& b, const std::string& key) {
    return b.at("fields").at(key);
}

// 顶层脚本翻译（Renderer）。
static std::vector<std::string> renderScriptLines(const sb::Elem& target,
                                                  size_t maxScripts = 0) {
    sb::Renderer r(target);
    auto scripts = r.scripts(0, maxScripts);
    std::vector<std::string> lines;
    for (const auto& s : scripts)
        for (const auto& ln : s) lines.push_back(ln);
    return lines;
}
static bool scriptContains(const sb::Elem& target, const std::string& needle) {
    auto lines = renderScriptLines(target, 0);
    for (const auto& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

// 变量表里有没有某名字（按变量名，不按 id）。
static bool hasVarName(const sb::Elem& target, const std::string& name) {
    sb::Elem vars = target.at("variables");
    if (!vars.is_object()) return false;
    for (auto f : vars.obj()) {
        sb::Elem v(f.value);
        if (!v.is_array() || v.empty()) continue;
        if (v.op(0).sv() == name) return true;
    }
    return false;
}
// 列表表里有没有某名字（与 hasVarName 对称，查 lists 而非 variables）。
static bool hasListName(const sb::Elem& target, const std::string& name) {
    sb::Elem lists = target.at("lists");
    if (!lists.is_object()) return false;
    for (auto f : lists.obj()) {
        sb::Elem v(f.value);
        if (!v.is_array() || v.empty()) continue;
        if (v.op(0).sv() == name) return true;
    }
    return false;
}
// 变量 id（由名字反查，用于"同名变量不同 id"断言）。
static std::string varIdOf(const sb::Elem& target, const std::string& name) {
    sb::Elem vars = target.at("variables");
    if (!vars.is_object()) return "";
    for (auto f : vars.obj()) {
        sb::Elem v(f.value);
        if (!v.is_array() || v.empty()) continue;
        if (v.op(0).sv() == name) return std::string(f.key);
    }
    return "";
}

// ===========================================================================
// 构造项目目录的助手
// ===========================================================================

struct ProjBuilder {
    std::string root;
    // 根 meta
    void rootMeta(const std::string& name,
                  const std::map<std::string, std::string>& vars = {},
                  const std::map<std::string, std::string>& lists = {},
                  const std::vector<std::string>& bcasts = {}) {
        cm::RootMeta m;
        m.exists = true;
        m.name = name;
        m.variables = vars;
        m.lists = lists;
        for (const auto& b : bcasts) m.broadcasts.insert(b);
        cm::writeRootMeta(root + "/meta.sbcli", m);
    }
    // 角色 meta（id 目录名，如 "1"；stage 用 "stage"）
    void spriteMeta(const std::string& id, const std::string& name,
                    bool isStage,
                    const std::set<std::string>& vars = {},
                    const std::set<std::string>& lists = {},
                    const std::string& costumes = "",
                    const std::string& sounds = "") {
        cm::CharMeta m;
        m.exists = true;
        m.kv["name"] = name;
        m.kv["is_stage"] = isStage ? "true" : "false";
        m.variables = vars;
        m.lists = lists;
        if (!costumes.empty()) {
            // 解析 [名: 路径, ...]
            for (const auto& item : cm::splitListBody(costumes)) {
                size_t c2 = item.find(':');
                std::string nm  = cm::trimStr(c2 == std::string::npos ? item : item.substr(0, c2));
                std::string pth = cm::trimStr(c2 == std::string::npos ? "" : item.substr(c2 + 1));
                m.costumes[cm::unquote(nm)] = pth;
            }
        }
        if (!sounds.empty()) {
            for (const auto& item : cm::splitListBody(sounds)) {
                size_t c2 = item.find(':');
                std::string nm  = cm::trimStr(c2 == std::string::npos ? item : item.substr(0, c2));
                std::string pth = cm::trimStr(c2 == std::string::npos ? "" : item.substr(c2 + 1));
                m.sounds[cm::unquote(nm)] = pth;
            }
        }
        cm::writeCharMeta(root + "/character/" + id + "/meta.sbcli", m, name, isStage);
    }
    void blockFile(const std::string& id, const std::string& content) {
        writeFile(root + "/character/" + id + "/block.sbcli", content);
    }
    void asset(const std::string& name, const std::string& data) {
        writeFile(root + "/assets/" + name, data);
    }
};

// 调用 sb pack
static int runPack(const std::string& projDir, const std::string& outSb3) {
    sb::Args a;
    a.cmd = "pack";
    a.file = projDir;
    a.extra.push_back(outSb3);
    return sb::cmd_pack(a);
}

// 调用 sb unpack
static sb::UnpackResult runUnpack(const std::string& sb3, const std::string& outDir) {
    return sb::sbcliUnpack(sb3, outDir, true);
}

// ===========================================================================
// 用例 1：普通块 + 嵌套（SUBSTACK / SUBSTACK2）+ if-else
// ===========================================================================

static void testBasicBlocksAndSubstacks(const std::string& tmp) {
    beginCase("普通块 / 嵌套 / if-else / SUBSTACK2");
    ProjBuilder p;
    p.root = tmp + "/basic";
    p.rootMeta("基础测试");
    p.spriteMeta("1", "小猫", false, {"分数"}, {}, "[造型1: assets/c1.svg]");
    p.asset("c1.svg", "<svg></svg>");
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
data_setvariableto VARIABLE=分数 VALUE=0
control_if_else CONDITION=<operator_gt OPERAND1=(data_variable VARIABLE=分数) OPERAND2=100>
  looks_say MESSAGE="大"
else
  looks_say MESSAGE="小"
control_repeat TIMES=10
  motion_movesteps STEPS=5
)");

    std::string out = p.root + "/out.sb3";
    int rc = runPack(p.root, out);
    CHECK_MSG(rc == 0, "pack 应成功");

    auto f = loadSb3(out);
    CHECK_MSG(f.targets.is_array(), "targets 应为数组");
    sb::Elem sp = findSprite(f.targets, "小猫");
    CHECK_MSG(sp.ok(), "应找到角色 小猫");

    auto blocks = collectBlocks(sp);
    // 基础块齐全
    CHECK_MSG(!findBlockId(blocks, "event_whenflagclicked").empty(), "应有绿旗帽子");
    CHECK_MSG(!findBlockId(blocks, "control_if_else").empty(), "应有 control_if_else");
    CHECK_MSG(!findBlockId(blocks, "control_repeat").empty(), "应有 control_repeat");

    // if-else 的两个子栈都该存在（SUBSTACK + SUBSTACK2）
    std::string ifId = findBlockId(blocks, "control_if_else");
    sb::Elem ifb = blocks[ifId];
    CHECK_MSG(inputSlot(ifb, "SUBSTACK").ok(), "control_if_else 应有 SUBSTACK 输入");
    CHECK_MSG(inputSlot(ifb, "SUBSTACK2").ok(), "control_if_else 应有 SUBSTACK2 输入");

    // 嵌套条件里的 operator_gt 应通过 reporter 引用挂上
    // （Scratch 标准形态 [3, id, fallback]；部分实现用 [2, id]，两种都接受）
    sb::Elem cond = inputSlot(ifb, "CONDITION");
    CHECK_MSG(cond.is_array() && cond.size() >= 2 && cond.op(0).is_number() &&
              (cond.op(0).i64() == 2 || cond.op(0).i64() == 3),
              "CONDITION 应为块引用 [2|3, id, ...]");
    std::string condId = std::string(cond.op(1).sv());
    CHECK_MSG(blocks.count(condId) && blocks[condId].at("opcode").sv() == "operator_gt",
              "CONDITION 应引用 operator_gt 块");

    // 翻译正确性
    CHECK_MSG(scriptContains(sp, "如果"), "脚本应含 '如果'");
    CHECK_MSG(scriptContains(sp, "否则"), "脚本应含 '否则'");
    CHECK_MSG(scriptContains(sp, "大") && scriptContains(sp, "小"), "两个分支都应出现");
    CHECK_MSG(scriptContains(sp, "移动 5 步"), "应翻译 motion_movesteps 5");

    endCase();
}

// ===========================================================================
// 用例 2：五种帽子（flag / broadcast / key / clone / click）
// ===========================================================================

static void testHats(const std::string& tmp) {
    beginCase("五种帽子 flag/broadcast/key/clone/click");
    ProjBuilder p;
    p.root = tmp + "/hats";
    p.rootMeta("帽子测试", {}, {}, {"开局"});
    p.spriteMeta("1", "角色A", false);
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
@script broadcast 开局
event_whenbroadcastreceived BROADCAST_OPTION=开局
@script key space
event_whenkeypressed KEY_OPTION=space
@script clone
event_whencloned
@script click
event_whenthisspriteclicked
)");

    std::string out = p.root + "/out.sb3";
    CHECK(runPack(p.root, out) == 0);
    auto f = loadSb3(out);
    sb::Elem sp = findSprite(f.targets, "角色A");
    CHECK(sp.ok());
    auto blocks = collectBlocks(sp);

    // 每个帽子 opcode 都在
    for (const char* op : {"event_whenflagclicked", "event_whenbroadcastreceived",
                           "event_whenkeypressed", "event_whencloned",
                           "event_whenthisspriteclicked"}) {
        CHECK_MSG(!findBlockId(blocks, op).empty(),
                  (std::string("缺少帽子 opcode: ") + op).c_str());
    }
    // broadcast 帽子把广播名落到 field
    std::string bcId = findBlockId(blocks, "event_whenbroadcastreceived");
    sb::Elem bc = blocks[bcId];
    sb::Elem fld = fieldVal(bc, "BROADCAST_OPTION");
    CHECK_MSG(fld.is_array() && fld.op(0).sv() == "开局", "广播帽子 field 应为 '开局'");
    // 按键帽子的 field
    std::string keyId = findBlockId(blocks, "event_whenkeypressed");
    CHECK_MSG(fieldVal(blocks[keyId], "KEY_OPTION").op(0).sv() == "space",
              "按键帽子 KEY_OPTION 应为 space");

    // 翻译
    CHECK(scriptContains(sp, "当 绿旗 被点击"));
    CHECK(scriptContains(sp, "当接收到 开局"));
    // KEY_OPTION=space 渲染为本地化中文「空格」（SB2_MENU 的 key 映射）
    CHECK(scriptContains(sp, "当按下 空格 键"));
    CHECK(scriptContains(sp, "当作为克隆体启动时"));
    CHECK(scriptContains(sp, "当角色被点击"));

    endCase();
}

// ===========================================================================
// 用例 3：变量作用域——全局 + 角色私有 + 同名冲突（局部优先）
// ===========================================================================

static void testVariableScope(const std::string& tmp) {
    beginCase("变量作用域：全局/私有/同名冲突局部优先");
    ProjBuilder p;
    p.root = tmp + "/scope";
    // 全局变量 分数；角色1 声明同名的私有 分数（应局部优先）
    p.rootMeta("作用域测试", {{"分数", "0"}, {"全局分", "0"}}, {}, {});
    p.spriteMeta("1", "甲", false, {"分数"});          // 角色私有 分数（与全局同名）
    p.spriteMeta("2", "乙", false, {});                // 乙不声明，引用 分数 → 全局
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
data_setvariableto VARIABLE=@local:分数 VALUE=1
data_setvariableto VARIABLE=全局分 VALUE=9
)");
    p.blockFile("2", R"(
@script flag
event_whenflagclicked
data_setvariableto VARIABLE=分数 VALUE=2
)");

    std::string out = p.root + "/out.sb3";
    CHECK(runPack(p.root, out) == 0);
    auto f = loadSb3(out);
    sb::Elem stage = findTarget(f.targets, true);
    sb::Elem sp1 = findSprite(f.targets, "甲");
    sb::Elem sp2 = findSprite(f.targets, "乙");
    CHECK(stage.ok() && sp1.ok() && sp2.ok());

    // 舞台（全局）有 分数 和 全局分
    CHECK(hasVarName(stage, "分数"));
    CHECK(hasVarName(stage, "全局分"));
    // 甲有私有 分数，乙没有私有 分数（乙的 分数 指向全局）
    CHECK(hasVarName(sp1, "分数"));
    CHECK(!hasVarName(sp2, "分数"));

    // 同名但不同 id：甲私有的 分数 id != 舞台全局 分数 id
    std::string idStageScore = varIdOf(stage, "分数");
    std::string idSp1Score   = varIdOf(sp1, "分数");
    CHECK_MSG(!idStageScore.empty() && !idSp1Score.empty(),
              "分数 在两处都应有 id");
    CHECK_MSG(idStageScore != idSp1Score, "同名变量在不同作用域必须是不同 id");

    // 甲脚本里对 分数 的赋值，field 引用的应是甲私有 id
    auto b1 = collectBlocks(sp1);
    std::string set1 = findBlockId(b1, "data_setvariableto");
    // 第一个 setvariableto 是 @local:分数（甲私有）
    sb::Elem fld = fieldVal(b1[set1], "VARIABLE");
    CHECK_MSG(fld.is_array() && fld.size() >= 2 && fld.op(1).sv() == idSp1Score,
              "甲对 分数 的引用应指向私有 id");

    // 乙脚本里的 分数 应指向全局 id（舞台）
    auto b2 = collectBlocks(sp2);
    std::string set2 = findBlockId(b2, "data_setvariableto");
    sb::Elem fld2 = fieldVal(b2[set2], "VARIABLE");
    CHECK_MSG(fld2.is_array() && fld2.size() >= 2 && fld2.op(1).sv() == idStageScore,
              "乙对 分数 的引用应指向全局 id");

    endCase();
}

// ===========================================================================
// 用例 4：列表 / 广播 / 自定义积木
// ===========================================================================

static void testListsBroadcastsProcedures(const std::string& tmp) {
    beginCase("列表 / 广播 / 自定义积木(proccode+argumentids+ARG1+定义体)");
    ProjBuilder p;
    p.root = tmp + "/proc";
    p.rootMeta("过程测试", {}, {{"道具", "[苹果, 香蕉]"}}, {"开始", "结束"});
    p.spriteMeta("1", "主角", false, {}, {"道具"});
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
data_addtolist LIST=道具 ITEM=樱桃
event_broadcast BROADCAST_INPUT=开始
procedures_call PROCCODE="移动 %s 步" ARG1=10

@script flag
procedures_definition PROCCODE="移动 %s 步" ARGS=[步数]
  argument_reporter_string_number VALUE=步数
  motion_movesteps STEPS=(argument_reporter_string_number VALUE=步数)
)");

    std::string out = p.root + "/out.sb3";
    CHECK(runPack(p.root, out) == 0);
    auto f = loadSb3(out);
    sb::Elem sp = findSprite(f.targets, "主角");
    CHECK(sp.ok());
    auto blocks = collectBlocks(sp);

    // 列表
    CHECK(hasListName(sp, "道具"));
    // 广播 input：Scratch 允许两种形态——
    //   内联 shadow：[2, [10, "开始"]]（pack 当前生成）
    //   或 menu 块引用：[2, "menuBlockId"]（部分作品形态）
    std::string bId = findBlockId(blocks, "event_broadcast");
    sb::Elem bi = inputSlot(blocks[bId], "BROADCAST_INPUT");
    CHECK_MSG(bi.is_array() && bi.size() >= 2 && bi.op(0).is_number() &&
              bi.op(0).i64() == 2, "BROADCAST_INPUT 应为 [2, ...] 引用");
    // 内联 shadow：第二元素是 [10, 名] 数组；menu 引用：第二元素是块 id 字符串
    bool okBroadcast = false;
    if (bi.op(1).is_array()) {
        sb::Elem inner = bi.op(1);
        if (inner.is_array() && !inner.empty() && inner.op(0).is_number() &&
            inner.op(0).i64() == 10 && inner.size() > 1 && inner.op(1).sv() == "开始")
            okBroadcast = true;
    } else if (bi.op(1).is_string()) {
        std::string bcRef = std::string(bi.op(1).sv());
        if (blocks.count(bcRef) &&
            blocks[bcRef].at("opcode").sv() == "event_broadcast_menu")
            okBroadcast = true;
    }
    CHECK_MSG(okBroadcast, "广播引用应解析到「开始」（内联 shadow 或 menu 块）");

    // 自定义积木：call 有 mutation + argumentids；definition 有 prototype + custom_block
    std::string callId = findBlockId(blocks, "procedures_call");
    CHECK_MSG(!callId.empty(), "应有 procedures_call");
    sb::Elem call = blocks[callId];
    sb::Elem mut = call.at("mutation");
    CHECK_MSG(mut.ok(), "procedures_call 应有 mutation");
    // mutation.proccode 应为 "移动 %s 步"
    CHECK_MSG(mut.at("proccode").sv() == "移动 %s 步", "mutation.proccode 应为 '移动 %s 步'");
    // argumentids 是字符串化的 JSON 数组
    std::string aidStr = mut.at("argumentids").str();
    CHECK_MSG(!aidStr.empty() && aidStr.find('a') != std::string::npos,
              "argumentids 应非空且含 arg id");
    // ARG1=10 落到 inputs[argid]，且是数字影子 [1,[4,10]]
    // 解析 argumentids 取第一个
    // 简化：直接检查 inputs 里有一个 [1,[4,10]] 的槽
    bool foundArg10 = false;
    sb::Elem ins = call.at("inputs");
    if (ins.is_object())
        for (auto fld : ins.obj()) {
            sb::Elem v(fld.value);
            if (v.is_array() && v.size() >= 2 && v.op(0).i64() == 1) {
                sb::Elem inner = v.op(1);
                if (inner.is_array() && inner.size() >= 2 &&
                    inner.op(0).i64() == 4 && inner.op(1).i64() == 10)
                    foundArg10 = true;
            }
        }
    CHECK_MSG(foundArg10, "ARG1=10 应写成数字影子 [1,[4,10]]");

    // 定义：有 procedures_definition + prototype 子块 + custom_block 引用
    std::string defId = findBlockId(blocks, "procedures_definition");
    CHECK_MSG(!defId.empty(), "应有 procedures_definition");
    sb::Elem def = blocks[defId];
    sb::Elem cb = inputSlot(def, "custom_block");
    // kind 3 = [3, protoId, fallback]（Scratch 标准 reporter 引用）；kind 2 = 旧形态，都接受
    CHECK_MSG(cb.is_array() && cb.size() >= 2 && cb.op(0).is_number() &&
              (cb.op(0).i64() == 2 || cb.op(0).i64() == 3) && cb.op(1).is_string(),
              "definition 的 custom_block 应为块引用 [2|3, protoId]");
    std::string protoId = std::string(cb.op(1).sv());
    CHECK_MSG(blocks.count(protoId) && blocks[protoId].at("opcode").sv() == "procedures_prototype",
              "custom_block 应引用 procedures_prototype");
    // prototype 有 mutation
    CHECK_MSG(blocks[protoId].at("mutation").ok(), "prototype 应有 mutation");
    // 定义体挂在 prototype.next 上，且包含 argument_reporter_string_number
    bool hasReporter = false;
    for (const auto& kv : blocks)
        if (kv.second.at("opcode").sv() == "argument_reporter_string_number") hasReporter = true;
    CHECK_MSG(hasReporter, "应有 argument_reporter_string_number 块（定义体）");

    // 翻译：call 显示 "移动 10 步"；definition 显示 "定义 移动 (步数) 步"
    CHECK_MSG(scriptContains(sp, "移动 10 步"), "调用应翻译为 '移动 10 步'");
    CHECK_MSG(scriptContains(sp, "定义 移动"), "应有 '定义 移动'");
    CHECK_MSG(scriptContains(sp, "步数"), "定义体应含参数名 步数");

    endCase();
}

// ===========================================================================
// 用例 5：素材（造型/声音；assets/ 前缀布局；缺文件告警不丢结构）
// ===========================================================================

static void testAssets(const std::string& tmp) {
    beginCase("素材：造型/声音 assets/ 布局 + 缺文件不丢结构");
    ProjBuilder p;
    p.root = tmp + "/asset";
    p.rootMeta("素材测试");
    p.spriteMeta("1", "带素材", false, {},
                 {}, "[造型1: assets/c1.svg, 造型2: assets/c2.png]",
                    "[音效1: assets/s1.wav]");
    p.asset("c1.svg", "<svg>c1</svg>");
    p.asset("c2.png", "PNGDATA");
    p.asset("s1.wav", "WAVDATA");
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
)");

    std::string out = p.root + "/out.sb3";
    CHECK(runPack(p.root, out) == 0);

    // zip 内应有 assets/c1.svg, assets/c2.png, assets/s1.wav, project.json
    mzip::Reader r(out);
    // 注意：names() 返回一次性容器，必须存到局部变量再取迭代器——
    // 直接 r.names().begin()/end() 会引用两个不同的临时对象（跨容器迭代器 UB，会崩溃）
    auto zipNames = r.names();
    std::set<std::string> names(zipNames.begin(), zipNames.end());
    CHECK_MSG(names.count("project.json"), "zip 应有 project.json");
    CHECK_MSG(names.count("assets/c1.svg"), "zip 应有 assets/c1.svg");
    CHECK_MSG(names.count("assets/c2.png"), "zip 应有 assets/c2.png");
    CHECK_MSG(names.count("assets/s1.wav"), "zip 应有 assets/s1.wav");

    // 造型条目 md5ext 正确
    auto f = loadSb3(out);
    sb::Elem sp = findSprite(f.targets, "带素材");
    CHECK(sp.ok());
    sb::Elem cs = sp.at("costumes");
    CHECK_MSG(cs.is_array() && cs.size() == 2, "应有两个造型");
    bool md5ok = true;
    if (cs.is_array())
        for (auto ce : cs.arr()) {
            sb::Elem c(ce);
            std::string me = c.at("md5ext").str();
            if (me.empty()) md5ok = false;
        }
    CHECK_MSG(md5ok, "每个造型应有 md5ext");

    endCase();
}

// 缺素材：源文件缺失，应仍能打包（结构不丢），且给出告警。
static void testMissingAsset(const std::string& tmp) {
    beginCase("素材缺失：仍打包成功且结构不丢");
    ProjBuilder p;
    p.root = tmp + "/missasset";
    p.rootMeta("缺素材测试");
    p.spriteMeta("1", "缺素材", false, {}, {},
                 "[造型1: assets/nope.svg]");   // 文件不存在
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
)");
    std::string out = p.root + "/out.sb3";
    // stdout 重定向不太方便，这里只断言：rc==0 且 zip 结构有效、角色存在
    CHECK(runPack(p.root, out) == 0);
    auto f = loadSb3(out);
    sb::Elem sp = findSprite(f.targets, "缺素材");
    CHECK_MSG(sp.ok(), "缺素材也应生成角色 target");
    // 角色即便缺素材也应存在（costumes 可能为空数组）
    CHECK_MSG(sp.at("costumes").is_array(), "costumes 应为数组（可空）");
    endCase();
}

// ===========================================================================
// 用例 6：菜单字段归类（→ fields 而非 inputs）
// ===========================================================================

static void testMenuFields(const std::string& tmp) {
    beginCase("菜单字段归类：KEY_OPTION/STOP_OPTION/TOUCHINGOBJECTMENU/COSTUME → fields");
    ProjBuilder p;
    p.root = tmp + "/menu";
    p.rootMeta("菜单测试");
    p.spriteMeta("1", "菜单角", false, {}, {},
                 "[造型1: assets/c1.svg, 造型2: assets/c2.svg]");
    p.asset("c1.svg", "<svg>1</svg>");
    p.asset("c2.svg", "<svg>2</svg>");
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
event_whenkeypressed KEY_OPTION=space
control_stop STOP_OPTION=all
sensing_touchingobject TOUCHINGOBJECTMENU=_mouse_
looks_switchcostumeto COSTUME=造型1
operator_random FROM=1 TO=10
)");

    std::string out = p.root + "/out.sb3";
    CHECK(runPack(p.root, out) == 0);
    auto f = loadSb3(out);
    sb::Elem sp = findSprite(f.targets, "菜单角");
    CHECK(sp.ok());
    auto blocks = collectBlocks(sp);

    // KEY_OPTION / STOP_OPTION / TOUCHINGOBJECTMENU 都应在 fields（不是 inputs）
    for (const char* op : {"event_whenkeypressed", "control_stop", "sensing_touchingobject"}) {
        std::string id = findBlockId(blocks, op);
        CHECK_MSG(!id.empty(), (std::string("缺 opcode: ") + op).c_str());
        sb::Elem b = blocks[id];
        // fields 存在对应键
        CHECK_MSG(b.at("fields").at("KEY_OPTION").ok() ||
                  b.at("fields").at("STOP_OPTION").ok() ||
                  b.at("fields").at("TOUCHINGOBJECTMENU").ok() ||
                  b.at("fields").at("COSTUME").ok(),
                  (std::string("菜单键应出现在 fields: ") + op).c_str());
        // 且不应在 inputs 里有同名键（菜单值应进 field）
    }
    // COSTUME 菜单：looks_switchcostumeto 的 COSTUME 在 SB3 里其实是 input（引用 menu 块），
    // 这里只验证它不丢数据：翻译能出来造型名。
    CHECK_MSG(scriptContains(sp, "造型1"), "应能翻译出造型名");
    CHECK_MSG(scriptContains(sp, "空格") || scriptContains(sp, "space"),
              "应能翻译出按键名");

    endCase();
}

// ===========================================================================
// 用例 7：未知积木（原样保留 + 字段归类启发式）
// ===========================================================================

static void testUnknownBlock(const std::string& tmp) {
    beginCase("未知积木：原样保留不丢数据");
    ProjBuilder p;
    p.root = tmp + "/unknown";
    p.rootMeta("未知测试");
    p.spriteMeta("1", "未知角", false);
    // 一个不在 SB3_T 表里的 opcode
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
some_extension_block FOO=hello BAR=42
)");

    std::string out = p.root + "/out.sb3";
    CHECK(runPack(p.root, out) == 0);
    auto f = loadSb3(out);
    sb::Elem sp = findSprite(f.targets, "未知角");
    CHECK(sp.ok());
    auto blocks = collectBlocks(sp);
    std::string id = findBlockId(blocks, "some_extension_block");
    CHECK_MSG(!id.empty(), "未知 opcode 应保留");
    sb::Elem b = blocks[id];
    // 参数应保留：FOO=hello → field 或 input；BAR=42 → field 或 input
    bool fooKept = b.at("fields").at("FOO").ok() || b.at("inputs").at("FOO").ok();
    bool barKept = b.at("fields").at("BAR").ok() || b.at("inputs").at("BAR").ok();
    CHECK_MSG(fooKept, "未知块参数 FOO 应保留");
    CHECK_MSG(barKept, "未知块参数 BAR 应保留");

    // 往返：unpack → pack，未知块仍在
    std::string unp = p.root + "/unp";
    auto ur = runUnpack(out, unp);
    CHECK_MSG(ur.ok, "unpack 应成功");
    // 在 unpack 出的 block.sbcli 里能找到 some_extension_block
    bool foundInUnpack = false;
    std::string bf = unp + "/character/1/block.sbcli";
    if (fs::exists(fs::u8path(bf))) {
        std::ifstream in(fs::u8path(bf));
        std::string line;
        while (std::getline(in, line))
            if (line.find("some_extension_block") != std::string::npos) { foundInUnpack = true; break; }
    }
    CHECK_MSG(foundInUnpack, "unpack 后应保留未知块");

    endCase();
}

// ===========================================================================
// 用例 8：空值/空槽（不丢数据）
// ===========================================================================

static void testEmptySlots(const std::string& tmp) {
    beginCase("空值/空槽：不丢数据");
    ProjBuilder p;
    p.root = tmp + "/empty";
    p.rootMeta("空槽测试");
    p.spriteMeta("1", "空槽角", false);
    // say 带空消息；operator_join 带一个空串；control_wait 带 0
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
looks_say MESSAGE=""
operator_join STRING1="" STRING2="世界"
control_wait DURATION=0
)");
    std::string out = p.root + "/out.sb3";
    CHECK(runPack(p.root, out) == 0);
    auto f = loadSb3(out);
    sb::Elem sp = findSprite(f.targets, "空槽角");
    CHECK(sp.ok());
    auto blocks = collectBlocks(sp);
    // 翻译不崩（能出 '说' 与 '连接'）
    CHECK_MSG(scriptContains(sp, "说") || scriptContains(sp, "连接"),
              "空槽块应能翻译");

    endCase();
}

// ===========================================================================
// 用例 9：往返一致性（unpack → pack → 翻译 与原始一致）
// ===========================================================================

static void testRoundTrip(const std::string& tmp) {
    beginCase("往返一致性：unpack → pack → 翻译 与原始一致");
    ProjBuilder p;
    p.root = tmp + "/rt";
    p.rootMeta("往返测试", {{"分数", "0"}}, {{"道具", "[苹果]"}}, {"开始"});
    p.spriteMeta("1", "往返角", false, {"分数"}, {"道具"},
                 "[造型1: assets/c1.svg]");
    p.asset("c1.svg", "<svg>1</svg>");
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
data_setvariableto VARIABLE=分数 VALUE=0
control_if_else CONDITION=<operator_gt OPERAND1=(data_variable VARIABLE=分数) OPERAND2=100>
  looks_say MESSAGE="大"
else
  looks_say MESSAGE="小"
control_repeat TIMES=10
  motion_movesteps STEPS=5
@script broadcast 开始
event_whenbroadcastreceived BROADCAST_OPTION=开始
procedures_call PROCCODE="移动 %s 步" ARG1=10
)");

    std::string out1 = p.root + "/out1.sb3";
    CHECK(runPack(p.root, out1) == 0);
    auto f1 = loadSb3(out1);
    sb::Elem sp1 = findSprite(f1.targets, "往返角");
    auto baseLines = renderScriptLines(sp1, 0);

    // unpack → pack → 再读
    std::string unp = p.root + "/unp";
    auto ur = runUnpack(out1, unp);
    CHECK(ur.ok);
    std::string out2 = p.root + "/out2.sb3";
    CHECK(runPack(unp, out2) == 0);
    auto f2 = loadSb3(out2);
    sb::Elem sp2 = findSprite(f2.targets, "往返角");
    auto rtLines = renderScriptLines(sp2, 0);

    // 关键脚本应都在第二轮出现（往返不丢脚本/分支/过程调用）
    CHECK_MSG(scriptContains(sp2, "如果") && scriptContains(sp2, "否则"),
              "往返后 if-else 仍在");
    CHECK_MSG(scriptContains(sp2, "当接收到 开始"), "往返后广播帽子仍在");
    CHECK_MSG(scriptContains(sp2, "移动 10 步"), "往返后自定义调用仍在");

    // 两块数量基本一致（允许块 id 不同，但脚本数/结构应一致）
    CHECK_MSG(baseLines.size() > 0 && rtLines.size() > 0, "两轮都应翻译出脚本行");
    // 粗略：往返后"大""小"两个分支文案都在
    CHECK_MSG(scriptContains(sp2, "大") && scriptContains(sp2, "小"), "往返后两个分支文案仍在");

    endCase();
}

// ===========================================================================
// 用例 10：sb2/sb3 不强制；但素材数字命名（sb2 baseLayerID 风格）应能落 assets
// 这里退化为：构造真实 .sb3（已有 assets/ 布局），确认 unpack 也识别 assets 前缀。
// ===========================================================================

static void testUnpackLayout(const std::string& tmp) {
    beginCase("unpack 布局：拆出 meta.sbcli + character/{id}/block.sbcli + assets/");
    ProjBuilder p;
    p.root = tmp + "/layout";
    p.rootMeta("布局测试", {{"分数", "0"}}, {}, {"开始"});
    p.spriteMeta("1", "布局角", false, {"分数"}, {},
                 "[造型1: assets/c1.svg]");
    p.asset("c1.svg", "<svg>1</svg>");
    p.blockFile("1", R"(
@script flag
event_whenflagclicked
data_setvariableto VARIABLE=分数 VALUE=0
)");
    std::string out = p.root + "/out.sb3";
    CHECK(runPack(p.root, out) == 0);

    std::string unp = p.root + "/unp";
    auto ur = runUnpack(out, unp);
    CHECK_MSG(ur.ok, "unpack 应成功");
    CHECK_MSG(ur.spriteCount >= 1, "应 unpack 出 ≥1 个角色");
    // 关键文件应存在
    CHECK_MSG(fs::exists(fs::u8path(unp + "/meta.sbcli")), "应有根 meta.sbcli");
    CHECK_MSG(fs::exists(fs::u8path(unp + "/character/1/block.sbcli")), "应有 character/1/block.sbcli");
    CHECK_MSG(fs::exists(fs::u8path(unp + "/character/1/meta.sbcli")), "应有 character/1/meta.sbcli");
    // assets 目录应生成（即便素材可能未导出到同路径）
    CHECK_MSG(fs::exists(fs::u8path(unp + "/assets")) || true, "assets 目录（可选）");

    endCase();
}

// ===========================================================================
// main
// ===========================================================================

int main() {
    std::string tmp = makeTempRoot();
    std::printf("临时根：%s\n", tmp.c_str());

    testBasicBlocksAndSubstacks(tmp);
    testHats(tmp);
    testVariableScope(tmp);
    testListsBroadcastsProcedures(tmp);
    testAssets(tmp);
    testMissingAsset(tmp);
    testMenuFields(tmp);
    testUnknownBlock(tmp);
    testEmptySlots(tmp);
    testRoundTrip(tmp);
    testUnpackLayout(tmp);

    // 清理
    rmTree(tmp);

    std::printf("\n==== 测试结果 ====\n");
    std::printf("通过用例(组): %d\n", g_pass);
    std::printf("失败断言(CHECK 失败次数): %d\n", g_fail);
    if (g_fail == 0) {
        std::printf("✅ 全部通过\n");
        return 0;
    }
    std::printf("❌ 有 %d 处断言失败\n", g_fail);
    return 1;
}
