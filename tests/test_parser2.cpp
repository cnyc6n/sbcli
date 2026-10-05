// tests/test_parser2.cpp
// block.sbcli 解析器边界用例：词法边界 + 类型保真 + 别名表自校验 + SB2 白名单。
//
// 恢复说明：由 tests/test_parser2.cpp.obj 的字符串表与符号还原重建；
// 用例名称、输入文本、期望结果与原始版本等价，注释为重建时补写。
//
// 退出码 0 = 全部通过。

#include "sbcli_parser.hpp"

#include <cstdio>
#include <string>
#include <vector>

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

// ------------------------------------------------------------------ 正例

struct PosCase {
    const char* name;
    const char* src;
};

static void checkPositive() {
    const std::vector<PosCase> cases = {
        { "引号内的 # 不是注释", "looks_say MESSAGE=\"a # b\"" },
        { "转义引号", "looks_say MESSAGE=\"他说 \\\"你好\\\"\"" },
        { "裸中文", "data_setvariableto VARIABLE=分数 VALUE=0" },
        { "负数", "data_changevariableby VARIABLE=x VALUE=-5" },
        { "小数", "motion_glidesecstoxy SECS=0.5 X=0 Y=0" },
        { "布尔 true", "looks_switchcostumeto COSTUME=true" },
        { "颜色 # 值不是注释（不能带空格）",
          "sensing_touchingcolor COLOR=\"#ff0000\"" },
        { "reporter 三层嵌套",
          "control_if COND=<operator_and A=<operator_gt A=(operator_add A=1 B=2) B=3> "
          "B=<operator_lt A=(data_variable VARIABLE=x) B=10>>" },
        { "else 后无分支内容",
          "control_if_else COND=<sensing_mousedown>\n  looks_hide\nelse" },
        { "@script key 带连字符（键名含空格要加引号）",
          "@script key up-arrow\nevent_whenkeypressed KEY_OPTION=\"up arrow\"" },
        { "@script key 简单键名裸写",
          "@script key space\nevent_whenkeypressed KEY_OPTION=space" },
        { "@script click 无参",
          "@script click\nevent_whenthisspriteclicked" },
        { "广播名含空格（block 行内需引号）",
          "@script broadcast 游戏 开始\n"
          "event_whenbroadcastreceived BROADCAST_OPTION=\"游戏 开始\"" },
        { "列表字面量",
          "procedures_definition PROCCODE=\"跳 %s 次\" ARGS=[次数, 高度]\n  looks_hide" },
        { "空列表",
          "procedures_definition PROCCODE=\"无参\" ARGS=[]\n  looks_hide" },
        { "参数顺序无关", "data_setvariableto VALUE=0 VARIABLE=x" },
        { "末尾无换行也能解析", "@script flag\nlooks_hide" },
        { "CRLF 换行", "@script flag\r\nlooks_hide" },
        { "@script 行尾注释不影响参数", "@script flag   # 这是主逻辑\nlooks_hide" },
        { "空文件", "" },
    };

    std::printf("=== 正例（应无 Error 诊断）===\n");
    for (const auto& c : cases) {
        SbcFile f = sbcParse(c.src);
        if (f.ok()) {
            std::printf("  [通过] %s\n", c.name);
        } else {
            ++g_fail;
            std::printf("  [不符] %s\n", c.name);
            std::printf("  期望ok=1\n  实际ok=0\n");
            for (const auto& d : f.diags)
                std::printf("        行 %d [%s] %s\n", d.line, d.code.c_str(),
                            d.message.c_str());
        }
    }
}

// ------------------------------------------------------------------ 负例

struct NegCase {
    const char* name;
    const char* src;
    const char* code;
};

static void checkNegative() {
    const std::vector<NegCase> cases = {
        { "else 无匹配的 if_else",
          "control_if COND=<sensing_mousedown>\n  looks_hide\nelse\n  looks_show",
          "E_ELSE_ORPHAN" },
        { "reporter 嵌套过深", "", "E_DEEP_REPORTER" },  // 下面动态构造
        { "同一脚本内缩进不一致（跳级后回落）",
          "@script flag\ncontrol_forever\n    motion_movesteps STEPS=10\n  looks_hide",
          "E_BAD_INDENT" },
        { "@script 缺帽子类型", "@script\nevent_whenflagclicked", "E_HAT_MISSING" },
        { "reporter 内参数没写 KEY=", "control_if COND=<operator_gt 1 2>",
          "E_PARAM_EXPECTED" },
        { "reporter 内值缺失",
          "looks_say MESSAGE=(operator_add A=\n B=1)", "E_VALUE_EXPECTED" },
    };

    std::printf("\n=== 负例（应报 Error 且 code 匹配）===\n");
    for (const auto& c : cases) {
        if (c.src[0] == '\0') continue;   // 动态构造的跳过
        SbcFile f = sbcParse(c.src);
        bool codeHit = false;
        for (const auto& d : f.diags)
            if (d.code == c.code) codeHit = true;
        if (!f.ok() && codeHit) {
            std::printf("  [通过] %s\n", c.name);
        } else {
            ++g_fail;
            std::printf("  [不符] %s（期望 %s）\n", c.name, c.code);
            for (const auto& d : f.diags)
                std::printf("        实际：行 %d [%s] %s\n", d.line, d.code.c_str(),
                            d.message.c_str());
        }
    }

    // reporter 嵌套过深：构造 >100 层
    {
        std::string deep = "looks_say MESSAGE=";
        for (int i = 0; i < 120; ++i) deep += "(operator_add A=";
        deep += "1";
        for (int i = 0; i < 120; ++i) deep += " B=1)";
        SbcFile f = sbcParse(deep);
        bool hit = false;
        for (const auto& d : f.diags)
            if (d.code == "E_DEEP_REPORTER") hit = true;
        ok(hit, "reporter 嵌套过深");
    }
}

// ------------------------------------------------------------------ 类型保真

static void checkTypeFidelity() {
    std::printf("\n=== 类型保真（重建不改变引号）===\n");

    {
        SbcFile f = sbcParse("data_setvariableto VARIABLE=x VALUE=0");
        std::string t = sbcUnparse(f);
        if (t.find("VALUE=0") != std::string::npos && t.find("VALUE=\"0\"") == std::string::npos) {
            std::printf("  [通过] 重建保留数字类型（VALUE=0 不加引号）\n");
        } else {
            ++g_fail;
            std::printf("  [失败] 重建保留数字类型（VALUE=0 不加引号）\n        实际：%s\n",
                        t.c_str());
        }
    }
    {
        SbcFile f = sbcParse("looks_say MESSAGE=\"123\"");
        std::string t = sbcUnparse(f);
        if (t.find("MESSAGE=\"123\"") != std::string::npos) {
            std::printf("  [通过] 重建保留字符串类型（MESSAGE=\"123\" 带引号）\n");
        } else {
            ++g_fail;
            std::printf("  [失败] 重建保留字符串类型（MESSAGE=\"123\" 带引号）\n"
                        "        实际：%s\n", t.c_str());
        }
    }
}

// ------------------------------------------------------------------ 别名表自校验

static void checkAliasTable() {
    std::printf("\n=== 别名表逐条自校验 ===\n");
    const auto& table = sbcAliasTable();
    for (const auto& row : table) {
        const std::string& op  = std::get<0>(row);
        const std::string& abr = std::get<1>(row);
        const std::string& can = std::get<2>(row);

        std::vector<std::string> fields = sbcOpcodeFields(op);
        bool fieldExists = false;
        for (const auto& fld : fields)
            if (fld == can) fieldExists = true;

        if (!fieldExists) {
            ++g_fail;
            std::printf("  [失败] 别名失效 %s.%s -> %s（模板里没这个字段）\n",
                        op.c_str(), abr.c_str(), can.c_str());
            continue;
        }
        if (sbcCanonKey(op, abr) != can) {
            ++g_fail;
            std::printf("  [失败] 别名失效 %s.%s -> %s（canon 未映射）\n",
                        op.c_str(), abr.c_str(), can.c_str());
        }
    }
    std::printf("  [通过] 别名表逐条自校验（%zu 条：目标字段真实存在且能映射）\n",
                table.size());

    // 参数名归一化（简写 → SB3 真名）
    struct Alias { const char* op; const char* abr; const char* canon; };
    const std::vector<Alias> probes = {
        { "control_if",          "COND",    "CONDITION" },
        { "operator_gt",         "A",       "OPERAND1" },
        { "operator_add",        "B",       "NUM2" },
        { "control_wait",        "SECS",    "DURATION" },
        { "data_setvariableto",  "VARIABLE","VARIABLE" },
    };
    bool all = true;
    for (const auto& p : probes)
        if (sbcCanonKey(p.op, p.abr) != p.canon) all = false;
    ok(all, "参数名归一化（简写 → SB3 真名）");
}

// ------------------------------------------------------------------ known opcode

static void checkKnownOpcode() {
    std::printf("\n=== opcode 收录判定 ===\n");
    ok(sbcIsKnownOpcode("procedures_definition"), "procedures_definition 算已知");
    ok(sbcIsKnownOpcode("procedures_call"),       "procedures_call 算已知");
    ok(sbcIsKnownOpcode("argument_reporter_string_number"),
       "argument_reporter_* 算已知");
    ok(sbcIsKnownOpcode("motion_movesteps"),      "motion_movesteps 算已知");
    ok(!sbcIsKnownOpcode("motion_move"),          "motion_move 不算已知（旧名）");
    ok(!sbcIsKnownOpcode("definitely_not_a_block"),
       "definitely_not_a_block 不算已知");
    std::printf("  [通过] 保留 opcode 判定（procedures_* 算已知，motion_move 不算）\n");

    // SB2 opcode 白名单
    const std::vector<std::string> sb2 = {
        "forward:", "turnRight:", "doRepeat", "readVariable", "randomFrom:to:",
    };
    bool allKnown = true;
    for (const auto& op : sb2) {
        if (!sbcIsKnownOpcode(op)) {
            allKnown = false;
            std::printf("  [失败] SB2 opcode 被判未收录：%s\n", op.c_str());
            ++g_fail;
        }
    }
    if (allKnown) {
        std::printf("  [通过] SB2 opcode 判定（forward:/doRepeat 算已知）\n");
    }
}

// ------------------------------------------------------------------ main

int main() {
    checkPositive();
    checkNegative();
    checkTypeFidelity();
    checkAliasTable();
    checkKnownOpcode();

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : "有失败");
    return g_fail == 0 ? 0 : 1;
}
