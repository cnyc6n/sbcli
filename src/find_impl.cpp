// src/find_impl.cpp —— find 的两格式搜索 worker（sb1 / sb3）
#include "find_impl.hpp"
#include "squeak.hpp"
#include "sb1.hpp"
#include "sb3.hpp"
#include <algorithm>
#include <cstdio>

namespace sb {
using json = Json;
// ---- find：统一搜索（两类都搜）----

// 按文件头判断是否 1.4（detectFormat 的轻量版）
bool isSb1File(const std::string& p) {
    return detectFormat(p) == "sb1";
}

// 大小写不敏感查找：不复制/转换整个字符串（haystack 为 UTF-8 字节）
bool findCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    // needle 已预先小写化；逐字节比较，ASCII 字母忽略大小写
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j) {
            unsigned char h = (unsigned char)haystack[i + j];
            unsigned char n = (unsigned char)needle[j];
            if (h >= 'A' && h <= 'Z') h = (unsigned char)(h - 'A' + 'a');
            if (h != n) break;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

// DOM 视图 → 文本（与 jsonStr(Json) 语义一致；无效/缺值 → ""）
std::string jsonStr(const Elem& v) {
    if (!v.ok()) return "";
    if (v.is_string()) return std::string(v.sv());
    if (v.is_integer()) return std::to_string(v.i64());
    if (v.is_float()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", v.f64());
        return buf;
    }
    return compactJson(v.raw());
}

std::vector<FindHit> sb1FindInFile(const std::string& p,
                                          const std::string& kwLower,
                                          bool withScript) {
    std::vector<FindHit> out;
    Sb1File f;
    try {
        f = sb1Load(p);
    } catch (const Sb1Error&) {
        return out;
    }
    Value root = f.table->get(1);
    for (auto& t : findTargets(root)) {
        std::string nm = targetName(t);
        std::vector<std::pair<std::string, std::string>> bag;
        std::vector<Media> images, sounds;
        targetMedia(t, images, sounds);
        for (auto& m : images) bag.emplace_back("造型名", m.name);
        for (auto& m : sounds) bag.emplace_back("声音名", m.name);
        for (auto& v : targetVariables(t)) {
            std::string val = asJson(v.value, 1).dump();
            if (v.value.kind == Value::Kind::Str) val = v.value.s;
            bag.emplace_back("变量", v.name + " = " + val);
        }
        if (t.obj) {
            for (auto& pair : collectStrings(borrowObj(t.obj), 4000, false))
                bag.emplace_back("文字", pair.second);
        }
        if (withScript) {
            for (auto& sc : targetScripts(t))
                for (auto& ln : renderScript(sc, 600))
                    bag.emplace_back("脚本", ln);
        }
        for (auto& kv : bag) {
            if (findCI(kv.second, kwLower))
                out.push_back({p, nm, kv.first, kv.second});
        }
    }
    return out;
}

std::vector<FindHit> sb3FindInFile(const std::string& p,
                                          const std::string& kwLower,
                                          bool withScript) {
    std::vector<FindHit> out;
    // 预筛 + 精确搜索共用同一个 Reader：先解压 project.json 文本粗查，
    // 不命中直接跳过（省 DOM 解析）；命中再用同一 Reader 走精确路径。
    mzip::Reader r(p);
    std::string entry;
    {
        auto names = r.names();
        for (auto& n : names) {
            std::string l = n;
            for (auto& c : l) c = (char)::tolower((unsigned char)c);
            if (l == "project.json") { entry = n; break; }
            if (l.size() > 5 && l.substr(l.size() - 5) == ".json" &&
                l.rfind("__macosx", 0) != 0 && entry.empty())
                entry = n;
        }
    }
    if (entry.empty()) return out;
    std::string raw;
    try {
        raw = r.readText(entry);
    } catch (...) {
        return out;
    }
    // 大小写不敏感粗查（不复制整个文本）
    if (!findCI(raw, kwLower)) return out;

    Sb3File sf;
    try {
        // 借用已打开的 Reader（r 在函数作用域内存活，noop deleter 不释放）
        sf = sb3LoadFromReader(
            std::shared_ptr<mzip::Reader>(&r, [](mzip::Reader*) {}), p);
    } catch (const Sb3Error&) {
        return out;
    }
    const Elem& targets = sf.targets;
    if (!targets.is_array()) return out;
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
            for (auto ce : costumes.arr())
                bag.emplace_back("造型名", std::string(Elem(ce).at("name").sv()));
        Elem sounds = t.at("sounds");
        if (sounds.is_array())
            for (auto se : sounds.arr())
                bag.emplace_back("声音名", std::string(Elem(se).at("name").sv()));
        Elem variables = t.at("variables");
        if (variables.is_object())
            for (auto f : variables.obj()) {
                Elem v(f.value);
                if (v.is_array() && !v.empty()) {
                    Elem n0 = v.op(0);
                    std::string nm0 = n0.ok() ? jsonStr(n0) : "";
                    std::string val = v.size() > 1 ? jsonStr(Elem(v.op(1))) : "";
                    bag.emplace_back("变量", nm0 + " = " + val);
                }
            }
        Elem lists = t.at("lists");
        if (lists.is_object())
            for (auto f : lists.obj()) {
                Elem v(f.value);
                if (v.is_array() && !v.empty()) {
                    Elem n0 = v.op(0);
                    std::string nm0 = n0.ok() ? jsonStr(n0) : "";
                    std::string joined;
                    if (v.size() > 1) {
                        Elem items = Elem(v.op(1));
                        if (items.is_array()) {
                            for (auto x : items.arr()) {
                                if (!joined.empty()) joined += "、";
                                joined += jsonStr(Elem(x));
                            }
                        }
                    }
                    bag.emplace_back("列表", nm0 + " = " + joined);
                }
            }
        Elem bcasts = t.at("broadcasts");
        if (bcasts.is_object())
            for (auto f : bcasts.obj()) bag.emplace_back("广播", jsonStr(Elem(f.value)));
        Sb3Collect res = sb3CollectText(t, true);
        for (auto& kv : res.dialog) bag.emplace_back("对话·" + kv.first, kv.second);
        for (auto& c : res.comments) bag.emplace_back("注释", c);
        for (auto& s : res.strings) bag.emplace_back("积木文字", std::get<2>(s));
        if (withScript) {
            Renderer r(t);
            int i = 1;
            for (auto& lines : r.scripts(0)) {
                for (auto& ln : lines)
                    bag.emplace_back("脚本" + std::to_string(i), ln);
                ++i;
            }
        }
        for (auto& kv : bag) {
            if (findCI(kv.second, kwLower))
                out.push_back({p, nm, kv.first, kv.second});
        }
    }
    return out;
}

} // namespace sb