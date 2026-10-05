// src/sbcli_parser.cpp
// block.sbcli 解析 + 重建的实现（契约见 docs/format.md）。
//
// 实现约定
// --------
// · 全程按 **UTF-8 字节** 处理。汉字是多字节序列，只要落在"不是空白、不是
//   分隔符"的分支里就会被整个吃掉，无需特殊处理。缩进按**列**数（字节数）算，
//   与"用空格缩进"的规范一致；Tab 会被单独报 W_TAB_INDENT，避免宽度歧义。
//
// · 分两步：先把源码切成**物理行**（保留 raw 原文，@script 的行参数要用），
//   再逐行词法化。积木不可能跨行（缩进即结构），所以逐行词法不会漏括号；
//   反倒是字符串/括号没闭合时能被天然地限制在一行内报错，定位更准。
//
// · 所有问题走 addDiag()，不抛异常、不提前 return。sbcParse() 外层再兜
//   一层 catch，保证任何输入都能拿回一个（可能残缺的）AST。
#include "sbcli_parser.hpp"
#include "sb3_tables.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sb {
namespace {

// ================================================================ 诊断小工具

void addDiag(std::vector<SbcDiag>& d, SbcSeverity sev, int line, int col,
             const char* code, const std::string& msg) {
    SbcDiag x;
    x.severity = sev;
    x.line     = line;
    x.col      = col;
    x.code     = code;
    x.message  = msg;
    d.push_back(std::move(x));
}
void err(std::vector<SbcDiag>& d, int line, int col, const char* code, const std::string& m) {
    addDiag(d, SbcSeverity::Error,   line, col, code, m);
}
void warn(std::vector<SbcDiag>& d, int line, int col, const char* code, const std::string& m) {
    addDiag(d, SbcSeverity::Warning, line, col, code, m);
}

bool isSpaceCh(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// 单独字符即分隔符：裸词遇到就断开。这些字符不可能出现在合法标识符里。
bool isSpecial(char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == ',' || c == '=' ||
           c == '"' || c == '#';
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpaceCh(s[b])) ++b;
    while (e > b && isSpaceCh(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool looksNumeric(const std::string& s) {
    if (s.empty()) return false;
    try {
        size_t n = 0;
        std::stod(s, &n);
        return n == s.size();
    } catch (...) { return false; }
}

// ================================================================ 行切分

struct Line {
    int                  no     = 0;
    int                  indent = 0;   // 前导空格列数
    std::string          raw;          // 去掉缩进后的整行原文（@script 行参数要用）
    std::vector<SbToken> toks;
};

std::vector<std::string> splitLines(const std::string& src) {
    std::vector<std::string> out;
    size_t b = 0;
    while (b <= src.size()) {
        size_t e = src.find('\n', b);
        if (e == std::string::npos) {
            if (b < src.size()) out.push_back(src.substr(b));
            break;
        }
        std::string one = src.substr(b, e - b);
        if (!one.empty() && one.back() == '\r') one.pop_back();
        out.push_back(one);
        b = e + 1;
    }
    return out;
}

// ================================================================ 词法（逐行）

struct LineLexer {
    // 持有**副本**而非引用：KEY= 之后紧跟的值（如 MESSAGE="你好"、COLOR=#fff）
    // 会被读进同一个裸词里，实现上是把值的部分插回缓冲区重新词法化。
    std::string        s;
    int                line;
    std::vector<SbToken>& out;
    std::vector<SbcDiag>& diags;
    size_t i = 0;

    LineLexer(std::string text, int ln, std::vector<SbToken>& o, std::vector<SbcDiag>& d)
        : s(std::move(text)), line(ln), out(o), diags(d) {}

    int col() const { return (int)i + 1; }

    void emit(SbTok t, const std::string& text, bool quoted = false) {
        SbToken tok;
        tok.type = t; tok.text = text; tok.line = line; tok.quoted = quoted;
        out.push_back(std::move(tok));
    }

    // 读一个"裸词"：一直吃到空白或分隔符。
    // '=' 是例外——必须允许它出现在词里，否则 VARIABLE=分数 会被切成
    // VARIABLE / 分数 两段，KEY= 就永远认不出来。遇到 '=' 后由调用方
    // 在第一个 '=' 处切开，把后半段退回输入流当值重新词法化。
    std::string readBare() {
        size_t b = i;
        while (i < s.size() && !isSpaceCh(s[i]) && !isSpecial(s[i]) && s[i] != '=') ++i;
        // 尾随的 '=' 属于 KEY= 形式，一并吃掉
        if (i < s.size() && s[i] == '=') ++i;
        return s.substr(b, i - b);
    }

    // 双引号字符串，按 format.md 1.2 解转义：\" \\ \n \t（另兼容 \r）
    std::string readQuoted() {
        ++i; // 开引号
        std::string v;
        bool closed = false;
        while (i < s.size()) {
            char c = s[i];
            if (c == '\\') {
                if (i + 1 >= s.size()) { ++i; break; }
                char n = s[i + 1];
                switch (n) {
                    case '"':  v += '"';  break;
                    case '\\': v += '\\'; break;
                    case 'n':  v += '\n'; break;
                    case 't':  v += '\t'; break;
                    case 'r':  v += '\r'; break;
                    default:
                        warn(diags, line, col(), "W_BAD_ESCAPE",
                             "未知转义 \\" + std::string(1, n) + "，已按字面保留");
                        v += '\\'; v += n;
                        break;
                }
                i += 2;
                continue;
            }
            if (c == '"') { ++i; closed = true; break; }
            v += c;
            ++i;
        }
        if (!closed) {
            err(diags, line, col(), "E_UNCLOSED_STRING", "字符串缺少闭合的双引号");
        }
        return v;
    }

    void run() {
        bool atLineStart = true;
        bool lastWasParam = false;   // 上一个 token 是不是 KEY=（用于 COLOR=#fff 的特判）
        while (i < s.size()) {
            char c = s[i];

            if (isSpaceCh(c)) { ++i; continue; }

            // 注释：整行剩下都是（词法阶段就吞掉，引号内的 # 走不到这里）
            // 例外：紧跟在 KEY= 后面的 # 是颜色值（COLOR=#ff0000），不是注释。
            if (c == '#' && !(lastWasParam && i > 0 && s[i - 1] == '=')) {
                size_t b = i;
                while (i < s.size()) ++i;
                emit(SbTok::COMMENT, s.substr(b));
                return;
            }
            if (c == '#') {
                // KEY=#ff0000 —— 吃掉到空白为止，当作值
                size_t b = i;
                while (i < s.size() && !isSpaceCh(s[i])) ++i;
                emit(SbTok::IDENT, s.substr(b));
                lastWasParam = false;
                continue;
            }

            if (c == '"') {
                std::string v = readQuoted();
                SbToken t;
                t.type = SbTok::STRING; t.text = v; t.line = line; t.quoted = true;
                out.push_back(t);
                atLineStart = false;
                continue;
            }

            SbTok p = SbTok::END;
            if      (c == '(') p = SbTok::LPAREN;
            else if (c == ')') p = SbTok::RPAREN;
            else if (c == '<') p = SbTok::LANGLE;
            else if (c == '>') p = SbTok::RANGLE;
            else if (c == '[') p = SbTok::LBRACKET;
            else if (c == ']') p = SbTok::RBRACKET;
            else if (c == ',') p = SbTok::COMMA;
            if (p != SbTok::END) {
                emit(p, std::string(1, c));
                ++i;
                atLineStart = false;
                continue;
            }

            std::string w = readBare();
            if (w.empty()) { ++i; continue; }  // 防死循环

            // KEY= → PARAM：在第一个 '=' 处切分，后半段退回输入流当值处理
            size_t eq = w.find('=');
            if (eq != std::string::npos && eq > 0) {
                std::string key = w.substr(0, eq);
                std::string rest = w.substr(eq + 1);   // '=' 之后的部分（可能是值）
                if (rest.size() >= 1 && rest[0] == '=') {   // "==" 容错
                    rest.erase(0, 1);
                    warn(diags, line, col(), "W_DOUBLE_EQ",
                         "参数 " + key + " 后面有两个等号，按一个处理");
                }
                emit(SbTok::PARAM, key);
                lastWasParam = true;
                atLineStart = false;
                // 值部分退回输入流，下一轮按正常值规则词法化（可能是字符串/reporter/裸词）
                if (!rest.empty()) {
                    s.insert(i, rest);   // 就地插回（行内容是副本，可改）
                }
                continue;
            }

            if (atLineStart) {
                if (w == "else")                 emit(SbTok::ELSE, w);
                else if (w[0] == '@')            emit(SbTok::DIRECTIVE, w);
                else                             emit(SbTok::OPCODE, w);
                atLineStart = false;
                continue;
            }

            // 行内裸词。是"值"还是"reporter 的 opcode"由语法阶段按位置判定
            // （括号内的第一个 token 才是 opcode），词法只标 IDENT。
            emit(SbTok::IDENT, w);
        }
    }
};

// 行内容里的注释起点（考虑引号），仅用于取 @script 的原文参数。
size_t commentStart(const std::string& s) {
    bool inStr = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && inStr) { ++i; continue; }
        if (c == '"') { inStr = !inStr; continue; }
        if (c == '#' && !inStr) return i;
    }
    return std::string::npos;
}

std::vector<Line> lexLines(const std::string& src, std::vector<SbcDiag>& diags) {
    std::vector<Line> lines;
    std::vector<std::string> raws = splitLines(src);
    lines.reserve(raws.size());

    for (size_t k = 0; k < raws.size(); ++k) {
        const std::string& raw = raws[k];
        Line L;
        L.no = (int)k + 1;

        size_t b = 0;
        while (b < raw.size() && (raw[b] == ' ' || raw[b] == '\t')) {
            if (raw[b] == '\t') {
                warn(diags, L.no, (int)b + 1, "W_TAB_INDENT",
                     "缩进里用了 Tab，建议统一用空格");
            }
            ++L.indent;
            ++b;
        }
        std::string body = raw.substr(b);
        size_t cs = commentStart(body);
        std::string code = (cs == std::string::npos) ? body : body.substr(0, cs);

        L.raw = trim(code);
        LineLexer lx(code, L.no, L.toks, diags);
        lx.run();

        // 缩进不为 2 的倍数：大概率是手滑（format.md 1.7 建议 2 格）
        if (L.indent % 2 != 0 && !L.toks.empty()) {
            warn(diags, L.no, 1, "W_ODD_INDENT",
                 "缩进 " + std::to_string(L.indent) + " 格不是 2 的倍数（建议每层 2 格）；"
                 "子块缩进必须严格比父块深");
        }
        lines.push_back(std::move(L));
    }
    return lines;
}

// ================================================================ 参数名归一化

struct Alias { const char* op; const char* from; const char* to; };

// format.md 早期版本的简写 → SB3_T 模板里的真实字段名。
//
// 2026-10 起 docs/format.md §2/§6 已统一写成真名（CONDITION/OPERAND1…），
// 这些只是**向后兼容**旧手写文件用的，不再鼓励新代码用。
// 只在"该 opcode 的模板确实含有目标字段"时才映射，绝不凭空造字段
// （sbcCanonKey 里校验，自测也逐条断言）。
const Alias ALIASES[] = {
    {"control_if",           "COND", "CONDITION"},
    {"control_if_else",      "COND", "CONDITION"},
    {"control_wait_until",   "COND", "CONDITION"},
    {"control_repeat_until", "COND", "CONDITION"},
    {"control_while",        "COND", "CONDITION"},

    {"operator_gt",     "A", "OPERAND1"}, {"operator_gt",     "B", "OPERAND2"},
    {"operator_lt",     "A", "OPERAND1"}, {"operator_lt",     "B", "OPERAND2"},
    {"operator_equals", "A", "OPERAND1"}, {"operator_equals", "B", "OPERAND2"},
    {"operator_and",    "A", "OPERAND1"}, {"operator_and",    "B", "OPERAND2"},
    {"operator_or",     "A", "OPERAND1"}, {"operator_or",     "B", "OPERAND2"},

    {"operator_add",      "A", "NUM1"}, {"operator_add",      "B", "NUM2"},
    {"operator_subtract", "A", "NUM1"}, {"operator_subtract", "B", "NUM2"},
    {"operator_multiply", "A", "NUM1"}, {"operator_multiply", "B", "NUM2"},
    {"operator_divide",   "A", "NUM1"}, {"operator_divide",   "B", "NUM2"},
    {"operator_mod",      "A", "NUM1"}, {"operator_mod",      "B", "NUM2"},

    {"operator_join",     "A", "STRING1"}, {"operator_join",     "B", "STRING2"},
    {"operator_contains", "A", "STRING1"}, {"operator_contains", "B", "STRING2"},

    {"operator_not", "COND", "OPERAND"},
    {"control_wait", "SECS", "DURATION"},
};

// 不在 SB3_T 表里、但 block.sbcli 明确支持的 opcode。
// 按 format.md §5.2 字面执行"不在表 → 报未收录"会把它们全误报，
// 所以 check 要用 sbcIsKnownOpcode()，不能直接查 SB3_T。
const char* const RESERVED_OPCODES[] = {
    "procedures_definition",
    "procedures_call",
    "argument_reporter_string_number",
    "argument_reporter_boolean",
};

std::vector<std::string> fieldsOfTemplate(const std::string& tmpl) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{') {
            size_t j = tmpl.find('}', i);
            if (j == std::string::npos) break;
            v.push_back(tmpl.substr(i + 1, j - i - 1));
            i = j + 1;
        } else ++i;
    }
    return v;
}

} // namespace

std::string sbcCanonKey(const std::string& opcode, const std::string& key) {
    auto it = SB3_T.find(opcode);
    if (it != SB3_T.end()) {
        std::vector<std::string> fields = fieldsOfTemplate(it->second);
        for (const auto& a : ALIASES) {
            if (opcode != a.op || key != a.from) continue;
            for (const auto& f : fields) if (f == a.to) return a.to;
        }
    }
    // procedures_* 不在 SB3_T 表里（PROCCODE / ARGS / ARG1..N 一律原样）
    return key;
}

std::vector<std::string> sbcOpcodeFields(const std::string& opcode) {
    auto it = SB3_T.find(opcode);
    if (it == SB3_T.end()) return {};
    return fieldsOfTemplate(it->second);
}

const std::vector<std::tuple<std::string, std::string, std::string>>& sbcAliasTable() {
    // 惰性构造：一次生成，之后共享。调用方拿到的引用长期有效。
    static const std::vector<std::tuple<std::string, std::string, std::string>> tbl = [] {
        std::vector<std::tuple<std::string, std::string, std::string>> v;
        for (const auto& a : ALIASES)
            v.emplace_back(a.op, a.from, a.to);
        return v;
    }();
    return tbl;
}

bool sbcIsKnownOpcode(const std::string& opcode) {
    if (SB3_T.count(opcode)) return true;
    // SB2_T 也必须在白名单里：format.md §5.2 原文是"不在 SB3_T/SB2_T 表 → 报未收录"，
    // 且 block.sbcli 可能来自 .sb2 反解（项目自带 SB2 支持，见 src/sb2.cpp）。
    // SB2_T 的 key 是 Scratch 2 原生名（forward: / doRepeat / readVariable …）。
    if (SB2_T.count(opcode)) return true;
    for (const char* r : RESERVED_OPCODES) if (opcode == r) return true;
    return false;
}

std::string sbcOpcodeTemplate(const std::string& opcode) {
    auto it = SB3_T.find(opcode);
    if (it != SB3_T.end()) return it->second;
    // SB2 块也返回模板，便于报错文案显示"这个块长什么样"。
    // 注意 SB2 模板用 {__1} 而不是具名字段 —— 与 sbcOpcodeFields 对 SB2
    // 返回空集一致：SB2 块不做逐参数校验。
    auto it2 = SB2_T.find(opcode);
    return (it2 == SB2_T.end()) ? std::string() : it2->second;
}

// ================================================================ id

std::string sbcVarId(const std::string& scope, const std::string& name) {
    // 确定性哈希（FNV-1a）：同名同 scope 必得同 id。
    // 不落盘——meta 格式没有 id 字段（见 sbcli_parser.hpp 的说明），
    // 所以 fix 与 pack 各自调用本函数即天然一致，且重复运行幂等。
    std::string seed = scope + "\x1f" + name;
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : seed) { h ^= c; h *= 1099511628211ull; }
    std::ostringstream os;
    os << std::hex << h;
    std::string s = os.str();
    while (s.size() < 16) s = "0" + s;
    return s.substr(0, 16);
}

std::string sbcBroadcastId(const std::string& name) {
    // 广播是全局的，没有 scope；用固定前缀隔离，避免与变量哈希撞车
    return sbcVarId("broadcast", name);
}

// ================================================================ 值对象

SbcValuePtr SbcValue::makeScalar(SbToken t) {
    auto v = std::make_shared<SbcValue>();
    v->kind = Kind::Scalar;
    v->scalar = std::move(t);
    return v;
}
SbcValuePtr SbcValue::makeReporter(SbToken open, bool isBool) {
    auto v = std::make_shared<SbcValue>();
    v->kind = Kind::Reporter;
    v->scalar = std::move(open);   // 复用 scalar 存 opcode 文本
    v->boolean = isBool;
    return v;
}
SbcValuePtr SbcValue::makeList(std::vector<SbToken> items) {
    auto v = std::make_shared<SbcValue>();
    v->kind = Kind::List;
    v->items = std::move(items);
    return v;
}

const SbcParam* SbcValue::arg(const std::string& canonKey) const {
    for (const auto& p : args) if (p.canon == canonKey) return &p;
    return nullptr;
}

const SbcParam* SbcBlock::find(const std::string& canonKey) const {
    for (const auto& p : params) if (p.canon == canonKey) return &p;
    return nullptr;
}

// ================================================================ 语法分析

namespace {

struct Parser {
    std::vector<Line>&   L;
    std::vector<SbcDiag>& diags;
    size_t i = 0;

    Parser(std::vector<Line>& lines, std::vector<SbcDiag>& d) : L(lines), diags(d) {}

    bool      eof()      const { return i >= L.size(); }
    const Line& cur()    const { return L[i]; }
    int       curIndent()const { return L[i].indent; }

    // ---------------------------------------------------- 值

    struct TokStream {
        const std::vector<SbToken>& t;
        size_t p = 0;
        int    line = 0;
        explicit TokStream(const std::vector<SbToken>& toks, int ln) : t(toks), line(ln) {}
        const SbToken& c() const { return t[p]; }
        SbTok k() const { return p < t.size() ? t[p].type : SbTok::END; }
        const SbToken& peek() const { return (p + 1 < t.size()) ? t[p + 1] : t.back(); }
        bool  end() const { return p >= t.size() || t[p].type == SbTok::END; }
        void  adv() { if (p < t.size()) ++p; }
    };

    SbcValuePtr parseValue(TokStream& ts, int depth) {
        if (ts.end()) {
            err(diags, ts.line, 0, "E_VALUE_EXPECTED", "参数缺少值（这一行到这里就结束了）");
            SbToken dummy; dummy.line = ts.line; dummy.type = SbTok::IDENT;
            return SbcValue::makeScalar(dummy);
        }
        switch (ts.k()) {
            case SbTok::LPAREN:
            case SbTok::LANGLE:
                return parseReporter(ts, depth);
            case SbTok::LBRACKET:
                return parseList(ts);
            case SbTok::IDENT:
            case SbTok::STRING: {
                auto v = SbcValue::makeScalar(ts.c());
                // 作用域前缀：@local:名 / @global:名（变量/列表同名消歧）
                // 支持两种形态：裸名（@local:分数）与引号名（@local:"Stage: ii"）。
                // 引号形态下词法器会切出 IDENT(@local:) + STRING(名字) 两个 token，
                // 这里在 IDENT 分支窥视下一个 STRING 合并。
                if (v->kind == SbcValue::Kind::Scalar &&
                    ts.k() == SbTok::IDENT) {
                    std::string raw = v->scalar.text;
                    const std::string LC = "@local:", GC = "@global:";
                    bool isLocal = (raw.rfind(LC, 0) == 0);
                    bool isGlobal = (raw.rfind(GC, 0) == 0);
                    if (isLocal || isGlobal) {
                        std::string nm = raw.substr(isLocal ? LC.size() : GC.size());
                        // 裸前缀（@local:）后若紧跟 STRING token（词法器切开的引号名），合并
                        if (nm.empty() && ts.peek().type == SbTok::STRING) {
                            nm = ts.peek().text;
                            ts.adv();   // 吃掉 STRING
                        }
                        v->scope = isLocal ? SbcValue::Scope::Local
                                           : SbcValue::Scope::Global;
                        v->scalar.text = nm;
                        v->scalar.quoted = false;   // 名字已提取，不再当引号文本
                    }
                }
                ts.adv();
                return v;
            }
            default: {
                std::string what = ts.c().text.empty() ? "行尾" : ("“" + ts.c().text + "”");
                err(diags, ts.line, 0, "E_VALUE_EXPECTED",
                    "参数需要一个值，但遇到 " + what);
                SbToken dummy; dummy.line = ts.line; dummy.type = SbTok::IDENT;
                return SbcValue::makeScalar(dummy);   // 不推进：交给调用方收尾
            }
        }
    }

    SbcValuePtr parseList(TokStream& ts) {
        int ln = ts.line;
        ts.adv(); // [
        std::vector<SbToken> items;
        bool wantItem = true;
        while (!ts.end()) {
            if (ts.k() == SbTok::RBRACKET) { ts.adv(); return SbcValue::makeList(std::move(items)); }
            if (ts.k() == SbTok::COMMA)    { ts.adv(); wantItem = true; continue; }
            if (ts.k() == SbTok::LBRACKET || ts.k() == SbTok::LPAREN || ts.k() == SbTok::LANGLE) {
                err(diags, ts.line, 0, "E_NESTED_LIST",
                    "列表里只能放标量，不支持嵌套列表或 reporter");
                int depth = 0;
                do {
                    if (ts.k() == SbTok::LBRACKET || ts.k() == SbTok::LPAREN || ts.k() == SbTok::LANGLE) ++depth;
                    if (ts.k() == SbTok::RBRACKET || ts.k() == SbTok::RPAREN || ts.k() == SbTok::RANGLE) --depth;
                    ts.adv();
                } while (depth > 0 && !ts.end());
                wantItem = false;
                continue;
            }
            if (ts.k() == SbTok::IDENT || ts.k() == SbTok::STRING) {
                if (!wantItem) warn(diags, ts.line, 0, "W_LIST_COMMA", "列表元素之间缺少逗号");
                items.push_back(ts.c());
                ts.adv();
                wantItem = false;
                continue;
            }
            ts.adv();
        }
        err(diags, ln, 0, "E_UNCLOSED_BRACKET", "列表缺少闭合的 ]");
        return SbcValue::makeList(std::move(items));
    }

    SbcValuePtr parseReporter(TokStream& ts, int depth) {
        bool isBool = (ts.k() == SbTok::LANGLE);
        SbToken open = ts.c();
        int ln = ts.line;
        ts.adv(); // ( 或 <

        if (depth > 100) {
            err(diags, ln, 0, "E_DEEP_REPORTER", "reporter 嵌套超过 100 层（防爆）");
            return SbcValue::makeReporter(open, isBool);
        }

        auto v = SbcValue::makeReporter(open, isBool);
        SbTok close = isBool ? SbTok::RANGLE : SbTok::RPAREN;

        // 括号内第一个 token = opcode
        if (ts.k() == SbTok::IDENT || ts.k() == SbTok::OPCODE) {
            v->scalar.text = ts.c().text;
            ts.adv();
        } else if (ts.k() == SbTok::STRING) {
            err(diags, ln, 0, "E_QUOTED_OPCODE", "积木名不该加引号");
            v->scalar.text = ts.c().text;
            ts.adv();
        } else if (ts.k() == close) {
            err(diags, ln, 0, "E_EMPTY_REPORTER",
                isBool ? "空的 <>：需要一个布尔积木" : "空的 ()：需要一个 reporter 积木");
            ts.adv();
            return v;
        } else {
            err(diags, ln, 0, "E_EMPTY_REPORTER",
                isBool ? "空的 <>：需要一个布尔积木" : "空的 ()：需要一个 reporter 积木");
            while (!ts.end() && ts.k() != close) ts.adv();
            if (ts.k() == close) ts.adv();
            return v;
        }

        // 命名参数；值可以是另一个 reporter（递归配对）
        while (!ts.end()) {
            if (ts.k() == close) { ts.adv(); return v; }
            // 关错了：本层在等 close，却撞上另一种括号的闭合符。
            // 报"括号不匹配"并让外层接手，否则会误报成"参数格式不对"，
            // 用户根本看不懂。
            bool wrongClose = (ts.k() == SbTok::RANGLE && !isBool) ||
                              (ts.k() == SbTok::RPAREN && isBool);
            if (wrongClose) {
                err(diags, ts.line, 0, isBool ? "E_UNCLOSED_ANGLE" : "E_UNCLOSED_PAREN",
                    std::string("括号不匹配：") + (isBool ? "<...> 里混进了 )"
                                                         : "(... 里混进了 >"));
                return v;
            }
            if (ts.k() != SbTok::PARAM) {
                err(diags, ts.line, 0, "E_PARAM_EXPECTED",
                    "积木参数要写成 KEY=值，遇到“" + ts.c().text + "”");
                ts.adv();
                continue;
            }
            SbcParam prm;
            prm.key  = ts.c().text;
            prm.line = ts.line;
            ts.adv();
            prm.canon = sbcCanonKey(v->scalar.text, prm.key);
            prm.value = parseValue(ts, depth + 1);
            v->args.push_back(std::move(prm));
        }

        err(diags, ln, 0, isBool ? "E_UNCLOSED_ANGLE" : "E_UNCLOSED_PAREN",
            isBool ? "条件 <...> 缺少闭合的 >" : "reporter (...) 缺少闭合的 )");
        return v;
    }

    void parseParams(TokStream& ts, const std::string& opcode,
                     std::vector<SbcParam>& out) {
        while (!ts.end()) {
            if (ts.k() != SbTok::PARAM) {
                err(diags, ts.line, 0, "E_PARAM_EXPECTED",
                    "积木参数要写成 KEY=值，遇到“" + ts.c().text + "”");
                ts.adv();
                continue;
            }
            SbcParam prm;
            prm.key  = ts.c().text;
            prm.line = ts.line;
            ts.adv();
            prm.canon = sbcCanonKey(opcode, prm.key);
            prm.value = parseValue(ts, 0);
            out.push_back(std::move(prm));
        }
    }

    // ---------------------------------------------------- 块与缩进栈

    // 解析一组同缩进的连续块行。base = 这一层的缩进列数。
    // 遇到缩进更浅的行、@script、else、或文件结束就返回。
    std::vector<SbcBlock> parseStack(int base) {
        std::vector<SbcBlock> out;
        while (!eof()) {
            const Line& ln = cur();

            if (ln.toks.empty()) { ++i; continue; }              // 空行/纯注释
            if (ln.toks[0].type == SbTok::COMMENT) { ++i; continue; }
            if (ln.toks[0].type == SbTok::DIRECTIVE) return out;  // 下一个脚本
            if (ln.toks[0].type == SbTok::ELSE)      return out;  // else 交给上层

            if (ln.indent < base) return out;                     // 缩进变浅，本层结束

            if (ln.indent > base) {
                // 缩进比本层深，但不是任何块的子栈（例如连续两级跳）
                err(diags, ln.no, 0, "E_BAD_INDENT",
                    "缩进不对：这一行比所在层多缩进了 " +
                    std::to_string(ln.indent - base) + " 格");
                // 仍然解析它，尽量不丢内容
            }

            if (ln.toks[0].type != SbTok::OPCODE) {
                err(diags, ln.no, 0, "E_NOT_A_BLOCK",
                    "这一行不是积木（应以 opcode 开头），遇到“" + ln.toks[0].text + "”");
                ++i;
                continue;
            }

            SbcBlock blk;
            blk.opcode = ln.toks[0].text;
            blk.line   = ln.no;

            TokStream ts(ln.toks, ln.no);
            ts.adv();                       // 吃掉 opcode
            parseParams(ts, blk.opcode, blk.params);
            ++i;                            // 本行结束

            collectSubstacks(blk, base);
            out.push_back(std::move(blk));
        }
        return out;
    }

    void collectSubstacks(SbcBlock& blk, int base) {
        int childIndent = eof() ? 0 : cur().indent;
        bool hasChild = !eof() && !cur().toks.empty() &&
                        cur().toks[0].type == SbTok::OPCODE && childIndent > base;

        if (blk.opcode == "control_if_else") {
            // 第一分支（"那么"）
            if (hasChild) blk.substacks.push_back(parseStack(childIndent));
            else          blk.substacks.emplace_back();

            // else 必须与 control_if_else 同级，且属于它
            if (!eof() && !cur().toks.empty() && cur().toks[0].type == SbTok::ELSE) {
                const Line& el = cur();
                if (el.indent != base) {
                    err(diags, el.no, 0, "E_ELSE_INDENT",
                        "else 要和 control_if_else 对齐（应缩进 " +
                        std::to_string(base) + " 格，这里是 " +
                        std::to_string(el.indent) + " 格）");
                }
                TokStream ts(el.toks, el.no);
                ts.adv();   // else
                if (!ts.end()) {
                    if (ts.k() == SbTok::PARAM) {
                        err(diags, el.no, 0, "E_ELSE_COND",
                            "else 后面不能带参数（Scratch 没有 else if；"
                            "要多分支就在分支里再嵌一个 control_if_else）");
                    } else {
                        err(diags, el.no, 0, "E_ELSE_TRAILING",
                            "else 行后面不能有多余内容");
                    }
                }
                blk.elseLine = el.no;
                ++i;
                int elseChild = eof() ? 0 : cur().indent;
                if (!eof() && !cur().toks.empty() &&
                    cur().toks[0].type == SbTok::OPCODE && elseChild > base) {
                    blk.substacks.push_back(parseStack(elseChild));
                } else {
                    blk.substacks.emplace_back();
                }
            } else if (blk.substacks.size() > 1) {
                blk.substacks.resize(1);
            }
            return;
        }

        if (hasChild) blk.substacks.push_back(parseStack(childIndent));
    }

    // ---------------------------------------------------- @script

    SbcScript parseScriptHeader(const Line& ln) {
        SbcScript sc;
        sc.line = ln.no;
        // 第一个空格切分：@script 后第一个 token = 帽子类型，剩余整行（trim）= 参数
        std::string rest = ln.raw;                       // 已去掉缩进和注释
        size_t sp = rest.find('@');
        std::string after = (sp == std::string::npos) ? rest : rest.substr(sp);
        // 去掉 "@script" 本身
        size_t cut = 0;
        while (cut < after.size() && !isSpaceCh(after[cut])) ++cut;
        std::string tail = trim(after.substr(cut));      // 帽子类型 + 参数
        size_t s2 = tail.find_first_of(" \t");
        if (s2 == std::string::npos) {
            sc.hat    = tail;
            sc.hatArg.clear();
        } else {
            sc.hat    = tail.substr(0, s2);
            sc.hatArg = trim(tail.substr(s2));
        }
        if (sc.hat.empty()) {
            err(diags, ln.no, 0, "E_HAT_MISSING", "@script 后面要写帽子类型");
            sc.hat = "other";
        } else if (sc.hat != "flag" && sc.hat != "broadcast" && sc.hat != "key" &&
                   sc.hat != "clone" && sc.hat != "click") {
            err(diags, ln.no, 0, "E_HAT_UNKNOWN",
                "未知帽子类型“" + sc.hat + "”；只能是 flag / broadcast / key / clone / click");
        } else if (sc.hatArg.empty() &&
                   (sc.hat == "broadcast" || sc.hat == "key")) {
            err(diags, ln.no, 0, "E_HAT_ARG_MISSING",
                "@script " + sc.hat + " 后面要写名字（如 @script " + sc.hat +
                (sc.hat == "key" ? " space" : " 游戏开始") + "）");
        }
        return sc;
    }

    // 只产出脚本列表；诊断直接写进 diags（引用的是调用方的 f.diags）
    std::vector<SbcScript> run() {
        std::vector<SbcScript> scripts;
        while (!eof()) {
            const Line& ln = cur();
            if (ln.toks.empty() || ln.toks[0].type == SbTok::COMMENT) { ++i; continue; }

            if (ln.toks[0].type == SbTok::DIRECTIVE) {
                if (ln.toks[0].text != "@script") {
                    err(diags, ln.no, 0, "E_DIRECTIVE_UNKNOWN",
                        "未知的指令“" + ln.toks[0].text + "”（目前只有 @script）");
                    ++i;
                    continue;
                }
                SbcScript sc = parseScriptHeader(ln);
                ++i;
                int base = eof() ? 0 : cur().indent;
                sc.blocks = parseStack(base);
                scripts.push_back(std::move(sc));
                continue;
            }

            if (ln.toks[0].type == SbTok::ELSE) {
                err(diags, ln.no, 0, "E_ELSE_ORPHAN",
                    "else 没有匹配的 control_if_else（else 必须紧跟在同一缩进的 "
                    "control_if_else 之后）");
                ++i;
                continue;
            }

            // 第一个 @script 之前就出现积木：隐式开一个 @script other
            SbcScript sc;
            sc.line  = ln.no;
            sc.hat   = "other";
            warn(diags, ln.no, 0, "W_NO_SCRIPT_HEADER",
                 "这些积木前面没有 @script，已归入一个隐式脚本（建议补上 @script 行）");
            sc.blocks = parseStack(ln.indent);
            scripts.push_back(std::move(sc));
        }
        return scripts;
    }
};

} // namespace

// ================================================================ 对外入口

std::vector<SbToken> sbcLex(const std::string& src, std::vector<SbcDiag>& diags) {
    // 行形态是主表示（见文件头注释）。这里把它摊平成 token 流，
    // 缩进挂在 NEWLINE 上（表示该 NEXT 行的缩进），末尾补 END。
    std::vector<SbToken> out;
    try {
        std::vector<Line> lines = lexLines(src, diags);
        for (size_t k = 0; k < lines.size(); ++k) {
            for (const auto& t : lines[k].toks) out.push_back(t);
            SbToken nl;
            nl.type = SbTok::NEWLINE;
            nl.line = lines[k].no;
            nl.indent = (k + 1 < lines.size()) ? lines[k + 1].indent : 0;
            out.push_back(nl);
        }
        SbToken e;
        e.type = SbTok::END;
        e.line = lines.empty() ? 1 : (int)lines.size() + 1;
        out.push_back(e);
    } catch (const std::exception& e) {
        err(diags, 0, 0, "E_LEX", std::string("词法分析异常：") + e.what());
    }
    return out;
}

SbcFile sbcParse(const std::string& src) {
    SbcFile f;
    try {
        // 去掉 UTF-8 BOM
        size_t off = (src.size() >= 3 && (unsigned char)src[0] == 0xEF &&
                      (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) ? 3 : 0;
        // 诊断容器是同一个 f.diags：词法与语法的错误都往里追加，不会互相覆盖
        std::vector<Line> lines = lexLines(src.substr(off), f.diags);
        Parser ps(lines, f.diags);
        f.scripts = ps.run();
    } catch (const std::exception& e) {
        err(f.diags, 0, 0, "E_PARSE", std::string("语法分析异常：") + e.what());
    }
    return f;
}

SbcFile sbcParseFile(const std::string& path) {
    SbcFile f;
    // 与 src/common.cpp 一致：内部统一 UTF-8，Windows 下必须转 wide 才能打开中文路径
#ifdef _WIN32
    auto openStream = [&](std::ifstream& in) {
        int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
        if (n > 0) {
            std::wstring w((size_t)n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &w[0], n);
            // MinGW 的 ifstream 没有 wstring 重载，但接受 const wchar_t*
            in.open(w.c_str(), std::ios::binary);
        } else {
            in.open(path, std::ios::binary);
        }
    };
#else
    auto openStream = [&](std::ifstream& in) { in.open(path, std::ios::binary); };
#endif
    std::ifstream in;
    openStream(in);
    if (!in) {
        err(f.diags, 0, 0, "E_FILE", "打不开文件：" + path);
        return f;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return sbcParse(ss.str());
}

bool SbcFile::ok() const {
    for (const auto& d : diags) if (d.isError()) return false;
    return true;
}

std::string SbcFile::report() const {
    std::ostringstream os;
    for (const auto& d : diags) {
        os << (d.line > 0 ? ("第 " + std::to_string(d.line) + " 行") : "文件");
        os << ": " << (d.isError() ? "错误" : "警告") << " [" << d.code << "] "
           << d.message << "\n";
    }
    return os.str();
}

// ================================================================ 重建

std::string sbcEscape(const std::string& raw) {
    std::string o;
    o.reserve(raw.size() + 8);
    for (char c : raw) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\t': o += "\\t";  break;
            case '\r': o += "\\r";  break;
            default:   o += c;      break;
        }
    }
    return o;
}

bool sbcNeedsQuotes(const std::string& text) {
    // 空串必须加引号，否则参数值的位置就是空的
    if (text.empty()) return true;
    // 含空白或分隔符：不加引号就会被切成多个 token
    for (char c : text) {
        if (isSpaceCh(c)) return true;
        if (isSpecial(c)) return true;
    }
    // 数字/布尔：引号会把它从"数字常量"变成"字符串"，语义就变了。
    // 判断依据是 token 原本是否带引号（quoted），不是内容——
    // 因为 MESSAGE="123" 这种"看起来是数字其实要当文本"的情况，
    // 引号是用户明确写出来的，必须保留。
    return false;
}

namespace {

// 裸标量回写：原文带引号，或内容不加引号就会歧义（空串/含空格/含分隔符）→ 加引号
std::string scalarText(const SbToken& t) {
    if (t.quoted || sbcNeedsQuotes(t.text)) return "\"" + sbcEscape(t.text) + "\"";
    return t.text;
}

void valueText(std::ostringstream& os, const SbcValue& v) {
    switch (v.kind) {
        case SbcValue::Kind::Scalar:
            os << scalarText(v.scalar);
            break;
        case SbcValue::Kind::List: {
            os << "[";
            for (size_t k = 0; k < v.items.size(); ++k) {
                if (k) os << ", ";
                os << scalarText(v.items[k]);
            }
            os << "]";
            break;
        }
        case SbcValue::Kind::Reporter: {
            os << (v.boolean ? "<" : "(") << v.scalar.text;
            for (const auto& a : v.args) {
                os << " " << a.key << "=";
                if (a.value) valueText(os, *a.value);
                else         os << "\"\"";
            }
            os << (v.boolean ? ">" : ")");
            break;
        }
    }
}

void blockText(std::ostringstream& os, const SbcBlock& b, int indent) {
    os << std::string(indent, ' ') << b.opcode;
    for (const auto& p : b.params) {
        os << " " << p.key << "=";
        if (p.value) valueText(os, *p.value);
        else         os << "\"\"";
    }
    os << "\n";

    if (b.opcode == "control_if_else") {
        for (const auto& blk : b.substacks.size() > 0 ? b.substacks[0]
                                                      : std::vector<SbcBlock>{})
            blockText(os, blk, indent + 2);
        if (b.hasElse()) {
            os << std::string(indent, ' ') << "else\n";
            for (const auto& blk : b.substacks.size() > 1 ? b.substacks[1]
                                                          : std::vector<SbcBlock>{})
                blockText(os, blk, indent + 2);
        }
        return;
    }
    for (const auto& ss : b.substacks)
        for (const auto& blk : ss)
            blockText(os, blk, indent + 2);
}

} // namespace

std::string sbcUnparse(const SbcFile& f) {
    std::ostringstream os;
    for (size_t k = 0; k < f.scripts.size(); ++k) {
        const SbcScript& s = f.scripts[k];
        if (k) os << "\n";
        os << "@script " << s.hat;
        if (!s.hatArg.empty()) os << " " << s.hatArg;
        os << "\n";
        for (const auto& b : s.blocks) blockText(os, b, 0);
    }
    return os.str();
}

std::string sbcTokensToText(const std::vector<SbToken>& toks) {
    std::ostringstream os;
    for (size_t k = 0; k < toks.size(); ++k) {
        const SbToken& t = toks[k];
        if (k) os << " ";
        switch (t.type) {
            case SbTok::IDENT:     os << scalarText(t);      break;
            case SbTok::STRING:    os << "\"" << sbcEscape(t.text) << "\""; break;
            case SbTok::LPAREN:    os << "(";  break;
            case SbTok::RPAREN:    os << ")";  break;
            case SbTok::LANGLE:    os << "<";  break;
            case SbTok::RANGLE:    os << ">";  break;
            case SbTok::LBRACKET:  os << "[";  break;
            case SbTok::RBRACKET:  os << "]";  break;
            case SbTok::COMMA:     os << ",";  break;
            case SbTok::PARAM:     os << t.text << "="; break;
            case SbTok::OPCODE:    os << t.text; break;
            case SbTok::ELSE:      os << "else"; break;
            case SbTok::DIRECTIVE: os << t.text; break;
            case SbTok::COMMENT:   os << "#" << t.text; break;
            case SbTok::NEWLINE:   os << "\n" << std::string(t.indent, ' '); break;
            case SbTok::END:       break;
        }
    }
    return os.str();
}

} // namespace sb
