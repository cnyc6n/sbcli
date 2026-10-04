// src/common.cpp
#include "common.hpp"
#include <sys/stat.h>
#include <sys/types.h>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <cstring>
#include <algorithm>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace sb {

// Windows 命令行参数通常是 ACP（中文系统是 GBK），转成 UTF-8 统一处理。
std::string acpToUtf8(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return s;
    int wn = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (wn <= 0) return s;
    std::wstring w(wn, 0);
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, nullptr, 0, nullptr, nullptr);
    if (un <= 0) return s;
    std::string out(un, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, &out[0], un, nullptr, nullptr);
    return out;
#else
    return s;
#endif
}

// 路径转换：程序内部统一用 UTF-8，Windows 下转成 wide 再交给 filesystem。
#ifdef _WIN32
static fs::path wpath(const std::string& s) {
    if (s.empty()) return fs::path();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return fs::path(s);  // 兜底：不转
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return fs::path(w);
}
#else
static fs::path wpath(const std::string& s) { return fs::path(s); }
#endif

std::string humanSize(long long size) {
    if (size < 0) return "?";
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = (double)size;
    int i = 0;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; ++i; }
    char buf[64];
    if (i == 0) std::snprintf(buf, sizeof(buf), "%.0f%s", v, units[i]);
    else        std::snprintf(buf, sizeof(buf), "%.1f%s", v, units[i]);
    return buf;
}

std::string truncate(const std::string& s, size_t n) {
    std::string out;
    out.reserve(std::min(s.size(), n + 1));
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') continue;
        else out += c;
        if (out.size() > n + 8) break; // 粗略控制
    }
    if (out.size() <= n) return out;
    // 按 UTF-8 回退，避免截断半个字符
    size_t cut = n - 1;
    while (cut > 0 && (out[cut] & 0xC0) == 0x80) --cut;
    return out.substr(0, cut) + "…";
}

std::string sanitizeFilename(const std::string& name, const std::string& fallback) {
    static const std::string invalid = "\\/:*?\"<>|\r\n\t";
    std::string s = name;
    // trim
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) s.clear();
    else s = s.substr(a, b - a + 1);
    for (char& c : s) if (invalid.find(c) != std::string::npos) c = '_';
    // 去掉首尾的点
    while (!s.empty() && (s.front() == '.' || s.front() == ' ')) s.erase(s.begin());
    while (!s.empty() && (s.back()  == '.' || s.back()  == ' ')) s.pop_back();
    return s.empty() ? fallback : s;
}

std::string fillProccode(const std::string& code,
                         const std::vector<std::string>& names) {
    // 拆分 "%n/%s/%b"
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < code.size()) {
        if (code[i] == '%' && i + 1 < code.size()) {
            char t = code[i + 1];
            if (t == 'n' || t == 's' || t == 'b') {
                parts.push_back(code.substr(i, 2));
                i += 2;
                continue;
            }
        }
        size_t j = i;
        while (j < code.size()) {
            if (code[j] == '%' && j + 1 < code.size() &&
                (code[j+1] == 'n' || code[j+1] == 's' || code[j+1] == 'b'))
                break;
            ++j;
        }
        parts.push_back(code.substr(i, j - i));
        i = j;
    }
    std::string out;
    size_t k = 0;
    for (auto& p : parts) {
        if (p == "%n" || p == "%s" || p == "%b") {
            out += "(" + (k < names.size() ? names[k] : std::string("?")) + ")";
            ++k;
        } else {
            out += p;
        }
    }
    return out;
}

void makeDirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(wpath(path), ec);
}

std::vector<uint8_t> readFileBinary(const std::string& path) {
    std::ifstream f(wpath(path), std::ios::binary);
    if (!f) throw Sb1Error("找不到文件：" + path);
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> out((size_t)n);
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    return out;
}

void writeFileBinary(const std::string& path, const uint8_t* data, size_t n) {
    std::ofstream f(wpath(path), std::ios::binary);
    if (!f) throw Sb1Error("无法写文件：" + path);
    f.write(reinterpret_cast<const char*>(data), (std::streamsize)n);
}

bool isFile(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(wpath(path), ec);
}

long long fileSize(const std::string& path) {
    std::error_code ec;
    auto n = fs::file_size(wpath(path), ec);
    return ec ? -1 : (long long)n;
}

int64_t fileMtime(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(wpath(path), ec);
    if (ec) return 0;
    // 转成 epoch seconds
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (int64_t)std::chrono::system_clock::to_time_t(sctp);
}

std::string extname(const std::string& path) {
    auto pos = path.find_last_of('.');
    auto sep = path.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    if (sep != std::string::npos && pos < sep) return "";
    std::string e = path.substr(pos);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return e;
}

std::string basename(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string dirname(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

std::string formatTime(int64_t epochSeconds) {
    std::time_t t = (std::time_t)epochSeconds;
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

std::vector<std::string> walkFiles(const std::string& root,
                                   const std::vector<std::string>& exts,
                                   const std::vector<std::string>& skipDirs) {
    std::vector<std::string> out;
    std::error_code ec;
    if (fs::is_regular_file(wpath(root), ec)) {
        std::string e = extname(root);
        for (auto& x : exts) if (e == x) { out.push_back(root); break; }
        return out;
    }
    if (!fs::is_directory(wpath(root), ec)) return out;

    for (auto it = fs::recursive_directory_iterator(wpath(root), ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        auto& p = it->path();
        std::string fname = p.filename().u8string();
        if (it->is_directory()) {
            if (!fname.empty() && fname[0] == '.') { it.disable_recursion_pending(); continue; }
            bool skip = false;
            for (auto& s : skipDirs) if (fname == s) { skip = true; break; }
            if (skip) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        std::string e = extname(p.u8string());
        for (auto& x : exts) if (e == x) { out.push_back(p.u8string()); break; }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace sb