// src/sbcli_project.cpp
// 项目脚手架：sb project init / add-*（docs/format.md §0 / §3）。
//
// 与 fix 的分工：
//   init / add-*  → 建结构（目录、meta 骨架、素材拷贝、显式登记）
//   fix           → 补引用（扫描 block.sbcli 引用到但没声明的名字）
// 两者共用 src/sbcli_meta.hpp 这一层读写，格式不会漂移。
//
// 幂等：init 对已存在的项目只补缺失部分，不覆盖已有 meta。
#include "sbcli_project.hpp"
#include "sbcli_meta.hpp"
#include "sbcli_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace sb {
namespace {

using namespace sb::meta;

ProjResult fail(const std::string& msg) {
    ProjResult r;
    r.ok = false;
    r.error = msg;
    return r;
}

// 角色目录名 → 数字 id（"1" → 1；非数字返回 -1）
int idOfLeaf(const std::string& leaf) {
    if (leaf.empty()) return -1;
    for (char c : leaf) if (!std::isdigit((unsigned char)c)) return -1;
    try { return std::stoi(leaf); } catch (...) { return -1; }
}

// 项目根 → 失败时统一报错文案
bool requireRoot(const std::string& project, std::string& root, ProjResult& err) {
    root = findProjectRoot(project);
    if (root.empty()) {
        err = fail("找不到项目根目录（需含 meta.sbcli 或 character/ 子目录）：" + project);
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------- 角色枚举

std::vector<std::pair<std::string, std::string>> sbcliListSprites(const std::string& project) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string root = findProjectRoot(project);
    if (root.empty()) return out;

    std::error_code ec;
    fs::path cd = fs::u8path(root) / "character";
    if (!fs::is_directory(cd, ec)) return out;

    std::vector<std::pair<int, std::string>> numbered;   // (id, leaf)
    for (const auto& de : fs::directory_iterator(cd, ec)) {
        if (!de.is_directory(ec)) continue;
        std::string leaf = norm(de.path().filename().u8string());
        if (leaf == "stage") continue;
        int id = idOfLeaf(leaf);
        if (id >= 0) numbered.push_back({id, leaf});
    }
    std::sort(numbered.begin(), numbered.end());
    for (const auto& p : numbered) {
        CharMeta cm = loadCharMeta(joinRel(joinRel(root, "character"), p.second) + "/meta.sbcli");
        std::string nm = cm.has("name") ? cm.get("name") : ("sprite" + p.second);
        out.push_back({p.second, nm});
    }
    return out;
}

int sbcliNextSpriteId(const std::string& project) {
    std::string root = findProjectRoot(project);
    if (root.empty()) return 1;
    std::error_code ec;
    fs::path cd = fs::u8path(root) / "character";
    if (!fs::is_directory(cd, ec)) return 1;
    int maxId = 0;
    for (const auto& de : fs::directory_iterator(cd, ec)) {
        if (!de.is_directory(ec)) continue;
        std::string leaf = norm(de.path().filename().u8string());
        int id = idOfLeaf(leaf);
        if (id > maxId) maxId = id;
    }
    return maxId + 1;
}

ProjResult sbcliResolveSprite(const std::string& project, const std::string& sprite,
                              std::string& outDir) {
    std::string root;
    ProjResult err;
    if (!requireRoot(project, root, err)) return err;

    auto list = sbcliListSprites(root);
    // 先按目录名（id）匹配
    for (const auto& p : list) {
        if (p.first == sprite) {
            outDir = joinRel(joinRel(root, "character"), p.first);
            return ProjResult{};
        }
    }
    // 再按 meta 里的 name 匹配
    for (const auto& p : list) {
        if (p.second == sprite) {
            outDir = joinRel(joinRel(root, "character"), p.first);
            return ProjResult{};
        }
    }
    std::string msg = "找不到角色「" + sprite + "」";
    if (!list.empty()) {
        msg += "（可选：";
        bool first = true;
        for (const auto& p : list) {
            if (!first) msg += "、";
            msg += p.first + "(" + p.second + ")";
            first = false;
        }
        msg += "）";
    }
    return fail(msg);
}

// ---------------------------------------------------------------- init

ProjResult sbcliProjectInit(const std::string& dir, const std::string& name) {
    ProjResult r;
    if (dir.empty()) return fail("缺少目录参数");

    std::string root = norm(dir);
    if (!makeDirs(root)) return fail("无法创建目录：" + root);

    std::string projName = name.empty()
                         ? norm(fs::path(root).filename().u8string())
                         : name;
    if (projName.empty()) projName = "未命名";

    // assets/（§0：素材放这里）
    std::string assets = joinRel(root, "assets");
    if (!dirExists(assets)) {
        if (makeDirs(assets)) r.created.push_back("assets/");
    } else {
        r.notes.push_back("assets/ 已存在，跳过");
    }

    // character/stage/
    std::string stageDir = joinRel(joinRel(root, "character"), "stage");
    std::string stageMeta = joinRel(stageDir, "meta.sbcli");
    if (!dirExists(stageDir)) {
        if (!makeDirs(stageDir)) return fail("无法创建目录：" + stageDir);
        r.created.push_back("character/stage/");
    } else {
        r.notes.push_back("character/stage/ 已存在，跳过");
    }
    if (!fileExists(stageMeta)) {
        CharMeta cm;
        cm.exists = false;
        writeCharMeta(stageMeta, cm, "Stage", true);
        r.created.push_back("character/stage/meta.sbcli");
    } else {
        r.notes.push_back("舞台 meta 已存在，保留原内容");
    }

    // 根 meta
    std::string rootMeta = joinRel(root, "meta.sbcli");
    if (!fileExists(rootMeta)) {
        RootMeta rm;
        rm.exists = false;
        rm.name = projName;
        rm.platform = "TurboWarp";   // 默认目标平台（可改 Scratch / Gandi / 留空）
        writeRootMeta(rootMeta, rm);
        // 补充说明性注释（writeRootMeta 不写注释，这里追加模板说明）
        {
            std::vector<std::string> lines = readLines(rootMeta);
            std::vector<std::string> out;
            out.push_back("# " + projName + " —— sbcli 项目定义");
            out.push_back("# 元数据字段均可选，删掉即不写入 sb3：");
            out.push_back("#   name         项目名（写入 sb3 meta.name 由平台决定）");
            out.push_back("#   author       作者");
            out.push_back("#   description  项目描述");
            out.push_back("#   platform     目标平台：Scratch / TurboWarp / Gandi");
            out.push_back("#   agent        导出工具标识（默认留空＝不写）");
            out.push_back("#   notes        本地备注（不写入 sb3）");
            out.push_back("");
            for (auto& l : lines) out.push_back(l);
            writeLines(rootMeta, out);
        }
        r.created.push_back("meta.sbcli");
    } else {
        r.notes.push_back("根 meta 已存在，保留原内容");
    }

    r.ok = true;
    return r;
}

// ---------------------------------------------------------------- add-sprite

ProjResult sbcliAddSprite(const std::string& project, const std::string& spriteName) {
    std::string root;
    ProjResult err;
    if (!requireRoot(project, root, err)) return err;
    if (spriteName.empty()) return fail("缺少角色名");

    int id = sbcliNextSpriteId(root);
    std::string dir = joinRel(joinRel(root, "character"), std::to_string(id));
    if (!makeDirs(dir)) return fail("无法创建目录：" + dir);

    CharMeta cm;
    cm.exists = false;
    writeCharMeta(joinRel(dir, "meta.sbcli"), cm, spriteName, false);

    ProjResult r;
    r.ok = true;
    r.created.push_back("character/" + std::to_string(id) + "/");
    r.created.push_back("character/" + std::to_string(id) + "/meta.sbcli");
    r.notes.push_back("角色「" + spriteName + "」= character/" + std::to_string(id));
    return r;
}

// ---------------------------------------------------------------- 素材

namespace {

// add-costume / add-sound 的共同流程：拷素材 → 登记进角色 meta
ProjResult addAsset(const std::string& project, const std::string& sprite,
                    const std::string& assetFile, const std::string& assetName,
                    bool isCostume) {
    std::string root;
    ProjResult err;
    if (!requireRoot(project, root, err)) return err;

    std::string dir;
    ProjResult rs = sbcliResolveSprite(root, sprite, dir);
    if (!rs.ok) return rs;

    if (assetFile.empty()) return fail("缺少素材文件路径");
    if (assetName.empty()) return fail(isCostume ? "缺少造型名" : "缺少声音名");
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(assetFile), ec))
        return fail("素材文件不存在：" + assetFile);

    // 拷进 assets/，保持原扩展名（造型可能是 svg/png，声音 wav/mp3）
    std::string ext = fs::u8path(assetFile).extension().u8string();
    std::string relAsset = "assets/" + assetName + ext;
    std::string dst = joinRel(root, relAsset);
    if (!copyFileTo(assetFile, dst))
        return fail("拷贝素材失败：" + assetFile + " → " + relAsset);

    std::string metaPath = joinRel(dir, "meta.sbcli");
    CharMeta cm = loadCharMeta(metaPath);
    std::string spriteDisplay = cm.has("name") ? cm.get("name")
                                               : norm(fs::path(dir).filename().u8string());
    bool isStage = (norm(fs::path(dir).filename().u8string()) == "stage");
    (isCostume ? cm.costumes : cm.sounds)[assetName] = relAsset;
    writeCharMeta(metaPath, cm, spriteDisplay, isStage);

    ProjResult r;
    r.ok = true;
    r.created.push_back(relAsset);
    r.created.push_back(relToRoot(root, metaPath));
    r.notes.push_back((isCostume ? "造型「" : "声音「") + assetName + "」→ " + relAsset);
    return r;
}

} // namespace

ProjResult sbcliAddCostume(const std::string& project, const std::string& sprite,
                           const std::string& assetFile, const std::string& costumeName) {
    return addAsset(project, sprite, assetFile, costumeName, true);
}

ProjResult sbcliAddSound(const std::string& project, const std::string& sprite,
                         const std::string& assetFile, const std::string& soundName) {
    return addAsset(project, sprite, assetFile, soundName, false);
}

// ---------------------------------------------------------------- 变量/列表/广播

namespace {

// scope 为空 → 根 meta；否则解析成角色目录 → 角色 meta
ProjResult addNamed(const std::string& project, const std::string& name,
                    const std::string& value, const std::string& scope,
                    int kind) {   // 0=variable 1=list 2=broadcast
    std::string root;
    ProjResult err;
    if (!requireRoot(project, root, err)) return err;
    if (name.empty()) return fail("缺少名字");

    ProjResult r;
    r.ok = true;

    if (scope.empty()) {
        // 根 meta
        std::string path = joinRel(root, "meta.sbcli");
        RootMeta rm = loadRootMeta(path);
        if (rm.name.empty()) rm.name = norm(fs::path(root).filename().u8string());
        bool added = false;
        if (kind == 0)      added = rm.variables.insert({name, value.empty() ? "0" : value}).second;
        else if (kind == 1) added = rm.lists.insert({name, value.empty() ? "[]" : value}).second;
        else                added = rm.broadcasts.insert(name).second;
        writeRootMeta(path, rm);
        r.created.push_back("meta.sbcli");
        r.notes.push_back(added ? ("已登记到根 meta：" + name)
                                : ("根 meta 里已存在，未改动：" + name));
        return r;
    }

    // 角色 meta
    std::string dir;
    ProjResult rs = sbcliResolveSprite(root, scope, dir);
    if (!rs.ok) return rs;
    // 广播是全局的（§4.2），指定角色也没意义 → 落根 meta
    if (kind == 2) {
        std::string path = joinRel(root, "meta.sbcli");
        RootMeta rm = loadRootMeta(path);
        if (rm.name.empty()) rm.name = norm(fs::path(root).filename().u8string());
        bool added = rm.broadcasts.insert(name).second;
        writeRootMeta(path, rm);
        r.created.push_back("meta.sbcli");
        r.notes.push_back("广播是全局的，已登记到根 meta（忽略 --sprite）：" + name);
        return r;
    }

    std::string metaPath = joinRel(dir, "meta.sbcli");
    CharMeta cm = loadCharMeta(metaPath);
    bool added = (kind == 0) ? cm.variables.insert(name).second
                             : cm.lists.insert(name).second;
    std::string spriteDisplay = cm.has("name") ? cm.get("name")
                                               : norm(fs::path(dir).filename().u8string());
    bool isStage = (norm(fs::path(dir).filename().u8string()) == "stage");
    writeCharMeta(metaPath, cm, spriteDisplay, isStage);
    r.created.push_back(relToRoot(root, metaPath));
    r.notes.push_back(added ? ("已登记到角色 meta：" + name)
                            : ("角色 meta 里已存在，未改动：" + name));
    (void)value;   // 角色 meta 的 variables/lists 只存名字（§3.2 用 {}），初值归根 meta
    return r;
}

} // namespace

ProjResult sbcliAddVariable(const std::string& project, const std::string& name,
                            const std::string& init, const std::string& scope) {
    return addNamed(project, name, init, scope, 0);
}
ProjResult sbcliAddList(const std::string& project, const std::string& name,
                        const std::string& items, const std::string& scope) {
    return addNamed(project, name, items, scope, 1);
}
ProjResult sbcliAddBroadcast(const std::string& project, const std::string& name) {
    return addNamed(project, name, std::string(), std::string(), 2);
}

// ---- sb project add-extension <项目> <扩展.js | URL> ----
// 1. 拿到源码：本地文件直读；URL 下载（http(s) 或 data:）
// 2. 用 node 工具解析 getInfo()（语法检查 + 拿到扩展 id/块定义）
// 3. 存为 extensions/<id>.js，并在 meta.sbcli 的 [extensions] 段登记
ProjResult sbcliAddExtension(const std::string& project, const std::string& src) {
    std::string root;
    ProjResult err;
    if (!requireRoot(project, root, err)) return err;
    if (src.empty()) return fail("缺少扩展文件或 URL");

    ProjResult r;
    r.ok = true;

    // ---- 1. 取源码 ----
    std::string code;
    std::string origin;       // 回显用：文件路径或 URL
    bool isUrl = src.find("://") != std::string::npos || src.rfind("data:", 0) == 0;
    if (isUrl) {
        origin = src;
        // 用 node 内联脚本下载（支持 http(s) / data: urlencoded / base64）
        std::string tmpJs = norm(fs::temp_directory_path().u8string()) + "/sbcli_ext_dl.js";
        std::string dl = "node -e \"const fs=require('fs');const u=process.argv[1];"
                         "(u.startsWith('data:')?Promise.resolve(u):new Promise((res,rej)=>{"
                         "const h=u.startsWith('https')?require('https'):require('http');"
                         "h.get(u,{headers:{'User-Agent':'sbcli'}},x=>{let d='';x.setEncoding('utf8');"
                         "x.on('data',c=>d+=c);x.on('end',()=>res(d));}).on('error',rej);"
                         "})).then(c=>{if(c.startsWith('data:')){const i=c.indexOf(',');const m=c.slice(5,i);"
                         "const b=c.slice(i+1);c=m.includes('base64')?Buffer.from(b,'base64').toString('utf8'):"
                         "decodeURIComponent(b);}fs.writeFileSync(process.argv[2],c);"
                         "}).catch(e=>{console.error(String(e));process.exit(1);})\" \""
                         + src + "\" \"" + tmpJs + "\"";
        if (std::system(dl.c_str()) != 0) return fail("下载扩展源码失败：" + src);
        std::ifstream in(fs::u8path(tmpJs), std::ios::binary);
        if (!in.good()) return fail("下载后读不到临时文件");
        std::stringstream ss; ss << in.rdbuf(); code = ss.str();
        std::error_code ec; fs::remove(fs::u8path(tmpJs), ec);
    } else {
        origin = src;
        std::ifstream in(fs::u8path(src), std::ios::binary);
        if (!in.good()) return fail("读不到扩展文件：" + src);
        std::stringstream ss; ss << in.rdbuf(); code = ss.str();
    }
    if (code.empty()) return fail("扩展源码为空");

    // ---- 2. 语法检查 + 解析 getInfo（写临时文件交给 node 工具）----
    std::string extId, extName;
    int blockCount = 0;
    {
        std::string tmpJs = norm(fs::temp_directory_path().u8string()) + "/sbcli_ext_parse.js";
        { std::ofstream out(fs::u8path(tmpJs), std::ios::binary); out << code; }
        std::string cmd = "node tools/fetch_tw_extension.js \"" + tmpJs + "\" 2>nul";
        FILE* p = popen(cmd.c_str(), "r");
        std::string outText;
        if (p) {
            char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), p)) > 0) outText.append(buf, n);
            pclose(p);
        }
        std::error_code ec; fs::remove(fs::u8path(tmpJs), ec);

        size_t mark = outText.rfind("___RESULT___");
        if (mark == std::string::npos)
            return fail("扩展语法检查失败：getInfo() 无法解析（可能依赖浏览器 API 或语法错误）");
        std::string js = outText.substr(mark + 12);
        // 极简提取 id / name / blocks 数（避免引入 JSON 依赖：用字符串查找）
        auto field = [&](const std::string& key) -> std::string {
            std::string pat = "\"" + key + "\":\"";
            size_t i = js.find(pat);
            if (i == std::string::npos) return {};
            i += pat.size();
            size_t j = js.find('"', i);
            return j == std::string::npos ? std::string() : js.substr(i, j - i);
        };
        if (js.find("\"error\":true") != std::string::npos)
            return fail("扩展语法检查失败：getInfo() 抛错（" + field("message") + "）");
        extId = field("id");
        extName = field("name");
        blockCount = (int)std::count(js.begin(), js.end(), '{');
        if (extId.empty()) return fail("扩展没有合法 id（getInfo().id 为空）");
    }

    // ---- 3. 落盘 + 登记 ----
    std::string extDir = joinRel(root, "extensions");
    if (!makeDirs(extDir)) return fail("无法创建 extensions/ 目录");
    std::string rel = "extensions/" + extId + ".js";
    std::string dst = joinRel(root, rel);
    {
        std::ofstream out(fs::u8path(dst), std::ios::binary);
        if (!out.good()) return fail("无法写入 " + rel);
        out << code;
    }
    r.created.push_back(rel);

    std::string metaPath = joinRel(root, "meta.sbcli");
    RootMeta rm = loadRootMeta(metaPath);
    if (rm.exists) {
        rm.extensions[extId] = rel;
    } else {
        rm.extensions[extId] = rel;
    }
    writeRootMeta(metaPath, rm);
    r.created.push_back("meta.sbcli（[extensions] 登记 " + extId + "）");

    r.notes.push_back("扩展：" + (extName.empty() ? extId : extName) + "（id=" + extId + "）");
    if (blockCount > 0) r.notes.push_back("已解析块定义，可用于语法检查");
    return r;
}

} // namespace sb
