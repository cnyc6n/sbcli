// src/sb3_render.cpp —— Renderer（积木翻译核心；读取走 simdjson DOM）
#include "sb3.hpp"
#include "sb3_tables.hpp"
#include "sb3_internal.hpp"
#include <algorithm>
#include <cstring>

namespace sb {

// ---- Renderer ----

Renderer::Renderer(const Elem& t) : m_t(t) {
    Elem blocks = t.at("blocks");
    if (blocks.is_object()) {
        for (auto f : blocks.obj())
            // 只存元素视图 + 键视图（不拷贝积木 JSON / 键字符串，避免大工程逐个分配）
            m_blocks[f.key] = Elem(f.value);
    }
    Elem variables = t.at("variables");
    if (variables.is_object()) {
        for (auto f : variables.obj()) {
            Elem v(f.value);
            if (!v.is_array() || v.empty()) continue;
            Elem name0 = v.op(0);
            m_varNames[std::string(f.key)] = name0.is_string()
                                             ? std::string(name0.sv())
                                             : (name0.ok() ? compactJson(name0.raw()) : "");
        }
    }
    Elem lists = t.at("lists");
    if (lists.is_object()) {
        for (auto f : lists.obj()) {
            Elem v(f.value);
            if (!v.is_array() || v.empty()) continue;
            Elem name0 = v.op(0);
            m_listNames[std::string(f.key)] = name0.is_string()
                                              ? std::string(name0.sv())
                                              : (name0.ok() ? compactJson(name0.raw()) : "");
        }
    }
    Elem broadcasts = t.at("broadcasts");
    if (broadcasts.is_object()) {
        for (auto f : broadcasts.obj()) {
            Elem v(f.value);
            m_bcastNames[std::string(f.key)] = v.is_string()
                                               ? std::string(v.sv())
                                               : (v.ok() ? compactJson(v.raw()) : "");
        }
    }
}

std::string Renderer::fieldTextOf(const Elem& b, const std::string& key) {
    Elem fields = b.at("fields");
    if (!fields.is_object()) return "";
    Elem v = fields.at(key);
    if (!v.ok()) return "";
    if (v.is_array()) {
        if (v.empty()) return "";
        Elem v0 = v.op(0);
        return v0.is_string() ? std::string(v0.sv()) : compactJson(v0.raw());
    }
    return v.is_string() ? std::string(v.sv()) : compactJson(v.raw());
}

std::string Renderer::rawPrim(const Elem& b, const std::string& key) {
    Elem inputs = b.at("inputs");
    if (!inputs.is_object()) return "";
    Elem arr = inputs.at(key);
    if (!arr.ok()) return "";
    if (arr.is_array() && arr.size() > 1) {
        Elem p = arr.op(1);
        if (p.is_array() && p.size() > 1) {
            Elem p1 = p.op(1);
            return p1.is_string() ? std::string(p1.sv()) : compactJson(p1.raw());
        }
        if (p.is_string()) return std::string(p.sv());
    }
    return "";
}

std::string Renderer::costumeNameByNumber(long long n) const {
    if (n < 1) return "";
    Elem cs = m_t.at("costumes");
    if (!cs.is_array()) return "";
    long long i = 1;
    for (auto ce : cs.arr()) {
        if (i == n) {
            Elem nameEl = Elem(ce).at("name");
            return nameEl.is_string() ? std::string(nameEl.sv()) : "";
        }
        ++i;
    }
    return "";
}

std::string Renderer::prim(const Elem& x, bool menu) {
    if (!x.ok() || x.is_null()) return "?";
    if (x.is_string()) return render(std::string(x.sv()));
    if (x.is_array() && !x.empty()) {
        int t = x.op(0).is_number() ? (int)x.op(0).i64() : 0;
        // 只取视图，不拷贝整个参数子树（大参数数组时是热点）
        Elem v = x.op(1);
        auto textOf = [&](const Elem& e) -> std::string {
            if (e.is_string()) {
                std::string s(e.sv());
                // 空字符串槽（作者留空）：渲染成 ∅
                return s.empty() ? "\u2205" : s;
            }
            if (e.ok()) {
                std::string s = compactJson(e.raw());
                // 空对象/空数组（如 shadow 占位）：∅
                if (s == "[]" || s == "{}") return "\u2205";
                return s;
            }
            return "\u2205";
        };
        if (t == 10) {
            std::string s = textOf(v);
            // 空文本槽（作者留空）：渲染成 ∅，避免出现裸空格（如 "( + 180)"）
            if (s.empty()) return "\u2205";
            // menu 翻译（SB2_MENU）
            if (menu) {
                auto it = SB2_MENU.find(s);
                if (it != SB2_MENU.end()) return it->second;
                // 大写形式（如 KEY_OPTION 的 "SPACE"）：统一转小写再查
                std::string low = s;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return (char)::tolower(c); });
                auto jt = SB2_MENU.find(low);
                if (jt != SB2_MENU.end()) return jt->second;
            }
            return s;
        }
        if (t == 4 || t == 5 || t == 6 || t == 7 || t == 8 || t == 9) {
            if (v.is_integer()) return std::to_string(v.i64());
            if (v.is_float()) {
                char buf[64]; std::snprintf(buf, sizeof(buf), "%g", v.f64());
                return buf;
            }
            if (v.is_string()) {
                std::string id(v.sv());
                // sb2 变量/列表 shadow 的 [4/5, id]：查变量表转名字（有映射才算）
                if (!id.empty()) {
                    auto vit = m_varNames.find(id);
                    if (vit != m_varNames.end()) return vit->second;
                    auto lit = m_listNames.find(id);
                    if (lit != m_listNames.end()) return lit->second;
                }
                return id;
            }
            return textOf(v);
        }
        if (t == 11 || t == 12 || t == 13) {
            if (v.is_array() && !v.empty()) {
                Elem v0 = v.op(0);
                return v0.is_string() ? std::string(v0.sv()) : compactJson(v0.raw());
            }
        }
        return textOf(v);
    }
    return compactJson(x.raw());
}

std::string Renderer::inputValue(const Elem& arr, bool menu) {
    if (!arr.is_array() || arr.empty()) return "?";
    int kind = arr.op(0).is_number() ? (int)arr.op(0).i64() : 0;
    if (kind == 1) return prim(arr.op(1), menu);
    if (kind == 2 || kind == 3) {
        Elem x = arr.op(1);
        if (x.is_string()) return render(std::string(x.sv()));
        return prim(x, menu);
    }
    return "?";
}

std::string Renderer::inputText(const Elem& b, const std::string& key, bool menu) {
    Elem inputs = b.at("inputs");
    if (!inputs.is_object()) return "?";
    Elem v = inputs.at(key);
    if (!v.ok()) return "?";
    return inputValue(v, menu);
}

std::string Renderer::valueOf(const Elem& b, const std::string& key, bool menu) {
    Elem fields = b.at("fields");
    if (fields.is_object()) {
        // 大小写不敏感匹配：模板用大写 {COLOR_PARAM}，但扩展 menu 块的 field
        // 键可能是小写（pen_menu_colorParam → "colorParam"）。精确匹配优先，找不到再遍历。
        Elem v;
        std::string actualKey;
        if (fields.contains(key)) {
            v = fields.at(key);
            actualKey = key;
        } else {
            // 大小写 + 下划线归一后匹配：模板键是大写+下划线（COLOR_PARAM），
            // 扩展 menu 块的 field 键可能是驼峰小写（colorParam）。
            // 只转小写不够（color_param ≠ colorparam），必须同时去下划线。
            auto norm = [](std::string s) {
                std::string out;
                for (char c : s) {
                    if (c == '_') continue;
                    out += (char)::tolower((unsigned char)c);
                }
                return out;
            };
            std::string target = norm(key);
            for (auto& ent : sortedEntriesOf(fields.obj())) {
                if (norm(std::string(ent.first)) == target) {
                    v = Elem(ent.second);
                    actualKey = std::string(ent.first);
                    break;
                }
            }
        }
        if (v.ok()) {
            // field 映射（全局 FIELD_MAP；键匹配也大小写不敏感）
            std::string val = v.is_array() ? (v.empty() ? "" : compactJson(v.op(0).raw()))
                                           : compactJson(v.raw());
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                val = val.substr(1, val.size() - 2);
            auto it = FIELD_MAP.find(actualKey);
            if (it == FIELD_MAP.end()) {
                // 用模板给的键（大写）再试一次 FIELD_MAP
                it = FIELD_MAP.find(key);
            }
            if (it != FIELD_MAP.end()) {
                auto jt = it->second.find(val);
                if (jt != it->second.end()) return jt->second;
            }
            return val;
        }
    }
    Elem inputs = b.at("inputs");
    if (inputs.is_object() && inputs.contains(key)) {
        return inputText(b, key, menu);
    }
    return "?";
}

std::string Renderer::generic(const Elem& b) {
    std::vector<std::string> bits;
    // 常见扩展积木参数名 → 中文（text/music/translate 等扩展）
    static const std::map<std::string, std::string> PARAM_ZH = {
        {"FONT", "字体"}, {"COLOR", "颜色"}, {"COLOR2", "颜色2"}, {"COLOR3", "颜色3"},
        {"TEXT", "文字"}, {"TEXT1", "文字1"}, {"TEXT2", "文字2"}, {"MESSAGE", "消息"},
        {"MUSIC", "音乐"}, {"FILE", "文件"}, {"PATH", "路径"}, {"NAME", "名称"},
        {"VOLUME", "音量"}, {"SPEED", "速度"}, {"PITCH", "音调"}, {"RATE", "速率"},
        {"LANG", "语言"}, {"LANGUAGE", "语言"}, {"URL", "地址"}, {"ID", "编号"},
        {"X", "x"}, {"Y", "y"}, {"SIZE", "大小"}, {"SCALE", "缩放"}, {"TIMES", "次数"},
        {"SECS", "秒数"}, {"START", "起点"}, {"END", "终点"}, {"INDEX", "序号"},
        {"DELAY", "延迟"}, {"DURATION", "时长"}, {"TIMES2", "次数2"}, {"ANGLE", "角度"},
        {"RADIUS", "半径"}, {"TARGET", "目标"}, {"OPTION", "选项"}, {"VALUE", "值"},
        {"ANSWER", "回答"}, {"HEIGHT", "高度"}, {"WIDTH", "宽度"}, {"COSTUME", "造型"},
    };
    auto zh = [&](const std::string& k) {
        auto it = PARAM_ZH.find(k);
        return it != PARAM_ZH.end() ? it->second : k;
    };
    // 键排序：与 nlohmann 的 std::map 迭代顺序一致（否则输出顺序会变）
    Elem fields = b.at("fields");
    if (fields.is_object()) {
        for (auto& k : sortedKeysOf(fields.obj()))
            bits.push_back(zh(k) + "=" + valueOf(b, k));
    }
    Elem inputs = b.at("inputs");
    if (inputs.is_object()) {
        for (auto& k : sortedKeysOf(inputs.obj()))
            bits.push_back(zh(k) + "=" + inputText(b, k));
    }
    std::string body;
    for (size_t i = 0; i < bits.size(); ++i) {
        if (i) body += ", ";
        body += bits[i];
    }
    std::string op = std::string(b.at("opcode").sv());
    return "⟨" + op + "⟩" + (body.empty() ? "" : ("(" + body + ")"));
}

std::string Renderer::customSignature(const Elem& b) {
    Elem inputs = b.at("inputs");
    if (!inputs.is_object()) return "自定义积木";
    Elem inp = inputs.at("custom_block");
    if (!inp.ok() || !inp.is_array() || inp.size() < 2) return "自定义积木";
    Elem sub = inp.op(1);
    if (!sub.is_string()) return "自定义积木";
    std::string_view sid = sub.sv();
    auto it = m_blocks.find(sid);
    if (it == m_blocks.end()) return "自定义积木";
    const Elem& proto = it->second;
    Elem mutation = proto.at("mutation");
    if (!mutation.ok()) return "自定义积木";
    std::string code;
    Elem pc = mutation.at("proccode");
    if (pc.is_string()) code = std::string(pc.sv());
    else                code = "?";
    std::vector<std::string> names;
    Elem an = mutation.at("argumentnames");
    if (an.is_string()) {
        try {
            Json arr = Json::parse(std::string(an.sv()));
            for (auto& n : arr) names.push_back(n.get<std::string>());
        } catch (...) {}
    }
    return fillProccode(code, names);
}

std::string Renderer::customCall(const Elem& b) {
    Elem mutation = b.at("mutation");
    if (!mutation.ok()) return "调用自定义积木";
    std::string code;
    Elem pc = mutation.at("proccode");
    if (pc.is_string()) code = std::string(pc.sv());
    if (code.empty()) return "调用自定义积木";
    std::vector<std::string> argids;
    Elem ai = mutation.at("argumentids");
    if (ai.is_string()) {
        try {
            Json arr = Json::parse(std::string(ai.sv()));
            for (auto& x : arr) argids.push_back(x.get<std::string>());
        } catch (...) {}
    }

    std::string out;
    size_t i = 0, k = 0;
    while (i < code.size()) {
        if (code[i] == '%' && i + 1 < code.size() &&
            (code[i+1] == 'n' || code[i+1] == 's' || code[i+1] == 'b')) {
            std::string aid = (k < argids.size()) ? argids[k++] : "";
            out += aid.empty() ? "?" : inputText(b, aid);
            i += 2;
        } else {
            out += code[i++];
        }
    }
    return out;
}

std::string Renderer::sb2Call(const Elem& b) {
    std::string code = rawPrim(b, "__1");
    if (code.empty()) return "调用自定义积木";
    std::vector<std::string> args;
    int i = 2;
    while (true) {
        Elem inputs = b.at("inputs");
        if (!inputs.contains("__" + std::to_string(i))) break;
        args.push_back(inputText(b, "__" + std::to_string(i)));
        ++i;
    }
    std::string out;
    size_t p = 0, k = 0;
    while (p < code.size()) {
        if (code[p] == '%' && p + 1 < code.size() &&
            (code[p+1] == 'n' || code[p+1] == 's' || code[p+1] == 'b')) {
            out += (k < args.size()) ? args[k++] : "?";
            p += 2;
        } else {
            out += code[p++];
        }
    }
    return out;
}

std::string Renderer::render(const std::string& bid) {
    // 递归深度保护：某些作品里块引用会成环（A 引用 B、B 引用 A），
    // 无限制递归会栈溢出。显式限深。
    if (m_renderDepth > 40) return "?";
    struct DepthGuard {
        int& ref;
        DepthGuard(int& r) : ref(r) { ++ref; }
        ~DepthGuard() { --ref; }
    } guard(m_renderDepth);

    // 哈希查找（m_blocks 存 id → 块视图）
    auto it = m_blocks.find(std::string_view(bid));
    if (it == m_blocks.end()) return "?";
    const Elem& b = it->second;
    std::string op = std::string(b.at("opcode").sv());
    if (op == "procedures_definition") return "定义 " + customSignature(b);
    if (op == "procedures_call")       return customCall(b);
    if (op == "procDef") {
        std::string code = fieldTextOf(b, "PROCCODE");
        std::string args = fieldTextOf(b, "ARGS");
        std::vector<std::string> names;
        std::string tmp;
        for (char c : args) {
            if (c == ' ' || c == '\t') { if (!tmp.empty()) { names.push_back(tmp); tmp.clear(); } }
            else tmp += c;
        }
        if (!tmp.empty()) names.push_back(tmp);
        return "定义 " + fillProccode(code, names);
    }
    if (op == "call") return sb2Call(b);

    // 自定义积木参数：Scratch 里是椭圆 reporter，用括号括起来，
    // 避免出现「按下 键 键?」这种参数名与模板文字重复的歧义。
    if (op == "argument_reporter_string_number" || op == "argument_reporter_boolean") {
        std::string v = valueOf(b, "VALUE");
        if (v.empty() || v == "?") return "?";
        return "(" + v + ")";
    }

    // 在两张表里找模板：分开查找，避免跨容器迭代器比较（UB）
    const std::string* tplPtr = nullptr;
    auto tit = SB3_T.find(op);
    if (tit != SB3_T.end()) tplPtr = &tit->second;
    else {
        auto t2 = SB2_T.find(op);
        if (t2 != SB2_T.end()) tplPtr = &t2->second;
    }
    if (tplPtr == nullptr) {
        missing[op]++;
        return generic(b);
    }
    const std::string& tpl = *tplPtr;

    // 处理 {NAME} 占位符（手写扫描，避免 MinGW 的 std::regex 偶发崩溃，也更快）
    std::string out;
    size_t p = 0;
    while (true) {
        size_t open = tpl.find('{', p);
        if (open == std::string::npos) {
            out.append(tpl, p, std::string::npos);
            break;
        }
        out.append(tpl, p, open - p);
        size_t close = tpl.find('}', open);
        if (close == std::string::npos) {
            out.append(tpl, open, std::string::npos);
            break;
        }
        std::string key = tpl.substr(open + 1, close - open - 1);
        // 菜单判断（SB2_MENU_POS）
        bool menu = false;
        auto mp = SB2_MENU_POS_MAP.find(op);
        if (mp != SB2_MENU_POS_MAP.end()) {
            if (key.size() > 2 && key[0] == '_' && key[1] == '_') {
                try {
                    int idx = std::stoi(key.substr(2));
                    if (mp->second.count(idx)) menu = true;
                } catch (...) {}
            }
        }
        out += valueOf(b, key, menu);
        p = close + 1;
    }
    // 造型/背景按编号切换时，注解出实际名字：换成 2 造型（=佩奇）
    if (op == "looks_switchcostumeto" || op == "looks_switchbackdropto") {
        const char* keyName = (op == "looks_switchcostumeto") ? "COSTUME" : "BACKDROP";
        // 用渲染后的值判断（原始可能是块 id；渲染后才是 "12" 这样的数字）
        std::string val = valueOf(b, keyName);
        if (!val.empty()) {
            bool numeric = true;
            for (char c : val) if (!std::isdigit((unsigned char)c)) { numeric = false; break; }
            if (numeric) {
                long long n = 0;
                try { n = std::stoll(val); } catch (...) { n = 0; }
                std::string nm = costumeNameByNumber(n);
                if (!nm.empty()) out += "（=" + nm + "）";
            }
        }
    }
    return out;
}

void Renderer::stack(const std::string& bid, int depth,
                     std::vector<std::string>& lines, int budget) {
    std::string cur = bid;
    while (!cur.empty() && (budget == 0 || (int)lines.size() < budget)) {
        auto it = m_blocks.find(std::string_view(cur));
        if (it == m_blocks.end()) return;
        const Elem& b = it->second;
        lines.push_back(std::string(depth * 4, ' ') + render(cur));

        for (auto& key : {"SUBSTACK", "SUBSTACK2"}) {
            Elem inputs = b.at("inputs");
            if (!inputs.is_object()) continue;
            Elem val = inputs.at(key);
            if (!val.is_array() || val.size() < 2) continue;
            Elem child = val.op(1);
            if (std::string(key) == "SUBSTACK2")
                lines.push_back(std::string(depth * 4, ' ') + "否则");
            if (child.is_string())
                stack(std::string(child.sv()), depth + 1, lines, budget);
        }
        Elem next = b.at("next");
        if (next.is_string()) cur = std::string(next.sv());
        else                  cur.clear();
    }
    if (budget > 0 && (int)lines.size() >= budget)
        lines.push_back(std::string(depth * 4, ' ') + "…（已截断）");
}

std::vector<std::vector<std::string>> Renderer::scripts(int budget,
                                                        size_t maxScriptCount) {
    std::vector<std::pair<std::string, Elem>> tops;
    Elem bl = m_t.at("blocks");
    if (bl.is_object()) {
        for (auto f : bl.obj()) {
            Elem b(f.value);
            if (!b.is_object()) continue;
            if (b.at("topLevel").b() && !b.at("shadow").b())
                tops.emplace_back(std::string(f.key), b);
        }
    }
    std::sort(tops.begin(), tops.end(), [](const auto& a, const auto& b) {
        long long ya = a.second.i64At("y", 0), yb = b.second.i64At("y", 0);
        if (ya != yb) return ya > yb;
        long long xa = a.second.i64At("x", 0), xb = b.second.i64At("x", 0);
        if (xa != xb) return xa < xb;
        // 同位置按积木 id 决胜（对齐旧版 nlohmann std::map 的迭代顺序）
        return a.first < b.first;
    });
    std::vector<std::vector<std::string>> out;
    for (auto& p : tops) {
        if (maxScriptCount > 0 && out.size() >= maxScriptCount) break;
        std::vector<std::string> lines;
        stack(p.first, 0, lines, budget);
        out.push_back(std::move(lines));
    }
    return out;
}

} // namespace sb