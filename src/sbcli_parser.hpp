// src/sbcli_parser.hpp
// block.sbcli 的解析 + 重建（契约见 docs/format.md）。
//
// 本模块只负责「文本 ⇄ 结构」，不做语义校验，也不抛异常：
//
//     文本 ──[sbcLex]──> SbToken 序列 ──[sbcParse]──> SbcFile ──[sbcUnparse]──> 文本
//
// · 所有问题收集进 SbcFile::diags（行号 + 稳定 code + 中文说明 + 严重级别），
//   一个文件里的多个错误一次全收，不会因为第一行坏了就看不到后面的错。
// · 语义校验（opcode 是否收录、参数是否齐全、ARG 数量、meta 引用）是
//   sb check 的职责，这里只报**结构性**错误（括号未闭合、缩进错、else 不匹配…）。
//
// 设计取舍与已知规范歧义见 docs/parser-notes.md。
#pragma once

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace sb {

// ------------------------------------------------------------------ 诊断

enum class SbcSeverity { Error, Warning, Info };

struct SbcDiag {
    SbcSeverity severity = SbcSeverity::Error;
    int         line     = 0;   // 1 起
    int         col      = 0;   // 1 起；0 = 整行，不定位列
    std::string code;           // 稳定短码，便于测试/i18n：E_UNCLOSED_STRING / W_ODD_INDENT …
    std::string message;        // 面向用户的中文说明

    bool isError() const { return severity == SbcSeverity::Error; }
};

// ------------------------------------------------------------------ 词法

enum class SbTok {
    IDENT,      // 裸标量：100 / true / 造型1 / 苹果 / _mouse_
    STRING,     // 双引号字符串（转义已按 format.md 1.2 解析）
    LPAREN,     // (
    RPAREN,     // )
    LANGLE,     // <
    RANGLE,     // >
    LBRACKET,   // [
    RBRACKET,   // ]
    COMMA,      // ,
    PARAM,      // KEY=    text 是键名，不含 '='
    OPCODE,     // 行首 opcode，或 reporter 括号内的第一个 token
    COMMENT,    // # 注释
    ELSE,       // 行首 else 关键字
    DIRECTIVE,  // @script
    NEWLINE,    // 行末；indent 是**其后那一行**的缩进列数
    END
};

// 唯一的权威中间表示。
//
// quoted 必须保留：format.md 1.2 允许裸写简单标识符，反过来写回时
// 若把裸标量统统加引号，100 会变成 "100" —— sb3 里那是字符串常量，
// 语义直接变了。所以原文是否带引号是**语义**信息，不是格式噪音。
struct SbToken {
    SbTok       type   = SbTok::END;
    std::string text;
    int         line   = 0;
    int         indent = 0;    // 仅 NEWLINE 有效
    bool        quoted = false; // 仅 IDENT/STRING 有效
};

std::vector<SbToken> sbcLex(const std::string& src, std::vector<SbcDiag>& diags);

// ------------------------------------------------------------------ AST

struct SbcValue;
using SbcValuePtr = std::shared_ptr<SbcValue>;

// 一个命名参数。
// key  = 源码里写的名字（重建用，保证 round-trip）
// canon= 归一化到 SB3 真实字段的名字（check / pack 用）
//
// 之所以要两个：docs/format.md §2 用了一批简写（COND= / A= / B= / SECS=），
// 与 src/sb3_tables.hpp 的 SB3_T 模板名（CONDITION / OPERAND1 / NUM1 /
// DURATION）不一致。解析器两边都接受，但对外统一给 canon，
// 免得 pack 出来的 sb3 字段对不上。别名是静默接受的（不产生诊断）。
struct SbcParam {
    std::string  key;
    std::string  canon;
    int          line = 0;
    SbcValuePtr  value;
};

// 一个参数值：裸标量 / reporter / 列表字面量，三选一。
struct SbcValue {
    enum class Kind : unsigned char { Scalar, Reporter, List };

    Kind        kind    = Kind::Scalar;
    SbToken     scalar;              // Scalar: 值 token；Reporter: '(' 或 '<' 那个 token
    bool        boolean = false;      // Reporter: 是否为 <...>（布尔 reporter）
    std::vector<SbcParam>   args;     // Reporter: 命名参数（可再嵌 reporter）
    std::vector<SbToken>    items;    // List: 元素 token（只允许标量）

    const std::string& text() const { return scalar.text; }
    bool isScalar()   const { return kind == Kind::Scalar; }
    bool isReporter() const { return kind == Kind::Reporter; }
    bool isList()     const { return kind == Kind::List; }

    // 取 reporter 的某个参数（按 canon 名匹配，找不到返回 nullptr）
    const SbcParam* arg(const std::string& canonKey) const;

    static SbcValuePtr makeScalar(SbToken t);
    static SbcValuePtr makeReporter(SbToken open, bool isBool);
    static SbcValuePtr makeList(std::vector<SbToken> items);
};

// 一个积木。
struct SbcBlock {
    std::string            opcode;
    int                    line = 0;
    std::vector<SbcParam>  params;      // 有序；重复 KEY 保留多份（由 check 报重复）

    // 缩进子栈，按出现顺序。control_if_else 且写了 else 时：
    //   [0] = 那么分支，[1] = 否则分支；其余 opcode 最多一个。
    std::vector<std::vector<SbcBlock>> substacks;
    int  elseLine = 0;   // else 所在行号；没有 else 时为 0

    const SbcParam* find(const std::string& canonKey) const;
    bool hasElse() const { return elseLine != 0; }
};

// 一个 @script 脚本。
struct SbcScript {
    std::string            hat = "other";  // flag/broadcast/key/clone/click/other
    std::string            hatArg;         // @script 行剩余参数（已 trim）
    int                    line = 0;
    std::vector<SbcBlock>  blocks;         // 顶层块序列
};

struct SbcFile {
    std::vector<SbcScript> scripts;
    std::vector<SbcDiag>   diags;

    bool ok() const;                  // 无 Error 级诊断
    std::string report() const;       // "行 N: 错误: …" 的可读文本
};

// 主入口：文本 → AST。内部捕获所有异常并转成诊断，保证不抛出。
SbcFile sbcParse(const std::string& src);

// 读文件（UTF-8，自动去 BOM）→ AST。读不到时返回一条 E_FILE 诊断。
SbcFile sbcParseFile(const std::string& path);

// ------------------------------------------------------------------ 重建

// AST → block.sbcli 文本（缩进统一 2 空格）。
// 语义无损（再解析回来 AST 相同），但**不保留注释与原始空格**。
std::string sbcUnparse(const SbcFile& f);

std::string sbcTokensToText(const std::vector<SbToken>& toks);

// ------------------------------------------------------------------ 工具

// 把 raw 变成可放进双引号的正文（\ " \n \t \r 控制字符 → 转义序列）
std::string sbcEscape(const std::string& raw);

// 该裸标量重建时是否必须加引号（含空白/分隔符/引号/# ，或会被当成数字/布尔）
bool sbcNeedsQuotes(const std::string& text);

// format.md 简写 → SB3 真实字段名；无别名时原样返回。
// 例：("operator_gt","A") → "OPERAND1"；("control_wait","SECS") → "DURATION"
std::string sbcCanonKey(const std::string& opcode, const std::string& key);

// 取某 opcode 在 SB3_T 模板里的全部字段名（用于 check 的"未知参数"判断）
std::vector<std::string> sbcOpcodeFields(const std::string& opcode);

// 别名表的唯一事实来源：{opcode, 简写} → 真名。
// 导出是给 check（提示"建议改成 X"）和 pack（反查）复用，
// 免得每处各抄一份、将来改 SB3_T 时漏改。
// 约束：目标字段名必须在该 opcode 的 SB3_T 模板里真实存在；
// 自测会逐条校验，写错直接挂测试（而不是运行时静默失效）。
const std::vector<std::tuple<std::string, std::string, std::string>>& sbcAliasTable();

// 该 opcode 是否"已知"：在 SB3_T 里，或是保留 opcode。
// check 的"未收录 opcode"判断用它 —— format.md §5.2 若按字面执行，
// procedures_definition / procedures_call 会被误报"未收录"（它们不在表里）。
bool sbcIsKnownOpcode(const std::string& opcode);

// 取 SB3_T 的原始翻译模板（"移动 {STEPS} 步"）；不在表里返回空串。
std::string sbcOpcodeTemplate(const std::string& opcode);

// ------------------------------------------------------------------ id

// 变量 / 列表 / 广播的 id（供 sb3 的 variables / broadcasts 用）。
//
// 设计：**纯函数，不落盘**。meta.sbcli 的格式（format.md §3.1/3.3）没有 id
// 字段，硬塞一个 [ids] 段会动到公共格式、连带影响 check 的 meta 解析。
// 改成"名字 → id"的确定性哈希：同名同 scope 必得同 id，所以 fix 与 pack
// 各自调用即天然一致，且重复运行结果稳定（幂等）。
//
// scope 用来隔离同名的全局变量与角色变量（传角色目录名，如 "1" / "stage"）。
std::string sbcVarId(const std::string& scope, const std::string& name);
// 广播是全局的，没有 scope
std::string sbcBroadcastId(const std::string& name);

} // namespace sb
