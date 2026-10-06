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

int loadExtensionsFromSb3(const Sb3File& sf, ExtInfo& out) {
    out.tried = true;
    Elem data(sf.data);
    Elem urls = data.at("extensionURLs");
    if (!urls.is_object()) return -1;
    if (sf.path.empty()) return 0;

    // 定位 tools/fetch_tw_extension.js：与可执行文件同目录或仓库 tools/
    std::string script = "tools/fetch_tw_extension.js";
    std::ifstream probe(script);
    if (!probe.good()) return 0;

    std::string text = runCmd("node", { script, sf.path });
    size_t mark = text.rfind("___RESULT___");
    if (mark == std::string::npos) return 0;
    std::string jsonOut = text.substr(mark + std::string("___RESULT___").size());
    while (!jsonOut.empty() && (jsonOut.back() == '\n' || jsonOut.back() == '\r'))
        jsonOut.pop_back();

    Json j;
    try { j = Json::parse(jsonOut); } catch (...) { return 0; }
    if (!j.is_object()) return 0;

    int count = 0;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& extId = it.key();
        const Json& ext = it.value();
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

} // namespace sb
