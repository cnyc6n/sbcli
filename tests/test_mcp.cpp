// tests/test_mcp.cpp
// `sb mcp` 自测：协议握手 + 错误处理 + 8 个工具清单 + 真跑若干工具。
//
// 设计：
//   · **直接调 mcpHandleLine(line)**（字符串进、字符串出），不造子进程，
//     这样断言失败能精确定位到是哪一条协议规则坏了。
//   · 一条请求一行响应 —— 每个响应都必须能被 Json::parse 解析（协议铁律）。
//   · 工具测试全部在临时目录里搭项目，不碰真项目；素材/作品由
//     `sb project init` + `sb pack` 自己造，不依赖 tests/sb3/ 的 fixture。
//   · 退出码 0 = 全部通过。

#include "sbcli_mcp.hpp"
#include "sbcli_project.hpp"
#include "sbcli_edit.hpp"
#include "common.hpp"
#include "jdoc.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace sb;

static int g_pass = 0;
static int g_fail = 0;

static void ok(bool cond, const std::string& what) {
    if (cond) {
        ++g_pass;
        std::printf("  [通过] %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("  [失败] %s\n", what.c_str());
    }
}

// ------------------------------------------------------------------ 工具

static fs::path tempRoot() {
    std::string base = "F:/temp";
    const char* t = std::getenv("TEMP");
    if (t && *t) base = t;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();
    std::error_code ec;
    fs::create_directories(fs::u8path(base), ec);
    static int seq = 0;
    for (int i = 0; i < 8; ++i) {
        fs::path dir = fs::u8path(base) /
                       ("mcp_test_" + std::to_string(++seq) + "_" +
                        std::to_string((long long)::rand()));
        fs::remove_all(dir, ec);
        if (fs::create_directories(dir, ec)) return dir;
    }
    return fs::current_path();
}

static void writeFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// 发一条请求，返回解析后的响应（解析失败返回 null Json 并计入失败）
static Json call(const std::string& req, const std::string& tag) {
    std::string resp = mcpHandleLine(req);
    if (resp.find('\n') != std::string::npos) {
        ++g_fail;
        std::printf("  [失败] %s：响应不是单行\n", tag.c_str());
    }
    try {
        return Json::parse(resp);
    } catch (const std::exception& e) {
        ++g_fail;
        std::printf("  [失败] %s：响应不是合法 JSON（%s）\n", tag.c_str(), e.what());
        std::printf("        原文：%s\n", resp.c_str());
        return Json();
    }
}

// tools/call 的便捷封装：返回 tool 结果里的 text（已解析）
static Json callTool(const std::string& name, const std::string& argsJson,
                     const std::string& tag, bool* isErrorOut = nullptr) {
    std::string req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                      "\"params\":{\"name\":\"" + name + "\",\"arguments\":" +
                      argsJson + "}}";
    Json resp = call(req, tag);
    if (!resp.is_object() || !resp.contains("result")) return Json();

    const Json& result = resp["result"];
    if (isErrorOut) *isErrorOut = result.value("isError", true);

    const Json& content = result["content"];
    if (!content.is_array() || content.size() == 0) return Json();
    std::string text = content[size_t(0)].value("text", std::string());
    try {
        return Json::parse(text);
    } catch (const std::exception& e) {
        ++g_fail;
        std::printf("  [失败] %s：content.text 不是合法 JSON（%s）\n", tag.c_str(), e.what());
        std::printf("        原文：%s\n", text.c_str());
        return Json();
    }
}

// JSON 字符串转义（拼请求用；只处理路径里的反斜杠与中文）
static std::string jstr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    out += '"';
    return out;
}

// ==========================================================================
// 1. 握手
// ==========================================================================

static void testHandshake() {
    std::printf("\n=== 1. 协议握手 ===\n");

    Json r = call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}",
                  "initialize");
    ok(r.is_object(), "initialize 返回对象");
    ok(r.value("jsonrpc", std::string()) == "2.0", "jsonrpc = 2.0");
    ok(r.contains("id") && r["id"].is_number_integer() && r["id"].get<int>() == 1,
       "id 原样回传（1）");

    Json result = r.contains("result") ? r["result"] : Json();
    ok(result.value("protocolVersion", std::string()) == "2024-11-05",
       "protocolVersion = 2024-11-05");
    ok(result.contains("capabilities") && result["capabilities"].contains("tools"),
       "capabilities.tools 存在");
    Json si = result.contains("serverInfo") ? result["serverInfo"] : Json();
    ok(si.value("name", std::string()) == "sbcli", "serverInfo.name = sbcli");
    ok(si.value("version", std::string()) == VERSION,
       std::string("serverInfo.version = ") + VERSION);

    // 无 id 的通知不响应（JSON-RPC 2.0 / MCP 的 initialized 都属这一类）
    ok(mcpHandleLine("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}").empty(),
       "通知（无 id）不产生响应");
    ok(mcpHandleLine("").empty(), "空行不产生响应");
}

// ==========================================================================
// 2. 错误处理
// ==========================================================================

static void testErrors() {
    std::printf("\n=== 2. 错误处理 ===\n");

    Json r = call("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"no/such/method\"}", "未知方法");
    ok(r.contains("error"), "未知方法返回 error");
    ok(r["error"].value("code", 0) == -32601, "错误码 -32601（Method not found）");
    ok(r.value("jsonrpc", std::string()) == "2.0", "错误响应也带 jsonrpc");
    ok(!r.contains("result"), "错误响应不含 result");

    Json bad = call("not json at all", "非法 JSON");
    ok(bad.contains("error") && bad["error"].value("code", 0) == -32700,
       "非法 JSON → -32700（Parse error）");

    // 未知工具不是协议错误，而是 tool 级别的 isError
    bool isErr = false;
    Json t = callTool("sb_not_a_tool", "{}", "未知工具", &isErr);
    ok(isErr, "未知工具 isError = true");
    ok(t.is_object() && !t.value("ok", true), "未知工具返回 ok=false");
}

// ==========================================================================
// 3. tools/list
// ==========================================================================

static void testToolsList() {
    std::printf("\n=== 3. tools/list ===\n");

    Json r = call("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}", "tools/list");
    ok(r.contains("result"), "tools/list 有 result");
    Json tools = r["result"].contains("tools") ? r["result"]["tools"] : Json();
    ok(tools.is_array(), "tools 是数组");
    ok(tools.size() == 8, "正好 8 个工具（实际 " + std::to_string(tools.size()) + "）");

    const char* want[] = { "sb_info", "sb_script", "sb_check", "sb_unpack",
                           "sb_pack", "sb_edit", "sb_rename", "sb_search" };
    std::vector<std::string> got;
    for (size_t i = 0; i < tools.size(); ++i) {
        const Json& t = tools[i];
        got.push_back(t.value("name", std::string()));
        std::string nm = t.value("name", std::string());
        ok(!t.value("description", std::string()).empty(), nm + " 有 description");

        Json sc = t.contains("inputSchema") ? t["inputSchema"] : Json();
        ok(sc.value("type", std::string()) == "object", nm + " inputSchema.type = object");
        ok(sc.contains("properties") && sc["properties"].is_object(),
           nm + " inputSchema.properties 是对象");
        ok(sc.contains("required") && sc["required"].is_array(), nm + " 有 required 数组");
    }
    for (const char* w : want) {
        bool found = false;
        for (const auto& g : got) if (g == w) found = true;
        ok(found, std::string("工具清单包含 ") + w);
    }
}

// ==========================================================================
// 4. 工具：sb_search（不需要磁盘）
// ==========================================================================

static void testSearch() {
    std::printf("\n=== 4. sb_search ===\n");

    Json r = callTool("sb_search", "{\"query\":\"广播\"}", "sb_search 广播");
    ok(r.is_object() && r.value("ok", false), "sb_search ok = true");
    ok(r.value("count", 0) >= 1, "搜「广播」至少 1 条结果");
    Json ms = r.contains("matches") ? r["matches"] : Json();
    bool hasBroadcast = false;
    for (size_t i = 0; i < ms.size(); ++i)
        if (ms[i].value("opcode", std::string()) == "event_broadcast") hasBroadcast = true;
    ok(hasBroadcast, "结果含 event_broadcast");

    Json r2 = callTool("sb_search", "{\"query\":\"looks_say\"}", "sb_search looks_say");
    ok(r2.value("count", 0) >= 1, "搜 looks_say 有结果");

    Json r3 = callTool("sb_search", "{\"query\":\"绝不可能存在的关键词zzz\"}", "sb_search 无结果");
    bool isErr = false;
    Json r4 = callTool("sb_search", "{\"query\":\"zzz\"}", "sb_search 空参", &isErr);
    (void)r3; (void)r4;
    ok(!isErr, "「搜不到」不算工具错误（isError=false）");

    // 缺必需参数 → 工具级错误
    Json miss = callTool("sb_search", "{}", "sb_search 缺参", &isErr);
    ok(isErr, "缺 query 参数 isError = true");
    ok(!miss.value("ok", true), "缺 query 参数 ok = false");
}

// ==========================================================================
// 5. 工具：sb_check
// ==========================================================================

static void testCheck(const fs::path& tmp) {
    std::printf("\n=== 5. sb_check ===\n");

    // 5a. 正常项目
    fs::path good = tmp / "proj_ok";
    ProjResult init = sbcliProjectInit(good.string(), "mcp测试");
    ok(init.ok, "sb project init 成功");
    ok(sbcliAddSprite(good.string(), "玩家").ok, "add-sprite 成功");
    writeFile(good / "character" / "1" / "block.sbcli",
              "@script flag\n"
              "event_whenflagclicked\n"
              "looks_say MESSAGE=你好\n");

    Json r = callTool("sb_check", "{\"project\":" + jstr(good.string()) + "}", "sb_check 正常");
    ok(r.is_object() && r.value("ok", false), "正常项目 ok = true");
    ok(r.value("errors", -1) == 0, "正常项目 errors = 0");

    // 5b. 故意写错 opcode → 诊断列表里有 unknown-opcode
    fs::path bad = tmp / "proj_bad";
    sbcliProjectInit(bad.string(), "mcp坏项目");
    sbcliAddSprite(bad.string(), "玩家");
    writeFile(bad / "character" / "1" / "block.sbcli",
              "@script flag\n"
              "totally_unknown_opcode_xyz FOOBAR=1\n");

    bool isErr = false;
    Json rb = callTool("sb_check", "{\"project\":" + jstr(bad.string()) + "}",
                       "sb_check 报错项目", &isErr);
    ok(!isErr, "诊断失败不是工具错误（isError=false，检查本身成功）");
    ok(!rb.value("ok", true), "有错误的项目 ok = false");
    ok(rb.value("errors", 0) >= 1, "有错误的项目 errors >= 1");
    Json ds = rb.contains("diagnostics") ? rb["diagnostics"] : Json();
    bool foundCode = false;
    for (size_t i = 0; i < ds.size(); ++i) {
        std::string code = ds[i].value("code", std::string());
        if (code == "unknown-opcode") foundCode = true;
        ok(ds[i].contains("level") && ds[i].contains("file") && ds[i].contains("message"),
           "诊断项含 level/file/message 字段");
    }
    ok(foundCode, "诊断里有 unknown-opcode");

    // 5c. 项目不存在 → 报错
    Json rn = callTool("sb_check", "{\"project\":" + jstr((tmp / "no_such_dir").string()) + "}",
                       "sb_check 不存在的目录", &isErr);
    ok(isErr, "不存在的项目 isError = true");
    ok(rn.is_object() && !rn.value("ok", true), "不存在的项目 ok = false");
}

// ==========================================================================
// 6. 工具：sb_pack → sb_info → sb_script → sb_unpack（自造作品，不依赖 fixture）
// ==========================================================================

static void testPackInfoScriptUnpack(const fs::path& tmp) {
    std::printf("\n=== 6. sb_pack / sb_info / sb_script / sb_unpack ===\n");

    fs::path proj = tmp / "proj_pack";
    sbcliProjectInit(proj.string(), "打包测试");
    sbcliAddSprite(proj.string(), "小猫");
    writeFile(proj / "character" / "1" / "block.sbcli",
              "@script flag\n"
              "event_whenflagclicked\n"
              "looks_say MESSAGE=你好呀\n");

    // 6a. pack（输出目录必须已存在：cmd_pack 不建父目录，这是既有行为）
    fs::path sb3 = tmp / "packed.sb3";
    bool isErr = false;
    Json rp = callTool("sb_pack",
                       "{\"projectDir\":" + jstr(proj.string()) +
                       ",\"outSb3\":" + jstr(sb3.string()) + "}",
                       "sb_pack", &isErr);
    ok(!isErr, "sb_pack isError = false（实际：" + rp.dump() + "）");
    ok(fs::exists(sb3), "sb_pack 生成了 .sb3 文件");
    // 注意：不能直接 fs::file_size —— 路径不存在时它抛异常，
    // 会把"断言失败"变成"进程崩溃"，后面的用例就全跑不到了。
    std::error_code ec;
    ok(fs::file_size(sb3, ec) > 0 && !ec, "生成的 .sb3 非空");

    // 6b. info（走 cmd_info + std::cout 捕获）
    Json ri = callTool("sb_info", "{\"file\":" + jstr(sb3.string()) + "}", "sb_info");
    ok(ri.is_object(), "sb_info 返回对象");
    ok(!ri.value("file", std::string()).empty(), "sb_info 有 file 字段");
    Json targets = ri.contains("targets") ? ri["targets"] : Json();
    ok(targets.is_array() && targets.size() >= 2, "sb_info 列出了目标（含舞台与角色）");
    bool hasCat = false;
    for (size_t i = 0; i < targets.size(); ++i)
        if (targets[i].value("name", std::string()) == "小猫") hasCat = true;
    ok(hasCat, "目标里有角色「小猫」");

    // 6c. script
    Json rs = callTool("sb_script", "{\"file\":" + jstr(sb3.string()) + "}", "sb_script");
    ok(rs.is_object(), "sb_script 返回对象");
    ok(rs.contains("targets") && rs["targets"].is_array(), "sb_script 有 targets 数组");
    std::string dump = rs.dump();
    ok(contains(dump, "小猫"), "脚本输出里有角色名");

    // 6d. unpack 回项目目录，再 check 一遍（闭环）
    fs::path back = tmp / "proj_back";
    Json ru = callTool("sb_unpack",
                       "{\"sb3\":" + jstr(sb3.string()) +
                       ",\"outDir\":" + jstr(back.string()) + ",\"force\":true}",
                       "sb_unpack", &isErr);
    ok(!isErr && ru.value("ok", false), "sb_unpack ok = true");
    ok(fs::exists(back / "meta.sbcli"), "unpack 生成了 meta.sbcli");
    ok(ru.value("sprites", -1) >= 1, "unpack 报告角色数 >= 1");

    Json rc = callTool("sb_check", "{\"project\":" + jstr(back.string()) + "}",
                       "unpack 后 sb_check");
    ok(rc.is_object() && rc.value("ok", false), "unpack 出来的项目 check 通过");
}

// ==========================================================================
// 7. 工具：sb_edit / sb_rename
// ==========================================================================

static void testEditRename(const fs::path& tmp) {
    std::printf("\n=== 7. sb_edit / sb_rename ===\n");

    fs::path proj = tmp / "proj_edit";
    sbcliProjectInit(proj.string(), "编辑测试");
    sbcliAddSprite(proj.string(), "玩家");
    fs::path block = proj / "character" / "1" / "block.sbcli";
    writeFile(block,
              "@script flag\n"
              "event_whenflagclicked\n"
              "looks_say MESSAGE=第一句\n");

    std::string before = readFile(block);

    // 7a. insert（整体走 sbcEditInsert → EditResult）
    bool isErr = false;
    Json ri = callTool("sb_edit",
                       "{\"project\":" + jstr(proj.string()) +
                       ",\"sprite\":\"1\",\"scriptIndex\":1,\"op\":\"insert\","
                       "\"after\":1,\"text\":\"looks_say MESSAGE=第二句\"}",
                       "sb_edit insert", &isErr);
    ok(!isErr && ri.value("ok", false), "sb_edit insert ok = true");
    ok(ri.value("changed", 0) >= 1, "sb_edit insert 报告 changed >= 1");
    std::string after = readFile(block);
    ok(after != before, "block.sbcli 内容变了");
    ok(contains(after, "第二句"), "插入的积木写进了文件");

    // 7b. 非法 op → 工具级错误
    Json rbad = callTool("sb_edit",
                         "{\"project\":" + jstr(proj.string()) + ",\"op\":\"explode\"}",
                         "sb_edit 非法 op", &isErr);
    ok(isErr, "非法 op isError = true");
    ok(!rbad.value("ok", true), "非法 op ok = false");

    // 7c. rename（sbcRename → EditResult）
    sbcliProjectInit((tmp / "proj_rename").string(), "改名测试");
    fs::path rp = tmp / "proj_rename";
    sbcliAddSprite(rp.string(), "玩家");
    writeFile(rp / "meta.sbcli",
              "name: 改名测试\n"
              "[variables]\n"
              "分数 = 0\n");
    writeFile(rp / "character" / "1" / "block.sbcli",
              "@script flag\n"
              "event_whenflagclicked\n"
              "data_changevariableby VARIABLE=分数 VALUE=1\n");

    Json rr = callTool("sb_rename",
                       "{\"project\":" + jstr(rp.string()) +
                       ",\"kind\":\"variable\",\"oldName\":\"分数\",\"newName\":\"得分\"}",
                       "sb_rename", &isErr);
    ok(!isErr && rr.value("ok", false), "sb_rename ok = true");
    std::string rb = readFile(rp / "character" / "1" / "block.sbcli");
    ok(contains(rb, "得分"), "脚本里的引用已改写为 得分");
    ok(contains(readFile(rp / "meta.sbcli"), "得分"), "meta 声明已改写为 得分");

    // 7d. 缺必需参数
    callTool("sb_rename", "{\"project\":" + jstr(rp.string()) + "}", "sb_rename 缺参", &isErr);
    ok(isErr, "缺 kind/oldName/newName 时 isError = true");
}

// ==========================================================================
// 8. stdout 纯净性：mcpServe 的每一行输出都必须是合法 JSON
// ==========================================================================

static void testServePurity() {
    std::printf("\n=== 8. stdout 纯净性（多请求流）===\n");

    std::istringstream in(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
        "\n"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"nope\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"sb_search\",\"arguments\":{\"query\":\"广播\"}}}\n");
    std::ostringstream out;
    mcpServe(in, out);

    std::string all = out.str();
    std::istringstream lines(all);
    std::string line;
    int nLines = 0, nBad = 0;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        ++nLines;
        try {
            Json j = Json::parse(line);
            if (!j.is_object() || !j.contains("jsonrpc")) ++nBad;
        } catch (...) {
            ++nBad;
            std::printf("        非 JSON 行：%s\n", line.c_str());
        }
    }
    ok(nLines == 4, "5 条输入（含 1 条通知 + 1 空行）→ 4 行响应（实际 " +
                    std::to_string(nLines) + "）");
    ok(nBad == 0, "每一行响应都是合法 JSON-RPC 对象");
}

// ==========================================================================
// main
// ==========================================================================

int main() {
    // 崩溃时缓冲输出会丢：改成每次 printf 都立刻落盘，便于定位
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("==== sb mcp 自测 ====\n");

    fs::path tmp = tempRoot();
    std::printf("临时目录：%s\n", tmp.string().c_str());

    testHandshake();
    testErrors();
    testToolsList();
    testSearch();
    testCheck(tmp);
    testPackInfoScriptUnpack(tmp);
    testEditRename(tmp);
    testServePurity();

    {
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    std::printf("\n==== 测试结果 ====\n");
    std::printf("通过断言：%d\n", g_pass);
    std::printf("失败断言：%d\n", g_fail);
    if (g_fail == 0) {
        std::printf("✅ 全部通过\n");
        return 0;
    }
    std::printf("❌ 有 %d 处断言失败\n", g_fail);
    return 1;
}
