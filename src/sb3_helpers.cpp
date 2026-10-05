// src/sb3_helpers.cpp —— 目标汇总 / 素材 / 扫描等辅助（读取走 simdjson DOM）
#include "sb3.hpp"
#include "sb3_internal.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>

namespace sb {

// ==========================================================================

Elem sb3BlocksOf(const Elem& target) {
    return target.at("blocks");
}

int sb3TopScriptCount(const Elem& target) {
    Elem bl = target.at("blocks");
    if (!bl.is_object()) return 0;
    int n = 0;
    for (auto f : bl.obj()) {
        Elem b(f.value);
        if (!b.is_object()) continue;
        if (b.at("topLevel").b() && !b.at("shadow").b()) ++n;
    }
    return n;
}

std::vector<std::pair<std::string, Elem>>
sb3TopScripts(const Elem& target) {
    std::vector<std::pair<std::string, Elem>> tmp;
    Elem bl = target.at("blocks");
    if (!bl.is_object()) return tmp;
    for (auto f : bl.obj()) {
        Elem b(f.value);
        if (!b.is_object()) continue;
        if (b.at("topLevel").b() && !b.at("shadow").b())
            tmp.emplace_back(std::string(f.key), b);
    }
    std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) {
        long long ya = a.second.i64At("y", 0), yb = b.second.i64At("y", 0);
        if (ya != yb) return ya > yb;
        long long xa = a.second.i64At("x", 0), xb = b.second.i64At("x", 0);
        if (xa != xb) return xa < xb;
        // 同位置按积木 id 决胜（对齐旧版 std::map 迭代顺序）
        return a.first < b.first;
    });
    return tmp;
}

std::vector<std::string> sb3AssetFile(const Elem& obj, const std::string& kind) {
    std::vector<std::string> out;
    auto add = [&](const std::string& x) {
        if (!x.empty() &&
            std::find(out.begin(), out.end(), x) == out.end())
            out.push_back(x);
    };
    std::string fmt;
    Elem df = obj.at("dataFormat");
    if (df.is_string()) fmt = std::string(df.sv());
    else {
        Elem sf = obj.at("soundFormat");
        if (sf.is_string()) fmt = std::string(sf.sv());
    }

    for (auto key : {"md5ext", "md5", "baseLayerMD5", "assetId"}) {
        Elem v = obj.at(key);
        if (!v.is_string()) continue;
        std::string val = std::string(v.sv());
        add(val);
        std::string low = val;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return (char)::tolower(c); });
        if (!fmt.empty() && low.rfind("." + fmt) != low.size() - (fmt.size() + 1))
            add(val + "." + fmt);
    }
    long long num = -1;
    Elem bid = obj.at("baseLayerID");
    if (bid.is_number()) num = bid.i64();
    else {
        Elem sid = obj.at("soundID");
        if (sid.is_number()) num = sid.i64();
    }
    if (num >= 0) {
        std::string ext;
        for (auto key : {"md5ext", "md5", "baseLayerMD5"}) {
            Elem v = obj.at(key);
            if (v.is_string()) {
                std::string val = std::string(v.sv());
                auto pos = val.find_last_of('.');
                if (pos != std::string::npos) { ext = val.substr(pos); break; }
            }
        }
        if (ext.empty() && !fmt.empty()) ext = "." + fmt;
        add(std::to_string(num) + ext);
    }
    return out;
}

std::string sb3ResolveEntry(const std::map<std::string, std::string>& lower,
                            const std::vector<std::string>& candidates) {
    auto tryKey = [&](const std::string& k) -> std::string {
        std::string l = k;
        std::transform(l.begin(), l.end(), l.begin(),
                       [](unsigned char ch) { return (char)::tolower(ch); });
        auto it = lower.find(l);
        return (it != lower.end()) ? it->second : std::string();
    };
    // 一级：候选原样精确匹配（含路径）
    for (auto& c : candidates) {
        std::string hit = tryKey(c);
        if (!hit.empty()) return hit;
    }
    // 二级：补/去 assets/ 前缀（Scratch 官方 .sb3 布局是 assets/<md5ext>；也有作品放 zip 根）
    for (auto& c : candidates) {
        if (c.rfind("assets/", 0) != 0) {
            std::string hit = tryKey("assets/" + c);
            if (!hit.empty()) return hit;
        } else {
            std::string hit = tryKey(c.substr(7));
            if (!hit.empty()) return hit;
        }
    }
    // 三级：basename 兜底（容忍任意子目录）
    for (auto& c : candidates) {
        std::string base = c;
        size_t sl = base.find_last_of("/\\");
        if (sl != std::string::npos) base = base.substr(sl + 1);
        if (base.empty()) continue;
        std::string bl = base;
        std::transform(bl.begin(), bl.end(), bl.begin(),
                       [](unsigned char ch) { return (char)::tolower(ch); });
        for (auto& kv : lower) {
            std::string name = kv.first;
            size_t s2 = name.find_last_of("/\\");
            if (s2 != std::string::npos) name = name.substr(s2 + 1);
            if (name == bl) return kv.second;
        }
    }
    return "";
}

// 提取文字（sb3_collect_text）
Sb3Collect sb3CollectText(const Elem& target, bool wantStrings) {
    Sb3Collect out;
    static const std::set<std::string> DIALOG_OPS = {
        "looks_say", "looks_sayforsecs", "looks_think",
        "looks_thinkforsecs", "sensing_askandwait",
    };
    Elem bl = target.at("blocks");
    if (bl.is_object()) {
        // 按键排序遍历（对齐 nlohmann std::map 顺序）；一次性收集再排序，
        // 避免对每个键 at()（simdjson 线性扫描）造成 O(n²)
        auto entries = sortedEntriesOf(bl.obj());
        // 惰性构造 Renderer：没有对话积木就不建整张块表（大工程几十万积木时省大量分配）
        std::optional<Renderer> r;
        for (auto& e : entries) {
            Elem b(e.second);
            if (!b.is_object()) continue;
            if (b.at("shadow").b()) continue;
            std::string op = std::string(b.at("opcode").sv());
            auto dit = DIALOG_OPS.find(op);
            if (dit != DIALOG_OPS.end()) {
                if (!r) r.emplace(target);
                std::string key = (op == "sensing_askandwait") ? "QUESTION" : "MESSAGE";
                std::string v = r->inputText(b, key);
                if (v != "?" && !v.empty()) {
                    std::string tag = (op.find("think") != std::string::npos) ? "思考"
                                     : (op == "sensing_askandwait") ? "询问" : "说";
                    out.dialog.emplace_back(tag, v);
                }
            }
            if (wantStrings) {
                Elem inputs = b.at("inputs");
                if (!inputs.is_object()) continue;
                for (auto inF : inputs.obj()) {
                    Elem arr(inF.value);
                    if (!arr.is_array() || arr.size() <= 1) continue;
                    Elem p = arr.op(1);
                    if (!p.is_array() || p.empty()) continue;
                    if (p.op(0).i64() != 10) continue;
                    std::string s;
                    Elem p1 = p.op(1);
                    if (p1.is_string()) s = std::string(p1.sv());
                    else               s = compactJson(p1.raw());
                    if (s.empty()) continue;
                    bool blank = true;
                    for (char c : s) if (!std::isspace((unsigned char)c)) { blank = false; break; }
                    if (!blank) out.strings.emplace_back(op, std::string(inF.key), s);
                }
            }
        }
    }
    // 注释
    Elem comments = target.at("comments");
    if (comments.is_object()) {
        for (auto& ck : sortedKeysOf(comments.obj())) {
            Elem v = comments.at(ck);
            std::string txt;
            if (v.is_string()) txt = std::string(v.sv());
            else if (v.is_object()) {
                Elem t2 = v.at("text");
                if (t2.is_string()) txt = std::string(t2.sv());
            }
            if (!txt.empty() && txt[0] == '{') {
                try {
                    Json j = Json::parse(txt);
                    if (j.is_object() && j.contains("text") && j["text"].is_string())
                        txt = j["text"].get<std::string>();
                } catch (...) {}
            }
            bool blank = true;
            for (char c : txt) if (!std::isspace((unsigned char)c)) { blank = false; break; }
            if (!blank) out.comments.push_back(txt);
        }
    }
    return out;
}

// 目标汇总行（sb3_target_rows）
Json sb3TargetRows(const Elem& targets) {
    Json rows = Json::array();
    if (!targets.is_array()) return rows;
    for (auto te : targets.arr()) {
        Elem t(te);
        if (!t.is_object()) continue;
        int real = 0;
        // opcode 用视图键，避免每个积木分配 std::string（大工程热点）
        std::map<std::string_view, int> ops;
        Elem bl = t.at("blocks");
        if (bl.is_object()) {
            for (auto f : bl.obj()) {
                Elem b(f.value);
                if (!b.is_object()) continue;
                if (b.at("shadow").b()) continue;
                ++real;
                std::string_view op = b.at("opcode").sv();
                if (!op.empty()) ops[op]++;
            }
        }
        Json r;
        r["name"]      = std::string(t.at("name").sv());
        r["isStage"]   = t.at("isStage").b();
        Elem costumes  = t.at("costumes");
        Elem sounds    = t.at("sounds");
        Elem variables = t.at("variables");
        Elem lists     = t.at("lists");
        r["costumes"]  = (long long)(costumes.is_array()  ? costumes.size()  : 0);
        r["sounds"]    = (long long)(sounds.is_array()    ? sounds.size()    : 0);
        r["blocks"]    = (long long)real;
        r["scripts"]   = (long long)sb3TopScriptCount(t);
        r["variables"] = (long long)(variables.is_object() ? variables.size() : 0);
        r["lists"]     = (long long)(lists.is_object() ? lists.size() : 0);
        if (t.at("isStage").b()) {
            r["x"] = nullptr; r["y"] = nullptr; r["size"] = nullptr; r["visible"] = nullptr;
        } else {
            Elem vx = t.at("x");
            Elem vy = t.at("y");
            Elem vs = t.at("size");
            Elem vv = t.at("visible");
            r["x"] = vx.ok() ? toJson(vx) : Json(nullptr);
            r["y"] = vy.ok() ? toJson(vy) : Json(nullptr);
            r["size"] = vs.ok() ? toJson(vs) : Json(nullptr);
            r["visible"] = vv.ok() ? toJson(vv) : Json(nullptr);
        }
        Json top = Json::array();
        // 取出现次数前 5 的 opcode
        std::vector<std::pair<int, std::string>> sorted;
        for (auto& kv : ops) sorted.emplace_back(kv.second, kv.first);
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& x, const auto& y) { return x.first > y.first; });
        for (size_t i = 0; i < sorted.size() && i < 5; ++i) top.push_back(sorted[i].second);
        r["top_opcodes"] = std::move(top);
        rows.push_back(std::move(r));
    }
    return rows;
}

// 单个文件摘要（s3_summarize）
Json sb3Summarize(const std::string& path) {
    Sb3File sf = sb3LoadAny(path);
    Json out;
    out["path"] = path;
    out["name"] = basename(path);
    out["size"] = fileSize(path);
    out["mtime"] = fileMtime(path);
    out["kind"] = sf.kind;
    const Elem& targets = sf.targets;
    Json stage = nullptr;
    int sprites = 0;
    long long blocks = 0, scripts = 0, costumes = 0, sounds = 0,
              variables = 0, lists = 0;
    if (targets.is_array()) {
        for (auto te : targets.arr()) {
            Elem t(te);
            if (!t.is_object()) continue;
            bool isStage = t.at("isStage").b();
            if (isStage) stage = toJson(t);
            else ++sprites;
            Elem bl = t.at("blocks");
            if (bl.is_object()) {
                for (auto f : bl.obj()) {
                    Elem b(f.value);
                    if (!b.is_object()) continue;
                    if (!b.at("shadow").b()) ++blocks;
                }
            }
            scripts += (long long)sb3TopScriptCount(t);
            Elem cs = t.at("costumes");
            Elem sd = t.at("sounds");
            Elem va = t.at("variables");
            Elem li = t.at("lists");
            if (cs.is_array())   costumes  += (long long)cs.size();
            if (sd.is_array())   sounds    += (long long)sd.size();
            if (va.is_object())  variables += (long long)va.size();
            if (li.is_object())  lists     += (long long)li.size();
        }
    }
    out["stage"] = (stage.is_object() && stage.contains("name")) ? stage["name"] : Json(nullptr);
    out["sprites"] = sprites;
    out["targets"] = (long long)(targets.is_array() ? targets.size() : 0);
    out["blocks"] = blocks;
    out["scripts"] = scripts;
    out["costumes"] = costumes;
    out["sounds"] = sounds;
    out["variables"] = variables;
    out["lists"] = lists;
    Elem d(sf.data);
    Elem ext = d.at("extensions");
    if (ext.is_array()) out["extensions"] = toJson(ext);
    else out["extensions"] = Json::array();
    Elem meta = d.at("meta");
    if (!meta.ok()) meta = d.at("info");
    if (meta.ok()) out["meta"] = toJson(meta);
    else out["meta"] = Json::object();
    return out;
}

std::vector<std::string> findSb3Files(const std::string& root) {
    return walkFiles(root, {".sb3", ".sb2", ".sprite3", ".sb"},
                     {"__MACOSX", "$RECYCLE.BIN", "System Volume Information"});
}

// 扫描目录下的 sbcli 项目，返回全部 character/*/block.sbcli 路径。
// （meta.sbcli 也参与搜索，由调用方自行读文件）
std::vector<std::string> findSbcliBlockFiles(const std::string& root) {
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::path base = std::filesystem::u8path(root);
    // 从 root 出发，向上最多 0 层找项目根（root 可能就是项目根）
    std::filesystem::path cur = base;
    for (int k = 0; k < 4; ++k) {
        if (std::filesystem::is_directory(cur / "character", ec)) {
            std::filesystem::path cd = cur / "character";
            for (auto& de : std::filesystem::directory_iterator(cd, ec)) {
                if (!de.is_directory(ec)) continue;
                std::filesystem::path bp = de.path() / "block.sbcli";
                if (std::filesystem::exists(bp, ec))
                    out.push_back(bp.u8string());
            }
            return out;
        }
        // 往上层找（用户可能传了 character/ 或某个子目录）
        std::filesystem::path par = cur.parent_path();
        if (par == cur) break;
        cur = par;
    }
    return out;
}

// 名称清单（sb3_name_lists）
std::map<std::string, std::vector<std::string>>
sb3NameLists(const Elem& targets) {
    std::map<std::string, std::vector<std::string>> d = {
        {"角色", {}}, {"造型", {}}, {"声音", {}}, {"变量", {}}, {"列表", {}}, {"广播", {}},
    };
    if (!targets.is_array()) return d;
    for (auto te : targets.arr()) {
        Elem t(te);
        if (!t.is_object()) continue;
        std::string label;
        Elem nameEl = t.at("name");
        // 与旧版 value("name", "?") 一致：缺失/非字符串 → "?"；空字符串保留 ""
        if (nameEl.is_string()) label = std::string(nameEl.sv());
        else                    label = "?";
        d["角色"].push_back(label + (t.at("isStage").b() ? "（舞台）" : ""));
        Elem costumes = t.at("costumes");
        if (costumes.is_array())
            for (auto ce : costumes.arr())
                d["造型"].push_back(label + "/" + std::string(Elem(ce).at("name").sv()));
        Elem sounds = t.at("sounds");
        if (sounds.is_array())
            for (auto se : sounds.arr())
                d["声音"].push_back(label + "/" + std::string(Elem(se).at("name").sv()));
        Elem variables = t.at("variables");
        if (variables.is_object())
            for (auto& vk : sortedKeysOf(variables.obj())) {
                Elem v = variables.at(vk);
                if (v.is_array() && !v.empty() && v.op(0).ok())
                    d["变量"].push_back(label + "/" + compactJson(v.op(0).raw()));
            }
        Elem lists = t.at("lists");
        if (lists.is_object())
            for (auto& lk : sortedKeysOf(lists.obj())) {
                Elem v = lists.at(lk);
                if (v.is_array() && !v.empty() && v.op(0).ok())
                    d["列表"].push_back(label + "/" + compactJson(v.op(0).raw()));
            }
    }
    std::set<std::string> seen;
    for (auto te : targets.arr()) {
        Elem t(te);
        if (!t.is_object()) continue;
        // 键排序遍历（对齐旧版 nlohmann 的 std::map 顺序）
        Elem bcasts = t.at("broadcasts");
        if (bcasts.is_object())
            for (auto& bk : sortedKeysOf(bcasts.obj())) {
                Elem bv = bcasts.at(bk);
                std::string nm = compactJson(bv.raw());
                if (seen.insert(nm).second) d["广播"].push_back(nm);
            }
        Elem bl = t.at("blocks");
        if (bl.is_object()) {
            // 一次性收集排序，避免 O(n²) 的逐键 at()
            for (auto& e : sortedEntriesOf(bl.obj())) {
                Elem b(e.second);
                if (!b.is_object()) continue;
                Elem fields = b.at("fields");
                if (fields.is_object()) {
                    Elem opF = fields.at("BROADCAST_OPTION");
                    if (opF.is_array() && !opF.empty() && opF.op(0).ok()) {
                        std::string nm = compactJson(opF.op(0).raw());
                        if (seen.insert(nm).second) d["广播"].push_back(nm);
                    }
                }
            }
        }
    }
    return d;
}

} // namespace sb