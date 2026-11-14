// src/common.hpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <regex>

namespace sb {

inline constexpr const char* VERSION = "1.3.0";

// ---- 统一退出码 ----
// 0 成功 / 1 通用失败 / 2 用法错误 / 3 输入无效 / 4 校验失败 /
// 5 编辑冲突 / 6 依赖缺失 / 7 权限或 IO
//
// 约定：命令入口（cmd_*）与 main() 返回 int，一律用本枚举具名常量，
// 不再写裸数字。详见 docs/exit-codes.md。
enum class ExitCode : int {
    Ok = 0, Fail = 1, Usage = 2, BadInput = 3,
    CheckFailed = 4, Conflict = 5, DepMissing = 6, IoError = 7
};

// 转 int（命令返回类型是 int，避免每个 return 都写 (int)ExitCode::X 强转）。
// 名字取 exitInt 而非 ec：各命令里大量存在局部变量 std::error_code ec，
// 再引入同名函数会命中「局部变量遮蔽」导致调用处报错。
inline constexpr int exitInt(ExitCode c) noexcept { return static_cast<int>(c); }

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