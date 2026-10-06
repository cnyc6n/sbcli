// src/ext_loader.cpp —— 自动联网解析 sb3 的扩展（extensionURLs）
//
// 仿照 TurboWarp VM 的扩展加载（virtual-machine.js loadProject）：
//   1. 读 sb3 顶层 extensionURLs（id → URL；data: 内嵌源码 或 https: 在线）
//   2. 调 node tools/fetch_tw_extension.js <sb3> 批量解析 getInfo()
//   3. 产出 opcode → 类型 表，供 Renderer 识别扩展积木
// 失败时不阻断渲染：返回 0，调用方用静态表/generic 兜底。
#include "ext_loader.hpp"
#include "common.hpp"
#include "sb3.hpp"
#include "sbcli_meta.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sb {

// sb::meta 的惯用别名（本文件用 cm::loadRootMeta 读项目 meta 的 [extensions] 段）
namespace cm = meta;

namespace {

// 定位 tools/fetch_tw_extension.js。
// 不能只写相对路径 "tools/..."：只有在仓库根目录运行时才成立，
// 在用户项目目录（如 F:\temp\addext）里跑 sb check / sb search 会静默失效。
// 顺序：环境变量 SBCLI_TOOLS → cwd/tools → exe 所在目录及其上两级。
static std::string locateFetchTool() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const char* env = std::getenv("SBCLI_TOOLS");
    if (env && *env) {
        fs::path p(env);
        if (fs::is_directory(p, ec)) p /= "fetch_tw_extension.js";
        if (fs::exists(p, ec)) return p.u8string();
    }
    std::vector<fs::path> cands;
    // 编译期固化的源码根（CMake target_compile_definitions）——最可靠：
    // 无论从哪个目录运行、exe 放在哪里，都能找到 tools/fetch_tw_extension.js。
#ifdef SBCLI_SOURCE_ROOT
    cands.push_back(fs::path(SBCLI_SOURCE_ROOT) / "tools" / "fetch_tw_extension.js");
#endif
    cands.push_back(fs::current_path(ec) / "tools" / "fetch_tw_extension.js");
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) {
        fs::path exe = fs::path(buf).parent_path();
        cands.push_back(exe / "tools" / "fetch_tw_extension.js");
        cands.push_back(exe.parent_path() / "tools" / "fetch_tw_extension.js");
        cands.push_back(exe.parent_path().parent_path() / "tools" / "fetch_tw_extension.js");
    }
#else
    char buf[4096] = {0};
    if (readlink("/proc/self/exe", buf, sizeof(buf) - 1) > 0) {
        fs::path exe = fs::path(buf).parent_path();
        cands.push_back(exe / "tools" / "fetch_tw_extension.js");
        cands.push_back(exe.parent_path() / "tools" / "fetch_tw_extension.js");
    }
#endif
    for (const auto& c : cands) if (!c.empty() && fs::exists(c, ec)) return c.u8string();
    return {};
}

// 拼命令行并执行。
//
// 转义规则**分平台**，这点踩过坑：
//   · POSIX（sh）：双引号里 \ 和 " 都要转义，路径用 / 不受影响。
//   · Windows（cmd.exe）：双引号里 **\ 不是转义符**，写 \\ 会原样传给程序。
//     而 Windows 路径天生含 \（F:\temp\...），照 POSIX 规则转义后 node 收到
//     F:\\temp\\...，fs.readFileSync 直接 ENOENT → 扩展解析静默失败。
//     所以 Windows 下只转义 "（用 ""），反斜杠保持原样。
std::string runCmd(const std::string& cmd, const std::vector<std::string>& args) {
    std::string full = cmd;
    for (const auto& a : args) {
        full += " \"";
        for (char c : a) {
#ifdef _WIN32
            if (c == '"') full += '"';   // cmd.exe 里用 "" 表示一个字面引号
            full += c;
#else
            if (c == '"' || c == '\\') full += '\\';
            full += c;
#endif
        }
        full += "\"";
    }
    std::string out;
    FILE* f = popen(full.c_str(), "r");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    pclose(f);
    return out;
}
} // namespace

// 解析 node 工具输出的扩展 JSON，填充 ExtInfo
// 解析 node 工具输出的扩展 JSON，填充 ExtInfo
static int fillFromJson(const std::string& jsonOut, ExtInfo& out) {
    Json j;
    try { j = Json::parse(jsonOut); } catch (...) { return 0; }
    if (!j.is_object()) return 0;
    int count = 0;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& extId = it.key();
        const Json& ext = it.value();
        if (ext.is_object() && ext.value("error", false)) continue;
        std::string name = ext.contains("name") && ext["name"].is_string()
                           ? ext["name"].get<std::string>() : extId;
        out.extNames[extId] = name;
        ++count;

        // getInfo().menus：菜单 id → 允许的取值集合。
        // 只收静态菜单；动态菜单（getItems 是函数）在 JS 侧已跳过，这里自然为空。
        // 菜单 id 在扩展内唯一，这里按 extId 加前缀隔离，避免不同扩展同名菜单串味。
        if (ext.contains("menus") && ext["menus"].is_object()) {
            for (auto mi = ext["menus"].begin(); mi != ext["menus"].end(); ++mi) {
                const Json& items = mi.value();
                if (!items.is_array()) continue;
                std::string key = extId + "::" + mi.key();
                std::set<std::string> vals;
                for (const auto& item : items) {
                    if (item.is_string())
                        vals.insert(item.get<std::string>());
                    else if (item.is_object() && item.contains("value") &&
                             item["value"].is_string())
                        vals.insert(item["value"].get<std::string>());
                }
                if (!vals.empty()) out.menuValues[key] = std::move(vals);
            }
        }
        if (!ext.contains("blocks") || !ext["blocks"].is_array()) continue;
        for (const auto& b : ext["blocks"]) {
            if (!b.is_object()) continue;
            std::string op = b.value("opcode", std::string());
            std::string t = b.value("type", std::string("command"));
            if (op.empty()) continue;
            int type = 0;
            if (t == "reporter") type = 1;
            else if (t == "boolean") type = 2;
            else if (t == "hat") type = 3;
            std::string fullOp = extId + "_" + op;
            out.blockTypes[fullOp] = type;
            out.blockOwner[fullOp] = extId;
            // 参数名（getInfo().arguments 的键）→ 用于 check 的参数校验
            if (b.contains("args") && b["args"].is_object()) {
                std::vector<std::string> names;
                std::map<std::string, std::string> menus;
                for (auto ai = b["args"].begin(); ai != b["args"].end(); ++ai) {
                    names.push_back(ai.key());
                    if (ai.value().is_object() && ai.value().contains("menu") &&
                        !ai.value()["menu"].is_null() && ai.value()["menu"].is_string()) {
                        menus[ai.key()] = ai.value()["menu"].get<std::string>();
                    }
                }
                if (!names.empty()) out.blockParams[fullOp] = names;
                if (!menus.empty()) out.blockMenus[fullOp] = menus;
            }
        }
    }
    return count;
}

int loadExtensionsFromSb3(const Sb3File& sf, ExtInfo& out) {
    out.tried = true;
    Elem data(sf.data);
    Elem urls = data.at("extensionURLs");
    if (!urls.is_object()) return -1;
    if (sf.path.empty()) return 0;

    // 定位 tools/fetch_tw_extension.js
    std::string script = locateFetchTool();
    if (script.empty()) return 0;

    std::string text = runCmd("node", { script, sf.path });
    size_t mark = text.rfind("___RESULT___");
    if (mark == std::string::npos) return 0;
    std::string jsonOut = text.substr(mark + std::string("___RESULT___").size());
    while (!jsonOut.empty() && (jsonOut.back() == '\n' || jsonOut.back() == '\r'))
        jsonOut.pop_back();
    return fillFromJson(jsonOut, out);
}

// （locateFetchTool 定义在文件上方，此处不再重复）

// 从项目目录解析扩展（unpack 导出的独立扩展源码 + meta [extensions] 登记的 URL）。
//
// 数据来源两处，互为补充：
//   · extensions/*.js          —— 本地源码（unpack 落盘 / add-extension 拷贝）
//   · meta.sbcli [extensions]  —— 权威登记：id → 本地相对路径 或 URL
// 登记了但本地没有源码的扩展：
//   · 值是 URL → 联网下载解析（复用 node 工具的 fetch 模式），
//     成功后缓存到 <项目>/.sbcli-cache/<id>.js，之后每次 check 直接解析缓存副本。
//   · 值不是 URL（相对路径但文件缺失 / 空值）→ 无法加载。
// 所有没加载到的登记 id 记入 failedIds（不崩、不误报：调用方把这类扩展的积木
// 从 unknown-opcode 错误降级为警告）。
int loadExtensionsFromDir(const std::string& projectDir, ExtInfo& out) {
    namespace fs = std::filesystem;
    out.tried = true;
    std::error_code ec;

    // ---- 1. meta [extensions] 登记（权威清单）----
    std::map<std::string, std::string> registered;
    cm::RootMeta rm = cm::loadRootMeta(projectDir + "/meta.sbcli");
    if (rm.exists) registered = rm.extensions;
    for (const auto& kv : registered) out.registeredIds.insert(kv.first);

    // ---- 2. 扫描 extensions/*.js 本地源码 ----
    std::string extDir = projectDir + "/extensions";
    std::vector<std::string> files;
    if (fs::is_directory(fs::u8path(extDir), ec))
        for (auto& e : fs::directory_iterator(fs::u8path(extDir), ec))
            if (e.is_regular_file() && e.path().extension() == ".js")
                files.push_back(e.path().u8string());

    if (registered.empty() && files.empty()) return -1;

    std::string script = locateFetchTool();
    if (script.empty()) {
        // 工具不可用（node 缺失/脚本丢失）：本地文件也解析不了，
        // 登记的扩展全部记 failed（registeredIds 仍在，check 靠它降级）。
        for (const auto& kv : registered)
            if (!out.extNames.count(kv.first)) out.failedIds.insert(kv.first);
        return 0;
    }

    // 解析单个源码文件（node 工具文件模式）。成功解析出一个扩展 → 返回 1。
    auto parseFile = [&](const std::string& f) -> int {
        std::string text = runCmd("node", { script, f });
        size_t mark = text.rfind("___RESULT___");
        if (mark == std::string::npos) return 0;
        std::string jsonOut = text.substr(mark + std::string("___RESULT___").size());
        while (!jsonOut.empty() && (jsonOut.back() == '\n' || jsonOut.back() == '\r'))
            jsonOut.pop_back();
        // 单文件模式返回的是单个扩展对象（含 id/name/blocks），包一层便于复用
        Json single;
        try { single = Json::parse(jsonOut); } catch (...) { return 0; }
        if (!single.is_object() || single.value("error", false)) return 0;
        // 用文件 basename（去掉 .js）作为扩展 id（与 meta 的登记一致）
        std::string base = fs::path(f).stem().u8string();
        Json wrapper = Json::object();
        if (single.contains("id") && single["id"].is_string())
            base = single["id"].get<std::string>();   // 优先扩展自报 id
        wrapper[base] = single;
        return fillFromJson(wrapper.dump(), out);
    };

    int total = 0;
    for (const auto& f : files) total += parseFile(f);

    // ---- 3. 登记了但本地没有源码的扩展 ----
    // 值含 :// 或 data: 前缀 → URL：下载（带缓存）；否则（相对路径缺文件 / 空值）→ failed
    std::string cacheDir = projectDir + "/.sbcli-cache";
    for (const auto& kv : registered) {
        const std::string& id = kv.first;
        if (id.empty() || out.extNames.count(id)) continue;   // 本地源码已解析
        const std::string& src = kv.second;
        bool isUrl = src.find("://") != std::string::npos || src.rfind("data:", 0) == 0;
        if (!isUrl) {
            out.failedIds.insert(id);
            continue;
        }
        std::string cacheFile = cacheDir + "/" + id + ".js";
        if (fs::is_regular_file(fs::u8path(cacheFile), ec)) {
            // 缓存命中：直接解析本地副本，不联网
            total += parseFile(cacheFile);
        } else {
            // 下载 + 解析 + 写缓存（fetch 模式：解析成功才落缓存，失败不污染）
            std::string text = runCmd("node", { script, "fetch", src, cacheFile });
            size_t mark = text.rfind("___RESULT___");
            if (mark != std::string::npos) {
                std::string jsonOut = text.substr(mark + std::string("___RESULT___").size());
                while (!jsonOut.empty() && (jsonOut.back() == '\n' || jsonOut.back() == '\r'))
                    jsonOut.pop_back();
                Json single;
                try { single = Json::parse(jsonOut); } catch (...) {}
                if (single.is_object() && !single.value("error", false)) {
                    Json wrapper = Json::object();
                    std::string base = id;                 // 登记的 id 优先，自报 id 次之
                    if (single.contains("id") && single["id"].is_string())
                        base = single["id"].get<std::string>();
                    wrapper[base] = single;
                    total += fillFromJson(wrapper.dump(), out);
                }
            }
            if (!out.extNames.count(id)) out.failedIds.insert(id);
        }
    }
    return total;
}

} // namespace sb