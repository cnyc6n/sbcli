// src/commands.cpp —— 统一命令分发层
//
// 对应 sb.py 的第三部分（检测格式 → 分派到 s1_cmd_* / s3_cmd_*）。
#include "commands.hpp"
#include "find_impl.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <set>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sb {

using json = Json;

// ==========================================================================
// 格式探测（detect_format）
// ==========================================================================

std::string detectFormat(const std::string& path) {
    // 用 wide 路径打开读头部（中文路径下 std::ifstream 打不开）
    char head[10] = {0};
    size_t got = 0;
#ifdef _WIN32
    // UTF-8 → wide → CreateFileW 读 10 字节
    int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
    if (wn > 0) {
        std::wstring w(wn, 0);
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &w[0], wn);
        HANDLE h = ::CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD rd = 0;
            if (::ReadFile(h, head, 10, &rd, nullptr)) got = (size_t)rd;
            ::CloseHandle(h);
        }
    }
#else
    std::ifstream f(path, std::ios::binary);
    if (f) { f.read(head, 10); got = (size_t)f.gcount(); }
#endif
    if (got >= 10 && (std::memcmp(head, "ScratchV02", 10) == 0 ||
                      std::memcmp(head, "ScratchV01", 10) == 0))
        return "sb1";
    if (got >= 2 && head[0] == 'P' && head[1] == 'K')
        return "sb3";
    std::string ext = extname(path);
    if (ext == ".sb" || ext == ".sprite")
        return "sb1";
    return "sb3";
}

// ==========================================================================
// 输出小工具
// ==========================================================================

static void printJson(const json& j) {
    std::cout << j.dump(2) << "\n";
}

std::string jsonStr(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number_integer()) return std::to_string(j.get<long long>());
    if (j.is_number_float()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", j.get<double>());
        return buf;
    }
    return j.dump();
}

static std::string padRight(std::string s, size_t w) {
    if (s.size() >= w) return s;
    return s + std::string(w - s.size(), ' ');
}

static std::string padLeft(std::string s, size_t w) {
    if (s.size() >= w) return s;
    return std::string(w - s.size(), ' ') + s;
}

// ==========================================================================
// 统一命令
// ==========================================================================

int cmd_info(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1") return s1_cmd_info(a);

    // _info_sb3
    Sb3File sf = sb3LoadAny(a.file);
    json rows = sb3TargetRows(sf.targets);
    json info;
    info["file"] = basename(a.file);
    info["size"] = fileSize(a.file);
    info["format"] = sf.kind;
    json targets = json::array();
    for (auto& r : rows) {
        json t;
        t["name"] = r.value("name", "");
        t["isStage"] = r.value("isStage", false);
        t["costumes"] = r.value("costumes", 0);
        t["sounds"] = r.value("sounds", 0);
        t["blocks"] = r.value("blocks", 0);
        targets.push_back(std::move(t));
    }
    info["targets"] = std::move(targets);
    Elem d(sf.data);
    Elem ext = d.at("extensions");
    if (ext.is_array()) info["extensions"] = toJson(ext);
    else                info["extensions"] = json::array();
    Elem meta = d.at("meta");
    if (!meta.ok()) meta = d.at("info");
    if (meta.ok()) info["meta"] = toJson(meta);
    else           info["meta"] = json::object();

    if (a.json) { printJson(info); return 0; }

    std::cout << "文件：" << jsonStr(info["file"]) << "  （"
              << humanSize(info["size"].get<long long>()) << "，" << sf.kind << "）\n";
    if (!info["meta"].empty()) {
        std::cout << "工程信息：\n";
        for (auto it = info["meta"].begin(); it != info["meta"].end(); ++it) {
            std::cout << "    " << it.key() << " = ";
            const json& v = it.value();
            if (v.is_string()) std::cout << v.get<std::string>();
            else std::cout << v.dump();
            std::cout << "\n";
        }
    }
    std::cout << "\n目标（舞台/角色）：" << rows.size() << " 个\n";
    for (auto& r : rows) {
        std::string k = r.value("isStage", false) ? "舞台" : "角色";
        std::cout << "  [" << k << "] " << jsonStr(r.value("name", ""))
                  << "   （造型 " << r.value("costumes", 0)
                  << " · 声音 " << r.value("sounds", 0)
                  << " · 积木 " << r.value("blocks", 0) << "）\n";
    }
    if (info["extensions"].is_array() && !info["extensions"].empty()) {
        std::cout << "\n扩展：";
        bool first = true;
        for (auto& e : info["extensions"]) {
            if (!first) std::cout << "、";
            first = false;
            std::cout << jsonStr(e);
        }
        std::cout << "\n";
    }
    return 0;
}

int cmd_sprites(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1") return s1_cmd_sprites(a);
    return s3_cmd_sprites(a);
}

int cmd_text(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1") return s1_cmd_text(a);
    return s3_cmd_text(a);
}

int cmd_script(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1") return s1_cmd_script(a);
    return s3_cmd_script(a);
}

int cmd_vars(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1") return s1_cmd_vars(a);
    return s3_cmd_vars(a);
}

int cmd_assets(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1")
        return s1_cmd_media(a);   // 1.4 的造型是 Squeak 位图，解码成 PNG 导出
    return s3_cmd_assets(a);
}

int cmd_media(Args& a) {
    return cmd_assets(a);
}

int cmd_blocks(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1") {
        std::cerr << "提示：Scratch 1.4 的脚本不是块字典结构，暂不支持统计。\n";
        return 2;
    }
    return s3_cmd_blocks(a);
}

int cmd_raw(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1") return s1_cmd_raw(a);
    // sb3 没有 raw 命令，回退到 json
    a.raw = false;
    return s3_cmd_json(a);
}

int cmd_json(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1") {
        std::cerr << "提示：Scratch 1.4 没有 project.json，使用 `sb raw` 查看对象树。\n";
        return 2;
    }
    return s3_cmd_json(a);
}

int cmd_refs(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1")
        return s1_cmd_refs(a);
    return s3_cmd_refs(a);
}

int cmd_events(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1")
        return s1_cmd_events(a);
    return s3_cmd_events(a);
}

int cmd_diff(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1") {
        std::cerr << "提示：diff 版本对比目前支持 Scratch 2/3（.sb3/.sb2），"
                  << "Scratch 1.4 暂未实现。\n";
        return 2;
    }
    return s3_cmd_diff(a);
}

int cmd_dup(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sb1") {
        std::cerr << "提示：dup 相似角色检测目前支持 Scratch 2/3（.sb3/.sb2），"
                  << "Scratch 1.4 暂未实现。\n";
        return 2;
    }
    return s3_cmd_dup(a);
}

// ---- ls：统一扫描（两类都列）----

static std::vector<std::string> findSb1Files(const std::string& root) {
    return walkFiles(root, {".sb", ".sprite"});
}

static json sb1LsRow(const std::string& p) {
    json r;
    r["path"] = p;
    r["name"] = basename(p);
    r["size"] = fileSize(p);
    r["mtime"] = fileMtime(p);
    r["format"] = "sb1";
    try {
        Sb1File f = sb1Load(p);
        Value root = f.table->get(1);
        auto targets = findTargets(root);
        int sprites = 0, costumes = 0, scripts = 0;
        for (auto& t : targets) {
            if (!t.isStage) ++sprites;
            std::vector<Media> images, sounds;
            targetMedia(t, images, sounds);
            costumes += (int)images.size();
            scripts += (int)targetScripts(t).size();
        }
        r["sprites"] = sprites;
        r["blocks"] = nullptr;
        r["costumes"] = costumes;
        r["scripts"] = scripts;
    } catch (const Sb1Error& e) {
        r["error"] = e.what();
    }
    return r;
}

int cmd_ls(Args& a) {
    std::vector<std::string> files1 = findSb1Files(a.path);
    std::vector<std::string> files3 = findSb3Files(a.path);
    std::set<std::string> seen;
    std::vector<std::string> files;
    for (auto& p : files1) if (seen.insert(p).second) files.push_back(p);
    for (auto& p : files3) if (seen.insert(p).second) files.push_back(p);

    if (files.empty()) {
        std::cerr << "这里没有找到 .sb / .sprite / .sb3 / .sb2 / .sprite3 文件\n";
        return 2;
    }

    json rows = json::array();
    for (auto& p : files) {
        std::string fmt = detectFormat(p);
        if (fmt == "sb1") {
            rows.push_back(sb1LsRow(p));
        } else {
            try {
                json r = sb3Summarize(p);
                r["format"] = "sb3";
                rows.push_back(std::move(r));
            } catch (const Sb3Error& e) {
                json r;
                r["path"] = p;
                r["name"] = basename(p);
                r["error"] = e.what();
                r["size"] = fileSize(p);
                r["mtime"] = fileMtime(p);
                r["format"] = "sb3";
                rows.push_back(std::move(r));
            }
        }
    }

    // 排序
    if (a.sort == "name") {
        std::sort(rows.begin(), rows.end(), [](const json& x, const json& y) {
            return jsonStr(x.value("name", "")) < jsonStr(y.value("name", ""));
        });
    } else if (a.sort == "size") {
        std::sort(rows.begin(), rows.end(), [](const json& x, const json& y) {
            return x.value("size", (long long)0) > y.value("size", (long long)0);
        });
    } else if (a.sort == "time") {
        std::sort(rows.begin(), rows.end(), [](const json& x, const json& y) {
            return x.value("mtime", (long long)0) > y.value("mtime", (long long)0);
        });
    } else if (a.sort == "blocks") {
        std::sort(rows.begin(), rows.end(), [](const json& x, const json& y) {
            long long bx = x.contains("blocks") && !x["blocks"].is_null()
                               ? x["blocks"].get<long long>() : 0;
            long long by = y.contains("blocks") && !y["blocks"].is_null()
                               ? y["blocks"].get<long long>() : 0;
            return bx > by;
        });
    }

    if (a.json) { printJson(rows); return 0; }

    std::cout << "目录：" << a.path << "\n";
    std::cout << "共 " << rows.size() << " 个作品\n\n";
    std::cout << padLeft("大小", 8) << "  " << padRight("修改时间", 17) << " "
              << padRight("格式", 4) << " " << padLeft("角色", 4) << " "
              << padLeft("积木", 7) << " " << padLeft("造型", 4) << " 名称\n";
    std::cout << std::string(82, '-') << "\n";
    int ok = 0;
    long long totBlocks = 0, totSprites = 0;
    for (auto& r : rows) {
        std::string ts = formatTime(r.value("mtime", (long long)0));
        if (r.contains("error")) {
            std::cout << padLeft(humanSize(r.value("size", (long long)0)), 8) << "  "
                      << padRight(ts, 17) << " "
                      << padRight(jsonStr(r.value("format", "?")), 4) << " "
                      << padLeft("--", 4) << " " << padLeft("--", 7) << " "
                      << padLeft("--", 4) << " " << jsonStr(r.value("name", ""))
                      << "  ← " << jsonStr(r["error"]) << "\n";
            continue;
        }
        ++ok;
        std::string blocks = (r.contains("blocks") && !r["blocks"].is_null())
                                 ? std::to_string(r["blocks"].get<long long>()) : "--";
        if (r.contains("blocks") && !r["blocks"].is_null())
            totBlocks += r["blocks"].get<long long>();
        totSprites += r.value("sprites", (long long)0);
        std::cout << padLeft(humanSize(r.value("size", (long long)0)), 8) << "  "
                  << padRight(ts, 17) << " "
                  << padRight(jsonStr(r.value("format", "?")), 4) << " "
                  << padLeft(std::to_string(r.value("sprites", (long long)0)), 4) << " "
                  << padLeft(blocks, 7) << " "
                  << padLeft(std::to_string(r.value("costumes", (long long)0)), 4)
                  << " " << jsonStr(r.value("name", "")) << "\n";
    }
    std::cout << std::string(82, '-') << "\n";
    std::cout << "合计：" << ok << " 个可读作品，" << totBlocks
              << " 块积木（仅 Scratch 2/3），" << totSprites << " 个角色\n";
    return 0;
}

int cmd_find(Args& a) {
    std::string kw = a.keyword;
    std::transform(kw.begin(), kw.end(), kw.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });

    std::vector<std::string> files1 = findSb1Files(a.dir);
    std::vector<std::string> files3 = findSb3Files(a.dir);
    auto filter = [&](std::vector<std::string>& v) {
        if (a.fileFilter.empty()) return;
        std::string f = a.fileFilter;
        std::transform(f.begin(), f.end(), f.begin(),
                       [](unsigned char c) { return (char)::tolower(c); });
        std::vector<std::string> keep;
        for (auto& p : v) {
            std::string b = basename(p);
            std::transform(b.begin(), b.end(), b.begin(),
                           [](unsigned char c) { return (char)::tolower(c); });
            if (b.find(f) != std::string::npos) keep.push_back(p);
        }
        v = std::move(keep);
    };
    filter(files1);
    filter(files3);

    std::vector<FindHit> hits;
    // ---- 多线程搜索：按 --jobs 分片（默认 4，与 sb.py 一致）----
    int jobs = a.jobs > 0 ? a.jobs : 4;
    // 交替混合两类文件，尽量均摊；去重（.sb 会被两类同时匹配）
    std::vector<std::string> files;
    files.reserve(files1.size() + files3.size());
    std::set<std::string> seenFiles;
    size_t n = std::max(files1.size(), files3.size());
    for (size_t i = 0; i < n; ++i) {
        if (i < files1.size() && seenFiles.insert(files1[i]).second)
            files.push_back(files1[i]);
        if (i < files3.size() && seenFiles.insert(files3[i]).second)
            files.push_back(files3[i]);
    }
    size_t nFiles = files.size();
    if (jobs > 1 && nFiles > 8) {
        std::vector<std::vector<FindHit>> results(jobs);
        std::vector<std::thread> threads;
        threads.reserve(jobs);
        for (int w = 0; w < jobs; ++w) {
            threads.emplace_back([&, w]() {
                for (size_t i = w; i < nFiles; i += jobs) {
                    const std::string& p = files[i];
                    auto h = isSb1File(p) ? sb1FindInFile(p, kw, a.script)
                                          : sb3FindInFile(p, kw, a.script);
                    auto& out = results[w];
                    out.insert(out.end(), h.begin(), h.end());
                }
            });
        }
        for (auto& t : threads) t.join();
        for (auto& r : results)
            hits.insert(hits.end(), r.begin(), r.end());
    } else {
        for (auto& p : files) {
            auto h = isSb1File(p) ? sb1FindInFile(p, kw, a.script)
                                  : sb3FindInFile(p, kw, a.script);
            hits.insert(hits.end(), h.begin(), h.end());
        }
    }
    // 确定性排序：文件 → 目标 → 标签 → 文本
    std::sort(hits.begin(), hits.end(), [](const FindHit& x, const FindHit& y) {
        if (x.file != y.file) return x.file < y.file;
        if (x.target != y.target) return x.target < y.target;
        if (x.where != y.where) return x.where < y.where;
        return x.text < y.text;
    });

    if (a.json) {
        json out;
        out["keyword"] = a.keyword;
        out["hits"] = json::array();
        for (auto& h : hits) {
            json j;
            j["file"] = basename(h.file);
            j["target"] = h.target;
            j["where"] = h.where;
            j["text"] = h.text;
            out["hits"].push_back(std::move(j));
        }
        printJson(out);
        return 0;
    }

    std::cout << "搜索「" << a.keyword << "」，命中 " << hits.size() << " 处\n\n";
    std::string cur;
    size_t shown = 0;
    for (auto& h : hits) {
        if (shown >= (size_t)a.limit) break;
        ++shown;
        if (h.file != cur) {
            cur = h.file;
            std::cout << "◆ " << basename(h.file) << "\n";
        }
        std::cout << "    [" << h.target << " · " << h.where << "] "
                  << truncate(h.text, 90) << "\n";
    }
    if (hits.size() > (size_t)a.limit)
        std::cout << "\n…还有 " << (hits.size() - (size_t)a.limit)
                  << " 处（--limit 调整）\n";
    return 0;
}

} // namespace sb
