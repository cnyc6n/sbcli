// src/sbcli_fix.cpp
// `sb fix` —— docs/format.md §4「发现即声明」。
//
// 立场：block.sbcli 是手写源文件，fix **绝不改它**。能自动补的只有 meta；
// 结构类问题（procedures_call 缺 ARG、else 错位…）只报告，交给人改。
//   理由见 docs/parser-notes.md §5：重建会丢注释，且 pack 不回写。
//
// meta 的读写走 src/sbcli_meta.hpp（与 sbcli_project.cpp 共用一层）——
// fix 与 add-* 写的是同一批文件，各写一份解析器迟早不一致。
//
// 关于 id：sbcVarId / sbcBroadcastId 是纯函数、不落盘（见 sbcli_parser.hpp），
// fix 与 pack 调同一个函数即一致且幂等。
#include "sbcli_fix.hpp"
#include "sbcli_parser.hpp"
#include "sbcli_meta.hpp"
#include "sb3_tables.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace sb {
namespace {

// meta 层与文件工具统一从 sb::meta 取，避免两份实现漂移
using namespace sb::meta;

// ================================================================ 引用收集

struct Refs {
    std::set<std::string> variables, lists, broadcasts, costumes, sounds;
};

// 按参数的 canon 名把标量值归到对应的引用集合
void takeRef(Refs& r, const std::string& canonKey, const std::string& nm, bool isStage) {
    if (nm.empty()) return;
    if      (canonKey == "VARIABLE")         r.variables.insert(nm);
    else if (canonKey == "LIST")             r.lists.insert(nm);
    else if (canonKey == "BROADCAST_INPUT" ||
             canonKey == "BROADCAST_OPTION") r.broadcasts.insert(nm);
    // 造型只对角色有意义；舞台用的是 BACKDROP，不该登记成 costume
    else if (canonKey == "COSTUME")          { if (!isStage) r.costumes.insert(nm); }
    else if (canonKey == "SOUND_MENU")       r.sounds.insert(nm);
}

// 递归收集一个积木（含 reporter 参数、子栈）里的引用
void collectBlock(const SbcBlock& b, Refs& r, bool isStage) {
    std::function<void(const SbcValue&)> walkValue = [&](const SbcValue& v) {
        if (v.kind != SbcValue::Kind::Reporter) return;
        for (const auto& p : v.args) {
            if (!p.value) continue;
            const std::string& k = p.canon.empty() ? p.key : p.canon;
            const SbcValue& pv = *p.value;
            if (pv.kind == SbcValue::Kind::Scalar)
                takeRef(r, k, pv.text(), isStage);
            else if (pv.kind == SbcValue::Kind::Reporter)
                walkValue(pv);
        }
    };
    for (const auto& p : b.params) {
        if (!p.value) continue;
        const std::string& k = p.canon.empty() ? p.key : p.canon;
        const SbcValue& pv = *p.value;
        if (pv.kind == SbcValue::Kind::Scalar)
            takeRef(r, k, pv.text(), isStage);
        else if (pv.kind == SbcValue::Kind::Reporter)
            walkValue(pv);
    }
    for (const auto& sub : b.substacks)
        for (const auto& cb : sub) collectBlock(cb, r, isStage);
}

// 变量初始值：第一次 data_setvariableto 的 VALUE（§4.3）
void collectVarInit(const SbcBlock& b, std::map<std::string, std::string>& init) {
    if (b.opcode == "data_setvariableto") {
        const SbcParam* vp = b.find("VARIABLE");
        const SbcParam* wp = b.find("VALUE");
        if (vp && wp && vp->value && wp->value &&
            vp->value->kind == SbcValue::Kind::Scalar &&
            wp->value->kind == SbcValue::Kind::Scalar) {
            const std::string& nm = vp->value->text();
            if (!nm.empty() && !init.count(nm)) {
                const SbToken& t = wp->value->scalar;
                // 引号是语义：字符串带引号写回，数字裸写（否则 "0" 会变成字符串常量）
                init[nm] = t.quoted ? ("\"" + sbcEscape(t.text) + "\"") : t.text;
            }
        }
    }
    for (const auto& sub : b.substacks)
        for (const auto& cb : sub) collectVarInit(cb, init);
}

// ================================================================ 结构问题报告

// fix 不修 block.sbcli，但把 pack 会踩的坑报告出来
void reportStructureIssues(const SbcFile& sf, const std::string& rel, FixReport& rep) {
    auto note = [&](int line, const char* level, const char* code, const std::string& msg) {
        FixNote n;
        n.file = rel; n.line = line; n.level = level; n.code = code; n.message = msg;
        rep.notes.push_back(std::move(n));
        if (std::string(level) == "error") ++rep.errors;
    };

    std::function<void(const SbcBlock&)> walk = [&](const SbcBlock& b) {
        if (b.opcode == "procedures_call") {
            std::string proccode;
            if (const SbcParam* p = b.find("PROCCODE"))
                if (p->value && p->value->kind == SbcValue::Kind::Scalar)
                    proccode = p->value->text();
            int nph = 0;
            for (size_t i = 0; i + 1 < proccode.size(); ++i)
                if (proccode[i] == '%' && std::strchr("snb", proccode[i + 1])) { ++nph; ++i; }
            int n = 0;
            for (const auto& p : b.params) {
                const std::string& k = p.canon.empty() ? p.key : p.canon;
                if (k.size() > 3 && k.compare(0, 3, "ARG") == 0 &&
                    std::all_of(k.begin() + 3, k.end(),
                                [](char c) { return std::isdigit((unsigned char)c); }))
                    ++n;
            }
            if (!proccode.empty() && nph != n)
                note(b.line, "error", "call-arg-count",
                     "procedures_call 给了 " + std::to_string(n) + " 个 ARG，但 PROCCODE「" +
                     proccode + "」有 " + std::to_string(nph) +
                     " 个占位符（pack 会缺参数，请手写补上）");
        }
        if (b.opcode == "control_if_else" && !b.hasElse() && b.substacks.size() < 2)
            note(b.line, "warn", "else-missing",
                 "control_if_else 没有 else 分支（否则分支为空，pack 时按空 SUBSTACK2 处理）");
        for (const auto& sub : b.substacks)
            for (const auto& cb : sub) walk(cb);
    };
    for (const auto& sc : sf.scripts)
        for (const auto& b : sc.blocks) walk(b);

    // 解析器报的结构错误也转达（用户跑 fix 时该看到）
    for (const auto& d : sf.diags)
        if (d.isError())
            note(d.line, "error", d.code.c_str(),
                 d.message + "（fix 不改 block.sbcli，请手动修）");
}

} // namespace（匿名段到此结束）

// ================================================================ 主入口

// id 生成在 src/sbcli_parser.cpp（声明在 sbcli_parser.hpp）：
// pack 也要用同一套函数，放在 parser 里两边都能调，避免各算各的。

FixReport sbcliFix(const std::string& rootOrDir, bool dryRun) {
    FixReport rep;
    rep.dryRun = dryRun;

    // ---- 定位项目根（与 check 一致）----
    std::string root;
    std::string in = norm(rootOrDir);
    {
        if (dirExists(in)) {
            if (dirExists(joinRel(in, "character")) || fileExists(joinRel(in, "meta.sbcli")))
                root = in;
        } else if (fileExists(in)) {
            std::string ps = in;
            for (int k = 0; k < 4; ++k) {
                size_t sp = ps.find_last_of('/');
                if (sp == std::string::npos) break;
                ps = ps.substr(0, sp);
                if (fileExists(joinRel(ps, "meta.sbcli")) || dirExists(joinRel(ps, "character"))) {
                    root = ps; break;
                }
            }
        }
        if (root.empty()) {
            // 也可能直接指到了 character/ 或 character/1/
            std::string ps = in;
            for (int k = 0; k < 4; ++k) {
                if (fileExists(joinRel(ps, "meta.sbcli")) || dirExists(joinRel(ps, "character"))) {
                    root = ps; break;
                }
                size_t sp = ps.find_last_of('/');
                if (sp == std::string::npos) break;
                ps = ps.substr(0, sp);
            }
        }
    }
    if (root.empty()) {
        rep.error = "找不到项目根目录（需含 meta.sbcli 或 character/ 子目录）：" + rootOrDir;
        return rep;
    }
    rep.root = root;

    // ---- 收集 block.sbcli ----
    std::vector<std::string> absFiles;
    if (fileExists(in) && norm(fs::u8path(in).filename().u8string()) == "block.sbcli") {
        absFiles.push_back(in);
    } else {
        std::vector<std::string> dirs;   // character 下的子目录（stage / 1 / 2 …）
        std::error_code ec;
        for (fs::path base : {fs::u8path(root) / "character", fs::u8path(root)}) {
            if (!fs::is_directory(base, ec)) continue;
            for (const auto& de : fs::directory_iterator(base, ec)) {
                if (!de.is_directory(ec)) continue;
                std::string d = norm(de.path().u8string());
                if (fileExists(joinRel(d, "block.sbcli"))) dirs.push_back(d);
            }
        }
        std::sort(dirs.begin(), dirs.end());
        for (const auto& d : dirs) absFiles.push_back(joinRel(d, "block.sbcli"));
    }
    if (absFiles.empty()) {
        rep.error = "项目里没有找到任何 block.sbcli（应在 character/stage/ 或 character/{id}/ 下）";
        return rep;
    }

    // ---- 根 meta ----
    std::string rootMetaPath = joinRel(root, "meta.sbcli");
    RootMeta   rmeta = loadRootMeta(rootMetaPath);
    if (rmeta.name.empty()) rmeta.name = norm(fs::u8path(root).filename().u8string());

    auto note = [&](const std::string& file, int line, const char* level,
                    const char* code, const std::string& msg) {
        FixNote n;
        n.file = file; n.line = line; n.level = level; n.code = code; n.message = msg;
        rep.notes.push_back(std::move(n));
        if (std::string(level) == "error") ++rep.errors;
    };

    // ---- 逐角色 ----
    for (const auto& abs : absFiles) {
        std::string rel  = relToRoot(root, abs);
        std::string dir  = norm(fs::u8path(abs).parent_path().u8string());
        std::string leaf = norm(fs::u8path(dir).filename().u8string());
        bool isStage = (leaf == "stage");
        rep.files.push_back(rel);

        SbcFile sf = sbcParseFile(abs);
        reportStructureIssues(sf, rel, rep);
        if (!sf.scripts.empty() && !sf.ok()) {
            // 结构错误已经在上面逐条转达，这里不再重复计数
        }

        CharMeta cm = loadCharMeta(joinRel(dir, "meta.sbcli"));
        std::string spriteName = cm.has("name") ? cm.get("name")
                                                : (isStage ? "Stage" : ("sprite" + leaf));
        int registeredHere = 0;   // 本文件新登记的条目数（rep.registered 是全局累计）

        Refs r;
        std::map<std::string, std::string> varInit;
        for (const auto& sc : sf.scripts) {
            if (sc.hat == "broadcast" && !sc.hatArg.empty()) r.broadcasts.insert(sc.hatArg);
            for (const auto& b : sc.blocks) { collectBlock(b, r, isStage); collectVarInit(b, varInit); }
        }

        // ---- 登记：变量 / 列表 ----
        // §4.2：舞台脚本里的变量 → 根 meta；其余 → 角色 meta
        for (const auto& nm : r.variables) {
            if (isStage) {
                if (rmeta.variables.count(nm)) continue;
                auto it = varInit.find(nm);
                rmeta.variables[nm] = (it == varInit.end()) ? "0" : it->second;
                ++rep.registered; ++registeredHere;
                note(rel, 0, "info", "registered-variable",
                     "变量「" + nm + "」登记到根 meta（舞台级，初值 " +
                     rmeta.variables[nm] + "）");
            } else {
                if (cm.variables.count(nm)) continue;
                // 已在根 meta 声明 = 全局变量，不建角色级影子条目
                if (rmeta.variables.count(nm)) continue;
                cm.variables.insert(nm);
                ++rep.registered; ++registeredHere;
                note(rel, 0, "info", "registered-variable",
                     "变量「" + nm + "」登记到角色 meta");
            }
        }
        for (const auto& nm : r.lists) {
            if (isStage) {
                if (rmeta.lists.count(nm)) continue;
                rmeta.lists[nm] = "[]";
                ++rep.registered; ++registeredHere;
                note(rel, 0, "info", "registered-list", "列表「" + nm + "」登记到根 meta");
            } else {
                if (cm.lists.count(nm)) continue;
                if (rmeta.lists.count(nm)) continue;   // 已全局声明，不建影子
                cm.lists.insert(nm);
                ++rep.registered; ++registeredHere;
                note(rel, 0, "info", "registered-list", "列表「" + nm + "」登记到角色 meta");
            }
        }
        // ---- 广播：一律根 meta（§4.2）----
        for (const auto& nm : r.broadcasts) {
            if (rmeta.broadcasts.count(nm)) continue;
            rmeta.broadcasts.insert(nm);
            ++rep.registered; ++registeredHere;
            note(rel, 0, "info", "registered-broadcast", "广播「" + nm + "」登记到根 meta");
        }
        // ---- 造型 / 声音：角色 meta；缺素材文件则报错（§4.2）----
        for (const auto& nm : r.costumes) {
            if (cm.costumes.count(nm)) {
                auto it = cm.costumes.find(nm);
                if (it->second.empty()) {
                    note(rel, 0, "error", "asset-missing",
                         "造型「" + nm + "」在 meta 里没有素材路径（请把文件放进 assets/ 并补上）");
                } else if (!fileExists(joinRel(root, it->second))) {
                    note(rel, 0, "error", "asset-missing",
                         "造型「" + nm + "」的素材文件不存在：" + it->second);
                }
                continue;
            }
            // 猜一个 assets/ 路径；文件不存在就报错，不臆造
            std::string guess = "assets/" + nm + ".svg";
            bool has = fileExists(joinRel(root, guess));
            cm.costumes[nm] = has ? guess : "";
            ++rep.registered; ++registeredHere;
            if (has) {
                note(rel, 0, "info", "registered-costume",
                     "造型「" + nm + "」登记到角色 meta（素材 " + guess + "）");
            } else {
                note(rel, 0, "error", "asset-missing",
                     "造型「" + nm + "」已登记但没有素材文件（期望 " + guess +
                     "；请把文件放进 assets/ 或手动补 costumes）");
            }
        }
        for (const auto& nm : r.sounds) {
            if (cm.sounds.count(nm)) {
                auto it = cm.sounds.find(nm);
                if (it->second.empty()) {
                    note(rel, 0, "error", "asset-missing",
                         "声音「" + nm + "」在 meta 里没有素材路径（请把文件放进 assets/ 并补上）");
                } else if (!fileExists(joinRel(root, it->second))) {
                    note(rel, 0, "error", "asset-missing",
                         "声音「" + nm + "」的素材文件不存在：" + it->second);
                }
                continue;
            }
            std::string guess = "assets/" + nm + ".mp3";
            bool has = fileExists(joinRel(root, guess));
            cm.sounds[nm] = has ? guess : "";
            ++rep.registered; ++registeredHere;
            if (has) {
                note(rel, 0, "info", "registered-sound",
                     "声音「" + nm + "」登记到角色 meta（素材 " + guess + "）");
            } else {
                note(rel, 0, "error", "asset-missing",
                     "声音「" + nm + "」已登记但没有素材文件（期望 " + guess +
                     "；请把文件放进 assets/ 或手动补 sounds）");
            }
        }

        // ---- 写盘 ----
        std::string cmPath = joinRel(dir, "meta.sbcli");
        // registered 是全局累计，不能用来判断"这个文件有没有变"——
        // 否则前一个角色登记过就会把后面每个角色都判成需要写盘。
        // 用本文件新增的条目数来判。
        bool changed = !cm.exists || registeredHere > 0 ||
                       charMetaNeedsDefaults(cm);
        if (changed) {
            if (!dryRun) writeCharMeta(cmPath, cm, spriteName, isStage);
            rep.written.push_back(relToRoot(root, cmPath));
        }
    }

    // ---- 根 meta 写盘 ----
    if (!dryRun) {
        bool need = !rmeta.variables.empty() || !rmeta.lists.empty() ||
                    !rmeta.broadcasts.empty() || !rmeta.exists;
        if (need) {
            writeRootMeta(rootMetaPath, rmeta);
            rep.written.push_back("meta.sbcli");
        }
    } else {
        bool need = !rmeta.variables.empty() || !rmeta.lists.empty() ||
                    !rmeta.broadcasts.empty() || !rmeta.exists;
        if (need) rep.written.push_back("meta.sbcli");
    }

    return rep;
}

} // namespace sb
