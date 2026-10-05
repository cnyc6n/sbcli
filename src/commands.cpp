// src/commands.cpp —— 统一命令分发层
//
// 对应 sb.py 的第三部分（检测格式 → 分派到 s1_cmd_* / s3_cmd_*）。
#include "commands.hpp"
#include "find_impl.hpp"
#include "sbcli_check.hpp"
#include "sbcli_fix.hpp"
#include "sbcli_view.hpp"
#include "sbcli_project.hpp"
#include "sbcli_parser.hpp"
#include "sbcli_meta.hpp"
#include "sb3_internal.hpp"
#include <filesystem>
#include "sbcli_unpack.hpp"
#include "sbcli_search.hpp"
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

static int sbcliCmdInfo(Args& a);
static int sbcliCmdSprites(Args& a);
static int sbcliCmdScript(Args& a);
static int sbcliCmdVars(Args& a);
static int sbcliCmdRefs(Args& a);
static int sbcliCmdEvents(Args& a);
static int sbcliCmdText(Args& a);
static int sbcliCmdBlocks(Args& a);

std::string detectFormat(const std::string& path) {
    // 目录：若是 sbcli 项目（含 meta.sbcli 或 character/）→ "sbcli"
    {
        std::error_code ec;
        std::string root = std::filesystem::path(path).string();
        if (std::filesystem::is_directory(root, ec)) {
            namespace mfs = std::filesystem;
            if (std::filesystem::exists(mfs::path(root) / "meta.sbcli", ec) ||
                std::filesystem::exists(mfs::path(root) / "character", ec))
                return "sbcli";
            return "dir";
        }
    }
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
    if (fmt == "sbcli") return sbcliCmdInfo(a);
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
    if (fmt == "sbcli") return sbcliCmdSprites(a);
    if (fmt == "sb1") return s1_cmd_sprites(a);
    return s3_cmd_sprites(a);
}

int cmd_text(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sbcli") return sbcliCmdText(a);
    if (fmt == "sb1") return s1_cmd_text(a);
    return s3_cmd_text(a);
}

// ==========================================================================
// sbcli 项目目录作为输入：复用 sbcliView（角色清单 + 中文脚本），
// 让 info/sprites/script/text/vars 等命令也能直接读项目目录。
// ==========================================================================

// script：项目目录 → 与 sb script 相同风格的中文脚本输出
static int sbcliCmdScript(Args& a) {
    ViewReport r = sbcliView(a.file);
    if (!r.error.empty()) { std::cerr << "错误：" << r.error << "\n"; return 2; }
    std::cout << "文件：" << basename(a.file) << "（sbcli 项目）\n";
    for (const auto& sp : r.sprites) {
        if (!a.sprite.empty() && sp.id != a.sprite && sp.name != a.sprite) continue;
        std::cout << "\n═══ " << (sp.isStage ? "舞台" : "角色") << "：" << sp.name
                  << (sp.isStage ? "（舞台）" : "") << " ═══\n";
        if (sp.scripts.empty()) { std::cout << "  （没有脚本）\n"; continue; }
        for (const auto& sc : sp.scripts) {
            std::cout << "\n  ── @" << sc.hat
                      << (sc.hatArg.empty() ? "" : " " + sc.hatArg) << " ──\n";
            for (const auto& l : sc.lines) {
                std::cout << "  " << std::string((size_t)l.indent * 2, ' ')
                          << l.text << "\n";
            }
        }
    }
    return 0;
}

// sprites：项目目录 → 角色清单（与 sb sprites 风格一致）
static int sbcliCmdSprites(Args& a) {
    ViewReport r = sbcliView(a.file);
    if (!r.error.empty()) { std::cerr << "错误：" << r.error << "\n"; return 2; }
    int nSprite = 0, nStage = 0;
    for (const auto& sp : r.sprites) (sp.isStage ? nStage : nSprite)++;
    std::cout << "文件：" << basename(a.file) << "（sbcli 项目）\n";
    std::cout << "共 " << r.sprites.size() << " 个目标：" << nStage << " 个舞台 + "
              << nSprite << " 个角色\n\n";
    for (const auto& sp : r.sprites) {
        std::cout << "[" << (sp.isStage ? "舞台" : "角色") << "] \"" << sp.name << "\"\n";
        std::cout << "       脚本 " << sp.scriptCount << " · 积木 " << sp.blockCount
                  << " · 造型 " << sp.costumeCount << " · 声音 " << sp.soundCount << "\n";
    }
    return 0;
}

// info：项目目录概要
static int sbcliCmdInfo(Args& a) {
    ViewReport r = sbcliView(a.file);
    if (!r.error.empty()) { std::cerr << "错误：" << r.error << "\n"; return 2; }
    int nSprite = 0, nStage = 0, nScript = 0, nBlock = 0, nCostume = 0, nSound = 0;
    for (const auto& sp : r.sprites) {
        (sp.isStage ? nStage : nSprite)++;
        nScript += sp.scriptCount; nBlock += sp.blockCount;
        nCostume += sp.costumeCount; nSound += sp.soundCount;
    }
    if (a.json) {
        Json out = Json::object();
        out["file"] = a.file;
        out["format"] = "sbcli-project";
        out["project"] = r.projectName;
        out["stageCount"] = nStage;
        out["spriteCount"] = nSprite;
        out["scriptCount"] = nScript;
        out["blockCount"] = nBlock;
        out["costumeCount"] = nCostume;
        out["soundCount"] = nSound;
        sb3JsonOut(out);
        return 0;
    }
    std::cout << "文件：" << basename(a.file) << "  （sbcli 项目）\n";
    if (!r.projectName.empty()) std::cout << "项目名：" << r.projectName << "\n";
    std::cout << "角色 " << nSprite << " 个 + 舞台 " << nStage << " 个\n";
    std::cout << "脚本 " << nScript << " · 积木 " << nBlock
              << " · 造型 " << nCostume << " · 声音 " << nSound << "\n";
    return 0;
}

int cmd_script(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sbcli") return sbcliCmdScript(a);
    if (fmt == "sb1") return s1_cmd_script(a);
    return s3_cmd_script(a);
}

// vars：项目目录 → 从根 meta 与各角色 meta 读变量/列表
static int sbcliCmdVars(Args& a) {
    std::string root = sb::meta::findProjectRoot(a.file);
    if (root.empty()) { std::cerr << "错误：不是 sbcli 项目目录：" << a.file << "\n"; return 2; }
    auto load = [&](const std::string& p) { return sb::meta::loadCharMeta(p + "/meta.sbcli"); };
    sb::meta::CharMeta rm = load(root);
    std::vector<std::string> chars;
    {
        std::error_code ec;
        std::filesystem::path cd = std::filesystem::u8path(root) / "character";
        if (std::filesystem::is_directory(cd, ec))
            for (auto& de : std::filesystem::directory_iterator(cd, ec))
                if (de.is_directory(ec)) chars.push_back(de.path().filename().string());
    }
    std::sort(chars.begin(), chars.end());
    if (a.json) {
        Json out = Json::object();
        out["file"] = a.file;
        out["format"] = "sbcli-project";
        Json gv = Json::array(), gl = Json::array();
        for (auto& nm : rm.variables) gv.push_back(nm);
        for (auto& nm : rm.lists)     gl.push_back(nm);
        out["globalVariables"] = gv;
        out["globalLists"] = gl;
        Json sprites = Json::object();
        for (auto& id : chars) {
            sb::meta::CharMeta m = load(root + "/character/" + id);
            Json s = Json::object();
            Json v = Json::array(), l = Json::array();
            for (auto& nm : m.variables) v.push_back(nm);
            for (auto& nm : m.lists)     l.push_back(nm);
            s["variables"] = v; s["lists"] = l;
            s["name"] = m.has("name") ? m.get("name") : id;
            sprites[id] = s;
        }
        out["sprites"] = sprites;
        sb3JsonOut(out);
        return 0;
    }
    std::cout << "文件：" << basename(a.file) << "（sbcli 项目）\n";
    auto dumpOne = [&](const std::string& where, const sb::meta::CharMeta& m) {
        if (m.variables.empty() && m.lists.empty()) return;
        std::cout << "\n【" << where << "】\n";
        for (auto& nm : m.variables) std::cout << "  变量 " << nm << "\n";
        for (auto& nm : m.lists)     std::cout << "  列表 " << nm << "\n";
    };
    dumpOne("全局（根 meta）", rm);
    for (auto& id : chars) {
        sb::meta::CharMeta m = load(root + "/character/" + id);
        std::string nm = m.has("name") ? m.get("name") : id;
        dumpOne((id == "stage" ? std::string("舞台") : "角色 " + id + "（" + nm + "）"), m);
    }
    return 0;
}

int cmd_vars(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sbcli") return sbcliCmdVars(a);
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
    if (fmt == "sbcli") return sbcliCmdBlocks(a);
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
    if (fmt == "sbcli") return sbcliCmdRefs(a);
    if (fmt == "sb1")
        return s1_cmd_refs(a);
    return s3_cmd_refs(a);
}

int cmd_events(Args& a) {
    std::string fmt = detectFormat(a.file);
    if (fmt == "sbcli") return sbcliCmdEvents(a);
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

// ==========================================================================
// check —— sbcli 项目目录的静态检查（docs/format.md §5）
// 用法：sb check <项目目录> [--json]
// ==========================================================================

int cmd_check(Args& a) {
    std::string target = a.file.empty() ? a.path : a.file;
    if (target.empty()) target = ".";

    CheckReport r = sbcliCheck(target);
    if (!r.error.empty()) {
        std::cerr << "错误：" << r.error << "\n";
        return 2;
    }

    // 按 文件 → 行号 排序输出
    std::vector<const CheckDiag*> ordered;
    ordered.reserve(r.diags.size());
    for (const auto& d : r.diags) ordered.push_back(&d);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const CheckDiag* x, const CheckDiag* y) {
                         if (x->file != y->file) return x->file < y->file;
                         return x->line < y->line;
                     });

    if (a.json) {
        Json out = Json::object();
        out["root"] = r.root;
        out["checked"] = (long long)r.files.size();
        Json files = Json::array();
        for (const auto& f : r.files) files.push_back(f);
        out["files"] = std::move(files);
        out["rootMetaFound"] = r.rootMetaFound;
        Json counts = Json::object();
        counts["error"] = r.errors;
        counts["warning"] = r.warnings;
        out["counts"] = std::move(counts);
        Json diags = Json::array();
        for (const CheckDiag* d : ordered) {
            Json o = Json::object();
            o["file"] = d->file;
            o["line"] = d->line;
            o["col"] = d->col;
            o["level"] = (d->level == CheckLevel::Error) ? "error" : "warning";
            o["category"] = d->category;
            o["code"] = d->code;
            o["message"] = d->message;
            diags.push_back(std::move(o));
        }
        out["diagnostics"] = std::move(diags);
        out["ok"] = (r.errors == 0);
        std::cout << out.dump(2) << "\n";
        return r.errors ? 1 : 0;
    }

    // 文本模式
    std::cout << "检查项目：" << r.root << "\n";
    if (r.files.empty()) {
        std::cout << "（没有可检查的 block.sbcli）\n";
        return 0;
    }
    std::cout << "已检查 " << r.files.size() << " 个脚本文件"
              << (r.rootMetaFound ? "" : "（未找到根 meta.sbcli）") << "\n\n";

    if (r.diags.empty()) {
        std::cout << "没有发现问题。\n";
        return 0;
    }

    std::string curFile;
    for (const CheckDiag* d : ordered) {
        if (d->file != curFile) {
            curFile = d->file;
            std::cout << curFile << ":\n";
        }
        std::cout << "  ";
        if (d->line > 0) {
            std::cout << "第 " << d->line << " 行";
            if (d->col > 0) std::cout << " 列 " << d->col;
            std::cout << "：";
        } else {
            std::cout << "文件级：";
        }
        std::cout << (d->level == CheckLevel::Error ? "错误" : "警告")
                  << " [" << d->category << "/" << d->code << "] "
                  << d->message << "\n";
    }

    std::cout << "\n共 " << r.errors << " 个错误、" << r.warnings << " 个警告。\n";
    if (r.errors > 0) std::cout << "有错误，`sb check` 视为不通过。\n";
    return r.errors ? 1 : 0;
}

// ==========================================================================
// fix —— 发现即声明（docs/format.md §4）
// 用法：sb fix <项目目录> [--json] [--dry-run]
//
// 只写 meta.sbcli，绝不碰 block.sbcli（那是手写源文件，且重建会丢注释）。
// 结构类问题（procedures_call 缺 ARG 等）只报告，交给人改。
// ==========================================================================

int cmd_fix(Args& a) {
    std::string target = a.file.empty() ? a.path : a.file;
    if (target.empty()) target = ".";

    FixReport r = sbcliFix(target, a.dryRun);
    if (!r.error.empty()) {
        std::cerr << "错误：" << r.error << "\n";
        return 2;
    }

    std::stable_sort(r.notes.begin(), r.notes.end(),
                     [](const FixNote& x, const FixNote& y) {
                         if (x.file != y.file) return x.file < y.file;
                         return x.line < y.line;
                     });

    if (a.json) {
        Json out = Json::object();
        out["root"] = r.root;
        out["dryRun"] = r.dryRun;
        Json files = Json::array();
        for (const auto& f : r.files) files.push_back(f);
        out["files"] = std::move(files);
        Json written = Json::array();
        for (const auto& w : r.written) written.push_back(w);
        out["written"] = std::move(written);
        out["registered"] = r.registered;
        out["errors"] = r.errors;
        Json notes = Json::array();
        for (const auto& n : r.notes) {
            Json o = Json::object();
            o["file"] = n.file;
            o["line"] = n.line;
            o["level"] = n.level;
            o["code"] = n.code;
            o["message"] = n.message;
            notes.push_back(std::move(o));
        }
        out["notes"] = std::move(notes);
        out["ok"] = (r.errors == 0);
        std::cout << out.dump(2) << "\n";
        return r.errors ? 1 : 0;
    }

    std::cout << "修复项目：" << r.root << (r.dryRun ? "（--dry-run，不写文件）" : "") << "\n";
    if (r.files.empty()) {
        std::cout << "（没有可处理的 block.sbcli）\n";
        return 0;
    }
    std::cout << "扫描 " << r.files.size() << " 个脚本文件\n";

    std::string curFile;
    for (const auto& n : r.notes) {
        if (n.file != curFile) {
            curFile = n.file;
            std::cout << "\n" << curFile << ":\n";
        }
        std::cout << "  ";
        if (n.line > 0) std::cout << "第 " << n.line << " 行：";
        else            std::cout << "文件级：";
        const char* lv = (n.level == "error") ? "错误" : (n.level == "warn" ? "警告" : "提示");
        std::cout << lv << " [" << n.code << "] " << n.message << "\n";
    }
    if (r.notes.empty()) std::cout << "\n没有需要登记的新引用。\n";

    std::cout << "\n新登记 " << r.registered << " 项";
    if (!r.written.empty()) {
        std::cout << "，写入 " << r.written.size() << " 个 meta：";
        bool first = true;
        for (const auto& w : r.written) {
            if (!first) std::cout << "、";
            std::cout << w;
            first = false;
        }
    }
    std::cout << "\n";
    if (r.errors > 0) {
        std::cout << "有 " << r.errors << " 个错误需要你处理"
                  << "（fix 不改动 block.sbcli，也不臆造素材文件）。\n";
    }
    return r.errors ? 1 : 0;
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
    std::vector<std::string> filesSbc = findSbcliBlockFiles(a.dir);
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

    // sbcli 项目目录的 block.sbcli（纯文本搜索）
    filter(filesSbc);

    // 项目文本文件的搜索 worker：大小写不敏感行匹配
    auto sbcFindInFile = [&](const std::string& p) -> std::vector<FindHit> {
        std::vector<FindHit> out;
        std::ifstream in(p, std::ios::binary);
        if (!in) return out;
        std::string line;
        int ln = 0;
        while (std::getline(in, line)) {
            ++ln;
            std::string low = line;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return (char)::tolower(c); });
            if (low.find(kw) == std::string::npos) continue;
            FindHit h;
            h.file = p;
            h.target = "";
            h.where = "";
            h.text = "行" + std::to_string(ln) + ": " + line;
            out.push_back(std::move(h));
        }
        return out;
    };

    std::vector<FindHit> hits;
    // ---- 多线程搜索：按 --jobs 分片（默认 4，与 sb.py 一致）----
    int jobs = a.jobs > 0 ? a.jobs : 4;
    // 交替混合文件，尽量均摊；去重（.sb 会被两类同时匹配）
    std::vector<std::string> files;
    files.reserve(files1.size() + files3.size() + filesSbc.size());
    std::set<std::string> seenFiles;
    size_t n = std::max({files1.size(), files3.size(), filesSbc.size()});
    for (size_t i = 0; i < n; ++i) {
        if (i < files1.size() && seenFiles.insert(files1[i]).second)
            files.push_back(files1[i]);
        if (i < files3.size() && seenFiles.insert(files3[i]).second)
            files.push_back(files3[i]);
        if (i < filesSbc.size() && seenFiles.insert(filesSbc[i]).second)
            files.push_back(filesSbc[i]);
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
                          : (std::find(filesSbc.begin(), filesSbc.end(), p) != filesSbc.end()
                             ? sbcFindInFile(p) : sb3FindInFile(p, kw, a.script));
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
                  : (std::find(filesSbc.begin(), filesSbc.end(), p) != filesSbc.end()
                     ? sbcFindInFile(p) : sb3FindInFile(p, kw, a.script));
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

// ==========================================================================
// view —— 浏览 sbcli 项目（角色清单 + 脚本中文）
// 用法：sb view <项目目录> [--json] [--sprite 角色id]
// ==========================================================================

int cmd_view(Args& a) {
    std::string target = a.file.empty() ? a.path : a.file;
    if (target.empty()) target = ".";

    ViewReport r = sbcliView(target);
    if (!r.error.empty()) {
        std::cerr << "错误：" << r.error << "\n";
        return 2;
    }

    // --sprite 过滤：按 id 或名字
    std::vector<const ViewSprite*> list;
    for (const auto& s : r.sprites) list.push_back(&s);
    if (!a.sprite.empty()) {
        std::vector<const ViewSprite*> keep;
        for (const ViewSprite* s : list)
            if (s->id == a.sprite || s->name == a.sprite) keep.push_back(s);
        if (keep.empty()) {
            std::cerr << "错误：没有找到角色「" << a.sprite << "」\n";
            return 2;
        }
        list = keep;
    }

    if (a.json) {
        Json out = Json::object();
        out["root"] = r.root;
        out["project"] = r.projectName;
        Json arr = Json::array();
        for (const ViewSprite* s : list) {
            Json js = Json::object();
            js["id"] = s->id;
            js["name"] = s->name;
            js["isStage"] = s->isStage;
            js["path"] = s->path;
            js["scripts"] = s->scriptCount;
            js["blocks"] = s->blockCount;
            js["costumes"] = s->costumeCount;
            js["sounds"] = s->soundCount;
            Json scs = Json::array();
            for (const auto& sc : s->scripts) {
                Json jsc = Json::object();
                jsc["hat"] = sc.hat;
                jsc["hatArg"] = sc.hatArg;
                jsc["line"] = sc.line;
                jsc["hatText"] = sc.hatText;
                Json lines = Json::array();
                for (const auto& l : sc.lines) {
                    Json jl = Json::object();
                    jl["indent"] = l.indent;
                    jl["text"] = l.text;
                    jl["opcode"] = l.opcode;
                    jl["srcLine"] = l.srcLine;
                    lines.push_back(std::move(jl));
                }
                jsc["lines"] = std::move(lines);
                scs.push_back(std::move(jsc));
            }
            js["scriptList"] = std::move(scs);
            arr.push_back(std::move(js));
        }
        out["sprites"] = std::move(arr);
        std::cout << out.dump(2) << "\n";
        return 0;
    }

    // 文本模式
    std::cout << "项目：" << r.root;
    if (!r.projectName.empty()) std::cout << "  （" << r.projectName << "）";
    std::cout << "\n";
    if (r.sprites.empty()) {
        std::cout << "（没有找到角色目录）\n";
        return 0;
    }

    std::cout << "\n角色 " << list.size() << " 个：\n";
    for (const ViewSprite* s : list) {
        std::cout << "  " << (s->isStage ? "[舞台]" : "[角色]")
                  << " " << s->id
                  << (s->name != s->id ? "（" + s->name + "）" : "")
                  << "  脚本 " << s->scriptCount
                  << " · 积木 " << s->blockCount
                  << " · 造型 " << s->costumeCount
                  << " · 声音 " << s->soundCount << "\n";
        if (!s->hasMeta)  std::cout << "        （无 meta.sbcli）\n";
        if (!s->hasBlock) std::cout << "        （无 block.sbcli）\n";
    }

    for (const ViewSprite* s : list) {
        if (s->scripts.empty()) continue;
        std::cout << "\n" << std::string(60, '=') << "\n";
        std::cout << (s->isStage ? "舞台 " : "角色 ") << s->id
                  << (s->name != s->id ? "（" + s->name + "）" : "") << "\n";
        std::cout << std::string(60, '=') << "\n";
        for (const auto& sc : s->scripts) {
            std::cout << "\n  ◆ " << sc.hatText
                      << "    @script " << sc.hat
                      << (sc.hatArg.empty() ? "" : " " + sc.hatArg)
                      << "  (行 " << sc.line << ")\n";
            for (const auto& l : sc.lines) {
                std::cout << "      "
                          << std::string((size_t)l.indent * 2, ' ')
                          << l.text << "\n";
            }
        }
    }
    return 0;
}

// ==========================================================================
// refs：项目目录 → 变量/列表读写交叉引用（与 sb3 版同风格）
// 数据源是 block.sbcli 的 AST（sbcParseFile），不经过 simdjson DOM。
// ==========================================================================

namespace {

// 从 AST 收集某脚本里对变量/列表的读写操作。
// actionByOp：写时的动作词（设置/增减/追加/删除项/清空/替换/插入/读取）。
struct ProjOp {
    int    script = 0;
    std::string action;
    std::string text;   // 渲染后的块文本（用 sbcliView 的能力）
};

void collectSbcRefs(const SbcBlock& b, std::map<std::string, std::vector<ProjOp>>& writes,
                    std::map<std::string, std::vector<ProjOp>>& reads,
                    std::map<std::string, std::vector<ProjOp>>& bcasts,
                    int scriptIdx) {
    const std::string& op = b.opcode;
    auto paramVal = [&](const char* canon) -> std::string {
        const SbcParam* p = b.find(canon);
        if (!p || !p->value || p->value->kind != SbcValue::Kind::Scalar) return {};
        std::string s = p->value->text();
        // 去掉可能的作用域前缀 @local:/@global:（只留名）
        if (s.rfind("@local:", 0) == 0)  s = s.substr(7);
        else if (s.rfind("@global:", 0) == 0) s = s.substr(8);
        // 去掉引号
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
        return s;
    };
    ProjOp o; o.script = scriptIdx;

    if (op == "data_setvariableto")      { o.action = "设置"; writes[paramVal("VARIABLE")].push_back(o); }
    else if (op == "data_changevariableby") { o.action = "增减"; writes[paramVal("VARIABLE")].push_back(o); }
    else if (op == "data_variable")      { o.action = "读取"; reads[paramVal("VARIABLE")].push_back(o); }
    else if (op == "data_showvariable" || op == "data_hidevariable") { reads[paramVal("VARIABLE")].push_back(o); }
    else if (op == "data_addtolist")     { o.action = "追加"; writes[paramVal("LIST")].push_back(o); }
    else if (op == "data_deleteoflist")  { o.action = "删除项"; writes[paramVal("LIST")].push_back(o); }
    else if (op == "data_deletealloflist"){ o.action = "清空"; writes[paramVal("LIST")].push_back(o); }
    else if (op == "data_insertatlist")  { o.action = "插入"; writes[paramVal("LIST")].push_back(o); }
    else if (op == "data_replaceitemoflist") { o.action = "替换"; writes[paramVal("LIST")].push_back(o); }
    else if (op == "data_itemoflist" || op == "data_lengthoflist" ||
             op == "data_listcontainsitem" || op == "data_listindexofitem") {
        o.action = "读取";
        reads[paramVal("LIST")].push_back(o);
    }
    else if (op == "data_listcontents")  { reads[paramVal("LIST")].push_back(o); }
    else if (op == "event_broadcast" || op == "event_broadcastandwait") {
        bcasts[paramVal("BROADCAST_INPUT")].push_back(o);
    }
    else if (op == "event_whenbroadcastreceived") {
        // 接收也算在 bcasts 里但区分方向；这里简化记录为接收
        const SbcParam* p = b.find("BROADCAST_OPTION");
        if (p && p->value && p->value->kind == SbcValue::Kind::Scalar) {
            std::string nm = p->value->text();
            if (nm.size() >= 2 && nm.front() == '"' && nm.back() == '"')
                nm = nm.substr(1, nm.size() - 2);
            o.action = "接收";
            bcasts[nm].push_back(o);
        }
    }

    // 递归子栈
    for (const auto& sub : b.substacks)
        for (const auto& cb : sub)
            collectSbcRefs(cb, writes, reads, bcasts, scriptIdx);
}

} // namespace

static int sbcliCmdRefs(Args& a) {
    // 项目目录 → 角色列表（复用 sbcliView 找到 character/ 下的目录）
    std::string root = sb::meta::findProjectRoot(a.file);
    if (root.empty()) { std::cerr << "错误：不是 sbcli 项目目录：" << a.file << "\n"; return 2; }

    // 名字 → 角色 → 操作
    struct RefEntry { int script; std::string action; std::string text; };
    std::map<std::string, std::map<std::string, std::vector<RefEntry>>> writes, reads;
    std::map<std::string, std::map<std::string, std::vector<RefEntry>>> bcasts;
    std::set<std::string> listNames;   // 名字是否列表

    /* 遍历角色目录 */ {
        std::error_code ec;
        std::filesystem::path cd = std::filesystem::u8path(root) / "character";
        std::vector<std::pair<std::string, std::string>> roles; // (dirId, name)
        if (std::filesystem::is_directory(cd, ec))
            for (auto& de : std::filesystem::directory_iterator(cd, ec))
                if (de.is_directory(ec)) {
                    std::string id = de.path().filename().u8string();
                    sb::meta::CharMeta m = sb::meta::loadCharMeta(
                        (de.path() / "meta.sbcli").u8string());
                    std::string nm = m.has("name") ? m.get("name") : id;
                    roles.emplace_back(id, nm);
                }
        std::sort(roles.begin(), roles.end());

        for (auto& role : roles) {
            std::filesystem::path bp = cd / role.first / "block.sbcli";
            if (!std::filesystem::exists(bp)) continue;
            SbcFile sf = sbcParseFile(bp.u8string());
            int scriptIdx = 0;
            for (const auto& sc : sf.scripts) {
                ++scriptIdx;
                std::map<std::string, std::vector<ProjOp>> w, r, bc;
                for (const auto& b : sc.blocks)
                    collectSbcRefs(b, w, r, bc, scriptIdx);
                // 合并进大表
                auto merge = [&role, &scriptIdx](auto& dst, const auto& src) {
                    for (auto& kv : src) {
                        for (auto& o : kv.second) {
                            RefEntry e; e.script = o.script; e.action = o.action;
                            e.text = "·脚本" + std::to_string(o.script);
                            dst[kv.first][role.second].push_back(std::move(e));
                        }
                    }
                };
                merge(writes, w);
                merge(reads, r);
                merge(bcasts, bc);
            }
        }
    }
    // 识别列表：从 meta 的 lists 字段
    {
        std::error_code ec;
        std::filesystem::path cd = std::filesystem::u8path(root) / "character";
        if (std::filesystem::is_directory(cd, ec))
            for (auto& de : std::filesystem::directory_iterator(cd, ec))
                if (de.is_directory(ec)) {
                    sb::meta::CharMeta m = sb::meta::loadCharMeta(
                        (de.path() / "meta.sbcli").u8string());
                    for (auto& l : m.lists) listNames.insert(l);
                }
        sb::meta::CharMeta rm = sb::meta::loadCharMeta(root + "/meta.sbcli");
        /* RootMeta 的 lists 是 map */ {
            sb::meta::RootMeta r2 = sb::meta::loadRootMeta(root + "/meta.sbcli");
            for (auto& kv : r2.lists) listNames.insert(kv.first);
        }
    }

    // ---- 输出 ----
    auto show = [&](const char* title,
                    std::map<std::string, std::map<std::string, std::vector<RefEntry>>>& tbl) {
        for (auto& kv : tbl) {
            if (!a.extra.empty() && kv.first.find(a.extra[0]) == std::string::npos) continue;
            std::cout << "\n" << (listNames.count(kv.first) ? "列表" : "变量")
                      << " " << kv.first << "\n";
            std::cout << "  " << title << "：\n";
            for (auto& role : kv.second) {
                std::cout << "    " << role.first << "\n";
                std::string cur;
                std::string curAction;
                for (auto& e : role.second) {
                    if (e.script != 0 && (cur != "·脚本" + std::to_string(e.script) ||
                                          curAction != e.action)) {
                        std::cout << "      · 脚本" << e.script << "\n";
                        cur = "·脚本" + std::to_string(e.script);
                        curAction = e.action;
                    }
                    std::cout << "          " << e.action << "：" << e.text << "\n";
                }
            }
        }
    };
    show("写", writes);
    show("读", reads);
    if (!a.extra.empty()) {
        for (auto& kv : bcasts) {
            if (kv.first.find(a.extra[0]) == std::string::npos) continue;
            std::cout << "\n广播 " << kv.first << "\n";
            for (auto& role : kv.second)
                for (auto& e : role.second)
                    std::cout << "  " << role.first << " ·脚本" << e.script
                              << " " << e.action << "\n";
        }
    }
    return 0;
}

// ==========================================================================
// events：项目目录 → 广播拓扑（与 sb3 版同风格）
// ==========================================================================

static int sbcliCmdEvents(Args& a) {
    std::string root = sb::meta::findProjectRoot(a.file);
    if (root.empty()) { std::cerr << "错误：不是 sbcli 项目目录：" << a.file << "\n"; return 2; }

    // 广播名 → (发送者/接收者: 角色·脚本)
    struct Ev { std::string who; int script; };
    std::map<std::string, std::vector<Ev>> senders, receivers;

    std::error_code ec;
    std::filesystem::path cd = std::filesystem::u8path(root) / "character";
    std::vector<std::pair<std::string, std::string>> roles; // (dirId, name)
    if (std::filesystem::is_directory(cd, ec))
        for (auto& de : std::filesystem::directory_iterator(cd, ec))
            if (de.is_directory(ec)) {
                std::string id = de.path().filename().u8string();
                sb::meta::CharMeta m = sb::meta::loadCharMeta(
                    (de.path() / "meta.sbcli").u8string());
                std::string nm = m.has("name") ? m.get("name") : id;
                roles.emplace_back(id, nm);
            }
    std::sort(roles.begin(), roles.end());

    for (auto& role : roles) {
        std::filesystem::path bp = cd / role.first / "block.sbcli";
        if (!std::filesystem::exists(bp)) continue;
        SbcFile sf = sbcParseFile(bp.u8string());
        int scriptIdx = 0;
        for (const auto& sc : sf.scripts) {
            ++scriptIdx;
            std::function<void(const SbcBlock&)> walk =
                [&](const SbcBlock& b) {
                    const std::string& op = b.opcode;
                    if (op == "event_broadcast" || op == "event_broadcastandwait") {
                        const SbcParam* p = b.find("BROADCAST_INPUT");
                        if (p && p->value && p->value->kind == SbcValue::Kind::Scalar) {
                            std::string nm = p->value->text();
                            if (nm.size() >= 2 && nm.front() == '"' && nm.back() == '"')
                                nm = nm.substr(1, nm.size() - 2);
                            Ev e; e.who = role.second; e.script = scriptIdx;
                            senders[nm].push_back(std::move(e));
                        }
                    } else if (op == "event_whenbroadcastreceived") {
                        const SbcParam* p = b.find("BROADCAST_OPTION");
                        if (p && p->value && p->value->kind == SbcValue::Kind::Scalar) {
                            std::string nm = p->value->text();
                            if (nm.size() >= 2 && nm.front() == '"' && nm.back() == '"')
                                nm = nm.substr(1, nm.size() - 2);
                            Ev e; e.who = role.second; e.script = scriptIdx;
                            receivers[nm].push_back(std::move(e));
                        }
                    }
                    for (const auto& sub : b.substacks)
                        for (const auto& cb : sub) walk(cb);
                };
            for (const auto& b : sc.blocks) walk(b);
        }
    }

    // 输出
    std::cout << "文件：" << basename(a.file) << "（sbcli 项目）\n";
    std::set<std::string> allB;
    for (auto& kv : senders) allB.insert(kv.first);
    for (auto& kv : receivers) allB.insert(kv.first);
    if (allB.empty()) { std::cout << "这个作品没有广播。\n"; return 0; }
    for (auto& bname : allB) {
        std::cout << "\n■ " << bname;
        bool hasS = senders.count(bname) && !senders[bname].empty();
        bool hasR = receivers.count(bname) && !receivers[bname].empty();
        if (!hasS) std::cout << "（⚠ 无发送者：可能是变量广播或残留）";
        else if (!hasR) std::cout << "（⚠ 无接收者：广播发出但没人监听，可能拼错或残留）";
        std::cout << "\n";
        if (hasS) {
            std::cout << "  发：";
            bool first = true;
            for (auto& e : senders[bname]) {
                if (!first) std::cout << "，";
                first = false;
                std::cout << e.who << "·脚本" << e.script;
            }
            std::cout << "\n";
        }
        if (hasR) {
            std::cout << "  收：";
            bool first = true;
            for (auto& e : receivers[bname]) {
                if (!first) std::cout << "，";
                first = false;
                std::cout << e.who << "·脚本" << e.script;
            }
            std::cout << "\n";
        }
    }
    return 0;
}

// ==========================================================================
// text：项目目录 → 提取文本参数（MESSAGE / 文本字面量等）
// ==========================================================================

static int sbcliCmdText(Args& a) {
    std::string root = sb::meta::findProjectRoot(a.file);
    if (root.empty()) { std::cerr << "错误：不是 sbcli 项目目录：" << a.file << "\n"; return 2; }

    std::vector<std::string> texts;
    std::error_code ec;
    std::filesystem::path cd = std::filesystem::u8path(root) / "character";
    std::vector<std::string> roleIds;
    if (std::filesystem::is_directory(cd, ec))
        for (auto& de : std::filesystem::directory_iterator(cd, ec))
            if (de.is_directory(ec)) roleIds.push_back(de.path().filename().u8string());
    std::sort(roleIds.begin(), roleIds.end());

    for (auto& id : roleIds) {
        std::filesystem::path bp = cd / id / "block.sbcli";
        if (!std::filesystem::exists(bp)) continue;
        SbcFile sf = sbcParseFile(bp.u8string());
        std::function<void(const SbcBlock&)> walk = [&](const SbcBlock& b) {
            // 文本型参数：canon 名是文本槽的（MESSAGE/STRING1/STRING2/ANSWER…）
            // 以及标量值本身就是「带引号」的文本字面量
            for (const auto& p : b.params) {
                if (!p.value || p.value->kind != SbcValue::Kind::Scalar) continue;
                const std::string& t = p.value->text();
                // 词法器把 STRING 存为「不含引号的解码值」；用 quoted 标记判断
                if (p.value->scalar.quoted)
                    texts.push_back(t);
            }
            for (const auto& sub : b.substacks)
                for (const auto& cb : sub) walk(cb);
        };
        for (const auto& sc : sf.scripts)
            for (const auto& b : sc.blocks) walk(b);
    }

    // 去重保序
    std::sort(texts.begin(), texts.end());
    texts.erase(std::unique(texts.begin(), texts.end()), texts.end());

    if (a.json) {
        Json out = Json::object();
        out["file"] = a.file;
        Json arr = Json::array();
        for (auto& s : texts) arr.push_back(s);
        out["texts"] = arr;
        sb3JsonOut(out);
        return 0;
    }
    for (auto& s : texts) std::cout << s << "\n";
    return 0;
}

// ==========================================================================
// blocks：项目目录 → opcode 使用统计
// ==========================================================================

static int sbcliCmdBlocks(Args& a) {
    std::string root = sb::meta::findProjectRoot(a.file);
    if (root.empty()) { std::cerr << "错误：不是 sbcli 项目目录：" << a.file << "\n"; return 2; }

    std::map<std::string, long long> opCount;
    std::error_code ec;
    std::filesystem::path cd = std::filesystem::u8path(root) / "character";
    std::vector<std::string> roleIds;
    if (std::filesystem::is_directory(cd, ec))
        for (auto& de : std::filesystem::directory_iterator(cd, ec))
            if (de.is_directory(ec)) roleIds.push_back(de.path().filename().u8string());
    std::sort(roleIds.begin(), roleIds.end());

    long long total = 0;
    for (auto& id : roleIds) {
        std::filesystem::path bp = cd / id / "block.sbcli";
        if (!std::filesystem::exists(bp)) continue;
        SbcFile sf = sbcParseFile(bp.u8string());
        std::function<void(const SbcBlock&)> walk = [&](const SbcBlock& b) {
            opCount[b.opcode]++; total++;
            for (const auto& sub : b.substacks)
                for (const auto& cb : sub) walk(cb);
        };
        for (const auto& sc : sf.scripts)
            for (const auto& b : sc.blocks) walk(b);
    }

    if (a.json) {
        Json out = Json::object();
        out["file"] = a.file;
        out["total"] = total;
        Json ops = Json::object();
        for (auto& kv : opCount) ops[kv.first] = kv.second;
        out["opcodes"] = ops;
        sb3JsonOut(out);
        return 0;
    }
    std::cout << "文件：" << basename(a.file) << "（sbcli 项目）\n";
    std::cout << "共 " << total << " 个积木、" << opCount.size() << " 种 opcode\n";
    // 按次数降序
    std::vector<std::pair<long long, std::string>> sorted;
    for (auto& kv : opCount) sorted.emplace_back(kv.second, kv.first);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& x, const auto& y) {
                  if (x.first != y.first) return x.first > y.first;
                  return x.second < y.second;
              });
    long long shown = 0;
    for (auto& kv : sorted) {
        std::cout << "  " << kv.second << " ×" << kv.first << "\n";
        if (a.limitSet && ++shown >= a.limit) break;
    }
    return 0;
}

// ==========================================================================
// project —— 项目脚手架：init / add-sprite / add-costume / add-sound /
//             add-variable / add-list / add-broadcast
// 用法：sb project <子命令> <项目目录> [参数...]
// ==========================================================================

int cmd_project(Args& a) {
    // 兼容两种调用：
    //   sb project init <目录> [名字]      → a.file=init, extra=[目录, 名字]
    //   sb project <目录> init [名字]      → a.file=<目录>, extra=[init, 名字]
    static const std::set<std::string> SUBS = {
        "init", "add-sprite", "add-costume", "add-sound",
        "add-variable", "add-list", "add-broadcast",
    };
    std::string sub;
    std::vector<std::string> rest;
    std::string proj;
    if (SUBS.count(a.file)) {
        sub = a.file;
        rest = a.extra;                     // extra 全部是参数
        proj = rest.empty() ? "" : rest[0];
        if (!proj.empty()) rest.erase(rest.begin());
    } else {
        proj = a.file;
        sub = a.extra.empty() ? "" : a.extra[0];
        rest.assign(a.extra.begin() + 1, a.extra.end());
    }

    auto show = [](const ProjResult& r) {
        if (!r.ok) { std::cerr << "错误：" << r.error << "\n"; return 2; }
        for (auto& c : r.created) std::cout << "  ✓ " << c << "\n";
        for (auto& n : r.notes)   std::cout << "  · " << n << "\n";
        return 0;
    };

    if (sub == "init") {
        return show(sbcliProjectInit(proj, rest.empty() ? "" : rest[0]));
    } else if (sub == "add-sprite") {
        if (rest.empty()) { std::cerr << "用法：sb project add-sprite <项目> <名字>\n"; return 2; }
        return show(sbcliAddSprite(proj, rest[0]));
    } else if (sub == "add-costume") {
        if (rest.size() < 3) { std::cerr << "用法：sb project add-costume <项目> <角色> <素材文件> <造型名>\n"; return 2; }
        return show(sbcliAddCostume(proj, rest[0], rest[1], rest[2]));
    } else if (sub == "add-sound") {
        if (rest.size() < 3) { std::cerr << "用法：sb project add-sound <项目> <角色> <素材文件> <声音名>\n"; return 2; }
        return show(sbcliAddSound(proj, rest[0], rest[1], rest[2]));
    } else if (sub == "add-variable") {
        if (rest.size() < 2) { std::cerr << "用法：sb project add-variable <项目> <名字> <初值> [角色]\n"; return 2; }
        return show(sbcliAddVariable(proj, rest[0], rest[1], rest.size() > 2 ? rest[2] : ""));
    } else if (sub == "add-list") {
        if (rest.size() < 2) { std::cerr << "用法：sb project add-list <项目> <名字> <元素...> [角色]\n"; return 2; }
        std::string items = rest[1];
        for (size_t i = 2; i + 1 < rest.size(); ++i) items += " " + rest[i];
        std::string scope = rest.size() >= 3 ? rest[rest.size() - 1] : "";
        return show(sbcliAddList(proj, rest[0], items, scope));
    } else if (sub == "add-broadcast") {
        if (rest.empty()) { std::cerr << "用法：sb project add-broadcast <项目> <名字>\n"; return 2; }
        return show(sbcliAddBroadcast(proj, rest[0]));
    } else {
        std::cerr << "未知 project 子命令：" << sub << "\n"
                  << "可用：init / add-sprite / add-costume / add-sound /\n"
                  << "      add-variable / add-list / add-broadcast\n";
        return 2;
    }
}

// ==========================================================================
// unpack —— 把 .sb3 作品拆成 sbcli 项目目录（pack 的逆操作）
// 用法：sb unpack <作品.sb3> <输出目录> [--force]
// ==========================================================================

int cmd_unpack(Args& a) {
    std::string in = a.file;
    std::string out;
    if (!a.extra.empty()) out = a.extra[0];
    if (out.empty() && !a.path.empty() && a.path != ".") out = a.path;
    if (in.empty() || out.empty()) {
        std::cerr << "用法：sb unpack <作品.sb3> <输出目录> [--force]\n";
        return 2;
    }

    UnpackResult r = sbcliUnpack(in, out, a.force);
    if (!r.ok) {
        std::cerr << "错误：" << r.error << "\n";
        return 2;
    }

    if (a.json) {
        Json js = Json::object();
        js["outDir"]      = r.outDir;
        js["project"]     = r.projectName;
        js["sprites"]     = r.spriteCount;
        js["scripts"]     = r.scriptCount;
        js["assets"]      = r.assetCount;
        js["unknownBlocks"] = r.unknownBlocks;
        std::cout << js.dump(2) << "\n";
        return 0;
    }

    std::cout << "已解包：" << r.outDir << "\n";
    std::cout << "  项目名：" << r.projectName << "\n";
    std::cout << "  角色数（不含舞台）：" << r.spriteCount << "\n";
    std::cout << "  脚本数：" << r.scriptCount << "\n";
    std::cout << "  导出素材：" << r.assetCount << "\n";
    if (!r.log.empty()) std::cout << r.log;
    if (r.unknownBlocks > 0)
        std::cout << "  注意：有 " << r.unknownBlocks
                  << " 个未知积木（已原样保留，行前带 # 未知积木 注释）\n";
    std::cout << "\n下一步：sb check " << r.outDir << "  →  sb view " << r.outDir << "\n";
    return 0;
}

} // namespace sb
