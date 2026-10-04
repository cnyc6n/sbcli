// src/sb1_cmds.cpp —— Scratch 1.4 命令
#include "sb1.hpp"
#include "sb1_tables.hpp"
#include "commands.hpp"
#include "jdoc.hpp"
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace sb {
using json = Json;

// 本地判断：值是否是一个积木块（List 且首元素为命令字符串）
static bool isBlock(const Value& x) {
    return x.kind == Value::Kind::List && x.list && !x.list->empty() &&
           (*x.list)[0].kind == Value::Kind::Str;
}
// ==========================================================================
// 命令层（对照 sb.py 的 s1_cmd_*）
// ==========================================================================

static void printJson1(const json& j) {
    std::cout << j.dump(2) << "\n";
}

// 信息表（sb1_project_info）：root 是 Dictionary → json 对象（去掉 thumbnail）
static json projectInfoJson(const Sb1File& f, int maxDepth) {
    json out = json::object();
    if (!f.info) return out;
    Value v = f.info->get(1);
    if (v.kind == Value::Kind::Map && v.map) {
        for (auto& kv : *v.map) {
            std::string k = (kv.first.kind == Value::Kind::Str)
                            ? kv.first.s : asJson(kv.first, 0).dump();
            if (k == "thumbnail") continue;
            out[k] = asJson(kv.second, maxDepth);
        }
    }
    return out;
}

// 统计类名出现次数（保持首次出现顺序，按次数降序）
static std::vector<std::pair<std::string, int>> classCounts(const Value& root) {
    std::map<std::string, int> counts;
    std::vector<std::string> order;
    walkObjects(root, [&](const SqueakObject& o) {
        if (!counts.count(o.cls)) order.push_back(o.cls);
        counts[o.cls]++;
    });
    std::vector<std::pair<std::string, int>> out;
    for (auto& c : order) out.emplace_back(c, counts[c]);
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& x, const auto& y) { return x.second > y.second; });
    return out;
}

// Value → 文本（变量值显示用）
static std::string valueText(const Value& v) {
    switch (v.kind) {
        case Value::Kind::Nil:    return "nil";
        case Value::Kind::Bool:   return v.b ? "true" : "false";
        case Value::Kind::Int:    return std::to_string(v.i);
        case Value::Kind::Double: { char buf[64]; std::snprintf(buf, sizeof(buf), "%g", v.d); return buf; }
        case Value::Kind::Str:    return v.s;
        default:                  return asJson(v, 2).dump();
    }
}

int s1_cmd_info(const Args& a) {
    Sb1File f = sb1Load(a.file);
    Value root = f.table->get(1);
    auto targets = findTargets(root);
    auto classes = classCounts(root);

    json info;
    info["file"] = basename(a.file);
    info["size"] = fileSize(a.file);
    info["format"] = "sb1";
    info["objects"] = (long long)f.table->size();
    info["root"] = root.obj ? root.obj->cls : "?";
    info["info_table_objects"] = f.info ? (long long)f.info->size() : 0;
    info["project_info"] = projectInfoJson(f, 2);
    json tgt = json::array();
    for (auto& t : targets) {
        json x;
        x["name"] = targetName(t);
        x["isStage"] = t.isStage;
        x["class"] = t.obj ? t.obj->cls : "?";
        x["fields"] = (long long)(t.obj ? t.obj->fields.size() : 0);
        tgt.push_back(std::move(x));
    }
    info["targets"] = std::move(tgt);
    json all = json::object();
    for (size_t i = 0; i < classes.size() && i < 20; ++i)
        all[classes[i].first] = classes[i].second;
    info["all_classes"] = std::move(all);

    if (a.json) { printJson1(info); return 0; }

    std::cout << "文件：" << info["file"].get<std::string>() << "  （"
              << humanSize(info["size"].get<long long>()) << "，Scratch 1.4）\n";
    std::cout << "对象表：" << info["objects"].get<long long>()
              << " 个对象 · 信息表 " << info["info_table_objects"].get<long long>()
              << " 个\n";
    std::cout << "根对象：" << info["root"].get<std::string>() << "\n";
    if (info["project_info"].is_object() && !info["project_info"].empty()) {
        std::cout << "工程信息：\n";
        for (auto it = info["project_info"].begin();
             it != info["project_info"].end(); ++it) {
            std::string v = it.value().is_string()
                            ? it.value().get<std::string>() : it.value().dump();
            std::cout << "    " << it.key() << " = " << truncate(v, 70) << "\n";
        }
    }
    std::cout << "\n目标（舞台/角色）：" << targets.size() << " 个\n";
    for (auto& t : targets) {
        std::string kind = t.isStage ? "舞台" : "角色";
        std::cout << "  [" << kind << "] " << targetName(t)
                  << "   (" << (t.obj ? t.obj->cls : "?") << ", "
                  << (t.obj ? t.obj->fields.size() : 0) << " 字段)\n";
    }
    std::cout << "\n出现最多的类：\n";
    for (size_t i = 0; i < classes.size() && i < 20; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%5d", classes[i].second);
        std::cout << "  " << buf << "  " << classes[i].first << "\n";
    }
    return 0;
}

int s1_cmd_sprites(const Args& a) {
    Sb1File f = sb1Load(a.file);
    Value root = f.table->get(1);
    auto targets = findTargets(root);

    json rows = json::array();
    for (auto& t : targets) {
        std::vector<Media> images, sounds;
        targetMedia(t, images, sounds);
        json x;
        x["name"] = targetName(t);
        x["isStage"] = t.isStage;
        x["class"] = t.obj ? t.obj->cls : "?";
        x["fields"] = (long long)(t.obj ? t.obj->fields.size() : 0);
        x["costumes"] = (long long)images.size();
        x["sounds"] = (long long)sounds.size();
        json vars = json::array();
        for (auto& v : targetVariables(t)) vars.push_back(v.name);
        x["variables"] = std::move(vars);
        x["scripts"] = (long long)targetScripts(t).size();
        rows.push_back(std::move(x));
    }

    if (a.json) { printJson1(rows); return 0; }

    std::cout << "文件：" << basename(a.file) << "  （Scratch 1.4）\n";
    std::cout << "共 " << rows.size() << " 个目标\n\n";
    int i = 1;
    long long sprites = 0, costumes = 0, sounds = 0, scripts = 0;
    for (auto& x : rows) {
        std::string kind = x["isStage"].get<bool>() ? "舞台" : "角色";
        std::cout << std::setw(4) << i << ". [" << kind << "] " << x["name"].get<std::string>() << "\n";
        int nv = (int)x["variables"].size();
        std::cout << "       造型 " << x["costumes"].get<long long>()
                  << " · 声音 " << x["sounds"].get<long long>()
                  << " · 变量 " << nv
                  << " · 脚本 " << x["scripts"].get<long long>() << "\n";
        if (!x["variables"].empty()) {
            std::cout << "       变量：";
            std::string sep;
            int shown = 0;
            for (auto& v : x["variables"]) {
                if (shown >= 10) break;
                std::cout << sep << v.get<std::string>();
                sep = "、"; ++shown;
            }
            std::cout << "\n";
        }
        if (!x["isStage"].get<bool>()) ++sprites;
        costumes += x["costumes"].get<long long>();
        sounds += x["sounds"].get<long long>();
        scripts += x["scripts"].get<long long>();
        ++i;
    }
    std::cout << "\n合计：" << sprites << " 个角色，" << costumes << " 个造型，"
              << sounds << " 个声音，" << scripts << " 段脚本\n";
    return 0;
}

int s1_cmd_text(const Args& a) {
    Sb1File f = sb1Load(a.file);
    Value root = f.table->get(1);
    auto targets = findTargets(root);

    json out;
    out["file"] = a.file;
    out["format"] = "sb1";
    json tgt = json::object();
    long long total = 0;
    for (auto& t : targets) {
        if (!t.obj) continue;
        std::string nm = targetName(t);
        auto str = collectStrings(borrowObj(t.obj), (size_t)(a.limit) * 10);
        json entry;
        entry["isStage"] = t.isStage;
        json arr = json::array();
        for (auto& p : str) { json x; x["path"] = p.first; x["text"] = p.second; arr.push_back(std::move(x)); }
        entry["strings"] = std::move(arr);
        tgt[nm] = std::move(entry);
        total += (long long)str.size();
    }
    out["targets"] = std::move(tgt);

    if (a.json) { printJson1(out); return 0; }

    std::cout << "文件：" << basename(a.file) << "  （Scratch 1.4）\n\n";
    for (auto it = out["targets"].begin(); it != out["targets"].end(); ++it) {
        std::string kind = it.value()["isStage"].get<bool>() ? "舞台" : "角色";
        size_t n = it.value()["strings"].size();
        std::cout << "▍" << it.key() << "（" << kind << "）—— " << n << " 条文字\n";
        size_t shown = 0;
        for (auto& s : it.value()["strings"]) {
            if (shown >= (size_t)a.limit) break;
            ++shown;
            std::cout << "    " << truncate(s["text"].get<std::string>(), 84) << "\n";
        }
        if (n > (size_t)a.limit)
            std::cout << "    …（还有 " << (n - (size_t)a.limit) << " 条）\n";
        std::cout << "\n";
    }
    std::cout << "合计：" << total << " 条文字\n";
    return 0;
}

int s1_cmd_script(const Args& a) {
    Sb1File f = sb1Load(a.file);
    Value root = f.table->get(1);
    auto targets = findTargets(root);
    int budget = a.limitSet && a.limit > 0 ? a.limit : 600;

    if (!a.sprite.empty()) {
        std::vector<Target> keep;
        for (auto& t : targets)
            if (targetName(t).size() == a.sprite.size()) {
                std::string n1 = targetName(t), n2 = a.sprite;
                std::transform(n1.begin(), n1.end(), n1.begin(), [](unsigned char c){return (char)::tolower(c);});
                std::transform(n2.begin(), n2.end(), n2.begin(), [](unsigned char c){return (char)::tolower(c);});
                if (n1 == n2) keep.push_back(t);
            }
        targets = std::move(keep);
        if (targets.empty()) {
            std::cerr << "没有叫「" << a.sprite << "」的角色\n";
            return 2;
        }
    }

    json payload = json::array();
    for (auto& t : targets) {
        if (!t.obj) continue;
        auto scripts = targetScripts(t);
        json rendered = json::array();
        for (auto& sc : scripts) {
            auto lines = renderScript(sc, budget);
            if (!lines.empty()) {
                json jl = json::array();
                for (auto& ln : lines) jl.push_back(ln);
                rendered.push_back(std::move(jl));
            }
        }
        json x;
        x["name"] = targetName(t);
        x["isStage"] = t.isStage;
        x["scriptCount"] = (long long)scripts.size();
        x["scripts"] = std::move(rendered);
        payload.push_back(std::move(x));
    }

    if (a.json) {
        json out;
        out["file"] = a.file;
        out["format"] = "sb1";
        out["targets"] = std::move(payload);
        printJson1(out);
        return 0;
    }

    std::cout << "文件：" << basename(a.file) << "  （Scratch 1.4）\n";
    for (auto& entry : payload) {
        std::string kind = entry["isStage"].get<bool>() ? "舞台" : "角色";
        std::cout << "\n═══ " << entry["name"].get<std::string>() << "（" << kind << "）═══\n";
        long long sc = entry["scriptCount"].get<long long>();
        if (!sc) { std::cout << "  （没有脚本）\n"; continue; }
        if (entry["scripts"].empty()) {
            std::cout << "  （找到 " << sc << " 个脚本容器，但没解析出积木）\n";
            continue;
        }
        int i = 1;
        for (auto& lines : entry["scripts"]) {
            std::cout << "\n  ── 脚本 " << i << " ──\n";
            size_t shown = 0;
            for (auto& ln : lines) {
                if (a.limitSet && a.limit > 0 && shown >= (size_t)a.limit) break;
                ++shown;
                std::cout << "  " << ln.get<std::string>() << "\n";
            }
            ++i;
        }
    }
    return 0;
}

int s1_cmd_vars(const Args& a) {
    Sb1File f = sb1Load(a.file);
    Value root = f.table->get(1);
    auto targets = findTargets(root);

    json out = json::object();
    for (auto& t : targets) {
        auto vs = targetVariables(t);
        if (vs.empty() || !t.obj) continue;
        json arr = json::array();
        for (auto& v : vs) {
            json x;
            x["name"] = v.name;
            x["value"] = asJson(v.value, 2);
            arr.push_back(std::move(x));
        }
        json entry;
        entry["isStage"] = t.isStage;
        entry["variables"] = std::move(arr);
        out[targetName(t)] = std::move(entry);
    }

    if (a.json) {
        json r;
        r["file"] = a.file;
        r["format"] = "sb1";
        r["targets"] = std::move(out);
        printJson1(r);
        return 0;
    }

    std::cout << "文件：" << basename(a.file) << "  （Scratch 1.4）\n\n";
    if (out.empty()) { std::cout << "没有找到变量。\n"; return 0; }
    for (auto it = out.begin(); it != out.end(); ++it) {
        std::string kind = it.value()["isStage"].get<bool>() ? "舞台" : "角色";
        std::cout << "▍" << it.key() << "（" << kind << "）\n";
        for (auto& v : it.value()["variables"]) {
            const json& jv = v["value"];
            std::string sv = jv.is_string() ? jv.get<std::string>() : jv.dump();
            std::cout << "    " << v["name"].get<std::string>() << " = "
                      << truncate(sv, 80) << "\n";
        }
        std::cout << "\n";
    }
    return 0;
}

int s1_cmd_media(const Args& a) {
    Sb1File f = sb1Load(a.file);
    Value root = f.table->get(1);
    auto targets = findTargets(root);

    json rows = json::array();
    struct Pair { std::string t, k, n; const SqueakObject* obj; };
    std::vector<Pair> pairs;
    for (auto& t : targets) {
        if (!t.obj) continue;
        std::string nm = targetName(t);
        std::vector<Media> images, sounds;
        targetMedia(t, images, sounds);
        for (auto& m : images) {
            json x;
            x["target"] = nm; x["kind"] = "造型"; x["name"] = m.name;
            x["class"] = m.obj ? m.obj->cls : "?";
            rows.push_back(std::move(x));
            pairs.push_back({nm, "造型", m.name, m.obj});
        }
        for (auto& m : sounds) {
            json x;
            x["target"] = nm; x["kind"] = "声音"; x["name"] = m.name;
            x["class"] = m.obj ? m.obj->cls : "?";
            rows.push_back(std::move(x));
            pairs.push_back({nm, "声音", m.name, m.obj});
        }
    }

    if (a.json) {
        json r;
        r["file"] = a.file;
        r["format"] = "sb1";
        r["media"] = std::move(rows);
        printJson1(r);
        return 0;
    }

    std::cout << "文件：" << basename(a.file) << "  （Scratch 1.4）\n";
    std::cout << "共 " << rows.size() << " 个素材\n\n";
    for (auto& x : rows)
        std::cout << "  [" << x["kind"].get<std::string>() << "] "
                  << x["target"].get<std::string>() << "/" << x["name"].get<std::string>() << "\n";

    if (!a.extract.empty()) {
        std::string outdir = a.extract;
        int ok = 0, skip = 0;
        for (auto& p : pairs) {
            if (p.k != "造型") { ++skip; continue; }
            if (!p.obj || p.obj->fields.size() <= 1) { ++skip; continue; }
            const Value& formV = p.obj->fields[1];
            if (formV.kind != Value::Kind::Obj || !formV.obj) { ++skip; continue; }
            RGBAImage img;
            if (!formToRGBA(*formV.obj, img)) { ++skip; continue; }
            std::string sub = joinPath(joinPath(outdir, sanitizeFilename(p.t)), "造型");
            makeDirs(sub);
            std::string dst = joinPath(sub, sanitizeFilename(p.n) + ".png");
            writePNG(dst, img.w, img.h, img.rgba);
            std::cout << "  导出 " << p.t << "/" << p.n << "  " << img.w << "×" << img.h << "\n";
            ++ok;
        }
        std::cout << "\n已导出 " << ok << " 个造型到：" << outdir << "\n";
        if (skip)
            std::cout << "   跳过 " << skip
                      << " 个声音（Squeak 的 SampledSound 里没有采样率，导不出 wav）\n";
    }
    return 0;
}

int s1_cmd_raw(const Args& a) {
    Sb1File f = sb1Load(a.file);
    int depth = a.depth > 0 ? a.depth : 3;
    json j;
    if (a.infoTable)
        j = projectInfoJson(f, depth);
    else {
        Value root = f.table->get(1);
        j = asJson(root, depth);
    }
    printJson1(j);
    return 0;
}

int s1_cmd_refs(const Args& a) {
    Sb1File f = sb1Load(a.file);
    Value root = f.table->get(1);

    // 递归遍历脚本里的块，提取变量/列表读写
    // 块结构：List[op名称, 参数…]，参数可能是嵌套块（List）
    struct Ref { std::string who; int script; std::string action; std::string name; std::string text; };
    std::map<std::string, std::vector<Ref>> writes, reads;

    // 遍历脚本里一个块；substacks 递归
    std::function<void(const Value&, const Target&, int, std::vector<Ref>&, std::vector<Ref>&)> walkBlock;
    walkBlock = [&](const Value& b, const Target& t, int scriptIdx,
                    std::vector<Ref>& w, std::vector<Ref>& r) {
        if (b.kind != Value::Kind::List || !b.list || b.list->empty()) return;
        const std::string& op = (*b.list)[0].s;
        // 参数：字符串直接取，嵌套块递归
        auto argStr = [&b](size_t i) -> std::string {
            if (i >= b.list->size()) return "";
            const Value& a = (*b.list)[i];
            if (a.kind == Value::Kind::Str) return a.s;
            if (a.kind == Value::Kind::Int) return std::to_string(a.i);
            return "";
        };
        auto add = [&](std::vector<Ref>& tbl, const std::string& action,
                       const std::string& name, const std::string& text) {
            if (name.empty() || (a.extra.size() && name.find(a.extra[0]) == std::string::npos)) return;
            Ref rf; rf.who = t.name; rf.script = scriptIdx;
            rf.action = action; rf.name = name; rf.text = text;
            tbl.push_back(std::move(rf));
        };
        std::string text;
        {
            // 简化的块文本：用 SB1_BLOCKS 模板替换参数
            std::string tmpl;
            auto it = SB1_BLOCKS.find(op);
            if (it != SB1_BLOCKS.end()) {
                tmpl = it->second;
                for (size_t i = 0; i < b.list->size() - 1; ++i) {
                    std::string key = "{" + std::to_string(i + 1) + "}";
                    std::string v = argStr(i + 1);
                    size_t pos = 0;
                    while ((pos = tmpl.find(key, pos)) != std::string::npos) {
                        tmpl.replace(pos, key.size(), v);
                        pos += v.size();
                    }
                }
                text = tmpl;
            } else {
                text = "⟨" + op + "⟩";
                for (size_t i = 1; i < b.list->size(); ++i) {
                    std::string v = argStr(i);
                    if (!v.empty()) text += " " + v;
                }
            }
        }

        // 变量
        if (op == "setVar:to:" || op == "setVariable" || op == "setVar:*:")
            add(w, "设置", argStr(1), text);
        else if (op == "changeVar:by:" || op == "changeVariable")
            add(w, "增减", argStr(1), text);
        else if (op == "readVariable")
            add(r, "读取", argStr(1), text);
        else if (op == "showVariable:" || op == "hideVariable:")
            add(w, op == "showVariable:" ? "显示变量" : "隐藏变量", argStr(1), text);
        // 列表
        else if (op == "append:toList:")
            add(w, "追加", argStr(2), text);
        else if (op == "deleteLine:ofList:")
            add(w, "删除项", argStr(2), text);
        else if (op == "insert:at:ofList:")
            add(w, "插入", argStr(3), text);
        else if (op == "setLine:ofList:to:")
            add(w, "替换", argStr(2), text);
        else if (op == "getLine:ofList:")
            add(r, "取项", argStr(2), text);
        else if (op == "lineCountOfList:")
            add(r, "取长度", argStr(1), text);
        else if (op == "list:contains:")
            add(r, "含项?", argStr(1), text);
        else if (op == "showList:" || op == "hideList:")
            add(w, op == "showList:" ? "显示列表" : "隐藏列表", argStr(1), text);

        // 递归子块（substacks 和嵌套表达式块）
        for (size_t i = 1; i < b.list->size(); ++i) {
            const Value& a0 = (*b.list)[i];
            if (a0.kind == Value::Kind::List && a0.list && !a0.list->empty()) {
                if (isBlock(a0) || isBlock((*a0.list)[0]))
                    walkBlock(a0, t, scriptIdx, w, r);
            }
        }
    };

    for (auto& t : findTargets(root)) {
        auto scripts = targetScripts(t);
        int scriptIdx = 0;
        for (auto& sc : scripts) {
            ++scriptIdx;
            std::vector<Ref> w, r;
            if (sc.kind == Value::Kind::List && sc.list && sc.list->size() > 1) {
                for (size_t i = 1; i < sc.list->size(); ++i)
                    walkBlock((*sc.list)[i], t, scriptIdx, w, r);
            }
            // 也可能 sc 直接是块列表
            if (sc.kind == Value::Kind::List && sc.list && !sc.list->empty() &&
                isBlock((*sc.list)[0])) {
                for (size_t i = 0; i < sc.list->size(); ++i)
                    walkBlock((*sc.list)[i], t, scriptIdx, w, r);
            }
            for (auto& x : w) writes[x.name + "\x01" + x.action].push_back(x);
            for (auto& x : r) reads[x.name + "\x01" + x.action].push_back(x);
        }
    }
    // 按名字分组输出
    std::map<std::string, std::pair<std::vector<Ref>, std::vector<Ref>>> grouped;
    for (auto& kv : writes) {
        std::string name = kv.first.substr(0, kv.first.find('\x01'));
        for (auto& x : kv.second) grouped[name].first.push_back(x);
    }
    for (auto& kv : reads) {
        std::string name = kv.first.substr(0, kv.first.find('\x01'));
        for (auto& x : kv.second) grouped[name].second.push_back(x);
    }
    std::cout << "文件：" << basename(a.file) << "\n";
    if (grouped.empty()) { std::cout << "\n没有找到变量/列表引用\n"; return 0; }
    for (auto& g : grouped) {
        std::cout << "\n变量/列表 " << g.first << "\n";
        if (!g.second.first.empty()) {
            std::cout << "  写：\n";
            for (auto& x : g.second.first)
                std::cout << "    " << x.who << "·脚本" << x.script
                          << "  " << x.action << "：" << (x.text.empty() ? "?" : x.text) << "\n";
        }
        if (!g.second.second.empty()) {
            std::cout << "  读：\n";
            for (auto& x : g.second.second)
                std::cout << "    " << x.who << "·脚本" << x.script
                          << "  " << x.action << "：" << (x.text.empty() ? "?" : x.text) << "\n";
        }
    }
    return 0;
}

int s1_cmd_events(const Args& a) {
    Sb1File f = sb1Load(a.file);
    Value root = f.table->get(1);

    struct Ev { std::string who; int script; std::string text; };
    std::map<std::string, std::vector<Ev>> senders, receivers;

    for (auto& t : findTargets(root)) {
        auto scripts = targetScripts(t);
        int scriptIdx = 0;
        for (auto& sc : scripts) {
            ++scriptIdx;
            // 遍历脚本内的块（含子块）
            std::function<void(const Value&, const Target&, int)> walk;
            walk = [&](const Value& b, const Target& tt, int si) {
                if (!isBlock(b)) return;
                const std::string& op = (*b.list)[0].s;
                auto argStr = [&b](size_t i) -> std::string {
                    if (i >= b.list->size()) return "";
                    const Value& v = (*b.list)[i];
                    if (v.kind == Value::Kind::Str) return v.s;
                    if (v.kind == Value::Kind::Int) return std::to_string(v.i);
                    return "";
                };
                auto add = [&](std::map<std::string, std::vector<Ev>>& tbl,
                               const std::string& nm, const std::string& text) {
                    if (nm.empty() || (a.extra.size() && nm.find(a.extra[0]) == std::string::npos)) return;
                    Ev e; e.who = tt.name; e.script = si; e.text = text;
                    tbl[nm].push_back(std::move(e));
                };
                std::string text = "⟨" + op + "⟩" + (argStr(1).empty() ? "" : " " + argStr(1));
                // 发送：broadcast: / doBroadcastAndWait
                if (op == "broadcast:" || op == "doBroadcastAndWait")
                    add(senders, argStr(1), text);
                // 接收：whenIReceive:（HAT_EVENTS 表里是 "whenIReceive:"）
                else if (op == "whenIReceive:")
                    add(receivers, argStr(1), text);
                else if (op == "EventHatMorph") {
                    // EventHatMorph 是 sb1 统一事件帽子：绿旗/按键/响度/广播都走它。
                    // 与 script 命令一致：系统事件（HAT_EVENTS 表 / "Scratch-" 前缀）
                    // 不是广播，过滤掉避免误报"孤儿广播"；广播接收原样收进 receivers，
                    // 不剥前缀——广播名保持原名，与发送端一致，避免误改真实广播名。
                    std::string nm = argStr(1);
                    bool isSystem = !nm.empty() &&
                        (HAT_EVENTS.count(nm) || nm.rfind("Scratch-", 0) == 0);
                    if (!isSystem)
                        add(receivers, nm, text);
                }
                // 递归子块
                for (size_t i = 1; i < b.list->size(); ++i) {
                    const Value& c = (*b.list)[i];
                    if (c.kind == Value::Kind::List && c.list && !c.list->empty())
                        walk(c, tt, si);
                }
            };
            // 遍历脚本顶层块：sb1 脚本是 ["Scratch-Stack", [块1, 块2…], …]，
            // 首元素是字符串标记（非块）；[块1, 块2…] 是一个"块列表容器"
            // （其 [0] 是块而非字符串）。两种容器结构都覆盖：
            //   · 脚本直接是块列表：[块, 块, …]
            //   · 脚本是 ["标记", [块, 块…], …]
            if (sc.kind == Value::Kind::List && sc.list) {
                if (!sc.list->empty() && isBlock((*sc.list)[0])) {
                    for (size_t i = 0; i < sc.list->size(); ++i)
                        walk((*sc.list)[i], t, scriptIdx);
                } else {
                    for (size_t i = 0; i < sc.list->size(); ++i) {
                        const Value& e = (*sc.list)[i];
                        // e 可能是 [块1, 块2…] 容器
                        if (e.kind == Value::Kind::List && e.list && !e.list->empty() &&
                            isBlock((*e.list)[0])) {
                            for (auto& blk : *e.list) walk(blk, t, scriptIdx);
                        } else {
                            walk(e, t, scriptIdx);
                        }
                    }
                }
            }
        }
    }

    if (a.json) {
        json out = json::object();
        out["file"] = a.file;
        json arr = json::array();
        for (auto& kv : senders) {
            json x = json::object();
            x["name"] = kv.first;
            json se = json::array(), re = json::array();
            for (auto& e : kv.second) {
                json j = json::object();
                j["who"] = e.who; j["script"] = e.script; j["block"] = e.text;
                se.push_back(std::move(j));
            }
            auto rit = receivers.find(kv.first);
            if (rit != receivers.end())
                for (auto& e : rit->second) {
                    json j = json::object();
                    j["who"] = e.who; j["script"] = e.script; j["block"] = e.text;
                    re.push_back(std::move(j));
                }
            x["senders"] = se;
            x["receivers"] = re;
            arr.push_back(x);
        }
        out["events"] = arr;
        printJson1(out);
        return 0;
    }

    std::cout << "文件：" << basename(a.file) << "\n";
    std::set<std::string> allB;
    for (auto& kv : senders) allB.insert(kv.first);
    for (auto& kv : receivers) allB.insert(kv.first);
    if (allB.empty()) { std::cout << "这个作品没有广播。\n"; return 0; }
    for (auto& bname : allB) {
        std::cout << "\n■ " << bname;
        auto si = senders.find(bname);
        auto ri = receivers.find(bname);
        bool hasS = (si != senders.end() && !si->second.empty());
        bool hasR = (ri != receivers.end() && !ri->second.empty());
        // 孤儿广播双向警示（与 sb3 events 对齐）
        if (!hasS)
            std::cout << "（⚠ 无发送者：可能是变量广播或残留）";
        else if (!hasR)
            std::cout << "（⚠ 无接收者：广播发出但没人监听，可能拼错或残留）";
        std::cout << "\n";
        if (hasS) {
            std::map<std::pair<std::string,int>, std::string> uniq;
            for (auto& e : si->second) uniq[{e.who, e.script}] = e.text;
            std::cout << "  发：";
            bool first = true;
            for (auto& u : uniq) {
                if (!first) std::cout << "，";
                first = false;
                std::cout << u.first.first << "·脚本" << u.first.second;
            }
            std::cout << "\n";
        }
        if (hasR) {
            std::map<std::pair<std::string,int>, std::string> uniq;
            for (auto& e : ri->second) uniq[{e.who, e.script}] = e.text;
            std::cout << "  收：";
            bool first = true;
            for (auto& u : uniq) {
                if (!first) std::cout << "，";
                first = false;
                std::cout << u.first.first << "·脚本" << u.first.second;
            }
            std::cout << "\n";
        }
    }
    return 0;
}

} // namespace sb
