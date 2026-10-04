// src/find_impl.hpp —— find 命令的两格式搜索 worker（commands.cpp 拆分）
#pragma once
#include "commands.hpp"
#include <string>
#include <vector>

namespace sb {

struct FindHit {
    std::string file, target, where, text;
};

// json → 文本（FindHit.text 用；定义在 commands.cpp）
std::string jsonStr(const Json& v);
// DOM 视图 → 文本（语义与 jsonStr(Json) 一致；定义在 find_impl.cpp）
std::string jsonStr(const Elem& v);

// 按文件头判断是否 1.4（detectFormat 的轻量版）
bool isSb1File(const std::string& p);

// 大小写不敏感查找：不复制/转换整个字符串（haystack 为 UTF-8 字节）
bool findCI(const std::string& haystack, const std::string& needle);

// 单个文件的搜索 worker（两类格式各自实现）
std::vector<FindHit> sb1FindInFile(const std::string& p,
                                   const std::string& kwLower,
                                   bool withScript);
std::vector<FindHit> sb3FindInFile(const std::string& p,
                                   const std::string& kwLower,
                                   bool withScript);

} // namespace sb