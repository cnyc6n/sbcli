// src/sb3_diff.cpp —— 版本对比（sb diff）：对比两个作品的
//   角色增删 / 每角色积木结构（opcode 分布）/ 变量列表变化。
#include "sb3.hpp"
#include "sb3_tables.hpp"
#include "sb3_internal.hpp"
#include "commands.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <set>

namespace sb {

namespace {

// 一个角色的快照：积木 opcode 计数 + 变量/列表名 + 顶层脚本数
struct SpriteSnap {
    std::map<std::string, long long> ops;           // opcode → 次数
    std::map<std::string, long long> varDecl;        // 变量 id → 1（仅计数集）
    std::set<std::string> varNames, listNames;
    long long topScripts = 0;
    long long costumes = 0, sounds = 0;
    // opcode → 该角色里一个代表块（渲染成 script 风格译文用）
    std::map<std::string, std::string> opExample;    // opcode → 块 id
};

SpriteSnap snapSprite(const Elem& t) {
    SpriteSnap s;
    Elem bl = t.at("blocks");
    if (bl.is_object()) {
        for (auto f : bl.obj()) {
            Elem b(f.value);
            if (!b.is_object()) continue;
            if (b.at("shadow").b()) continue;
            std::string op(b.at("opcode").sv());
            s.ops[op]++;
            if (!s.opExample.count(op)) s.opExample[op] = std::string(f.key);
            if (b.at("topLevel").b()) ++s.topScripts;
        }
    }
    Elem vars = t.at("variables");
    if (vars.is_object()) {
        for (auto& vk : sortedKeysOf(vars.obj())) {
            Elem v = vars.at(vk);
            if (v.is_array() && !v.empty() && v.op(0).ok()) {
                Elem n0 = v.op(0);
                std::string nm = n0.is_string() ? std::string(n0.sv())
                                                : compactJson(n0.raw());
                s.varNames.insert(nm);
            }
        }
    }
    Elem lists = t.at("lists");
    if (lists.is_object()) {
        for (auto& lk : sortedKeysOf(lists.obj())) {
            Elem v = lists.at(lk);
            if (v.is_array() && !v.empty() && v.op(0).ok()) {
                Elem n0 = v.op(0);
                std::string nm = n0.is_string() ? std::string(n0.sv())
                                                : compactJson(n0.raw());
                s.listNames.insert(nm);
            }
        }
    }
    Elem cs = t.at("costumes");
    if (cs.is_array()) s.costumes = (long long)cs.size();
    Elem so = t.at("sounds");
    if (so.is_array()) s.sounds = (long long)so.size();
    return s;
}

std::map<std::string, SpriteSnap> snapAll(const Elem& targets) {
    std::map<std::string, SpriteSnap> out;
    if (targets.is_array()) {
        for (auto te : targets.arr()) {
            Elem t(te);
            if (!t.is_object()) continue;
            std::string nm(std::string(t.at("name").sv()));
            bool stg = t.at("isStage").b();
            std::string key = stg ? "Stage" : nm;
            SpriteSnap s = snapSprite(t);
            // 同名的多个角色（克隆体技术）会覆盖键：保留第一个调用 order
            if (!out.count(key) || !stg) out[key] = std::move(s);
        }
    }
    return out;
}

} // namespace

int s3_cmd_diff(const Args& a) {
    if (a.extra.empty()) {
        std::cerr << "用法：sb diff <旧文件> <新文件> [--by-sprite]\n";
        return 2;
    }
    std::string oldF = a.file, newF = a.extra[0];
    Sb3File sfO = sb3LoadAny(oldF);
    Sb3File sfN = sb3LoadAny(newF);

    auto oldS = snapAll(sfO.targets);
    auto newS = snapAll(sfN.targets);

    if (a.json) {
        Json out = Json::object();
        out["old"] = oldF;
        out["new"] = newF;
        Json removed = Json::array(), added = Json::array(), changed = Json::array();
        for (auto& kv : oldS) if (!newS.count(kv.first)) removed.push_back(kv.first);
        for (auto& kv : newS) if (!oldS.count(kv.first)) added.push_back(kv.first);
        for (auto& kv : newS) {
            auto oIt = oldS.find(kv.first);
            if (oIt == oldS.end()) continue;
            Json c = Json::object();
            c["sprite"] = kv.first;
            // opcode 增减
            Json ops = Json::object();
            for (auto& op : kv.second.ops) {
                long long d = op.second - (oIt->second.ops.count(op.first) ? oIt->second.ops.at(op.first) : 0);
                if (d != 0) ops[op.first] = d;
            }
            for (auto& op : oIt->second.ops)
                if (!kv.second.ops.count(op.first)) ops[op.first] = -op.second;
            c["opcodeDelta"] = ops;
            // 变量/列表增删
            Json varsA = Json::array();
            for (auto& v : kv.second.varNames) if (!oIt->second.varNames.count(v)) varsA.push_back(v);
            for (auto& v : oIt->second.varNames) if (!kv.second.varNames.count(v)) varsA.push_back("-" + v);
            c["varDelta"] = varsA;
            Json listsA = Json::array();
            for (auto& v : kv.second.listNames) if (!oIt->second.listNames.count(v)) listsA.push_back(v);
            for (auto& v : oIt->second.listNames) if (!kv.second.listNames.count(v)) listsA.push_back("-" + v);
            c["listDelta"] = listsA;
            c["scriptsDelta"] = (long long)(kv.second.topScripts - oIt->second.topScripts);
            c["costumesDelta"] = (long long)(kv.second.costumes - oIt->second.costumes);
            c["soundsDelta"] = (long long)(kv.second.sounds - oIt->second.sounds);
            changed.push_back(std::move(c));
        }
        out["removedSprites"] = removed;
        out["addedSprites"] = added;
        out["changedSprites"] = changed;
        sb3JsonOut(out);
        return 0;
    }

    std::cout << "对比：" << basename(oldF) << "  →  " << basename(newF) << "\n\n";

    // 角色增删
    std::vector<std::string> removed, added;
    for (auto& kv : oldS) if (!newS.count(kv.first)) removed.push_back(kv.first);
    for (auto& kv : newS) if (!oldS.count(kv.first)) added.push_back(kv.first);
    if (!removed.empty()) {
        std::cout << "删除的角色：\n";
        for (auto& s : removed) std::cout << "  - " << s << "\n";
    }
    if (!added.empty()) {
        std::cout << "新增的角色：\n";
        for (auto& s : added) std::cout << "  + " << s
                            << "（积木 " << newS.at(s).ops.size() << " 种，"
                            << "脚本 " << newS.at(s).topScripts << "）\n";
    }

    // 角色级变化
    auto countDelta = [](auto& ns, auto& os) {
        long long n = 0, o = 0;
        for (auto& kv : ns.ops) n += kv.second;
        for (auto& kv : os.ops) o += kv.second;
        return n - o;
    };
    for (auto& kv : newS) {
        auto oIt = oldS.find(kv.first);
        if (oIt == oldS.end()) continue;
        SpriteSnap& ns = kv.second;
        SpriteSnap& os = oIt->second;
        // opcode 变化
        std::map<std::string, long long> delta;
        for (auto& op : ns.ops) {
            long long d = op.second - (os.ops.count(op.first) ? os.ops.at(op.first) : 0);
            if (d != 0) delta[op.first] = d;
        }
        for (auto& op : os.ops)
            if (!ns.ops.count(op.first)) delta[op.first] = -op.second;
        // 变量/列表变化
        std::vector<std::string> vAdd, vDel, lAdd, lDel;
        for (auto& v : ns.varNames) if (!os.varNames.count(v)) vAdd.push_back(v);
        for (auto& v : os.varNames) if (!ns.varNames.count(v)) vDel.push_back(v);
        for (auto& v : ns.listNames) if (!os.listNames.count(v)) lAdd.push_back(v);
        for (auto& v : os.listNames) if (!ns.listNames.count(v)) lDel.push_back(v);

        bool any = !delta.empty() || !vAdd.empty() || !vDel.empty() ||
                   !lAdd.empty() || !lDel.empty() ||
                   ns.topScripts != os.topScripts ||
                   ns.costumes != os.costumes || ns.sounds != os.sounds;
        if (!any) continue;

        // 默认：折叠视图（一行摘要）。--by-sprite / --detail：展开积木明细。
        if (!a.bySprite && !a.detail) {
            std::cout << "  " << kv.first << "：";
            std::vector<std::string> parts;
            if (ns.topScripts != os.topScripts)
                parts.push_back("脚本 " + std::to_string(os.topScripts) + "→"
                                + std::to_string(ns.topScripts));
            if (ns.costumes != os.costumes)
                parts.push_back("造型 " + std::to_string(os.costumes) + "→"
                                + std::to_string(ns.costumes));
            if (ns.sounds != os.sounds)
                parts.push_back("声音 " + std::to_string(os.sounds) + "→"
                                + std::to_string(ns.sounds));
            long long bd = countDelta(ns, os);
            if (bd != 0) parts.push_back("积木 " + std::to_string(bd > 0 ? bd : -bd)
                                         + (bd > 0 ? " 增" : " 减"));
            if (!delta.empty())
                parts.push_back(std::to_string(delta.size()) + " 种积木变化");
            if (!vAdd.empty() || !vDel.empty())
                parts.push_back("变量±" + std::to_string(vAdd.size() + vDel.size()));
            if (!lAdd.empty() || !lDel.empty())
                parts.push_back("列表±" + std::to_string(lAdd.size() + lDel.size()));
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) std::cout << "，";
                std::cout << parts[i];
            }
            std::cout << "\n";
            continue;
        }

        std::cout << "\n──── 角色：" << kv.first << "\n";
        if (ns.topScripts != os.topScripts) {
            long long d = ns.topScripts - os.topScripts;
            std::cout << "  脚本：" << os.topScripts << " → " << ns.topScripts
                      << "（" << (d > 0 ? "+" : "") << d << "）\n";
        }
        if (ns.costumes != os.costumes)
            std::cout << "  造型：" << os.costumes << " → " << ns.costumes << "\n";
        if (ns.sounds != os.sounds)
            std::cout << "  声音：" << os.sounds << " → " << ns.sounds << "\n";
        if (!delta.empty()) {
            std::cout << "  积木变化：\n";
            // 按绝对值排序
            std::vector<std::pair<long long, std::string>> srt;
            for (auto& d : delta) srt.emplace_back(d.second, d.first);
            std::sort(srt.begin(), srt.end(), [](const auto& x, const auto& y) {
                return std::llabs(x.first) > std::llabs(y.first);
            });
            for (auto& d : srt) {
                std::string label;
                // 优先：用新文件里该 opcode 的代表块渲染成 script 风格译文（真实参数）
                if (sfN.targets.is_array()) {
                    for (auto te : sfN.targets.arr()) {
                        Elem t(te);
                        if (!t.is_object()) continue;
                        std::string tn(std::string(t.at("name").sv()));
                        bool stg = t.at("isStage").b();
                        std::string key = stg ? "Stage" : tn;
                        if (key != kv.first) continue;
                        auto exIt = ns.opExample.find(d.second);
                        if (exIt != ns.opExample.end()) {
                            Renderer rr(t);
                            label = rr.render(exIt->second);
                        }
                        break;
                    }
                }
                // 回退：模板原文（去占位符）
                if (label.empty()) {
                    const std::string* tplPtr = nullptr;
                    auto it = SB3_T.find(d.second);
                    if (it != SB3_T.end()) tplPtr = &it->second;
                    else {
                        auto t2 = SB2_T.find(d.second);
                        if (t2 != SB2_T.end()) tplPtr = &t2->second;
                    }
                    if (tplPtr) {
                        label = *tplPtr;
                        label.erase(std::remove(label.begin(), label.end(), '{'), label.end());
                        label.erase(std::remove(label.begin(), label.end(), '}'), label.end());
                    } else {
                        label = d.second;
                    }
                }
                if (label.size() > 50) label = label.substr(0, 50) + "…";
                std::cout << "    " << (d.first > 0 ? "+" : "")
                          << d.first << "  " << label << "\n";
            }
        }
        if (!vAdd.empty()) {
            std::cout << "  新增变量：";
            for (auto& v : vAdd) std::cout << v << "、";
            std::cout << "\n";
        }
        if (!vDel.empty()) {
            std::cout << "  删除变量：";
            for (auto& v : vDel) std::cout << v << "、";
            std::cout << "\n";
        }
        if (!lAdd.empty()) {
            std::cout << "  新增列表：";
            for (auto& v : lAdd) std::cout << v << "、";
            std::cout << "\n";
        }
        if (!lDel.empty()) {
            std::cout << "  删除列表：";
            for (auto& v : lDel) std::cout << v << "、";
            std::cout << "\n";
        }
    }
    std::cout << "\n（全部对比完成；用 --json 可输出结构化结果）\n";
    return 0;
}

// ---- sb dup：检测相似（疑似复制粘贴）角色 ----

namespace {

// 两角色积木结构的 Jaccard 相似度（按 opcode 集合）
double spriteSimilarity(const SpriteSnap& a, const SpriteSnap& b) {
    if (a.ops.empty() && b.ops.empty()) return 0;
    std::set<std::string> inter, uni;
    for (auto& kv : a.ops) uni.insert(kv.first);
    for (auto& kv : b.ops) {
        if (a.ops.count(kv.first)) inter.insert(kv.first);
        uni.insert(kv.first);
    }
    if (uni.empty()) return 0;
    return (double)inter.size() / (double)uni.size();
}

} // namespace

int s3_cmd_dup(const Args& a) {
    Sb3File sf = sb3LoadAny(a.file);
    auto snaps = snapAll(sf.targets);
    std::vector<std::string> names;
    for (auto& kv : snaps) names.push_back(kv.first);
    std::sort(names.begin(), names.end());

    double thr = 0.6;  // 相似度阈值（默认 60%）
    // --threshold N 独立控制阈值（0-100，防呆）；--limit 回归"展示条数上限"
    if (a.threshold >= 0 && a.threshold <= 100)
        thr = (double)a.threshold / 100.0;
    else if (a.threshold > 100)
        thr = 1.0;

    if (a.json) {
        Json out = Json::object();
        out["file"] = a.file;
        out["threshold"] = thr;
        Json groups = Json::array();
        std::set<std::string> used;
        for (size_t i = 0; i < names.size(); ++i) {
            if (used.count(names[i])) continue;
            Json g = Json::object();
            g["primary"] = names[i];
            Json similar = Json::array();
            for (size_t j = i + 1; j < names.size(); ++j) {
                if (used.count(names[j])) continue;
                double sim = spriteSimilarity(snaps[names[i]], snaps[names[j]]);
                if (sim >= thr) {
                    Json s = Json::object();
                    s["sprite"] = names[j];
                    s["similarity"] = sim;
                    similar.push_back(std::move(s));
                }
            }
            if (!similar.empty()) {
                // 必须在 move 之前收集 used（similar 之后会被移动走）
                for (auto& s : similar)
                    used.insert(s["sprite"].get<std::string>());
                used.insert(names[i]);
                g["duplicates"] = std::move(similar);
                groups.push_back(std::move(g));
            }
        }
        out["groups"] = groups;
        sb3JsonOut(out);
        return 0;
    }

    std::cout << "文件：" << basename(a.file) << "（相似度阈值 "
              << (int)(thr * 100) << "%）\n\n";
    std::set<std::string> used;
    int nGroups = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        if (used.count(names[i])) continue;
        std::vector<std::pair<double, std::string>> sims;
        for (size_t j = i + 1; j < names.size(); ++j) {
            if (used.count(names[j])) continue;
            double sim = spriteSimilarity(snaps[names[i]], snaps[names[j]]);
            if (sim >= thr) sims.emplace_back(sim, names[j]);
        }
        if (sims.empty()) continue;
        ++nGroups;
        std::cout << "◆ " << names[i] << "\n";
        // --limit N：每个组最多显示 N 条（-1 默认显示全部）
        size_t shown = 0;
        size_t maxShow = a.limitSet ? (size_t)a.limit : (size_t)-1;
        for (auto& s : sims) {
            if (shown >= maxShow) {
                std::cout << "    └ …还有 " << (sims.size() - shown)
                          << " 条（用 --limit 调整）\n";
                break;
            }
            ++shown;
            used.insert(s.second);
            std::cout << "    └ " << (int)(s.first * 100) << "% 相似  "
                      << s.second << "\n";
        }
        used.insert(names[i]);
    }
    if (!nGroups) std::cout << "没有发现相似度 ≥ " << (int)(thr * 100)
                            << "% 的角色（调整阈值：--threshold N N=0-100）。\n";
    return 0;
}

} // namespace sb