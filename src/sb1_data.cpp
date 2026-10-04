// src/sb1_data.cpp —— Scratch 1.4 数据访问（目标/媒体/变量/脚本/字符串/脚本渲染）
#include "sb1.hpp"
#include "commands.hpp"
#include "jdoc.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <functional>
#include <set>
#include <unordered_set>
#include <iostream>
#include <map>
#include "sb1_tables.hpp"
#include "zip.hpp"

namespace sb {

using json = Json;

// ---- 目标识别 ----

std::vector<Target> findTargets(const Value& root) {
    std::vector<Target> out;
    auto add = [&](const SqueakObject* o, bool isStage) {
        for (auto& t : out) if (t.obj == o) return;
        Target t;
        t.obj = o; t.isStage = isStage;
        t.name = nameOf(borrowObj(o));
        if (t.name.empty()) t.name = isStage ? "Stage" : "?";
        out.push_back(std::move(t));
    };

    if (root.kind == Value::Kind::Obj && root.obj) {
        if (root.obj->cls == "Stage")  add(root.obj.get(), true);
        if (root.obj->cls == "Sprite") add(root.obj.get(), false);
    }
    for (auto* s : findAll(root, "Sprite")) add(s, false);
    // Stage 放最前
    for (auto* s : findAll(root, "Stage")) {
        Target t; t.obj = s; t.isStage = true;
        t.name = nameOf(borrowObj(s));
        if (t.name.empty()) t.name = "Stage";
        bool found = false;
        for (auto& x : out) if (x.obj == s) { found = true; break; }
        if (!found) out.insert(out.begin(), t);
    }
    return out;
}

std::string targetName(const Target& t) {
    return t.name.empty() ? (t.isStage ? "Stage" : "?") : t.name;
}

void targetMedia(const Target& t,
                 std::vector<Media>& images, std::vector<Media>& sounds) {
    auto m = t.obj->named("media");
    if (!m || m->kind != Value::Kind::List || !m->list) return;
    for (auto& x : *m->list) {
        if (x.kind != Value::Kind::Obj || !x.obj) continue;
        std::string nm = nameOf(x);
        if (nm.empty()) nm = "?";
        if (x.obj->cls == "Sound") sounds.push_back({nm, x.obj.get()});
        else                        images.push_back({nm, x.obj.get()});
    }
}

std::vector<VarEntry> targetVariables(const Target& t) {
    std::vector<VarEntry> out;
    auto v = t.obj->named("variables");
    if (!v || v->kind != Value::Kind::Map || !v->map) return out;
    for (auto& kv : *v->map) {
        std::string name = (kv.first.kind == Value::Kind::Str) ? kv.first.s : "?";
        VarEntry e; e.name = name;
        if (kv.second.kind == Value::Kind::Obj && kv.second.obj) {
            e.value = kv.second.obj->fields.empty() ? Value::makeNil()
                                                    : kv.second.obj->fields[0];
        } else if (kv.second.kind == Value::Kind::List && kv.second.list
                   && !kv.second.list->empty()) {
            e.value = kv.second.list->back();
        } else {
            e.value = kv.second;
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<Value> targetScripts(const Target& t) {
    std::vector<Value> out;
    auto s = t.obj->named("scripts");
    if (!s) return out;
    if (s->kind == Value::Kind::List && s->list) return *s->list;
    if (s->kind == Value::Kind::Obj && s->obj)   return s->obj->fields;
    return out;
}

// ---- 字符串收集 ----

std::vector<std::pair<std::string, std::string>>
collectStrings(const Value& root, size_t limit, bool withPath) {
    std::vector<std::pair<std::string, std::string>> out;
    std::unordered_set<std::string> seen;   // 哈希去重（保序交给 vector）

    std::function<void(const Value&, const std::string&, int)> rec;
    rec = [&](const Value& x, const std::string& path, int depth) {
        if (out.size() >= limit || depth > 8) return;
        if (x.kind == Value::Kind::Str) {
            std::string s = x.s;
            // trim
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) return;
            s = s.substr(a, b - a + 1);
            if (s.size() > 1 && !seen.count(s)) {
                seen.insert(s);
                out.emplace_back(withPath ? path : "", s);
            }
        } else if (x.kind == Value::Kind::Color) {
            // Python 里颜色直接就是 "#rrggbb" / "#rrggbbaa" 字符串，
            // 所以也会被当文字收集（同一 seen 去重）。
            char buf[16];
            uint32_t c = x.color;
            if ((c & 0xFF) == 0xFF)
                std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                              (c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF);
            else
                std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
                              (c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF,
                              c & 0xFF);
            std::string s = buf;
            if (!seen.count(s)) {
                seen.insert(s);
                out.emplace_back(withPath ? path : "", s);
            }
        } else if (x.kind == Value::Kind::Obj && x.obj) {
            const std::string& cls = x.obj->cls;
            if (cls == "Bitmap" || cls == "SoundBuffer" || cls == "Form" ||
                cls == "ColorForm" || cls == "SampledSound") return;
            auto it = FIELD_NAMES.find(cls);
            if (it == FIELD_NAMES.end()) {
                auto ai = FIELD_ALIASES.find(cls);
                if (ai != FIELD_ALIASES.end()) it = FIELD_NAMES.find(ai->second);
            }
            // 路径前缀（只在需要时构建，避免每层都分配 string）
            std::string prefix;
            if (withPath)
                prefix = path + "/" + cls + ".";
            for (size_t i = 0; i < x.obj->fields.size(); ++i) {
                std::string nm;
                if (it != FIELD_NAMES.end() && i < it->second.size())
                    nm = it->second[i];
                else
                    nm = "#" + std::to_string(i);
                if (withPath) rec(x.obj->fields[i], prefix + nm, depth + 1);
                else          rec(x.obj->fields[i], path, depth + 1);
            }
        } else if (x.kind == Value::Kind::List && x.list) {
            for (size_t i = 0; i < x.list->size(); ++i) {
                if (withPath) rec((*x.list)[i],
                                  path + "[" + std::to_string(i) + "]", depth + 1);
                else          rec((*x.list)[i], path, depth + 1);
            }
        } else if (x.kind == Value::Kind::Map && x.map) {
            for (auto& kv : *x.map) {
                if (!withPath) { rec(kv.second, path, depth + 1); continue; }
                std::string k = (kv.first.kind == Value::Kind::Str)
                                ? kv.first.s
                                : asJson(kv.first, 0).dump();
                rec(kv.second, path + "[" + k + "]", depth + 1);
            }
        }
    };
    rec(root, "", 0);
    return out;
}

// ---- 脚本翻译 ----

static bool isBlock(const Value& x) {
    return x.kind == Value::Kind::List && x.list && !x.list->empty() &&
           (*x.list)[0].kind == Value::Kind::Str;
}

static bool isSubstack(const Value& x) {
    return x.kind == Value::Kind::List && x.list && !x.list->empty() &&
           (*x.list)[0].kind == Value::Kind::List;
}

static std::string exprText(const Value& x);

static std::string renderOp(const std::string& op,
                            const std::vector<Value>& args) {
    std::vector<std::string> vals;
    for (auto& a : args) vals.push_back(exprText(a));

    if (op == "changeVariable" || op == "changeList") {
        std::string name = vals.size() > 0 ? vals[0] : "?";
        std::string action = vals.size() > 1 ? vals[1] : "";
        std::string val = vals.size() > 2 ? vals[2] : "";
        auto it = SB1_BLOCKS.find(action);
        if (it != SB1_BLOCKS.end()) {
            std::string t = it->second;
            auto rep = [&](const std::string& key, const std::string& v) {
                size_t pos = 0;
                while ((pos = t.find(key, pos)) != std::string::npos) {
                    t.replace(pos, key.size(), v);
                    pos += v.size();
                }
            };
            rep("{1}", name); rep("{2}", val); rep("{3}", val);
            return t;
        }
        std::string s = "⟨" + op + " " + action + "⟩ " + name;
        if (!val.empty()) s += " " + val;
        return s;
    }
    if (op == "EventHatMorph") {
        std::string name = vals.empty() ? "" : vals[0];
        auto it = HAT_EVENTS.find(name);
        if (it != HAT_EVENTS.end()) return it->second;
        if (name.rfind("Scratch-", 0) == 0) return "当 " + name;
        return name.empty() ? "当…" : ("当接收到 " + name);
    }
    if (op == "KeyEventHatMorph")
        return "当按下 " + (vals.empty() ? std::string("?") : vals[0]) + " 键";
    if (op == "MouseClickEventHatMorph")
        return "当角色被点击";

    auto it = SB1_BLOCKS.find(op);
    if (it != SB1_BLOCKS.end()) {
        std::string t = it->second;
        for (size_t i = 0; i < vals.size(); ++i) {
            std::string key = "{" + std::to_string(i + 1) + "}";
            size_t pos = 0;
            while ((pos = t.find(key, pos)) != std::string::npos) {
                t.replace(pos, key.size(), vals[i]);
                pos += vals[i].size();
            }
        }
        // 剩下未匹配的 {n} → "?"（手写扫描）
        {
            std::string t2;
            t2.reserve(t.size());
            for (size_t i = 0; i < t.size();) {
                if (t[i] == '{' && i + 2 < t.size() && t[i+1] >= '0' && t[i+1] <= '9' &&
                    t[i+2] == '}') {
                    t2 += '?';
                    i += 3;
                } else {
                    t2 += t[i++];
                }
            }
            t = std::move(t2);
        }
        return t;
    }
    std::string body;
    for (auto& v : vals) {
        if (v.empty()) continue;
        if (!body.empty()) body += " ";
        body += v;
    }
    return "⟨" + op + "⟩" + (body.empty() ? "" : (" " + body));
}

static std::string exprText(const Value& x) {
    switch (x.kind) {
        case Value::Kind::Nil:    return "";
        case Value::Kind::Bool:   return x.b ? "true" : "false";
        case Value::Kind::Int:    return std::to_string(x.i);
        case Value::Kind::Double: {
            char buf[64]; std::snprintf(buf, sizeof(buf), "%g", x.d);
            return buf;
        }
        case Value::Kind::Str:    return x.s;
        case Value::Kind::List: {
            if (isBlock(x)) {
                std::vector<Value> args;
                for (size_t i = 1; i < x.list->size(); ++i) {
                    if (!isSubstack((*x.list)[i])) args.push_back((*x.list)[i]);
                }
                return renderOp((*x.list)[0].s, args);
            }
            std::string out;
            for (auto& a : *x.list) {
                if (isSubstack(a)) continue;
                std::string t = exprText(a);
                if (t.empty()) continue;
                if (!out.empty()) out += " ";
                out += t;
            }
            return out;
        }
        default: return asJson(x, 0).dump();
    }
}

static void walkStack(const std::vector<Value>& blocks, int depth,
                      std::vector<std::string>& out, int budget) {
    if ((int)out.size() >= budget) {
        out.push_back(std::string(depth * 4, ' ') + "…（已截断）");
        return;
    }
    for (auto& b : blocks) {
        if ((int)out.size() >= budget) {
            out.push_back(std::string(depth * 4, ' ') + "…（已截断）");
            return;
        }
        if (!isBlock(b)) continue;
        const std::string& op = (*b.list)[0].s;
        std::vector<Value> args;
        std::vector<std::vector<Value>> subs;
        for (size_t i = 1; i < b.list->size(); ++i) {
            if (isSubstack((*b.list)[i])) {
                std::vector<Value> sub;
                for (auto& x : *(*b.list)[i].list) sub.push_back(x);
                subs.push_back(std::move(sub));
            } else {
                args.push_back((*b.list)[i]);
            }
        }
        out.push_back(std::string(depth * 4, ' ') + renderOp(op, args));
        for (size_t i = 0; i < subs.size(); ++i) {
            if (i == 1) out.push_back(std::string(depth * 4, ' ') + "否则");
            walkStack(subs[i], depth + 1, out, budget);
        }
    }
}

std::vector<std::string> renderScript(const Value& script, int budget) {
    std::vector<Value> blocks;
    if (script.kind == Value::Kind::List && script.list &&
        script.list->size() >= 2 && (*script.list)[1].kind == Value::Kind::List) {
        blocks = *(*script.list)[1].list;
    }
    std::vector<std::string> out;
    walkStack(blocks, 0, out, budget);
    return out;
}


} // namespace sb
