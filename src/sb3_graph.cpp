// src/sb3_graph.cpp —— 作品"交叉引用图"：变量/列表读写引用（sb refs）
// 以及后续的广播拓扑（sb events）。
#include "sb3.hpp"
#include "sb3_internal.hpp"
#include "commands.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>

namespace sb {

namespace {

// 单个脚本里对某变量/列表的一次操作
struct Op {
    std::string action;   // 写分类：设置 / 增减 / 追加 / 替换 / 删除项 / 清空 / 插入 / 读取
    int scriptIdx = 0;    // 脚本编号（1 起）
    std::string text;     // 该积木的译文（如「将 i 设为 0」）
};

// 收集某个顶层脚本链路上的所有块 id（沿 next + SUBSTACK + inputs 里的 reporter 引用）
// blocks: 该 target 的 blocks 对象视图；递归防环。
void collectFromInput(const Elem& blocks, const Elem& val,
                      std::set<std::string>& out, int depth);

void collectScriptBlocks(const Elem& blocks, std::string_view bid,
                         std::set<std::string>& out, int depth = 0) {
    if (depth > 200) return;
    std::string cur(bid);
    while (!cur.empty()) {
        if (!out.insert(cur).second) return;   // 已访问（防环）
        Elem b = blocks.at(cur);
        if (!b.is_object()) return;
        Elem inputs = b.at("inputs");
        if (inputs.is_object()) {
            for (auto f : inputs.obj())
                collectFromInput(blocks, Elem(f.value), out, depth + 1);
        }
        Elem next = b.at("next");
        if (next.is_string()) cur = std::string(next.sv());
        else                  cur.clear();
    }
}

// inputs 值可能是 ["1", reporter块id] / ["2", [..]] / ["3", [..]] 等嵌套数组。
// 递归找其中的块 id 引用（字符串且存在于 blocks），把它们也算进脚本。
void collectFromInput(const Elem& blocks, const Elem& val,
                      std::set<std::string>& out, int depth) {
    if (depth > 200) return;
    if (val.is_string()) {
        std::string s(val.sv());
        // 跳过标记数字（"1"/"2"…）与普通文本：只有作为块 id 存在才算
        if (blocks.contains(s)) collectScriptBlocks(blocks, s, out, depth);
        return;
    }
    if (val.is_array()) {
        for (auto e : val.arr())
            collectFromInput(blocks, Elem(e), out, depth);
    }
}

// 取块上的字段文本：fields[fieldKey][0] 或 fields[fieldKey]（字符串）
std::string fieldName(const Elem& b, const char* fieldKey) {
    Elem fields = b.at("fields");
    if (!fields.is_object()) return "";
    Elem f = fields.at(fieldKey);
    if (!f.ok()) return "";
    if (f.is_array() && !f.empty()) {
        Elem n0 = f.op(0);
        return n0.is_string() ? std::string(n0.sv()) : "";
    }
    return f.is_string() ? std::string(f.sv()) : "";
}

struct VarInfo {
    std::map<std::string, std::vector<Op>> writes;   // 角色 → 写操作
    std::map<std::string, std::vector<Op>> reads;    // 角色 → 读操作
    bool isList = false;
};

} // namespace

int s3_cmd_refs(const Args& a) {
    Sb3File sf = sb3LoadAny(a.file);
    const Elem& targets = sf.targets;

    // 变量/列表名（id → 名字）与类型
    std::map<std::string, VarInfo> vars;   // key: 变量名
    // 名字 → (读/写 计数) 的交叉引用
    std::map<std::string, std::map<std::string, std::vector<Op>>> writes; // 名 → 角色 → ops
    std::map<std::string, std::map<std::string, std::vector<Op>>> reads;  // 名 → 角色 → ops
    std::set<std::string> isListName;

    // 名字过滤（可省）：空格分隔多关键词，任一命中即显示（OR）
    std::string filter = a.extra.empty() ? "" : a.extra[0];
    std::vector<std::string> filters;
    {
        std::string cur;
        for (char c : filter) {
            if (c == ' ' || c == '\t') {
                if (!cur.empty()) { filters.push_back(cur); cur.clear(); }
            } else cur += c;
        }
        if (!cur.empty()) filters.push_back(cur);
    }
    // --regex：整个过滤参数按正则匹配（如 "^Sprite1$" 精确锚定）
    std::optional<std::regex> filterRe;
    if (a.regex && !filters.empty()) {
        try { filterRe.emplace(filters[0]); }
        catch (const std::regex_error&) { /* 非法正则回退子串 */ }
    }
    auto matchFilter = [&](const std::string& name) {
        if (filters.empty()) return true;
        if (filterRe) return std::regex_search(name, *filterRe);
        for (auto& f : filters)
            if (name.find(f) != std::string::npos) return true;
        return false;
    };
    std::string spriteWant = a.sprite;
    if (!spriteWant.empty())
        std::transform(spriteWant.begin(), spriteWant.end(), spriteWant.begin(),
                       [](unsigned char c) { return (char)::tolower(c); });

    // 变量/列表名 → 声明角色（全局=Stage，局部=对应 Sprite）；同名消歧用
    std::map<std::string, std::string> varScope;

    if (targets.is_array()) {
        // 先收集一遍声明范围（所有 target 的 variables/lists）
        for (auto te : targets.arr()) {
            Elem t(te);
            if (!t.is_object()) continue;
            std::string tn = std::string(t.at("name").sv());
            bool stg = t.at("isStage").b();
            Elem vd = t.at("variables");
            if (vd.is_object())
                for (auto& vk : sortedKeysOf(vd.obj())) {
                    Elem v = vd.at(vk);
                    if (v.is_array() && !v.empty() && v.op(0).ok()) {
                        std::string nm = v.op(0).is_string() ? std::string(v.op(0).sv())
                                                              : compactJson(v.op(0).raw());
                        if (!nm.empty() && !varScope.count(nm))
                            varScope[nm] = stg ? "全局" : tn;
                    }
                }
            Elem ld = t.at("lists");
            if (ld.is_object())
                for (auto& lk : sortedKeysOf(ld.obj())) {
                    Elem v = ld.at(lk);
                    if (v.is_array() && !v.empty() && v.op(0).ok()) {
                        std::string nm = v.op(0).is_string() ? std::string(v.op(0).sv())
                                                              : compactJson(v.op(0).raw());
                        if (!nm.empty() && !varScope.count(nm))
                            varScope[nm] = stg ? "全局" : tn;
                    }
                }
        }
        for (auto te : targets.arr()) {
            Elem t(te);
            if (!t.is_object()) continue;
            std::string tname = std::string(t.at("name").sv());
            // --sprite 过滤（不区分大小写）
            if (!spriteWant.empty()) {
                std::string low = tname;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return (char)::tolower(c); });
                if (low != spriteWant) continue;
            }
            Elem blocks = t.at("blocks");
            if (!blocks.is_object()) continue;

            Renderer renderer(t);

            // 收集本角色 variables/lists：id → 名字（用于 shadow 引用解析）
            std::map<std::string, std::string> varId2Name, listId2Name;
            Elem varDecls = t.at("variables");
            if (varDecls.is_object())
                for (auto& vk : sortedKeysOf(varDecls.obj())) {
                    Elem v = varDecls.at(vk);
                    if (v.is_array() && !v.empty() && v.op(0).ok())
                        varId2Name[vk] = v.op(0).is_string() ? std::string(v.op(0).sv())
                                                             : compactJson(v.op(0).raw());
                }
            Elem listDecls = t.at("lists");
            if (listDecls.is_object())
                for (auto& lk : sortedKeysOf(listDecls.obj())) {
                    Elem v = listDecls.at(lk);
                    if (v.is_array() && !v.empty() && v.op(0).ok())
                        listId2Name[lk] = v.op(0).is_string() ? std::string(v.op(0).sv())
                                                             : compactJson(v.op(0).raw());
                }
            std::vector<std::pair<std::string, Elem>> tops;
            for (auto f : blocks.obj()) {
                Elem b(f.value);
                if (!b.is_object()) continue;
                if (b.at("topLevel").b() && !b.at("shadow").b())
                    tops.emplace_back(std::string(f.key), b);
            }
            std::sort(tops.begin(), tops.end(), [](const auto& x, const auto& y) {
                long long ya = x.second.i64At("y", 0), yb = y.second.i64At("y", 0);
                if (ya != yb) return ya > yb;
                return x.second.i64At("x", 0) < y.second.i64At("x", 0);
            });

            // 每个脚本：收集块集合 → 分类其中的变量/列表操作
            int scriptIdx = 0;
            for (auto& tp : tops) {
                ++scriptIdx;
                std::set<std::string> bidSet;
                collectScriptBlocks(blocks, tp.first, bidSet);
                for (auto& bid : bidSet) {
                    Elem b = blocks.at(bid);
                    if (!b.is_object()) continue;
                    std::string op = std::string(b.at("opcode").sv());
                    auto record = [&](std::map<std::string, std::map<std::string, std::vector<Op>>>& tbl,
                                      const std::string& name, const std::string& action, bool isList,
                                      bool isWrite) {
                        if (!matchFilter(name)) return;
                        if (name.empty()) return;
                        if (isList) isListName.insert(name);
                        Op o; o.action = action; o.scriptIdx = scriptIdx;
                        // 渲染该块的译文（省略号截断，防超长）
                        o.text = renderer.render(bid);
                        if (o.text.size() > 80) o.text = o.text.substr(0, 80) + "…";
                        (isWrite ? writes : reads)[name][tname].push_back(std::move(o));
                        if (isWrite) {
                            VarInfo& vi = vars[name];
                            vi.isList = isList;
                        }
                    };
                    if (op == "data_setvariableto") {
                        record(writes, fieldName(b, "VARIABLE"), "设置", false, true);
                    } else if (op == "data_changevariableby") {
                        record(writes, fieldName(b, "VARIABLE"), "增减", false, true);
                    } else if (op == "data_variable") {
                        record(reads, fieldName(b, "VARIABLE"), "读取", false, false);
                    } else if (op == "data_addtolist") {
                        record(writes, fieldName(b, "LIST"), "追加", true, true);
                    } else if (op == "data_replaceitemoflist") {
                        record(writes, fieldName(b, "LIST"), "替换", true, true);
                    } else if (op == "data_deleteoflist") {
                        record(writes, fieldName(b, "LIST"), "删除项", true, true);
                    } else if (op == "data_deletealloflist") {
                        record(writes, fieldName(b, "LIST"), "清空", true, true);
                    } else if (op == "data_insertatlist") {
                        record(writes, fieldName(b, "LIST"), "插入", true, true);
                    } else if (op == "data_itemoflist") {
                        record(reads, fieldName(b, "LIST"), "取项", true, false);
                    } else if (op == "data_lengthoflist") {
                        record(reads, fieldName(b, "LIST"), "取长度", true, false);
                    } else if (op == "data_listcontainsitem") {
                        record(reads, fieldName(b, "LIST"), "含项?", true, false);
                    }
                    // P1：表达式里内联的 shadow reporter（没有独立块）——
                    // 真正形态是 [3, [12, "变量名", "id"], [4,""]]（变量）
                    // 或 [3, [13, "列表名", "id"], [4,""]]（列表）。
                    // 12=变量 shadow、13=列表 shadow；名字在 [1]，id 在 [2]。
                    std::function<void(const Elem&)> scanShadow;
                    scanShadow = [&](const Elem& v) {
                        if (v.is_array()) {
                            Elem k0 = v.op(0);
                            if (k0.is_number()) {
                                long long k = k0.i64();
                                if ((k == 12 || k == 13) && v.size() > 1) {
                                    Elem nameEl = v.op(1);
                                    std::string nm = nameEl.is_string() ? std::string(nameEl.sv()) : "";
                                    // 名字为空时用 id 查映射
                                    if (nm.empty() && v.size() > 2) {
                                        std::string id = v.op(2).is_string() ? std::string(v.op(2).sv()) : "";
                                        if (k == 12) {
                                            auto it = varId2Name.find(id);
                                            if (it != varId2Name.end()) nm = it->second;
                                        } else {
                                            auto it = listId2Name.find(id);
                                            if (it != listId2Name.end()) nm = it->second;
                                        }
                                    }
                                    if (!nm.empty()) {
                                        if (k == 13) isListName.insert(nm);
                                        record(reads, nm, "读取", k == 13, false);
                                    }
                                }
                            }
                            for (auto e : v.arr()) scanShadow(Elem(e));
                        }
                    };
                    Elem inputs = b.at("inputs");
                    if (inputs.is_object())
                        for (auto f : inputs.obj()) scanShadow(Elem(f.value));
                }
            }
        }
    }

    // 汇总输出：按名称字母序（与 vars 输出一致用 map 默认序）
    if (a.json) {
        Json out = Json::object();
        out["file"] = a.file;
        Json arr = Json::array();
        // 把 角色→Op列表 合并成 角色→[去重计数行]，按脚本聚合
        auto mergeRole = [](const std::map<std::string, std::vector<Op>>& ops) {
            std::map<std::string, Json> byRole;
            for (auto& rk : ops) {
                Json rows = Json::array();
                for (auto& o : rk.second) {
                    bool found = false;
                    for (auto& row : rows) {
                        if (row["action"] == o.action && row["script"] == o.scriptIdx &&
                            row["block"] == o.text) {
                            row["count"] = row.contains("count")
                                ? row["count"].get<long long>() + 1 : 2;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        Json op = Json::object();
                        op["action"] = o.action;
                        op["script"] = o.scriptIdx;
                        op["block"] = o.text;
                        rows.push_back(std::move(op));
                    }
                }
                byRole[rk.first] = std::move(rows);
            }
            return byRole;
        };
        for (auto& kv : writes) {
            Json x = Json::object();
            x["name"] = kv.first;
            auto st = varScope.find(kv.first);
            if (st != varScope.end()) x["scope"] = st->second;
            x["list"] = isListName.count(kv.first);
            x["writes"] = mergeRole(kv.second);
            auto rit = reads.find(kv.first);
            x["reads"] = (rit != reads.end()) ? mergeRole(rit->second)
                                              : Json::object();
            arr.push_back(std::move(x));
        }
        out["refs"] = arr;
        sb3JsonOut(out);
        return 0;
    }

    // 变量/列表名范围标注（同名消歧）
    auto scopeTag = [&](const std::string& name) -> std::string {
        auto it = varScope.find(name);
        if (it == varScope.end()) return "";
        return it->second == "全局" ? "（全局）"
                                    : ("（局部·" + it->second + "）");
    };
    // ---- 反查视图：--by-sprite 按角色视角（该角色读/写了哪些变量）----
    if (a.bySprite) {
        struct OneOp { std::string action; int script; long long count; std::string text; };
        std::map<std::string, std::map<std::string, std::vector<OneOp>>> roleW, roleR;
        auto addOp = [&](std::map<std::string, std::map<std::string, std::vector<OneOp>>>& tbl,
                         const std::string& role, const std::string& name,
                         const Op& o) {
            auto& vec = tbl[role][name];
            for (auto& x : vec)
                if (x.action == o.action && x.script == o.scriptIdx && x.text == o.text) {
                    x.count++; return;
                }
            vec.push_back({o.action, o.scriptIdx, 1, o.text});
        };
        for (auto& kv : writes)
            for (auto& rk : kv.second)
                for (auto& o : rk.second) addOp(roleW, rk.first, kv.first, o);
        for (auto& kv : reads)
            for (auto& rk : kv.second)
                for (auto& o : rk.second) addOp(roleR, rk.first, kv.first, o);

        std::cout << "文件：" << basename(a.file) << "（反查：角色视角）\n";
        bool any2 = false;
        // 角色名排序输出
        std::set<std::string> roles;
        for (auto& rk : roleW) roles.insert(rk.first);
        for (auto& rk : roleR) roles.insert(rk.first);
        for (auto& role : roles) {
            // 该角色的变量（写+读合并）
            std::set<std::string> vnames;
            for (auto& vk : roleW[role]) vnames.insert(vk.first);
            for (auto& vk : roleR[role]) vnames.insert(vk.first);
            bool roleShown = false;
            for (auto& vn : vnames) {
                if (!matchFilter(vn)) continue;
                if (!roleShown) { std::cout << "\n──── 角色：" << role << "\n"; roleShown = true; any2 = true; }
                std::cout << "  " << (isListName.count(vn) ? "列表" : "变量")
                          << " " << vn << scopeTag(vn) << "\n";
                auto wi = roleW[role].find(vn);
                if (wi != roleW[role].end()) {
                    for (auto& x : wi->second)
                        std::cout << "      写 脚本" << x.script << "  " << x.action
                                  << "：" << (x.text.empty() ? "?" : x.text)
                                  << (x.count > 1 ? " ×" + std::to_string(x.count) : "") << "\n";
                }
                auto ri = roleR[role].find(vn);
                if (ri != roleR[role].end()) {
                    for (auto& x : ri->second)
                        std::cout << "      读 脚本" << x.script << "  " << x.action
                                  << "：" << (x.text.empty() ? "?" : x.text)
                                  << (x.count > 1 ? " ×" + std::to_string(x.count) : "") << "\n";
                }
            }
        }
        if (!any2) std::cout << "\n没有找到变量/列表引用\n";
        return 0;
    }

    auto listOne = [&](const std::string& name, bool isList) {
        std::cout << "\n" << (isList ? "列表" : "变量") << " " << name
                  << scopeTag(name) << "\n";
        auto show = [&](const char* title,
                        const std::map<std::string, std::vector<Op>>& ops) {
            if (ops.empty()) return;
            std::cout << "  " << title << "：\n";
            // 角色 → (脚本号 → 去重的动作:译文 ×次数)
            std::map<std::string, std::map<int, std::vector<std::pair<std::pair<std::string,std::string>, long long>>>> byScript;
            for (auto& rk : ops) {
                for (auto& o : rk.second) {
                    auto& list = byScript[rk.first][o.scriptIdx];
                    bool found = false;
                    for (auto& item : list)
                        if (item.first.first == o.action && item.first.second == o.text) {
                            item.second++; found = true; break;
                        }
                    if (!found)
                        list.emplace_back(std::make_pair(o.action, o.text), 1);
                }
            }
            for (auto& role : byScript) {
                std::cout << "    " << role.first << "\n";
                for (auto& sc : role.second) {
                    std::cout << "      · 脚本" << sc.first << "\n";
                    for (auto& item : sc.second)
                        std::cout << "          " << item.first.first << "："
                                  << (item.first.second.empty() ? "?" : item.first.second)
                                  << (item.second > 1 ? "  ×" + std::to_string(item.second) : "")
                                  << "\n";
                }
            }
        };
        auto wi = writes.find(name);
        if (wi != writes.end()) show("写", wi->second);
        auto ri = reads.find(name);
        if (ri != reads.end()) show("读", ri->second);
    };

    // 只列有写引用的（未提及 = 无引用）；过滤时只列匹配的
    std::cout << "文件：" << basename(a.file) << "\n";
    bool any = false;
    for (auto& kv : writes) {
        if (!matchFilter(kv.first)) continue;
        any = true;
        listOne(kv.first, isListName.count(kv.first));
    }
    if (!any) std::cout << "\n没有找到变量/列表引用"
                        << (filters.empty() ? "" : ("（匹配「" + filter + "」）")) << "\n";
    return 0;
}

// ---- sb events：广播拓扑（谁发、谁收） ----

namespace {

// 广播引用值 → 广播名。BROADCAST_OPTION/BROADCAST_INPUT 的值可能是：
//   ["广播名"] 或 ["广播名", "id"] 数组，也可能直接是字符串。
// 有 broadcasts 映射时优先用名字本身（Scratch 3 的 BROADCAST_OPTION 存名字）。
std::string bcastNameOf(const Elem& v) {
    if (!v.ok()) return "";
    if (v.is_array() && !v.empty()) {
        Elem n0 = v.op(0);
        if (n0.is_string()) return std::string(n0.sv());
        if (n0.is_number()) return std::to_string(n0.i64());
        return "";
    }
    if (v.is_string()) return std::string(v.sv());
    return "";
}

} // namespace

int s3_cmd_events(const Args& a) {
    Sb3File sf = sb3LoadAny(a.file);
    const Elem& targets = sf.targets;

    // 广播名 → {发送者: [(角色, 脚本N, 译文)], 接收者: [...]}
    struct Edge { std::string who; int script; std::string text; };
    std::map<std::string, std::vector<Edge>> senders, receivers;

    if (targets.is_array()) {
        for (auto te : targets.arr()) {
            Elem t(te);
            if (!t.is_object()) continue;
            std::string tname = std::string(t.at("name").sv());
            Elem blocks = t.at("blocks");
            if (!blocks.is_object()) continue;

            Renderer renderer(t);

            // 顶层脚本（按 y 降序）
            std::vector<std::pair<std::string, Elem>> tops;
            for (auto f : blocks.obj()) {
                Elem b(f.value);
                if (!b.is_object()) continue;
                if (b.at("topLevel").b() && !b.at("shadow").b())
                    tops.emplace_back(std::string(f.key), b);
            }
            std::sort(tops.begin(), tops.end(), [](const auto& x, const auto& y) {
                long long ya = x.second.i64At("y", 0), yb = y.second.i64At("y", 0);
                if (ya != yb) return ya > yb;
                return x.second.i64At("x", 0) < y.second.i64At("x", 0);
            });

            int scriptIdx = 0;
            for (auto& tp : tops) {
                ++scriptIdx;
                std::set<std::string> bidSet;
                collectScriptBlocks(blocks, tp.first, bidSet);
                for (auto& bid : bidSet) {
                    Elem b = blocks.at(bid);
                    if (!b.is_object()) continue;
                    std::string op = std::string(b.at("opcode").sv());
                    auto add = [&](std::map<std::string, std::vector<Edge>>& tbl,
                                   const std::string& bname) {
                        if (bname.empty()) return;
                        Edge e; e.who = tname; e.script = scriptIdx;
                        e.text = renderer.render(bid);
                        if (e.text.size() > 60) e.text = e.text.substr(0, 60) + "…";
                        tbl[bname].push_back(std::move(e));
                    };
                    if (op == "event_whenbroadcastreceived") {
                        add(receivers, bcastNameOf(b.at("fields").at("BROADCAST_OPTION")));
                    } else if (op == "event_broadcast" || op == "event_broadcastandwait" ||
                               op == "broadcast:") {
                        // 广播名：用 Renderer 渲染 BROADCAST_INPUT 输入值
                        // （兼容内联 shadow ["2",["10","名"]]、menu 块、变量引用等形式）
                        add(senders, renderer.inputText(b, "BROADCAST_INPUT"));
                    }
                }
            }
        }
    }

    // 输出
    if (a.json) {
        Json out = Json::object();
        out["file"] = a.file;
        Json arr = Json::array();
        // 合并全部广播名（发送+接收）
        std::set<std::string> allB;
        for (auto& kv : senders) allB.insert(kv.first);
        for (auto& kv : receivers) allB.insert(kv.first);
        for (auto& bname : allB) {
            Json x = Json::object();
            x["name"] = bname;
            auto si = senders.find(bname);
            auto ri = receivers.find(bname);
            bool hasS = (si != senders.end() && !si->second.empty());
            bool hasR = (ri != receivers.end() && !ri->second.empty());
            // 孤儿标记：无发送者 / 无接收者
            if (!hasS) x["orphanSend"] = true;
            if (!hasR) x["orphanRecv"] = true;
            Json se = Json::array(), re = Json::array();
            if (hasS)
                for (auto& e : si->second) {
                    Json j = Json::object();
                    j["who"] = e.who; j["script"] = e.script; j["block"] = e.text;
                    se.push_back(std::move(j));
                }
            if (hasR)
                for (auto& e : ri->second) {
                    Json j = Json::object();
                    j["who"] = e.who; j["script"] = e.script; j["block"] = e.text;
                    re.push_back(std::move(j));
                }
            x["senders"] = se;
            x["receivers"] = re;
            arr.push_back(x);
        }
        out["events"] = arr;
        sb3JsonOut(out);
        return 0;
    }

    std::cout << "文件：" << basename(a.file) << "\n";
    // 合并所有广播名（发送+接收），按键序
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
        // 孤儿广播：只有接收没有发送（可能是变量广播 / 消息拼错 / 残留）
        if (!hasS)
            std::cout << "（⚠ 无发送者：可能是变量广播或残留）";
        // 反向：只有发送没有接收（莫名广播，可能拼错接收方）
        else if (!hasR)
            std::cout << "（⚠ 无接收者：广播发出但没人监听，可能拼错或残留）";
        std::cout << "\n";
        if (si != senders.end()) {
            // 去重发送者（同一角色可能同一脚本多次发同一广播：按 角色+脚本 去重）
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
        if (ri != receivers.end()) {
            std::cout << "  收：";
            bool first = true;
            std::map<std::pair<std::string,int>, std::string> uniq;
            for (auto& e : ri->second) uniq[{e.who, e.script}] = e.text;
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