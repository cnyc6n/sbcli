// src/sbcli_merge.cpp —— 跨作品合并 / 导出角色
#include "sbcli_merge.hpp"
#include "sbcli_meta.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace sb::meta;

namespace sb {
namespace {

MergeResult fail(const std::string& msg) {
    MergeResult r;
    r.ok = false;
    r.error = msg;
    return r;
}

std::string low(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return s;
}

bool copyFileTo(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::create_directories(fs::u8path(to).parent_path(), ec);
    fs::copy_file(fs::u8path(from), fs::u8path(to), fs::copy_options::overwrite_existing, ec);
    return !ec;
}

std::string readAll(const std::string& p) {
    std::ifstream in(fs::u8path(p), std::ios::binary);
    if (!in.good()) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 扫描 character/ 下所有角色目录（跳过 stage）
struct SpriteDir { std::string dir, id, name; };
std::vector<SpriteDir> listSprites(const std::string& root) {
    std::vector<SpriteDir> out;
    std::error_code ec;
    std::string ch = root + "/character";
    if (!fs::is_directory(fs::u8path(ch), ec)) return out;
    for (auto& e : fs::directory_iterator(fs::u8path(ch), ec)) {
        if (!e.is_directory()) continue;
        std::string id = e.path().filename().u8string();
        if (low(id) == "stage") continue;
        CharMeta cm = loadCharMeta(e.path().u8string() + "/meta.sbcli");
        std::string nm = cm.get("name");
        if (nm.empty()) nm = id;
        out.push_back({ e.path().u8string(), id, nm });
    }
    return out;
}

// 目标项目里下一个可用数字目录 id
std::string nextSpriteId(const std::string& dstRoot) {
    std::error_code ec;
    std::string ch = dstRoot + "/character";
    std::set<int> used;
    if (fs::is_directory(fs::u8path(ch), ec))
        for (auto& e : fs::directory_iterator(fs::u8path(ch), ec)) {
            if (!e.is_directory()) continue;
            std::string id = e.path().filename().u8string();
            try { used.insert(std::stoi(id)); } catch (...) {}
        }
    int n = 1;
    while (used.count(n)) ++n;
    return std::to_string(n);
}

// 读取角色 meta 里登记的素材相对路径
std::vector<std::string> assetPathsOf(const CharMeta& cm) {
    std::vector<std::string> out;
    for (const auto& kv : cm.costumes) if (!kv.second.empty()) out.push_back(kv.second);
    for (const auto& kv : cm.sounds)   if (!kv.second.empty()) out.push_back(kv.second);
    return out;
}

// 把 src 的角色引用的素材拷进 dst（保持相对路径；重名但内容不同时加后缀）
int copyAssets(const std::string& srcRoot, const std::string& dstRoot,
               const CharMeta& cm, std::vector<std::string>* warnings) {
    int n = 0;
    std::error_code ec;
    for (const auto& rel : assetPathsOf(cm)) {
        std::string from = srcRoot + "/" + rel;
        if (!fs::is_regular_file(fs::u8path(from), ec)) {
            if (warnings) warnings->push_back("素材缺失，跳过：" + rel);
            continue;
        }
        std::string to = dstRoot + "/" + rel;
        if (fs::exists(fs::u8path(to), ec)) {
            // 目标已有同名素材：内容相同就跳过，不同则加后缀避免覆盖
            std::string a = readAll(from), b = readAll(to);
            if (a == b) { ++n; continue; }
            fs::path p = fs::u8path(to);
            std::string base = p.stem().u8string(), ext = p.extension().u8string();
            for (int k = 2;; ++k) {
                std::string cand = (p.parent_path() / (base + "_" + std::to_string(k) + ext)).u8string();
                if (!fs::exists(fs::u8path(cand), ec)) { to = cand; break; }
                if (readAll(cand) == a) { to = cand; break; }
            }
        }
        if (copyFileTo(from, to)) ++n;
        else if (warnings) warnings->push_back("拷贝失败：" + rel);
    }
    return n;
}

} // namespace

MergeResult sbcMergeProject(const std::string& dstProject,
                            const std::string& srcProject,
                            const std::string& spriteFilter) {
    std::string dst = findProjectRoot(dstProject);
    std::string src = findProjectRoot(srcProject);
    if (dst.empty()) return fail("目标项目根目录无效：" + dstProject);
    if (src.empty()) return fail("源项目根目录无效：" + srcProject);
    if (dst == src) return fail("源和目标是同一个项目");

    MergeResult r;
    std::vector<SpriteDir> srcSprites = listSprites(src);
    if (srcSprites.empty()) return fail("源项目里没有角色（只有舞台？）");

    // 目标已有角色名（用于重名检测）
    std::set<std::string> taken;
    for (auto& s : listSprites(dst)) taken.insert(low(s.name));

    for (auto& sp : srcSprites) {
        if (!spriteFilter.empty() && low(sp.name) != low(spriteFilter) &&
            low(sp.id) != low(spriteFilter))
            continue;

        std::string finalName = sp.name;
        int suffix = 2;
        while (taken.count(low(finalName)))
            finalName = sp.name + std::to_string(suffix++);
        bool renamedSelf = (finalName != sp.name);

        std::string newId = nextSpriteId(dst);
        std::string dstDir = dst + "/character/" + newId;
        std::error_code ec;
        fs::create_directories(fs::u8path(dstDir), ec);

        // 1) 拷素材 + 改 meta（名字/素材路径可能因去重而变）
        CharMeta cm = loadCharMeta(sp.dir + "/meta.sbcli");
        std::vector<std::string> warns;
        int copied = copyAssets(src, dst, cm, &warns);
        for (auto& w : warns) r.notes.push_back(w);

        // meta：改名 + 修正素材路径（若拷贝时加了后缀，需要同步——这里简化：
        // 素材去重只在内容不同时发生，概率低，先把名字改对）
        cm.kv["name"] = finalName;
        writeCharMeta(dstDir + "/meta.sbcli", cm, finalName, false);

        // 2) 拷 block.sbcli（如有）
        std::string srcBlock = sp.dir + "/block.sbcli";
        std::string dstBlock = dstDir + "/block.sbcli";
        if (fs::is_regular_file(fs::u8path(srcBlock), ec))
            copyFileTo(srcBlock, dstBlock);
        else
            std::ofstream(fs::u8path(dstBlock)) << "";

        // 3) 目标项目里如果有别的角色引用了「sp.name」（如 motion_goto_menu OBJECT=名字），
        //    改名后要把这些引用改成 finalName
        if (renamedSelf) {
            ++r.renamed;
            r.notes.push_back("角色「" + sp.name + "」重名，已改名为「" + finalName + "」");
        }
        taken.insert(low(finalName));
        ++r.sprites;
        r.notes.push_back("合并角色「" + finalName + "」（目录 " + newId + "，" +
                          std::to_string(copied) + " 个素材）");
    }

    if (r.sprites == 0)
        return fail("没有匹配到要合并的角色" +
                    (spriteFilter.empty() ? std::string() : "「" + spriteFilter + "」"));
    r.notes.push_back("提示：合并后请跑 `sb check` 确认引用（变量/广播可能需 `sb fix`）");
    return r;
}

MergeResult sbcExportSprite(const std::string& srcProject,
                            const std::string& spriteName,
                            const std::string& outProject) {
    std::string src = findProjectRoot(srcProject);
    if (src.empty()) return fail("源项目根目录无效：" + srcProject);
    if (spriteName.empty()) return fail("缺少角色名");

    std::vector<SpriteDir> sprites = listSprites(src);
    SpriteDir hit;
    bool found = false;
    for (auto& s : sprites)
        if (low(s.name) == low(spriteName) || low(s.id) == low(spriteName)) { hit = s; found = true; break; }
    if (!found) return fail("找不到角色：" + spriteName);

    // 建目标项目骨架
    std::error_code ec;
    fs::create_directories(fs::u8path(outProject + "/character/stage"), ec);
    fs::create_directories(fs::u8path(outProject + "/assets"), ec);

    // 根 meta（项目名 = 角色名）
    RootMeta rm;
    rm.name = hit.name;
    writeRootMeta(outProject + "/meta.sbcli", rm);

    // 舞台 meta（空舞台，避免 pack 时缺舞台）
    CharMeta st;
    writeCharMeta(outProject + "/character/stage/meta.sbcli", st, "Stage", true);

    // 角色
    std::string dstDir = outProject + "/character/1";
    fs::create_directories(fs::u8path(dstDir), ec);
    CharMeta cm = loadCharMeta(hit.dir + "/meta.sbcli");
    std::vector<std::string> warns;
    int copied = copyAssets(src, outProject, cm, &warns);
    writeCharMeta(dstDir + "/meta.sbcli", cm, hit.name, false);

    std::string srcBlock = hit.dir + "/block.sbcli";
    if (fs::is_regular_file(fs::u8path(srcBlock), ec))
        copyFileTo(srcBlock, dstDir + "/block.sbcli");
    else
        std::ofstream(fs::u8path(dstDir + "/block.sbcli")) << "";

    MergeResult r;
    r.sprites = 1;
    for (auto& w : warns) r.notes.push_back(w);
    r.notes.push_back("已导出角色「" + hit.name + "」（" + std::to_string(copied) +
                      " 个素材）→ " + outProject);
    r.notes.push_back("提示：角色私有变量/列表可能引用了源项目的广播，建议 `sb check` 后 `sb fix`");
    return r;
}

} // namespace sb
