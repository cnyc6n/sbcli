// src/sb3_cmds.cpp —— Scratch 2/3 命令（读取走 simdjson DOM）
#include "sb3.hpp"
#include "sb3_tables.hpp"
#include "sb3_internal.hpp"
#include "commands.hpp"    // Args 完整定义
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cctype>
#include <sstream>

namespace sb {
using json = Json;
// ==========================================================================
// 命令（对照 sb.py 的 s3_cmd_*）
// ==========================================================================

void sb3JsonOut(const json& j) { std::cout << j.dump(2) << "\n"; }

std::string sb3View(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) { char b[64]; std::snprintf(b, sizeof(b), "%g", v.get<double>()); return b; }
    return v.dump();
}

// DOM 版本（用于扩展名、变量值等从元素直接取文本）
std::string sb3View(const Elem& v) {
    if (v.is_string()) return std::string(v.sv());
    if (v.is_integer()) return std::to_string(v.i64());
    if (v.is_float()) { char b[64]; std::snprintf(b, sizeof(b), "%g", v.f64()); return b; }
    if (v.ok()) return compactJson(v.raw());
    return "";
}

// script 命令的 --limit 语义：最多显示 N 个完整脚本（0 = 全部）。
// 脚本内部行数不限（不再把脚本砍在半块积木里）。
int sb3ScriptBudget(const Args& a) {
    return (a.limitSet && a.limit > 0) ? a.limit : 0;
}

int s3_cmd_sprites(const Args& a) {
    Sb3File sf = sb3LoadAny(a.file);
    json rows = sb3TargetRows(sf.targets);
    if (a.json) {
        json out;
        out["file"] = a.file;
        out["kind"] = sf.kind;
        out["targets"] = std::move(rows);
        sb3JsonOut(out);
        return 0;
    }
    std::cout << "文件：" << basename(a.file) << "  （" << humanSize(fileSize(a.file))
              << "，" << sf.kind << "）\n";
    int nStage = 0, nSprite = 0;
    for (auto& r : rows) if (r.value("isStage", false)) ++nStage; else ++nSprite;
    std::cout << "共 " << rows.size() << " 个目标：" << nStage << " 个舞台 + "
              << nSprite << " 个角色\n\n";
    long long totB = 0, totS = 0, totC = 0, totSounds = 0;
    for (auto& r : rows) {
        std::string kind = r.value("isStage", false) ? "舞台" : "角色";
        if (!r.value("isStage", false)) {
            std::cout << "  " << r["name"].dump() << "\n";
        } else {
            std::cout << "[" << kind << "] " << r["name"].dump() << "\n";
        }
        std::cout << "       造型 " << r.value("costumes", 0)
                  << " · 声音 " << r.value("sounds", 0)
                  << " · 变量 " << r.value("variables", 0)
                  << " · 列表 " << r.value("lists", 0)
                  << " · 积木 " << r.value("blocks", 0)
                  << " · 脚本 " << r.value("scripts", 0) << "\n";
        totB += r.value("blocks", 0LL);
        totS += r.value("scripts", 0LL);
        totC += r.value("costumes", 0LL);
        totSounds += r.value("sounds", 0LL);
    }
    Elem ext = Elem(sf.data).at("extensions");
    if (ext.is_array() && !ext.empty()) {
        std::cout << "\n用到的扩展：";
        bool first = true;
        for (auto e : ext.arr()) {
            if (!first) std::cout << "、";
            first = false;
            std::cout << sb3View(Elem(e));
        }
        std::cout << "\n";
    }
    std::cout << "\n合计：" << nSprite << " 个角色，" << totB << " 块积木，"
              << totS << " 段脚本，" << totC << " 个造型，" << totSounds << " 个声音\n";
    return 0;
}

int s3_cmd_text(const Args& a) {
    Sb3File sf = sb3LoadAny(a.file);
    const Elem& targets = sf.targets;
    auto names = sb3NameLists(targets);
    json out;
    out["file"] = a.file;
    out["kind"] = sf.kind;
    json dg = json::object(), cm = json::object(), st = json::object();
    if (targets.is_array()) {
        for (auto te : targets.arr()) {
            Elem t(te);
            if (!t.is_object()) continue;
            std::string nm = std::string(t.at("name").sv());
            Sb3Collect res = sb3CollectText(t, a.strings);
            if (!res.dialog.empty()) {
                json arr = json::array();
                for (auto& kv : res.dialog) arr.push_back(json::array({kv.first, kv.second}));
                dg[nm] = std::move(arr);
            }
            if (!res.comments.empty()) {
                json arr = json::array();
                for (auto& c : res.comments) arr.push_back(c);
                cm[nm] = std::move(arr);
            }
            if (!res.strings.empty()) {
                json arr = json::array();
                for (auto& s : res.strings) {
                    const std::string& txt = std::get<2>(s);
                    // --drop-numbers：过滤纯数字/符号串（如 "2"、"-44"、"1.5"），
                    // 只保留含字母/中文/空格的“可读文字”
                    if (a.dropNumbers) {
                        bool readable = false;
                        for (unsigned char c : txt) {
                            if (c >= 0x80 || std::isalpha(c)) { readable = true; break; }
                        }
                        if (!readable) continue;
                    }
                    arr.push_back(json::array({std::get<0>(s), std::get<1>(s), txt}));
                }
                if (!arr.empty()) st[nm] = std::move(arr);
            }
        }
    }
    out["names"] = names;
    out["dialog"] = dg;
    out["comments"] = cm;
    out["strings"] = st;

    if (a.json) { sb3JsonOut(out); return 0; }

    std::cout << "文件：" << basename(a.file) << "  （" << sf.kind << "）\n\n";
    if (!dg.empty()) {
        std::cout << "──── 对话内容（说 / 思考 / 询问）\n";
        for (auto it = dg.begin(); it != dg.end(); ++it) {
            std::cout << "  ▍" << it.key() << "\n";
            for (auto& item : it.value())
                std::cout << "      " << item[0].dump() << "：" << truncate(sb3View(item[1]), 70) << "\n";
        }
        std::cout << "\n";
    }
    if (!a.dialogOnly) {
        std::cout << "──── 名称\n";
        for (auto& key : {"角色", "造型", "声音", "变量", "列表", "广播"}) {
            auto& vals = names[key];
            if (vals.empty()) continue;
            std::cout << "  " << key << "（" << vals.size() << "）：";
            for (size_t i = 0; i < vals.size() && i < (size_t)a.limit; ++i) {
                if (i) std::cout << "、";
                std::cout << vals[i];
            }
            if (vals.size() > (size_t)a.limit) std::cout << "…";
            std::cout << "\n";
        }
        std::cout << "\n";
        if (!cm.empty()) {
            std::cout << "──── 注释\n";
            for (auto it = cm.begin(); it != cm.end(); ++it) {
                std::cout << "  ▍" << it.key() << "\n";
                for (auto& c : it.value())
                    std::cout << "      " << truncate(c.dump(), 80) << "\n";
            }
            std::cout << "\n";
        }
        if (!st.empty()) {
            std::cout << "──── 积木里的文字\n";
            for (auto it = st.begin(); it != st.end(); ++it) {
                std::cout << "  ▍" << it.key() << "\n";
                size_t shown = 0;
                for (auto& item : it.value()) {
                    if (shown >= (size_t)a.limit) break;
                    ++shown;
                    std::cout << "      [" << item[0].dump() << "." << item[1].dump()
                              << "] " << truncate(sb3View(item[2]), 60) << "\n";
                }
            }
            std::cout << "\n";
        }
    }
    long long nd = 0, nc = 0;
    for (auto it = dg.begin(); it != dg.end(); ++it) nd += (long long)it.value().size();
    for (auto it = cm.begin(); it != cm.end(); ++it) nc += (long long)it.value().size();
    std::cout << "合计：" << nd << " 条对话/提示，" << nc << " 条注释，"
              << names["角色"].size() << " 个角色\n";
    return 0;
}

int s3_cmd_script(const Args& a) {
    Sb3File sf = sb3LoadAny(a.file);
    const Elem& targets = sf.targets;
    // 不拷贝整个 targets 数组：只有 --sprite 过滤时才构造过滤列表
    std::vector<Elem> keep;
    bool filtered = false;
    if (!a.sprite.empty()) {
        std::string want = a.sprite;
        std::transform(want.begin(), want.end(), want.begin(),
                       [](unsigned char c) { return (char)::tolower(c); });
        if (targets.is_array()) {
            for (auto te : targets.arr()) {
                Elem t(te);
                if (!t.is_object()) continue;
                std::string nm = std::string(t.at("name").sv());
                std::transform(nm.begin(), nm.end(), nm.begin(),
                               [](unsigned char c) { return (char)::tolower(c); });
                if (nm == want) keep.push_back(t);
            }
        }
        if (keep.empty()) {
            std::cerr << "没有叫「" << a.sprite << "」的角色\n";
            return 2;
        }
        filtered = true;
    }
    auto forEachTarget = [&](auto&& fn) {
        if (filtered) {
            for (auto& t : keep) fn(t);
        } else if (targets.is_array()) {
            for (auto te : targets.arr()) fn(Elem(te));
        }
    };

    if (a.json) {
        json payload = json::array();
        forEachTarget([&](const Elem& t) {
            if (!t.is_object()) return;
            Renderer r(t);
            json scripts = json::array();
            for (auto& lines : r.scripts(0, (size_t)sb3ScriptBudget(a))) {
                std::string joined;
                for (auto& ln : lines) {
                    if (!joined.empty()) joined += "\n";
                    joined += ln;
                }
                scripts.push_back(joined);
            }
            json x;
            x["name"] = std::string(t.at("name").sv());
            x["scripts"] = std::move(scripts);
            payload.push_back(std::move(x));
        });
        json out;
        out["file"] = a.file;
        out["targets"] = std::move(payload);
        sb3JsonOut(out);
        return 0;
    }

    std::ostringstream out;
    out << "文件：" << basename(a.file) << "  （" << sf.kind << "）\n";
    forEachTarget([&](const Elem& t) {
        if (!t.is_object()) return;
        Renderer r(t);
        std::string flag = t.at("isStage").b() ? "（舞台）" : "";
        out << "\n═══ 角色：" << std::string(t.at("name").sv()) << flag << " ═══\n";
        // --limit N：最多渲染 N 个完整脚本（行内不限，不再砍半块）
        auto scripts = r.scripts(0, (size_t)sb3ScriptBudget(a));
        if (scripts.empty()) { out << "  （没有脚本）\n"; return; }
        int totalScripts = sb3TopScriptCount(t);
        int i = 1;
        for (auto& lines : scripts) {
            out << "\n  ── 脚本 " << i << " ──\n";
            for (auto& ln : lines) out << "  " << ln << "\n";
            ++i;
        }
        if (a.limitSet && a.limit > 0 && (int)scripts.size() < totalScripts)
            out << "\n  …还有 " << (totalScripts - (int)scripts.size())
                << " 个脚本（用 --limit 调整）\n";
        if (!r.missing.empty()) {
            out << "\n  ※ 未收录的积木（共 " << r.missing.size() << " 种、";
            long long total = 0;
            for (auto& kv : r.missing) total += kv.second;
            out << total << " 次），原始 opcode：\n";
            if (a.showUnknown) {
                for (auto& kv : r.missing)
                    out << "      " << kv.second << "×  " << kv.first << "\n";
            } else {
                std::string joined;
                int n = 0;
                for (auto& kv : r.missing) {
                    if (n >= 6) break;
                    if (!joined.empty()) joined += "、";
                    joined += kv.first + "×" + std::to_string(kv.second);
                    ++n;
                }
                out << "  ※ 未收录的积木（原样显示）：" << joined;
                if (r.missing.size() > 6)
                    out << "（还有 " << r.missing.size() - 6
                        << " 种，用 --show-unknown 看全）";
                out << "\n";
            }
        }
    });
    std::cout << out.str();
    return 0;
}

int s3_cmd_blocks(const Args& a) {
    Sb3File sf = sb3LoadAny(a.file);
    const Elem& targets = sf.targets;
    std::map<std::string, long long> per;
    std::map<std::string_view, long long> total;
    std::map<std::string, std::map<std::string_view, long long>> perTarget;
    if (targets.is_array()) {
        for (auto te : targets.arr()) {
            Elem t(te);
            if (!t.is_object()) continue;
            std::string nm = std::string(t.at("name").sv());
            Elem bl = t.at("blocks");
            if (!bl.is_object()) continue;
            for (auto f : bl.obj()) {
                Elem b(f.value);
                if (!b.is_object()) continue;
                if (b.at("shadow").b()) continue;
                std::string_view op = b.at("opcode").sv();
                perTarget[nm][op]++;
                total[op]++;
            }
        }
    }
    long long sum = 0;
    for (auto& kv : total) {
        sum += kv.second;
        per[std::string(kv.first)] = kv.second;
    }
    if (a.json) {
        json out;
        out["file"] = a.file;
        out["total"] = total;
        out["perTarget"] = perTarget;
        sb3JsonOut(out);
        return 0;
    }
    std::cout << "文件：" << basename(a.file) << "\n";
    std::cout << "积木总数：" << sum << "（" << total.size() << " 种）\n\n";
    std::cout << "──── 按种类排序\n";
    std::vector<std::pair<long long, std::string>> sorted;
    for (auto& kv : per) sorted.emplace_back(kv.second, kv.first);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& x, const auto& y) { return x.first > y.first; });
    size_t nShown = 0;
    for (auto& kv : sorted) {
        if (nShown >= (size_t)a.limit) break;
        ++nShown;
        std::string label;
        const std::string* tplPtr = nullptr;
        auto it = SB3_T.find(kv.second);
        if (it != SB3_T.end()) tplPtr = &it->second;
        else {
            auto t2 = SB2_T.find(kv.second);
            if (t2 != SB2_T.end()) tplPtr = &t2->second;
        }
        if (tplPtr != nullptr) {
            // 把 {xxx} 占位符统一替换成 …（手写扫描）
            label = *tplPtr;
            size_t q = 0;
            while (true) {
                size_t o = label.find('{', q);
                if (o == std::string::npos) break;
                size_t c = label.find('}', o);
                if (c == std::string::npos) break;
                label.replace(o, c - o + 1, "…");
                q = o + 1;
            }
        }
        std::cout << "  " << std::setw(6) << kv.first << "  " << kv.second;
        if (!label.empty()) std::cout << "  " << label;
        std::cout << "\n";
    }
    if (a.bySprite) {
        for (auto& kv : perTarget) {
            if (kv.second.empty()) continue;
            long long s = 0;
            for (auto& op : kv.second) s += op.second;
            std::cout << "\n──── 角色：" << kv.first << "（" << s << " 块）\n";
            std::vector<std::pair<long long, std::string>> arr;
            for (auto& op : kv.second) arr.emplace_back(op.second, op.first);
            std::sort(arr.begin(), arr.end(),
                      [](const auto& x, const auto& y) { return x.first > y.first; });
            for (size_t i = 0; i < arr.size() && i < 10; ++i)
                std::cout << "  " << std::setw(6) << arr[i].first << "  "
                          << arr[i].second << "\n";
        }
    }
    return 0;
}

int s3_cmd_vars(const Args& a) {
    Sb3File sf = sb3LoadAny(a.file);
    const Elem& targets = sf.targets;
    json out = json::object();
    if (targets.is_array()) {
        for (auto te : targets.arr()) {
            Elem t(te);
            if (!t.is_object()) continue;
            std::string nm = std::string(t.at("name").sv());
            json entry;
            entry["variables"] = json::array();
            entry["lists"] = json::array();
            Elem variables = t.at("variables");
            if (variables.is_object()) {
                for (auto& vk : sortedKeysOf(variables.obj())) {
                    Elem v = variables.at(vk);
                    json x;
                    if (v.is_array() && !v.empty()) {
                        Elem n0 = v.op(0);
                        x["name"] = n0.ok() ? compactJson(n0.raw()) : "";
                    } else {
                        x["name"] = "";
                    }
                    if (v.is_array() && v.size() > 1) x["value"] = toJson(Elem(v.op(1)));
                    else                               x["value"] = json(nullptr);
                    entry["variables"].push_back(std::move(x));
                }
            }
            Elem lists = t.at("lists");
            if (lists.is_object()) {
                for (auto& lk : sortedKeysOf(lists.obj())) {
                    Elem v = lists.at(lk);
                    json x;
                    if (v.is_array() && !v.empty()) {
                        Elem n0 = v.op(0);
                        x["name"] = n0.ok() ? compactJson(n0.raw()) : "";
                    } else {
                        x["name"] = "";
                    }
                    if (v.is_array() && v.size() > 1) x["values"] = toJson(Elem(v.op(1)));
                    else                               x["values"] = json::array();
                    entry["lists"].push_back(std::move(x));
                }
            }
            if (!entry["variables"].empty() || !entry["lists"].empty())
                out[nm] = std::move(entry);
        }
    }
    if (a.json) {
        json r;
        r["file"] = a.file;
        r["targets"] = std::move(out);
        sb3JsonOut(r);
        return 0;
    }
    std::cout << "文件：" << basename(a.file) << "\n\n";
    if (out.empty()) { std::cout << "这个作品没有全局/角色变量。\n"; return 0; }
    for (auto it = out.begin(); it != out.end(); ++it) {
        std::cout << "──── 角色：" << it.key() << "\n";
        for (auto& v : it.value()["variables"]) {
            std::string val = sb3View(v["value"]);
            if (!a.full) val = truncate(val, 60);
            std::cout << "  变量 " << sb3View(v["name"]) << " = " << val << "\n";
        }
        for (auto& l : it.value()["lists"]) {
            std::string vals;
            if (l["values"].is_array()) {
                for (auto& x : l["values"]) {
                    if (!vals.empty()) vals += "、";
                    vals += sb3View(x);
                }
            }
            std::cout << "  列表 " << sb3View(l["name"]) << "（" << l["values"].size()
                      << " 项）\n";
            if (a.full) {
                int i = 1;
                for (auto& x : l["values"])
                    std::cout << "      " << i++ << ". " << truncate(sb3View(x), 80) << "\n";
            } else {
                std::cout << "      " << truncate(vals, 100) << "\n";
            }
        }
        std::cout << "\n";
    }
    return 0;
}

int s3_cmd_assets(const Args& a) {
    Sb3File sf = sb3LoadAny(a.file);
    const Elem& targets = sf.targets;
    json rows = json::array();
    struct AssetRow { std::string k, t, n, f; std::vector<std::string> candidates; };
    std::vector<AssetRow> rowsV;
    if (targets.is_array()) {
        for (auto te : targets.arr()) {
            Elem t(te);
            if (!t.is_object()) continue;
            std::string tn = std::string(t.at("name").sv());
            Elem costumes = t.at("costumes");
            if (costumes.is_array()) {
                for (auto ce : costumes.arr()) {
                    Elem c(ce);
                    std::string fmt = std::string(c.at("dataFormat").sv());
                    if (fmt.empty()) fmt = "?";
                    rowsV.push_back({"造型", tn, std::string(c.at("name").sv()),
                                     fmt, sb3AssetFile(c, "costume")});
                }
            }
            Elem sounds = t.at("sounds");
            if (sounds.is_array()) {
                for (auto se : sounds.arr()) {
                    Elem s(se);
                    std::string fmt = std::string(s.at("dataFormat").sv());
                    if (fmt.empty()) fmt = "?";
                    rowsV.push_back({"声音", tn, std::string(s.at("name").sv()),
                                     fmt, sb3AssetFile(s, "sound")});
                }
            }
        }
    }
    auto names = sf.zip->names();
    std::map<std::string, std::string> lower;
    for (auto& n : names) { std::string l = n; for (auto& c : l) c = (char)::tolower((unsigned char)c); lower[l] = n; }

    if (a.json) {
        json arr = json::array();
        for (auto& r : rowsV) {
            json x;
            x["kind"] = r.k;
            x["target"] = r.t;
            x["name"] = r.n;
            x["format"] = r.f;
            x["zipEntries"] = r.candidates;
            x["zipEntry"] = sb3ResolveEntry(lower, r.candidates);
            arr.push_back(std::move(x));
        }
        json out;
        out["file"] = a.file;
        out["assets"] = std::move(arr);
        sb3JsonOut(out);
        return 0;
    }

    std::cout << "文件：" << basename(a.file) << "\n";
    int nImg = 0, nSnd = 0;
    for (auto& r : rowsV) (r.k == "造型") ? ++nImg : ++nSnd;
    std::cout << "共 " << rowsV.size() << " 个素材（" << nImg << " 造型 / "
              << nSnd << " 声音）\n\n";
    int missing = 0;
    for (auto& r : rowsV) {
        std::string real = sb3ResolveEntry(lower, r.candidates);
        std::string size;
        if (!real.empty()) {
            try { size = "  " + humanSize((long long)sf.zip->entrySize(real)); }
            catch (...) { size = ""; }
        } else {
            size = "  ← 压缩包里找不到";
            ++missing;
        }
        std::cout << "  [" << r.k << "] " << r.t << "/" << r.n << "  (" << r.f << ")" << size << "\n";
    }
    if (missing) std::cout << "\n  注意：有 " << missing << " 个素材在压缩包里找不到对应文件。\n";
    if (!a.extract.empty()) {
        std::string outdir = a.extract;
        int cnt = 0;
        for (auto& r : rowsV) {
            std::string real = sb3ResolveEntry(lower, r.candidates);
            if (real.empty()) continue;
            std::string sub = joinPath(joinPath(outdir, sanitizeFilename(r.t)),
                                       r.k == "造型" ? "造型" : "声音");
            makeDirs(sub);
            size_t pos = real.find_last_of('.');
            std::string ext = (pos != std::string::npos) ? real.substr(pos) : std::string();
            if (ext.empty() && r.f == "svg") ext = ".svg";
            std::string dst = joinPath(sub, sanitizeFilename(r.n + ext));
            try { sf.zip->writeToFile(real, dst); ++cnt; } catch (...) {}
        }
        std::cout << "\n已导出 " << cnt << " 个文件到：" << outdir << "\n";
        std::cout << "   目录结构：<角色>/造型/…  <角色>/声音/…\n";
    }
    return 0;
}

int s3_cmd_json(const Args& a) {
    Sb3File sf = sb3LoadAny(a.file);
    if (a.raw) {
        std::string entry = "project.json";
        auto names = sf.zip->names();
        bool found = false;
        for (auto& n : names) {
            std::string l = n;
            for (auto& c : l) c = (char)::tolower((unsigned char)c);
            if (l == "project.json") { entry = n; found = true; break; }
        }
        if (!found) {
            for (auto& n : names) {
                std::string l = n;
                for (auto& c : l) c = (char)::tolower((unsigned char)c);
                if (l.size() > 5 && l.substr(l.size() - 5) == ".json") { entry = n; found = true; break; }
            }
        }
        if (found) std::cout << sf.zip->readText(entry) << "\n";
        return 0;
    }
    std::cout << prettyJson(sf.data, 2) << "\n";
    return 0;
}

int s3_cmd_ls(const Args& a) {
    auto files = findSb3Files(a.path);
    if (files.empty()) {
        std::cerr << "这里没有找到 .sb3 / .sb2 / .sprite3 / 改名的 .sb 文件\n";
        return 2;
    }
    json rows = json::array();
    for (auto& p : files) {
        try {
            rows.push_back(sb3Summarize(p));
        } catch (const Sb3Error& e) {
            json r;
            r["path"] = p; r["name"] = basename(p); r["error"] = e.what();
            r["size"] = fileSize(p); r["mtime"] = fileMtime(p);
            rows.push_back(std::move(r));
        }
    }
    if (a.sort == "name") {
        std::sort(rows.begin(), rows.end(), [](const json& x, const json& y) {
            return sb3View(x.value("name", "")) < sb3View(y.value("name", ""));
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
            return x.value("blocks", (long long)0) > y.value("blocks", (long long)0);
        });
    }
    if (a.json) { sb3JsonOut(rows); return 0; }
    std::cout << "目录：" << a.path << "（Scratch 2/3）\n";
    std::cout << "共 " << rows.size() << " 个作品\n\n";
    std::cout << "    大小  修改时间          角色     积木  造型 名称\n";
    std::cout << std::string(78, '-') << "\n";
    int ok = 0;
    long long totB = 0, totS = 0;
    for (auto& r : rows) {
        std::string ts = formatTime(r.value("mtime", (long long)0));
        if (r.contains("error")) {
            std::cout << std::setw(8) << humanSize(r.value("size", (long long)0)) << "  "
                      << ts << "  " << std::setw(4) << "--" << " " << std::setw(7)
                      << "--" << " " << std::setw(4) << "--" << " "
                      << sb3View(r.value("name", "")) << "  ← " << sb3View(r["error"]) << "\n";
            continue;
        }
        ++ok;
        totB += r.value("blocks", 0LL);
        totS += r.value("sprites", 0LL);
        std::cout << std::setw(8) << humanSize(r.value("size", (long long)0)) << "  "
                  << ts << "  " << std::setw(4) << r.value("sprites", 0LL) << " "
                  << std::setw(7) << r.value("blocks", 0LL) << " "
                  << std::setw(4) << r.value("costumes", 0LL) << " "
                  << sb3View(r.value("name", "")) << "\n";
    }
    std::cout << std::string(78, '-') << "\n";
    std::cout << "合计：" << ok << " 个可读作品，" << totB << " 块积木，"
              << totS << " 个角色\n";
    return 0;
}

int s3_cmd_find(const Args& a) {
    auto files = findSb3Files(a.dir);
    if (!a.fileFilter.empty()) {
        std::string f = a.fileFilter;
        std::transform(f.begin(), f.end(), f.begin(),
                       [](unsigned char c) { return (char)::tolower(c); });
        std::vector<std::string> keep;
        for (auto& p : files) {
            std::string b = basename(p);
            std::transform(b.begin(), b.end(), b.begin(),
                           [](unsigned char c) { return (char)::tolower(c); });
            if (b.find(f) != std::string::npos) keep.push_back(p);
        }
        files = std::move(keep);
    }
    std::string kw = a.keyword;
    std::transform(kw.begin(), kw.end(), kw.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });

    struct Hit { std::string p, t, w, v; };
    std::vector<Hit> hits;
    for (auto& p : files) {
        Sb3File sf;
        try { sf = sb3LoadAny(p); } catch (const Sb3Error&) { continue; }
        const Elem& targets = sf.targets;
        if (!targets.is_array()) continue;
        for (auto te : targets.arr()) {
            Elem t(te);
            if (!t.is_object()) continue;
            std::string nm;
            Elem nameEl = t.at("name");
            if (nameEl.is_string()) nm = std::string(nameEl.sv());
            else                    nm = "?";
            std::vector<std::pair<std::string, std::string>> bag;
            Elem costumes = t.at("costumes");
            if (costumes.is_array())
                for (auto ce : costumes.arr()) bag.emplace_back("造型名", std::string(Elem(ce).at("name").sv()));
            Elem sounds = t.at("sounds");
            if (sounds.is_array())
                for (auto se : sounds.arr()) bag.emplace_back("声音名", std::string(Elem(se).at("name").sv()));
            Elem variables = t.at("variables");
            if (variables.is_object())
                for (auto f : variables.obj()) {
                    Elem v(f.value);
                    if (v.is_array() && !v.empty()) {
                        Elem n0 = v.op(0);
                        std::string nm0 = n0.ok() ? sb3View(n0) : "";
                        std::string val = v.size() > 1 ? sb3View(Elem(v.op(1))) : "";
                        bag.emplace_back("变量", nm0 + " = " + val);
                    }
                }
            Elem lists = t.at("lists");
            if (lists.is_object())
                for (auto f : lists.obj()) {
                    Elem v(f.value);
                    if (v.is_array() && !v.empty()) {
                        Elem n0 = v.op(0);
                        std::string nm0 = n0.ok() ? sb3View(n0) : "";
                        std::string joined;
                        if (v.size() > 1 && Elem(v.op(1)).is_array()) {
                            for (auto x : Elem(v.op(1)).arr()) {
                                if (!joined.empty()) joined += "、";
                                joined += sb3View(Elem(x));
                            }
                        }
                        bag.emplace_back("列表", nm0 + " = " + joined);
                    }
                }
            Elem bcasts = t.at("broadcasts");
            if (bcasts.is_object())
                for (auto f : bcasts.obj()) bag.emplace_back("广播", sb3View(Elem(f.value)));
            Sb3Collect res = sb3CollectText(t, true);
            for (auto& kv : res.dialog) bag.emplace_back("对话·" + kv.first, kv.second);
            for (auto& c : res.comments) bag.emplace_back("注释", c);
            for (auto& s : res.strings) bag.emplace_back("积木文字", std::get<2>(s));
            if (a.script) {
                Renderer r(t);
                int i = 1;
                for (auto& lines : r.scripts(0)) {
                    for (auto& ln : lines) bag.emplace_back("脚本" + std::to_string(i), ln);
                    ++i;
                }
            }
            for (auto& kv : bag) {
                std::string low = kv.second;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return (char)::tolower(c); });
                if (low.find(kw) != std::string::npos)
                    hits.push_back({p, nm, kv.first, kv.second});
            }
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
        if (x.p != y.p) return x.p < y.p;
        if (x.t != y.t) return x.t < y.t;
        if (x.w != y.w) return x.w < y.w;
        return x.v < y.v;
    });
    if (a.json) {
        json out;
        out["keyword"] = a.keyword;
        out["hits"] = json::array();
        for (auto& h : hits) {
            json x;
            x["file"] = basename(h.p); x["target"] = h.t; x["where"] = h.w; x["text"] = h.v;
            out["hits"].push_back(std::move(x));
        }
        sb3JsonOut(out);
        return 0;
    }
    std::cout << "在 " << files.size() << " 个作品里搜索「" << a.keyword << "」，命中 "
              << hits.size() << " 处\n\n";
    std::string cur;
    size_t shown = 0;
    for (auto& h : hits) {
        if (shown >= (size_t)a.limit) break;
        ++shown;
        if (h.p != cur) { cur = h.p; std::cout << "◆ " << basename(h.p) << "\n"; }
        std::cout << "    [" << h.t << " · " << h.w << "] " << truncate(h.v, 90) << "\n";
    }
    if (hits.size() > (size_t)a.limit)
        std::cout << "\n…还有 " << (hits.size() - (size_t)a.limit)
                  << " 处（--limit 调整）\n";
    return 0;
}

} // namespace sb