// src/ext_loader.hpp —— 自动联网解析 sb3 扩展
#pragma once
#include <map>
#include <string>

namespace sb {
struct Sb3File;

struct ExtInfo {
    std::map<std::string, int> blockTypes;        // opcode → 类型（0..3）
    std::map<std::string, std::string> extNames;  // 扩展id → 名称
    bool tried = false;                            // 是否尝试过（避免重复 spawn）
};

// 从 sb3 的 extensionURLs 自动解析扩展（node tools/fetch_tw_extension.js）。
// 返回 -1 = 无 extensionURLs；0 = 解析失败；N = 扩展数。
int loadExtensionsFromSb3(const Sb3File& sf, ExtInfo& out);

// 从项目目录的 extensions/*.js（unpack 导出的扩展源码）解析扩展。
// 返回 -1 = 无扩展目录；0 = 无可用源码；N = 成功解析的扩展数。
int loadExtensionsFromDir(const std::string& projectDir, ExtInfo& out);
}
