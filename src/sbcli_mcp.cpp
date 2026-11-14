// src/sbcli_mcp.cpp —— MCP server 实现（stdio JSON-RPC 2.0，行模式）
//
// 铁律：stdout 只能有 JSON。所有命令的输出都被捕获后塞进 tool 结果的
// text 字段；日志/告警仍然走 stderr，绝不污染协议流。
//
// 工具实现保持"薄"：能直接调返回结构的 API 就调 API
// （sbcliCheck / sbcEdit* / sbcRename / sbSearch / sbcliUnpack），
// 只有 cmd_info / cmd_script / cmd_pack 这种直接打印的才用 RAII 捕获 std::cout。
#include "sbcli_mcp.hpp"

#include "sbcli_check.hpp"
#include "sbcli_edit.hpp"
#include "sbcli_pack.hpp"
#include "sbcli_search.hpp"
#include "sbcli_unpack.hpp"
#include "common.hpp"
#include "jdoc.hpp"

#include <iostream>
#include <sstream>

namespace sb {
namespace {

using json = Json;

// ---------------------------------------------------------------------------
// std::cout 捕获（RAII）：把 rdbuf 换成 ostringstream，析构时必然还原。
// 用于复用那些「解析 + 直接打印」的 cmd_*（sb_info / sb_script / sb_pack）。
// ---------------------------------------------------------------------------
class CoutCapture {
public:
    CoutCapture() {
        std::cout.flush();
        old_ = std::cout.rdbuf(buf_.rdbuf());
    }
    ~CoutCapture() {
        std::cout.flush();
        std::cout.rdbuf(old_);
    }
    CoutCapture(const CoutCapture&) = delete;
    CoutCapture& operator=(const CoutCapture&) = delete;

    std::string str() const { return buf_.str(); }

private:
    std::ostringstream buf_;
    std::streambuf*    old_ = nullptr;
};

// ---------------------------------------------------------------------------
// JSON 小工具
// ---------------------------------------------------------------------------

json obj() { return Json::object(); }
json arr() { return Json::array(); }

// 字符串数组 → JSON 数组
json strArray(const std::vector<std::string>& v) {
    json a = arr();
    for (const auto& s : v) a.push_back(s);
    return a;
}

// 把捕获到的文本变成"合法单行 JSON 字符串"：
// 本身能解析就直接紧凑重排（cmd_* --json 的输出），否则包一层 {ok, text}。
std::string asJsonText(const std::string& raw, bool ok) {
    if (raw.empty()) return ok ? "{}" : "{\"ok\":false}";
    try {
        json parsed = Json::parse(raw);
        return parsed.dump();     // 紧凑单行，便于塞进 content[].text
    } catch (...) {
    }
    json o = obj();
    o["ok"] = ok;
    o["text"] = raw;
    return o.dump();
}

// 保守修复"非法反斜杠转义"：Windows 路径 `"G:\文档\x.sb3"` 在 JSON 里是非法的
// （\文 不是合法转义），但人写请求时极易踩。只把**不合法的** `\` 补成 `\\`，
// 合法转义原样保留；只在严格解析失败后做一次重试，正常 JSON 不受任何影响。
bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
std::string repairBackslashes(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\') { out += c; continue; }
        if (i + 1 >= s.size()) { out += "\\\\"; continue; }
        char n = s[i + 1];
        bool okEsc = (n == '"' || n == '\\' || n == '/' || n == 'b' ||
                      n == 'f' || n == 'n' || n == 'r' || n == 't');
        bool okU = (n == 'u' && i + 5 < s.size() &&
                    isHex(s[i + 2]) && isHex(s[i + 3]) &&
                    isHex(s[i + 4]) && isHex(s[i + 5]));
        if (okEsc) { out += c; out += n; ++i; }
        else if (okU) { out.append(s, i, 6); i += 5; }
        else { out += "\\\\"; }   // 非法转义：把这个 \ 本身转义
    }
    return out;
}

// 解析一行请求：先严格解析，失败再试一次反斜杠修复。
bool parseRequest(const std::string& line, json& out) {
    try {
        out = Json::parse(line);
        return true;
    } catch (...) {
    }
    try {
        out = Json::parse(repairBackslashes(line));
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// 参数取值
// ---------------------------------------------------------------------------

// 注意：本项目 Json 没有 at()，只有 contains()/find()/operator[]；
// 这里先 contains 再下标，避免 operator[] 在缺键时抛 type_error。
const json* argOf(const json& a, const char* key) {
    if (!a.is_object() || !a.contains(key)) return nullptr;
    return &a[key];
}
std::string argStr(const json& a, const char* key) {
    const json* v = argOf(a, key);
    return (v && v->is_string()) ? v->get<std::string>() : std::string();
}
int argInt(const json& a, const char* key, int def = 0) {
    const json* v = argOf(a, key);
    if (!v) return def;
    if (v->is_number_integer()) return v->get<int>();
    if (v->is_string()) {
        try { return std::stoi(v->get<std::string>()); } catch (...) {}
    }
    return def;
}
bool argBool(const json& a, const char* key, bool def = false) {
    const json* v = argOf(a, key);
    return (v && v->is_boolean()) ? v->get<bool>() : def;
}

// ---------------------------------------------------------------------------
// tools/list —— 8 个工具 + JSON Schema
// ---------------------------------------------------------------------------

json toolsList() {
    auto prop = [](const char* type, const char* desc) {
        json p = obj();
        p["type"] = type;
        p["description"] = desc;
        return p;
    };
    auto schema = [&](json props, json required) {
        json s = obj();
        s["type"] = "object";
        s["properties"] = std::move(props);
        s["required"] = std::move(required);
        return s;
    };

    json tools = arr();
    auto add = [&](const char* name, const char* desc, json schema) {
        json t = obj();
        t["name"] = name;
        t["description"] = desc;
        t["inputSchema"] = std::move(schema);
        tools.push_back(std::move(t));
    };

    {
        json p = obj();
        p["file"] = prop("string", "Scratch 作品路径（.sb3/.sb2/.sb3/.sprite3 或 sbcli 项目目录）");
        add("sb_info", "读取作品概要：格式、目标（舞台/角色）、造型/声音/积木数、工程信息",
            schema(p, {"file"}));
    }
    {
        json p = obj(), req = arr();
        p["file"] = prop("string", "作品文件路径或 sbcli 项目目录");
        p["sprite"] = prop("string", "只看这个角色（可选）");
        req.push_back("file");
        add("sb_script", "把积木脚本翻译成可读文字（按角色分组）", schema(p, req));
    }
    {
        json p = obj();
        p["project"] = prop("string", "sbcli 项目目录");
        add("sb_check", "检查项目脚本的语法/结构/引用问题，返回诊断列表", schema(p, {"project"}));
    }
    {
        json p = obj(), req = arr();
        p["sb3"] = prop("string", "输入的 .sb3 文件");
        p["outDir"] = prop("string", "输出项目目录");
        p["force"] = prop("boolean", "输出目录非空时覆盖（可选，默认 false）");
        req.push_back("sb3");
        req.push_back("outDir");
        add("sb_unpack", "把 .sb3 拆成 sbcli 项目目录（pack 的逆操作）", schema(p, req));
    }
    {
        json p = obj();
        p["projectDir"] = prop("string", "sbcli 项目目录");
        p["outSb3"] = prop("string", "输出的 .sb3 路径");
        add("sb_pack", "把 sbcli 项目目录打包成 .sb3", schema(p, {"projectDir", "outSb3"}));
    }
    {
        json p = obj(), req = arr();
        p["project"] = prop("string", "sbcli 项目目录");
        p["sprite"] = prop("string", "角色名或 id（可选）");
        p["path"] = prop("string", "子栈路径，如 1.2.0（脚本1.第2块.子栈0）");
        p["scriptIndex"] = prop("integer", "脚本序号（1 起），未给 path 时必填");
        p["op"] = prop("string", "操作：insert / delete / replace");
        p["block"] = prop("integer", "块序号（1 起；delete/replace 用）");
        p["text"] = prop("string", "积木文本，如 looks_say MESSAGE=你好（insert/replace 用）");
        p["after"] = prop("integer", "插入位置（0=最前，省略=末尾）");
        req.push_back("project");
        req.push_back("op");
        add("sb_edit", "脚本级编辑：插入/删除/替换积木，可定位到 if/else 子栈内部", schema(p, req));
    }
    {
        json p = obj(), req = arr();
        p["project"] = prop("string", "sbcli 项目目录");
        p["kind"] = prop("string", "类型：variable / list / broadcast / sprite");
        p["oldName"] = prop("string", "旧名");
        p["newName"] = prop("string", "新名");
        req.push_back("project");
        req.push_back("kind");
        req.push_back("oldName");
        req.push_back("newName");
        add("sb_rename", "重命名变量/列表/广播/角色，并同步改写所有引用与 meta 声明",
            schema(p, req));
    }
    {
        json p = obj();
        p["query"] = prop("string", "中文概念或英文 opcode，如 广播 / looks_say");
        add("sb_search", "查积木语法手册：模板 + 参数清单 + 示例", schema(p, {"query"}));
    }
    return tools;
}

// ---------------------------------------------------------------------------
// tools/call
// ---------------------------------------------------------------------------

struct ToolOut {
    bool        isError = false;
    std::string text;
};

ToolOut fail(const std::string& msg) {
    ToolOut o;
    o.isError = true;
    json j = obj();
    j["ok"] = false;
    j["error"] = msg;
    o.text = j.dump();
    return o;
}

// 走 cmd_* 的通用外壳：捕获 stdout，rc 非 0 视为错误。
ToolOut viaCmd(const std::string& name, Args& a) {
    CoutCapture cap;
    int rc = 0;
    if      (name == "sb_info")   rc = cmd_info(a);
    else if (name == "sb_script") rc = cmd_script(a);
    else if (name == "sb_pack")   rc = cmd_pack(a);
    ToolOut o;
    std::string text = cap.str();
    o.isError = (rc != 0);
    o.text = asJsonText(text, !o.isError);
    if (o.isError && text.empty()) {
        json j = obj();
        j["ok"] = false;
        j["error"] = name + " 失败（退出码 " + std::to_string(rc) + "）";
        o.text = j.dump();
    }
    return o;
}

ToolOut runInfo(const json& args) {
    Args a;
    a.cmd = "info";
    a.file = argStr(args, "file");
    if (a.file.empty()) return fail("缺少参数 file");
    a.json = true;   // 让 cmd_info 直接输出 JSON
    return viaCmd("sb_info", a);
}

ToolOut runScript(const json& args) {
    Args a;
    a.cmd = "script";
    a.file = argStr(args, "file");
    if (a.file.empty()) return fail("缺少参数 file");
    a.sprite = argStr(args, "sprite");
    a.json = true;
    return viaCmd("sb_script", a);
}

ToolOut runCheck(const json& args) {
    std::string project = argStr(args, "project");
    if (project.empty()) return fail("缺少参数 project");

    CheckReport r = sbcliCheck(project);
    json j = obj();
    j["ok"] = r.error.empty() && r.errors == 0;
    j["root"] = r.root;
    j["rootMetaFound"] = r.rootMetaFound;
    j["errors"] = r.errors;
    j["warnings"] = r.warnings;
    j["checked"] = (long long)r.files.size();
    if (!r.error.empty()) j["error"] = r.error;

    json ds = arr();
    for (const auto& d : r.diags) {
        json o = obj();
        o["file"] = d.file;
        o["line"] = d.line;
        o["col"] = d.col;
        o["level"] = (d.level == CheckLevel::Error) ? "error" : "warning";
        o["category"] = d.category;
        o["code"] = d.code;
        o["message"] = d.message;
        ds.push_back(std::move(o));
    }
    j["diagnostics"] = std::move(ds);

    ToolOut o;
    o.text = j.dump();
    o.isError = !r.error.empty();   // 诊断本身不算工具错误（与 check 命令退出码一致的是 errors>0）
    return o;
}

ToolOut runUnpack(const json& args) {
    std::string in  = argStr(args, "sb3");
    std::string out = argStr(args, "outDir");
    if (in.empty())  return fail("缺少参数 sb3");
    if (out.empty()) return fail("缺少参数 outDir");

    UnpackResult r = sbcliUnpack(in, out, argBool(args, "force"));
    json j = obj();
    j["ok"] = r.ok;
    if (!r.error.empty()) j["error"] = r.error;
    j["outDir"] = r.outDir;
    j["project"] = r.projectName;
    j["sprites"] = r.spriteCount;
    j["scripts"] = r.scriptCount;
    j["assets"] = r.assetCount;
    j["unknownBlocks"] = r.unknownBlocks;

    ToolOut o;
    o.text = j.dump();
    o.isError = !r.ok;
    return o;
}

ToolOut runPack(const json& args) {
    std::string dir = argStr(args, "projectDir");
    std::string out = argStr(args, "outSb3");
    if (dir.empty()) return fail("缺少参数 projectDir");
    if (out.empty()) return fail("缺少参数 outSb3");

    Args a;
    a.cmd = "pack";
    a.file = dir;
    a.extra.push_back(out);
    // cmd_pack 直接打印结果，没有返回结构的 API（见 sbcli_pack.cpp）
    ToolOut o = viaCmd("sb_pack", a);
    if (!o.isError) {
        json j = obj();
        j["ok"] = true;
        j["outSb3"] = out;
        j["output"] = o.text;
        o.text = j.dump();
    }
    return o;
}

ToolOut runEdit(const json& args) {
    std::string project = argStr(args, "project");
    std::string op      = argStr(args, "op");
    if (project.empty()) return fail("缺少参数 project");
    if (op.empty())      return fail("缺少参数 op（insert / delete / replace）");

    std::string sprite = argStr(args, "sprite");
    std::string path   = argStr(args, "path");
    std::string text   = argStr(args, "text");
    int scriptIndex    = argInt(args, "scriptIndex", -1);
    int block          = argInt(args, "block", -1);
    int after          = argInt(args, "after", -1);
    if (!path.empty() && after < 0) after = -1;

    EditResult r;
    if (op == "insert") {
        if (text.empty()) return fail("insert 需要参数 text");
        r = path.empty() ? sbcEditInsert(project, sprite, scriptIndex,
                                         after < 0 ? 0 : after, text)
                         : sbcEditInsertAt(project, sprite, path, text, after);
    } else if (op == "delete") {
        r = path.empty() ? sbcEditDelete(project, sprite, scriptIndex, block)
                         : sbcEditDeleteAt(project, sprite, path);
    } else if (op == "replace") {
        if (text.empty()) return fail("replace 需要参数 text");
        r = path.empty() ? sbcEditReplace(project, sprite, scriptIndex, block, text)
                         : sbcEditReplaceAt(project, sprite, path, text);
    } else {
        return fail("未知 op：" + op + "（只支持 insert / delete / replace）");
    }

    json j = obj();
    j["ok"] = r.ok;
    if (!r.error.empty()) j["error"] = r.error;
    j["changed"] = r.changed;
    j["notes"] = strArray(r.notes);

    ToolOut o;
    o.text = j.dump();
    o.isError = !r.ok;
    return o;
}

ToolOut runRename(const json& args) {
    std::string project = argStr(args, "project");
    std::string kind    = argStr(args, "kind");
    std::string oldName = argStr(args, "oldName");
    std::string newName = argStr(args, "newName");
    if (project.empty()) return fail("缺少参数 project");
    if (kind.empty())    return fail("缺少参数 kind");
    if (oldName.empty()) return fail("缺少参数 oldName");
    if (newName.empty()) return fail("缺少参数 newName");

    EditResult r = sbcRename(project, kind, oldName, newName);
    json j = obj();
    j["ok"] = r.ok;
    if (!r.error.empty()) j["error"] = r.error;
    j["changed"] = r.changed;
    j["notes"] = strArray(r.notes);

    ToolOut o;
    o.text = j.dump();
    o.isError = !r.ok;
    return o;
}

ToolOut runSearch(const json& args) {
    std::string query = argStr(args, "query");
    if (query.empty()) return fail("缺少参数 query");

    SearchResult r = sbSearch(query, argStr(args, "extDir"));
    json j = obj();
    j["ok"] = !r.matches.empty();
    j["keyword"] = r.keyword;
    j["count"] = (int)r.matches.size();
    json ms = arr();
    for (const auto& m : r.matches) {
        json o = obj();
        o["opcode"] = m.opcode;
        o["category"] = m.category;
        o["template"] = m.templateText;
        o["example"] = m.example;
        json ps = arr();
        for (const auto& p : m.params) {
            json pp = obj();
            pp["name"] = p.name;
            pp["type"] = p.type;
            ps.push_back(std::move(pp));
        }
        o["params"] = std::move(ps);
        ms.push_back(std::move(o));
    }
    j["matches"] = std::move(ms);
    j["suggestions"] = strArray(r.suggestions);

    ToolOut o;
    o.text = j.dump();
    return o;   // 「没搜到」不是工具错误
}

ToolOut runTool(const std::string& name, const json& args) {
    if (name == "sb_info")   return runInfo(args);
    if (name == "sb_script") return runScript(args);
    if (name == "sb_check")  return runCheck(args);
    if (name == "sb_unpack") return runUnpack(args);
    if (name == "sb_pack")   return runPack(args);
    if (name == "sb_edit")   return runEdit(args);
    if (name == "sb_rename") return runRename(args);
    if (name == "sb_search") return runSearch(args);
    return fail("未知工具：" + name);
}

// ---------------------------------------------------------------------------
// JSON-RPC 响应
// ---------------------------------------------------------------------------

json errorResp(const json& id, int code, const char* msg) {
    json r = obj();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    json e = obj();
    e["code"] = code;
    e["message"] = msg;
    r["error"] = std::move(e);
    return r;
}

// 是不是传输层的头行（Content-Length 模式的头）：跳过即可。
bool isHeaderLine(const std::string& s) {
    static const char* kHeads[] = { "Content-Length:", "Content-Type:" };
    for (const char* h : kHeads) {
        size_t n = std::char_traits<char>::length(h);
        if (s.size() >= n) {
            bool same = true;
            for (size_t i = 0; i < n; ++i) {
                char a = s[i], b = h[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) { same = false; break; }
            }
            if (same) return true;
        }
    }
    return false;
}

} // namespace

std::string mcpHandleLine(const std::string& rawLine) {
    std::string line = rawLine;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t'))
        line.pop_back();

    if (line.empty() || isHeaderLine(line)) return {};   // 头行/空行：不是请求

    json req;
    if (!parseRequest(line, req)) {
        return errorResp(json(nullptr), -32700, "Parse error").dump();
    }
    if (!req.is_object()) return errorResp(json(nullptr), -32600, "Invalid Request").dump();

    bool hasId = req.contains("id");
    json id = hasId ? req["id"] : json(nullptr);

    std::string method = req.value("method", std::string());
    json params = req.contains("params") ? req["params"] : obj();

    // JSON-RPC 2.0：没有 id 的通知不回应（MCP 的 initialized 就属于这一类）
    if (!hasId || method.rfind("notifications/", 0) == 0)
        return {};

    json resp = obj();
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;

    if (method == "initialize") {
        json result = obj();
        result["protocolVersion"] = "2024-11-05";
        json caps = obj();
        caps["tools"] = obj();
        result["capabilities"] = std::move(caps);
        json si = obj();
        si["name"] = "sbcli";
        si["version"] = VERSION;
        result["serverInfo"] = std::move(si);
        resp["result"] = std::move(result);
    } else if (method == "tools/list") {
        json result = obj();
        result["tools"] = toolsList();
        resp["result"] = std::move(result);
    } else if (method == "tools/call") {
        std::string name = params.value("name", std::string());
        json args = params.contains("arguments") ? params["arguments"] : obj();

        ToolOut o;
        try {
            o = runTool(name, args);
        } catch (const std::exception& e) {
            o = fail(e.what());
        }

        json content = arr();
        json item = obj();
        item["type"] = "text";
        item["text"] = o.text;
        content.push_back(std::move(item));

        json result = obj();
        result["content"] = std::move(content);
        result["isError"] = o.isError;
        resp["result"] = std::move(result);
    } else if (method == "ping") {
        resp["result"] = obj();
    } else {
        return errorResp(id, -32601, "Method not found").dump();
    }

    return resp.dump();   // dump() 默认紧凑单行
}

void mcpServe(std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        std::string resp = mcpHandleLine(line);
        if (resp.empty()) continue;
        out << resp << "\n";
        out.flush();
    }
}

int cmd_mcp(Args&) {
    mcpServe(std::cin, std::cout);
    return 0;
}

} // namespace sb
