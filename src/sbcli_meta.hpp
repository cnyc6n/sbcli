// src/sbcli_meta.hpp
// meta.sbcli 的读写（docs/format.md §3）。被 sbcli_fix 与 sbcli_project 共用。
//
// 为什么单独抽一层：fix（补引用）和 add-*（建结构）写的是同一批 meta 文件，
// 各写一份解析器迟早会不一致（比如括号风格、素材列表格式）。
//
// 为什么放进 sb::meta 子命名空间：cmd-check 的 sbcli_check.cpp 里有一套同名的
// 文件级工具（trimStr / joinRel …）在匿名命名空间里。若这里也把它们放进
// 顶层 sb，将来 check.cpp 包含本头文件时会出现二义性。用子命名空间隔开，
// 两边互不干扰。
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace sb {
namespace meta {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- 路径/文件

inline std::string norm(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 3 && s.back() == '/') s.pop_back();
    return s;
}
inline std::string joinRel(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}
inline bool fileExists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(p), ec);
}
inline bool dirExists(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(fs::u8path(p), ec);
}
inline bool makeDirs(const std::string& p) {
    std::error_code ec;
    return fs::create_directories(fs::u8path(p), ec) || fs::is_directory(fs::u8path(p), ec);
}
inline std::string relToRoot(const std::string& root, const std::string& abs) {
    std::error_code ec;
    fs::path rp = fs::relative(fs::u8path(abs), fs::u8path(root), ec);
    return ec ? norm(abs) : norm(rp.u8string());
}
inline std::string trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
inline std::vector<std::string> readLines(const std::string& p) {
    std::vector<std::string> out;
    std::ifstream f(fs::u8path(p), std::ios::binary);
    if (!f) return out;
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (all.size() >= 3 && (unsigned char)all[0] == 0xEF &&
        (unsigned char)all[1] == 0xBB && (unsigned char)all[2] == 0xBF)
        all.erase(0, 3);
    std::istringstream is(all);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}
inline bool writeLines(const std::string& p, const std::vector<std::string>& lines) {
    std::error_code ec;
    fs::path fp = fs::u8path(p);
    if (fp.has_parent_path()) fs::create_directories(fp.parent_path(), ec);
    std::ofstream f(fp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    for (const auto& l : lines) f << l << "\n";
    return (bool)f;
}
inline bool copyFileTo(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::path fp = fs::u8path(to);
    if (fp.has_parent_path()) fs::create_directories(fp.parent_path(), ec);
    return fs::copy_file(fs::u8path(from), fp,
                         fs::copy_options::overwrite_existing, ec) || fs::exists(fp, ec);
}
inline std::string unquote(std::string s) {
    s = trimStr(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}
// 去掉包裹符并按逗号切开（不处理引号内的逗号——meta 里足够用）
inline std::vector<std::string> splitListBody(const std::string& raw) {
    std::string body = trimStr(raw);
    if (body.size() >= 2) {
        char a = body.front(), b = body.back();
        if ((a == '[' && b == ']') || (a == '{' && b == '}') || (a == '(' && b == ')'))
            body = body.substr(1, body.size() - 2);
    }
    std::vector<std::string> out;
    std::string cur;
    bool inStr = false;
    for (char c : body) {
        if (c == '"') { inStr = !inStr; cur += c; continue; }
        if (c == ',' && !inStr) { out.push_back(trimStr(cur)); cur.clear(); continue; }
        cur += c;
    }
    if (!trimStr(cur).empty()) out.push_back(trimStr(cur));
    return out;
}

// ---------------------------------------------------------------- 渲染

// 变量/列表/广播名的集合。
// 注意 §3.2 的括号不统一：variables / lists 用花括号，broadcasts 用方括号。
// 读取侧两种都接受，但写回要按原格式，否则 diff / 人眼会对不上。
inline std::string renderNames(const std::set<std::string>& s, bool braces) {
    std::string out;
    for (const auto& n : s) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return braces ? ("{" + out + "}") : ("[" + out + "]");
}
// 角色级变量（带初始值）：{局部血量 = 100, 速度 = 2}
// 值已经在调用方格式化成 sbcli 字面量（数字裸写 / 文本带引号）。
inline std::string renderVariables(const std::map<std::string, std::string>& m) {
    std::string out;
    for (const auto& kv : m) {
        if (!out.empty()) out += ", ";
        out += kv.first + " = " + kv.second;
    }
    return "{" + out + "}";
}
// 角色级列表（带初始内容）：{道具 = [苹果, 香蕉]}
inline std::string renderLists(const std::map<std::string, std::string>& m) {
    std::string out;
    for (const auto& kv : m) {
        if (!out.empty()) out += ", ";
        out += kv.first + " = " + kv.second;
    }
    return "{" + out + "}";
}
// 造型/声音：[造型1: assets/p1.svg, 造型2: assets/p2.svg]
inline std::string renderAssets(const std::map<std::string, std::string>& m) {
    std::string out;
    for (const auto& kv : m) {
        if (!out.empty()) out += ", ";
        out += kv.first;
        if (!kv.second.empty()) out += ": " + kv.second;
    }
    return "[" + out + "]";
}

// ---------------------------------------------------------------- 角色 meta

// §3.2 的属性默认值
inline std::map<std::string, std::string> defaultCharMeta(const std::string& spriteName,
                                                          bool isStage) {
    return {
        {"name",            spriteName},
        {"is_stage",        isStage ? "true" : "false"},
        {"x",               "0"},
        {"y",               "0"},
        {"size",            "100"},
        {"direction",       "90"},
        {"visible",         "true"},
        {"current_costume", "1"},
        {"rotation_style",  "all_around"},
        {"layer_order",     "1"},
        {"draggable",       "false"},
    };
}

struct CharMeta {
    bool exists = false;
    std::vector<std::string> lines;                      // 原文件全部行（保留未管字段）
    std::map<std::string, std::string> kv;
    std::set<std::string> variables, lists, broadcasts;
    std::map<std::string, std::string> costumes, sounds; // 名字 → 素材相对路径

    bool has(const std::string& k) const { return kv.count(k) != 0; }
    std::string get(const std::string& k) const {
        auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    }
};

inline CharMeta loadCharMeta(const std::string& path) {
    CharMeta m;
    if (!fileExists(path)) return m;
    m.exists = true;
    for (const std::string& raw : readLines(path)) {
        std::string s;
        bool inStr = false;
        for (char c : raw) {
            if (c == '"') { inStr = !inStr; s += c; continue; }
            if (c == '#' && !inStr) break;
            s += c;
        }
        s = trimStr(s);
        if (s.empty()) continue;
        size_t colon = s.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trimStr(s.substr(0, colon));
        std::string val = trimStr(s.substr(colon + 1));
        m.kv[key] = val;

        if (key == "variables" || key == "lists") {
            for (const auto& item : splitListBody(val)) {
                size_t eq = item.find('=');
                std::string nm = unquote(eq == std::string::npos ? item : item.substr(0, eq));
                if (!nm.empty()) (key == "variables" ? m.variables : m.lists).insert(nm);
            }
        } else if (key == "broadcasts") {
            for (const auto& nm : splitListBody(val)) {
                std::string n = unquote(nm);
                if (!n.empty()) m.broadcasts.insert(n);
            }
        } else if (key == "costumes" || key == "sounds") {
            for (const auto& item : splitListBody(val)) {
                size_t c2 = item.find(':');
                std::string nm  = trimStr(c2 == std::string::npos ? item : item.substr(0, c2));
                std::string pth = trimStr(c2 == std::string::npos ? "" : item.substr(c2 + 1));
                if (!nm.empty()) (key == "costumes" ? m.costumes : m.sounds)[unquote(nm)] = pth;
            }
        }
    }
    m.lines = readLines(path);
    return m;
}

// 是否存在"缺默认键"（§3.2 属性）。fix 靠它决定要不要重写一个没有新引用的文件。
inline bool charMetaNeedsDefaults(const CharMeta& m) {
    for (const auto& kv : defaultCharMeta(std::string(), false)) {
        if (kv.first == "name") continue;   // name 由调用方决定
        if (!m.has(kv.first)) return true;
    }
    return false;
}

inline void writeCharMeta(const std::string& path, const CharMeta& m,
                          const std::string& spriteName, bool isStage) {
    std::vector<std::string> out;
    auto defs = defaultCharMeta(spriteName, isStage);

    if (m.exists) {
        // 已存在：保留原顺序、原值；5 个集合字段统一在末尾重写
        for (const std::string& raw : m.lines) {
            std::string s = trimStr(raw);
            size_t colon = s.find(':');
            std::string key = (colon == std::string::npos) ? std::string()
                                                           : trimStr(s.substr(0, colon));
            if (key.empty()) { out.push_back(raw); continue; }
            if (key == "variables" || key == "lists" || key == "broadcasts" ||
                key == "costumes" || key == "sounds")
                continue;
            out.push_back(raw);
        }
        for (const auto& kv : defs)
            if (!m.has(kv.first)) out.push_back(kv.first + ": " + kv.second);
    } else {
        for (const auto& kv : defs) out.push_back(kv.first + ": " + kv.second);
    }
    out.push_back("costumes: "   + renderAssets(m.costumes));
    out.push_back("sounds: "     + renderAssets(m.sounds));
    out.push_back("variables: "  + renderNames(m.variables, true));
    out.push_back("lists: "      + renderNames(m.lists, true));
    out.push_back("broadcasts: " + renderNames(m.broadcasts, false));
    writeLines(path, out);
}

// ---------------------------------------------------------------- 根 meta

struct RootMeta {
    bool exists = false;
    std::string name;
    std::map<std::string, std::string> variables, lists;   // 名 → 初始值文本
    std::set<std::string> broadcasts;
    std::vector<std::string> extra;   // 其它段/未知行，原样保留
};

inline RootMeta loadRootMeta(const std::string& path) {
    RootMeta m;
    if (!fileExists(path)) return m;
    m.exists = true;
    std::string section;
    for (const std::string& raw : readLines(path)) {
        std::string s;
        bool inStr = false;
        for (char c : raw) {
            if (c == '"') { inStr = !inStr; s += c; continue; }
            if (c == '#' && !inStr) break;
            s += c;
        }
        s = trimStr(s);
        if (s.empty()) continue;

        if (s.front() == '[' && s.back() == ']') {
            std::string sec = trimStr(s.substr(1, s.size() - 2));
            section = (sec == "variables" || sec == "lists" || sec == "broadcasts") ? sec : "";
            if (section.empty()) m.extra.push_back(raw);
            continue;
        }
        size_t colon = s.find(':');
        if (section.empty() && colon != std::string::npos) {
            if (trimStr(s.substr(0, colon)) == "name") {
                m.name = unquote(trimStr(s.substr(colon + 1)));
                continue;
            }
            m.extra.push_back(raw);
            continue;
        }
        if (section.empty()) { m.extra.push_back(raw); continue; }

        if (section == "broadcasts") {
            std::string nm = unquote(s);
            if (!nm.empty()) m.broadcasts.insert(nm);
        } else {
            size_t eq = s.find('=');
            std::string nm  = unquote(eq == std::string::npos ? s : s.substr(0, eq));
            std::string val = trimStr(eq == std::string::npos ? "" : s.substr(eq + 1));
            if (!nm.empty()) (section == "variables" ? m.variables : m.lists)[nm] = val;
        }
    }
    return m;
}

inline void writeRootMeta(const std::string& path, const RootMeta& m) {
    std::vector<std::string> out;
    if (!m.name.empty()) { out.push_back("name: " + m.name); out.push_back(""); }
    if (!m.variables.empty()) {
        out.push_back("[variables]");
        for (const auto& kv : m.variables) out.push_back(kv.first + " = " + kv.second);
        out.push_back("");
    }
    if (!m.lists.empty()) {
        out.push_back("[lists]");
        for (const auto& kv : m.lists) out.push_back(kv.first + " = " + kv.second);
        out.push_back("");
    }
    if (!m.broadcasts.empty()) {
        out.push_back("[broadcasts]");
        for (const auto& b : m.broadcasts) out.push_back(b);
        out.push_back("");
    }
    for (const auto& e : m.extra) out.push_back(e);
    while (!out.empty() && out.back().empty()) out.pop_back();
    writeLines(path, out);
}

// ---------------------------------------------------------------- 项目定位

// 把用户输入（项目根 / character/1 / block.sbcli）解析成项目根。
// 与 sbcli_check.cpp 的逻辑一致：含 meta.sbcli 或 character/ 子目录即视为根。
inline std::string findProjectRoot(const std::string& input) {
    std::string in = norm(input);
    if (dirExists(in)) {
        if (dirExists(joinRel(in, "character")) || fileExists(joinRel(in, "meta.sbcli")))
            return in;
    } else if (fileExists(in)) {
        std::string ps = norm(fs::u8path(in).parent_path().u8string());
        for (int k = 0; k < 4; ++k) {
            if (fileExists(joinRel(ps, "meta.sbcli")) || dirExists(joinRel(ps, "character")))
                return ps;
            size_t sp = ps.find_last_of('/');
            if (sp == std::string::npos) break;
            ps = ps.substr(0, sp);
        }
    }
    // 可能直接指到了 character/ 或 character/1/
    std::string ps = in;
    for (int k = 0; k < 4; ++k) {
        if (fileExists(joinRel(ps, "meta.sbcli")) || dirExists(joinRel(ps, "character")))
            return ps;
        size_t sp = ps.find_last_of('/');
        if (sp == std::string::npos) break;
        ps = ps.substr(0, sp);
    }
    return std::string();
}

} // namespace meta
} // namespace sb
