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

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace sb {

namespace {
std::string runCmd(const std::string& cmd, const std::vector<std::string>& args) {
    std::string full = cmd;
    for (const auto& a : args) {
        full += " \"";
        for (char c : a) {
            if (c == '"' || c == '\\') full += '\\';
            full += c;
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
            out.blockTypes[extId + "_" + op] = type;
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
    std::string script = "tools/fetch_tw_extension.js";
    std::ifstream probe(script);
    if (!probe.good()) return 0;

    std::string text = runCmd("node", { script, sf.path });
    size_t mark = text.rfind("___RESULT___");
    if (mark == std::string::npos) return 0;
    std::string jsonOut = text.substr(mark + std::string("___RESULT___").size());
    while (!jsonOut.empty() && (jsonOut.back() == '\n' || jsonOut.back() == '\r'))
        jsonOut.pop_back();
    return fillFromJson(jsonOut, out);
}

// 从项目目录的 extensions/*.js 解析（unpack 导出的独立扩展源码）
int loadExtensionsFromDir(const std::string& projectDir, ExtInfo& out) {
    namespace fs = std::filesystem;
    out.tried = true;
    std::string extDir = projectDir + "/extensions";
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(extDir), ec)) return -1;

    // 收集所有 .js 文件
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(fs::u8path(extDir), ec)) {
        if (e.is_regular_file() && e.path().extension() == ".js")
            files.push_back(e.path().u8string());
    }
    if (files.empty()) return 0;

    std::string script = "tools/fetch_tw_extension.js";
    std::ifstream probe(script);
    if (!probe.good()) return 0;

    int total = 0;
    for (const auto& f : files) {
        std::string text = runCmd("node", { script, f });
        size_t mark = text.rfind("___RESULT___");
        if (mark == std::string::npos) continue;
        std::string jsonOut = text.substr(mark + std::string("___RESULT___").size());
        while (!jsonOut.empty() && (jsonOut.back() == '\n' || jsonOut.back() == '\r'))
            jsonOut.pop_back();
        // 单文件模式返回的是单个扩展对象（含 id/name/blocks），包一层便于复用
        Json single;
        try { single = Json::parse(jsonOut); } catch (...) { continue; }
        if (!single.is_object() || single.value("error", false)) continue;
        // 用文件 basename（去掉 .js）作为扩展 id（与 meta 的登记一致）
        std::string base = fs::path(f).stem().u8string();
        Json wrapper = Json::object();
        if (single.contains("id") && single["id"].is_string())
            base = single["id"].get<std::string>();   // 优先扩展自报 id
        wrapper[base] = single;
        total += fillFromJson(wrapper.dump(), out);
    }
    return total;
}

} // namespace sb