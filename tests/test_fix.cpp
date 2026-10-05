// tests/test_fix.cpp
// sb fix 自测：变量/广播登记、舞台变量归根、初值、幂等、手写字段保留、
// 缺素材报错且不臆造、素材存在时接路径、id 确定性。
//
// 恢复说明：由 tests/test_fix.cpp.obj 的字符串表与符号还原重建；
// 断言内容与原始版本等价，注释为重建时补写。
//
// 全部在临时目录里搭项目，不碰真项目。
// 退出码 0 = 全部通过。

#include "sbcli_fix.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using namespace sb;

static int g_fail = 0;

static void ok(bool cond, const char* name) {
    if (cond) {
        std::printf("  [通过] %s\n", name);
    } else {
        ++g_fail;
        std::printf("  [失败] %s\n", name);
    }
}

// ------------------------------------------------------------------ 工具

static void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

static fs::path tempRoot(const char* tag) {
    static int seq = 0;
    // 唯一目录名（带序号），避免上一轮残留污染测试结果；
    // 同时显式校验 remove_all 的失败（磁盘/权限问题会静默留下旧文件）。
    for (int attempt = 0; attempt < 8; ++attempt) {
        fs::path base = fs::temp_directory_path() /
                        ("sbcli_fix_test_" + std::string(tag) + "_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(base, ec);
        if (fs::create_directories(base, ec) && fs::is_directory(base)) return base;
    }
    // 兜底：当前目录下
    fs::path fallback = fs::current_path() / ("sbcli_fix_test_" + std::string(tag) + "_fb");
    std::error_code ec;
    fs::remove_all(fallback, ec);
    fs::create_directories(fallback, ec);
    return fallback;
}

// 玩家脚本：引用 分数 / 速度 / 广播 游戏结束·游戏开始 / 角色 苹果
static const char* kPlayerBlock =
    "# 玩家脚本\n"
    "@script flag\n"
    "event_whenflagclicked\n"
    "data_setvariableto VARIABLE=分数 VALUE=0\n"
    "control_forever\n"
    "  control_if_else CONDITION=<sensing_touchingobject TOUCHINGOBJECTMENU=苹果>\n"
    "    data_changevariableby VARIABLE=分数 VALUE=10\n"
    "  else\n"
    "    data_changevariableby VARIABLE=分数 VALUE=-5\n"
    "  control_if CONDITION=<operator_gt OPERAND1=(data_variable VARIABLE=分数) OPERAND2=100>\n"
    "    event_broadcast BROADCAST_INPUT=游戏结束\n"
    "@script broadcast 游戏开始\n"
    "event_whenbroadcastreceived BROADCAST_OPTION=游戏开始\n"
    "control_repeat TIMES=10\n"
    "  data_changevariableby VARIABLE=速度 VALUE=1\n"
    "  looks_say MESSAGE=\"前进\"\n";

// 舞台脚本：舞台变量归根
static const char* kStageBlock =
    "@script flag\n"
    "event_whenflagclicked\n"
    "data_setvariableto VARIABLE=全局分 VALUE=7\n";

// ------------------------------------------------------------------ 主用例

static void testMain() {
    std::printf("=== 主用例：登记行为 ===\n");
    fs::path root = tempRoot("main");

    writeFile(root / "meta.sbcli", "name: 测试\n");
    writeFile(root / "character" / "1" / "block.sbcli", kPlayerBlock);
    writeFile(root / "character" / "stage" / "block.sbcli", kStageBlock);

    // dry-run：不写文件（written 记录的是"将要写"的清单，磁盘必须没变）
    FixReport dry = sbcliFix(root.string(), true);
    ok(dry.error.empty(), "dry-run 不报错");
    std::printf("        扫描到 %zu 个 block.sbcli\n", dry.files.size());
    ok(dry.files.size() == 2, "扫描到 2 个 block.sbcli");
    {
        std::error_code ec;
        bool spriteMetaExists = fs::exists(root / "character" / "1" / "meta.sbcli", ec);
        ok(!spriteMetaExists, "dry-run 不写文件（角色 meta 未生成）");
    }
    std::printf("        dry-run 登记 %d 项\n", dry.registered);

    // 真跑
    FixReport rep = sbcliFix(root.string(), false);
    ok(rep.error.empty(), "fix 不报顶层错误");
    std::printf("        登记 %d 项，错误 %d\n", rep.registered, rep.errors);
    ok(rep.registered > 0, "fix 登记了条目");

    // 角色 meta
    std::string spriteMeta = readFile(root / "character" / "1" / "meta.sbcli");
    std::printf("        [角色 meta]\n%s\n", spriteMeta.c_str());
    ok(contains(spriteMeta, "name:"), "角色 meta 有 name");
    ok(contains(spriteMeta, "is_stage: false"), "角色 meta is_stage=false");
    ok(contains(spriteMeta, "分数"), "角色 meta 登记了变量 分数");
    ok(contains(spriteMeta, "速度"), "角色 meta 登记了变量 速度");
    ok(!contains(spriteMeta, "全局分"), "角色 meta 不含舞台变量 全局分");

    // 根 meta
    std::string rootMeta = readFile(root / "meta.sbcli");
    std::printf("        [根 meta]\n%s\n", rootMeta.c_str());
    ok(contains(rootMeta, "[broadcasts]"), "根 meta 有 [broadcasts] 段");
    ok(contains(rootMeta, "游戏开始"), "根 meta 登记广播 游戏开始");
    ok(contains(rootMeta, "游戏结束"), "根 meta 登记广播 游戏结束");
    ok(contains(rootMeta, "[variables]"), "根 meta 有 [variables] 段");
    ok(contains(rootMeta, "全局分"), "根 meta 登记舞台变量 全局分");
    ok(contains(rootMeta, "全局分 = 7"),
       "舞台变量初值取第一次 setvariableto 的 VALUE (=7)");

    // TOUCHINGOBJECTMENU 是角色名，不登记成造型
    ok(!contains(spriteMeta, "苹果"), "TOUCHINGOBJECTMENU 的 苹果 不登记成造型");

    // 幂等
    FixReport again = sbcliFix(root.string(), false);
    std::string sprite2 = readFile(root / "character" / "1" / "meta.sbcli");
    std::string root2   = readFile(root / "meta.sbcli");
    ok(again.registered == 0, "第二次 fix 不再新增登记（幂等）");
    ok(sprite2 == spriteMeta, "第二次 fix 后角色 meta 内容不变");
    ok(root2 == rootMeta,     "第二次 fix 后根 meta 内容不变");
}

// ------------------------------------------------------------------ 手写字段保留

static void testPreserve() {
    std::printf("\n=== 手写字段保留 + 补默认属性 ===\n");
    fs::path root = tempRoot("preserve");

    writeFile(root / "meta.sbcli", "name: p\n");
    writeFile(root / "character" / "1" / "block.sbcli",
              "@script flag\nlooks_hide\n");
    writeFile(root / "character" / "1" / "meta.sbcli",
              "name: hero\n"
              "x: 42\n"
              "rotation_style: left_right\n"
              "costumes: []\n"
              "sounds: []\n"
              "variables: {}\n"
              "lists: {}\n"
              "broadcasts: []\n");

    sbcliFix(root.string(), false);
    std::string m = readFile(root / "character" / "1" / "meta.sbcli");
    std::printf("        [角色 meta]\n%s\n", m.c_str());

    ok(contains(m, "name: hero"),               "保留手写 name");
    ok(contains(m, "x: 42"),                    "保留手写 x");
    ok(contains(m, "rotation_style: left_right"),
       "保留手写 rotation_style");
    ok(contains(m, "is_stage:"),                "补齐缺失的 is_stage");
    ok(contains(m, "visible: true"),            "补齐缺失的 visible");
}

// ------------------------------------------------------------------ 素材

static void testAssets() {
    std::printf("\n=== 素材登记与缺失报错 ===\n");
    fs::path root = tempRoot("assets");

    writeFile(root / "meta.sbcli", "name: a\n");
    // 角色 3 引用不存在的素材
    writeFile(root / "character" / "3" / "block.sbcli",
              "@script flag\nlooks_switchcostumeto COSTUME=造型1\n");

    FixReport r1 = sbcliFix(root.string(), false);
    bool hasMissing = false;
    for (const auto& n : r1.notes)
        if (n.code == "asset-missing") hasMissing = true;
    ok(hasMissing, "缺素材文件时报错（asset-missing）");
    std::string m3 = readFile(root / "character" / "3" / "meta.sbcli");
    ok(!contains(m3, "assets/造型1.svg"), "素材不存在时不臆造路径");

    // 角色 2 引用存在的素材
    fs::path root2 = tempRoot("assets2");
    writeFile(root2 / "meta.sbcli", "name: a\n");
    writeFile(root2 / "character" / "2" / "block.sbcli",
              "@script flag\nlooks_switchcostumeto COSTUME=猫\n");
    writeFile(root2 / "assets" / "猫.svg", "<svg/>\n");

    sbcliFix(root2.string(), false);
    std::string m2 = readFile(root2 / "character" / "2" / "meta.sbcli");
    std::printf("        [角色 meta]\n%s\n", m2.c_str());
    ok(contains(m2, "猫: assets/猫.svg"), "素材存在时自动接上 assets/路径");
}

// ------------------------------------------------------------------ id 确定性

static void testIds() {
    std::printf("\n=== id 确定性 ===\n");
    ok(sbcVarId("1", "血量") == sbcVarId("1", "血量"),
       "同一 scope+名字 的 id 稳定");
    ok(sbcVarId("1", "血量") != sbcVarId("stage", "血量"),
       "不同 scope 的同名变量 id 不同");
    ok(sbcVarId("1", "血量") != sbcVarId("1", "分数"),
       "同 scope 不同名 id 不同");
    ok(sbcBroadcastId("游戏开始") == sbcBroadcastId("游戏开始"),
       "广播 id 稳定");
}

// ------------------------------------------------------------------ main

// ------------------------------------------------------------------ 全局作用域去重
// 回归：已用 add-variable/add-list 在根 meta 声明为全局的变量/列表，
// fix 不应再在角色 meta 建同名影子条目（否则 pack 时作用域判断会被干扰）。
// 本用例在恢复测试时补入——原版测试未覆盖此路径（反例检验发现）。
static void testGlobalScope() {
    std::printf("\n=== 全局作用域去重 ===\n");
    fs::path root = tempRoot("global");
    // 根 meta 已声明 得分（变量）与 道具（列表）为全局
    writeFile(root / "meta.sbcli",
              "name: g\n\n[variables]\n得分 = 0\n\n[lists]\n道具 = []\n");
    // 角色脚本同时引用这两个全局名字
    writeFile(root / "character" / "1" / "block.sbcli",
              "@script flag\n"
              "data_changevariableby VARIABLE=得分 VALUE=1\n"
              "data_addtolist LIST=道具 ITEM=苹果\n");

    sbcliFix(root.string(), false);
    std::string cm = readFile(root / "character" / "1" / "meta.sbcli");
    std::printf("        [角色 meta]\n%s\n", cm.c_str());
    ok(!contains(cm, "得分"), "已全局声明的变量不在角色 meta 建影子");
    ok(!contains(cm, "道具"), "已全局声明的列表不在角色 meta 建影子");

    // 反向：未全局声明的名字仍应登记到角色 meta
    fs::path root2 = tempRoot("global2");
    writeFile(root2 / "meta.sbcli", "name: g2\n");
    writeFile(root2 / "character" / "1" / "block.sbcli",
              "@script flag\n"
              "data_changevariableby VARIABLE=局部量 VALUE=1\n");
    sbcliFix(root2.string(), false);
    std::string cm2 = readFile(root2 / "character" / "1" / "meta.sbcli");
    ok(contains(cm2, "局部量"), "未全局声明的变量照常登记到角色 meta");
}

int main() {
    std::printf("sbcli_fix_test\n\n");
    testMain();
    testPreserve();
    testAssets();
    testGlobalScope();
    testIds();

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : "有失败");
    return g_fail == 0 ? 0 : 1;
}
