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
}
