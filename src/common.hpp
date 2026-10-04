// src/common.hpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <regex>

namespace sb {

inline constexpr const char* VERSION = "3.0";

class Sb1Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Sb3Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---- 文件/字符串小工具 ----

// 把 Windows 命令行参数（ACP/GBK）转成 UTF-8（程序内部统一 UTF-8）
std::string acpToUtf8(const std::string& s);

std::string humanSize(long long size);
std::string truncate(const std::string& s, size_t n = 60);
std::string sanitizeFilename(const std::string& name,
                             const std::string& fallback = "unnamed");

// 把 "%n %s %b" 风格的 proccode 用 names 替换
std::string fillProccode(const std::string& code,
                         const std::vector<std::string>& names);

// 递归建目录（mkdir -p）
void makeDirs(const std::string& path);

// 读出整个文件
std::vector<uint8_t> readFileBinary(const std::string& path);

// 写出二进制
void writeFileBinary(const std::string& path, const uint8_t* data, size_t n);

// 判断是不是普通文件
bool isFile(const std::string& path);

// 文件大小 / 修改时间
long long fileSize(const std::string& path);
int64_t   fileMtime(const std::string& path);

// 递归列目录，返回匹配扩展名的文件（小写扩展名，含点）
std::vector<std::string> walkFiles(const std::string& root,
                                   const std::vector<std::string>& exts,
                                   const std::vector<std::string>& skipDirs = {});

// 取扩展名（小写，含点）
std::string extname(const std::string& path);
// 取文件名（含扩展名）
std::string basename(const std::string& path);
// 取目录部分
std::string dirname(const std::string& path);
// 拼接路径
std::string joinPath(const std::string& a, const std::string& b);

// 时间格式化 YYYY-MM-DD HH:MM
std::string formatTime(int64_t epochSeconds);

} // namespace sb