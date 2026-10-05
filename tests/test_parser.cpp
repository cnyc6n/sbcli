// tests/test_parser.cpp
// block.sbcli 解析器自测：真实样例 + 负例 + 重建 round-trip。
//
// 恢复说明：本文件由 tests/test_parser.cpp.obj 的字符串表与符号还原重建，
// 断言内容（负例输入、期望错误码、round-trip 不变量）与原始版本等价；
// 注释与局部组织为重建时补写。
//
// 退出码 0 = 全部通过。

#include "sbcli_parser.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace sb;

static int g_fail = 0;

static const char* sevName(SbcSeverity s) {
    switch (s) {
        case SbcSeverity::Error:   return "错误";
        case SbcSeverity::Warning: return "警告";
        case SbcSeverity::Info:    return "提示";
    }
    return "?";
}

static const char* kindName(SbcValue::Kind k) {
    switch (k) {
        case SbcValue::Kind::Scalar:   return "scalar";
        case SbcValue::Kind::Reporter: return "reporter(";
        case SbcValue::Kind::List:     return "list [";
    }
    return "?";
}

// ------------------------------------------------------------------ AST 打印

static void dumpValue(const SbcValuePtr& v, std::string indent);

static void dumpBlock(const SbcBlock& b, const std::string& indent) {
    std::printf("%s%s\n", indent.c_str(), b.opcode.c_str());
    for (const auto& p : b.params) {
        std::printf("%s  %s = ", indent.c_str(), p.key.c_str());
        if (p.value) dumpValue(p.value, indent + "  ");
        else         std::printf("\n");
    }
    for (size_t i = 0; i < b.substacks.size(); ++i) {
        const char* label = (b.substacks.size() > 1)
                          ? (i == 0 ? "SUBSTACK 那么" : "SUBSTACK2 否则")
                          : "SUBSTACK";
        std::printf("%s  [%s]\n", indent.c_str(), label);
        for (const auto& sb2 : b.substacks[i]) dumpBlock(sb2, indent + "    ");
    }
}

static void dumpValue(const SbcValuePtr& v, std::string indent) {
    if (!v) { std::printf("\n"); return; }
    std::printf("%s", kindName(v->kind));
    if (v->kind == SbcValue::Kind::Scalar) {
        std::printf(" %s%s%s\n", v->scalar.quoted ? "\"" : "", v->text().c_str(),
                    v->scalar.quoted ? "\"" : "");
    } else if (v->kind == SbcValue::Kind::Reporter) {
        std::printf(" %s\n", v->text().c_str());
        for (const auto& a : v->args) {
            std::printf("%s    %s -> ", indent.c_str(), a.key.c_str());
            dumpValue(a.value, indent + "    ");
        }
    } else {
        for (size_t i = 0; i < v->items.size(); ++i)
            std::printf("%s%s", i ? ", " : "", v->items[i].text.c_str());
        std::printf("]\n");
    }
}

static void dumpAst(const SbcFile& f) {
    std::printf("=== AST ===\n");
    for (size_t i = 0; i < f.scripts.size(); ++i) {
        const SbcScript& s = f.scripts[i];
        std::printf("script #%zu  @script %s%s%s (行 %d)\n", i, s.hat.c_str(),
                    s.hatArg.empty() ? "" : " ", s.hatArg.c_str(), s.line);
        for (const auto& b : s.blocks) dumpBlock(b, "  ");
    }
    std::printf("=== 诊断 (%zu) ===\n", f.diags.size());
    for (const auto& d : f.diags)
        std::printf("  行 %d: [%s] %s\n", d.line, d.code.c_str(), d.message.c_str());
    std::printf("结果：%s\n", f.ok() ? "OK（无错误）" : "有错误");
}

// ------------------------------------------------------------------ 负例

struct NegCase {
    const char* name;
    const char* src;
    const char* expect;   // 期望的诊断 code
};

static void checkNegative() {
    const std::vector<NegCase> cases = {
        { "未闭合字符串", "control_wait SECS=\"1", "E_UNCLOSED_STRING" },
        { "未闭合圆括号",
          "control_if COND=<operator_gt A=(data_variable VARIABLE=x B=1>",
          "E_UNCLOSED_PAREN" },
        { "未闭合尖括号", "control_if COND=<operator_gt A=1 B=2", "E_UNCLOSED_ANGLE" },
        { "else 带参数",
          "control_if_else COND=<sensing_mousedown>\n"
          "  looks_hide\n"
          "else COND=<sensing_mousedown>\n"
          "  looks_show", "E_ELSE_COND" },
        { "else 缩进错",
          "control_if_else COND=<sensing_mousedown>\n"
          "  looks_hide\n"
          "    else\n"
          "  looks_show", "E_ELSE_INDENT" },
        { "孤儿 else", "looks_hide\nelse", "E_ELSE_ORPHAN" },
        { "未知帽子", "@script touchdown\nevent_whenflagclicked", "E_HAT_UNKNOWN" },
        { "broadcast 缺名字",
          "@script broadcast\nevent_whenbroadcastreceived BROADCAST_OPTION=x",
          "E_HAT_ARG_MISSING" },
        { "参数没写 KEY=", "control_wait 1", "E_PARAM_EXPECTED" },
        { "值位置缺值", "control_wait SECS=", "E_VALUE_EXPECTED" },
        { "未知指令", "@sceipt flag\nlooks_hide", "E_DIRECTIVE_UNKNOWN" },
    };

    std::printf("\n=== 负例自检（每条都应报 Error）===\n");
    for (const auto& c : cases) {
        SbcFile f = sbcParse(c.src);
        bool ok = !f.ok();
        bool codeHit = false;
        for (const auto& d : f.diags)
            if (d.code == c.expect) codeHit = true;
        if (ok && codeHit) {
            std::printf("  [通过] %s\n", c.name);
        } else {
            ++g_fail;
            std::printf("  [漏报] %s（期望 %s）\n", c.name, c.expect);
            for (const auto& d : f.diags)
                std::printf("        实际：行 %d [%s] %s（%s）\n", d.line,
                            d.code.c_str(), d.message.c_str(), sevName(d.severity));
        }
    }
}

// ------------------------------------------------------------------ round-trip

static void checkRoundTrip(const std::string& src, const char* label) {
    SbcFile f1 = sbcParse(src);
    if (!f1.ok()) {
        ++g_fail;
        std::printf("  [失败] %s 首次解析就有错\n", label);
        return;
    }
    std::string t1 = sbcUnparse(f1);
    SbcFile f2 = sbcParse(t1);
    if (!f2.ok()) {
        ++g_fail;
        std::printf("  [失败] 重建结果再解析报错：\n%s", f2.report().c_str());
        return;
    }
    std::string t2 = sbcUnparse(f2);
    if (t1 != t2) {
        ++g_fail;
        std::printf("  [失败] 二次重建不一致\n");
        return;
    }
    std::printf("  [通过] 重建文本稳定（%zu 字节）\n", t1.size());
}

// ------------------------------------------------------------------ main

int main() {
    // 1) 真实样例（内联，跨平台：不依赖本机文件路径）
    const std::string sample = R"(
@script flag
event_whenflagclicked
data_setvariableto VARIABLE=分数 VALUE=0
control_forever
  control_if_else COND=<operator_gt A=(data_variable VARIABLE=分数) B=100>
    event_broadcast BROADCAST_INPUT=游戏结束
  else
    data_changevariableby VARIABLE=分数 VALUE=1

@script broadcast 游戏开始
event_whenbroadcastreceived BROADCAST_OPTION=游戏开始
data_changevariableby VARIABLE=分数 VALUE=-1

@script flag
procedures_definition PROCCODE="移动 %s 步" ARGS=[步数]
  argument_reporter_string_number VALUE=步数
  motion_movesteps STEPS=(argument_reporter_string_number VALUE=步数)
)";
    SbcFile real = sbcParse(sample);
    std::printf("解析内联真实样例…\n");
    dumpAst(real);
    if (!real.ok()) {
        ++g_fail;
        std::printf("存在问题\n");
    } else {
        std::printf("全部通过\n");
    }
    if (real.scripts.size() != 3) {
        ++g_fail;
        std::printf("[失败] 真实样例应解析出 3 个脚本，实际 %zu\n", real.scripts.size());
    }
    if (!real.diags.empty()) {
        ++g_fail;
        std::printf("[失败] 真实样例应 0 条诊断，实际 %zu\n", real.diags.size());
    }

    // 2) 负例
    checkNegative();

    // 3) round-trip
    std::printf("\n=== 重建 round-trip ===\n");
    checkRoundTrip(
        "@script flag\n"
        "event_whenflagclicked\n"
        "data_setvariableto VARIABLE=分数 VALUE=0\n"
        "control_forever\n"
        "  control_if_else CONDITION=<sensing_mousedown>\n"
        "    looks_hide\n"
        "  else\n"
        "    looks_show\n",
        "嵌套 + else");

    std::printf("\n--- 重建输出 ---\n");
    {
        SbcFile f = sbcParse(
            "@script flag\n"
            "event_whenflagclicked\n"
            "control_if_else CONDITION=<sensing_mousedown>\n"
            "  looks_hide\n"
            "else\n"
            "  looks_show\n");
        std::string t = sbcUnparse(f);
        std::printf("%s", t.c_str());
        std::printf("----------------\n");
    }

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : "有失败");
    return g_fail == 0 ? 0 : 1;
}
