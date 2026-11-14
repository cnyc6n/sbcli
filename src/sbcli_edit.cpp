// src/sbcli_edit.cpp —— 脚本级编辑 + 重命名实现
#include "sbcli_edit.hpp"
#include "sbcli_meta.hpp"
#include "sbcli_parser.hpp"

#include <algorithm>
#include <functional>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace sb {
namespace {
// 错误码约定（输出到 --json 的 "code" 字段）：
//   E_NO_PROJECT   项目根无效
//   E_NO_SPRITE    角色不存在
//   E_BAD_PATH     路径语法错 / 参数形态非法
//   E_OUT_OF_RANGE 序号越界
//   E_PARSE        积木文本解析失败
//   E_STRUCT       block.sbcli 结构错误
//   E_IO           读写失败
EditResult failWith(const std::string& code, const std::string& msg) {
    EditResult r;
    r.ok = false;
    r.error = msg;
    r.code = code;
    return r;
}

std::string low(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return s;
}

// 把「角色名 或 目录 id」解析成 character/<id> 目录；sprite 为空时取第一个非舞台角色。
// 找不到返回空串。
std::string resolveSpriteDir(const std::string& root, const std::string& sprite) {
    std::string chDir = root + "/character";
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(chDir), ec)) return {};

    struct Cand { std::string dir, name; bool isStage; };
    std::vector<Cand> cands;
    for (auto& e : fs::directory_iterator(fs::u8path(chDir), ec)) {
        if (!e.is_directory()) continue;
        std::string d = e.path().u8string();
        meta::CharMeta cm = meta::loadCharMeta(d + "/meta.sbcli");
        bool isStage = (low(fs::path(d).filename().u8string()) == "stage");
        cands.push_back({ d, cm.get("name"), isStage });
    }
    if (cands.empty()) return {};

    if (sprite.empty()) {
        // 默认：第一个非舞台角色
        for (auto& c : cands) if (!c.isStage) return c.dir;
        return cands.front().dir;
    }

    std::string want = low(sprite);
    // 先按目录 id 精确匹配（"1" / "stage"），再按名字（忽略大小写）
    for (auto& c : cands) {
        std::string id = fs::path(c.dir).filename().u8string();
        if (low(id) == want) return c.dir;
    }
    for (auto& c : cands) {
        if (low(c.name) == want) return c.dir;
    }
    return {};
}

// 读 → 解析 → 校验结构错误 → 编辑 → 写回
EditResult editBlockFile(const std::string& blockPath,
                         const std::function<EditResult(SbcFile&)>& mutate) {
    std::ifstream in(fs::u8path(blockPath), std::ios::binary);
    if (!in.good()) return failWith("E_IO", "读不到 " + blockPath);
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    std::string src = ss.str();

    SbcFile f = sbcParse(src);
    // 结构错误就不动（避免把坏文件写得更坏）
    for (const auto& d : f.diags)
        if (d.severity == SbcSeverity::Error)
            return failWith("E_STRUCT",
                            "block.sbcli 有结构错误（第 " + std::to_string(d.line) +
                            " 行：" + d.message + "），先 `sb check` 修好再编辑");

    EditResult r = mutate(f);
    if (!r.ok) return r;

    std::string out = sbcUnparseKeepComments(f, src);
    if (!out.empty() && out.back() != '\n') out += '\n';
    // 头部注释保留（sbcUnparse 不含它，但我们不想丢"由 sb unpack 生成"这类说明）
    if (src.rfind("#", 0) == 0) {
        size_t nl = src.find('\n');
        if (nl != std::string::npos) out = src.substr(0, nl + 1) + out;
    }

    std::ofstream ofs(fs::u8path(blockPath), std::ios::binary);
    if (!ofs.good()) return failWith("E_IO", "写不了 " + blockPath);
    ofs << out;
    return r;
}

// 解析一行积木文本成 SbcBlock（复用解析器：包一层临时脚本再取第 1 块）
bool parseOneBlock(const std::string& text, SbcBlock& out, std::string& err) {
    std::string probe = "@script flag\n" + text + "\n";
    SbcFile f = sbcParse(probe);
    for (const auto& d : f.diags)
        if (d.severity == SbcSeverity::Error) {
            err = "第 " + std::to_string(d.line) + " 行：" + d.message;
            return false;
        }
    if (f.scripts.empty() || f.scripts[0].blocks.empty()) {
        err = "解析不出积木（应以 opcode 开头）";
        return false;
    }
    out = f.scripts[0].blocks[0];
    return true;
}

} // namespace

// ---------------------------------------------------------------- P0 编辑

EditResult sbcEditInsert(const std::string& project, const std::string& sprite,
                         int scriptIdx, int afterBlock, const std::string& blockText) {
    std::string root = meta::findProjectRoot(project);
    if (root.empty()) return failWith("E_NO_PROJECT", "找不到项目根目录：" + project);
    std::string dir = resolveSpriteDir(root, sprite);
    if (dir.empty()) return failWith("E_NO_SPRITE", "找不到角色：" + (sprite.empty() ? "(默认)" : sprite));

    SbcBlock nb;
    std::string perr;
    if (!parseOneBlock(blockText, nb, perr)) return failWith("E_PARSE", "积木文本解析失败：" + perr);

    return editBlockFile(dir + "/block.sbcli", [&](SbcFile& f) -> EditResult {
        if (scriptIdx < 1 || (size_t)scriptIdx > f.scripts.size())
            return failWith("E_OUT_OF_RANGE", "脚本序号越界：只有 " + std::to_string(f.scripts.size()) + " 个脚本");
        auto& blocks = f.scripts[(size_t)scriptIdx - 1].blocks;
        if (afterBlock < 0 || (size_t)afterBlock > blocks.size())
            return failWith("E_OUT_OF_RANGE", "位置越界：该脚本有 " + std::to_string(blocks.size()) + " 个顶层块");
        blocks.insert(blocks.begin() + afterBlock, nb);
        EditResult r;
        r.changed = 1;
        r.notes.push_back("已在脚本 " + std::to_string(scriptIdx) + " 的第 " +
                          std::to_string(afterBlock) + " 块之后插入：" + blockText);
        return r;
    });
}

EditResult sbcEditDelete(const std::string& project, const std::string& sprite,
                         int scriptIdx, int blockIdx) {
    std::string root = meta::findProjectRoot(project);
    if (root.empty()) return failWith("E_NO_PROJECT", "找不到项目根目录：" + project);
    std::string dir = resolveSpriteDir(root, sprite);
    if (dir.empty()) return failWith("E_NO_SPRITE", "找不到角色：" + (sprite.empty() ? "(默认)" : sprite));

    return editBlockFile(dir + "/block.sbcli", [&](SbcFile& f) -> EditResult {
        if (scriptIdx < 1 || (size_t)scriptIdx > f.scripts.size())
            return failWith("E_OUT_OF_RANGE", "脚本序号越界：只有 " + std::to_string(f.scripts.size()) + " 个脚本");
        auto& blocks = f.scripts[(size_t)scriptIdx - 1].blocks;
        if (blockIdx < 1 || (size_t)blockIdx > blocks.size())
            return failWith("E_OUT_OF_RANGE", "块序号越界：该脚本有 " + std::to_string(blocks.size()) + " 个顶层块");
        std::string op = blocks[(size_t)blockIdx - 1].opcode;
        blocks.erase(blocks.begin() + (blockIdx - 1));
        EditResult r;
        r.changed = 1;
        r.notes.push_back("已删除脚本 " + std::to_string(scriptIdx) + " 的第 " +
                          std::to_string(blockIdx) + " 块（" + op + "）");
        return r;
    });
}

EditResult sbcEditReplace(const std::string& project, const std::string& sprite,
                          int scriptIdx, int blockIdx, const std::string& blockText) {
    std::string root = meta::findProjectRoot(project);
    if (root.empty()) return failWith("E_NO_PROJECT", "找不到项目根目录：" + project);
    std::string dir = resolveSpriteDir(root, sprite);
    if (dir.empty()) return failWith("E_NO_SPRITE", "找不到角色：" + (sprite.empty() ? "(默认)" : sprite));

    SbcBlock nb;
    std::string perr;
    if (!parseOneBlock(blockText, nb, perr)) return failWith("E_PARSE", "积木文本解析失败：" + perr);

    return editBlockFile(dir + "/block.sbcli", [&](SbcFile& f) -> EditResult {
        if (scriptIdx < 1 || (size_t)scriptIdx > f.scripts.size())
            return failWith("E_OUT_OF_RANGE", "脚本序号越界：只有 " + std::to_string(f.scripts.size()) + " 个脚本");
        auto& blocks = f.scripts[(size_t)scriptIdx - 1].blocks;
        if (blockIdx < 1 || (size_t)blockIdx > blocks.size())
            return failWith("E_OUT_OF_RANGE", "块序号越界：该脚本有 " + std::to_string(blocks.size()) + " 个顶层块");
        std::string oldOp = blocks[(size_t)blockIdx - 1].opcode;
        blocks[(size_t)blockIdx - 1] = nb;
        EditResult r;
        r.changed = 1;
        r.notes.push_back("已替换脚本 " + std::to_string(scriptIdx) + " 的第 " +
                          std::to_string(blockIdx) + " 块（" + oldOp + " → " + nb.opcode + "）");
        return r;
    });
}

EditResult sbcEditDropScript(const std::string& project, const std::string& sprite,
                             int scriptIdx) {
    std::string root = meta::findProjectRoot(project);
    if (root.empty()) return failWith("E_NO_PROJECT", "找不到项目根目录：" + project);
    std::string dir = resolveSpriteDir(root, sprite);
    if (dir.empty()) return failWith("E_NO_SPRITE", "找不到角色：" + (sprite.empty() ? "(默认)" : sprite));

    return editBlockFile(dir + "/block.sbcli", [&](SbcFile& f) -> EditResult {
        if (scriptIdx < 1 || (size_t)scriptIdx > f.scripts.size())
            return failWith("E_OUT_OF_RANGE", "脚本序号越界：只有 " + std::to_string(f.scripts.size()) + " 个脚本");
        std::string hat = f.scripts[(size_t)scriptIdx - 1].hat;
        f.scripts.erase(f.scripts.begin() + (scriptIdx - 1));
        EditResult r;
        r.changed = 1;
        r.notes.push_back("已删除脚本 " + std::to_string(scriptIdx) + "（@" + hat + "）");
        return r;
    });
}

EditResult sbcEditAddScript(const std::string& project, const std::string& sprite,
                            const std::string& hat, const std::string& bodyText) {
    std::string root = meta::findProjectRoot(project);
    if (root.empty()) return failWith("E_NO_PROJECT", "找不到项目根目录：" + project);
    std::string dir = resolveSpriteDir(root, sprite);
    if (dir.empty()) return failWith("E_NO_SPRITE", "找不到角色：" + (sprite.empty() ? "(默认)" : sprite));
    if (hat.empty()) return failWith("E_PARSE", "缺少 @script 类型（flag / broadcast 名 / key 名 …）");

    // 整段 body 作为临时文件解析
    std::string probe = "@script " + hat + "\n" + bodyText + "\n";
    SbcFile nf = sbcParse(probe);
    for (const auto& d : nf.diags)
        if (d.severity == SbcSeverity::Error)
            return failWith("E_PARSE", "脚本解析失败：第 " + std::to_string(d.line) + " 行：" + d.message);
    if (nf.scripts.empty()) return failWith("E_PARSE", "解析不出脚本");

    return editBlockFile(dir + "/block.sbcli", [&](SbcFile& f) -> EditResult {
        SbcScript s = nf.scripts[0];
        f.scripts.push_back(std::move(s));
        EditResult r;
        r.changed = 1;
        r.notes.push_back("已追加脚本 @" + hat + "（" +
                          std::to_string(f.scripts.back().blocks.size()) + " 个顶层块）");
        return r;
    });
}


// ---------------------------------------------------------------- P2 重命名

// 递归改写块里的参数值，返回改动次数。
static int rewriteParams(std::vector<SbcParam>& params, const std::string& key,
                         const std::string& from, const std::string& to) {
    int n = 0;
    for (auto& p : params) {
        if (p.canon == key || p.key == key) {
            if (p.value && p.value->kind == SbcValue::Kind::Scalar &&
                p.value->scalar.text == from) {
                p.value->scalar.text = to;
                p.value->scalar.quoted = sbcNeedsQuotes(to);
                ++n;
            }
        }
        if (p.value && p.value->kind == SbcValue::Kind::Reporter)
            n += rewriteParams(p.value->args, key, from, to);
    }
    return n;
}

static int rewriteBlocks(std::vector<SbcBlock>& blocks, const std::string& key,
                         const std::string& from, const std::string& to) {
    int n = 0;
    for (auto& b : blocks) {
        n += rewriteParams(b.params, key, from, to);
        for (auto& sub : b.substacks) n += rewriteBlocks(sub, key, from, to);
    }
    return n;
}

EditResult sbcRename(const std::string& project, const std::string& kind,
                     const std::string& oldName, const std::string& newName) {
    std::string root = meta::findProjectRoot(project);
    if (root.empty()) return failWith("E_NO_PROJECT", "找不到项目根目录：" + project);
    if (oldName.empty() || newName.empty()) return failWith("E_BAD_PATH", "旧名和新名都不能为空");
    if (oldName == newName) return failWith("E_BAD_PATH", "新旧名字相同，无需重命名");

    // 一个 kind 可能对应多个参数键（如造型既可能是 COSTUME 也可能是
    // looks_costume 桩块里的 COSTUME；声音是 SOUND_MENU / sounds_menu 的 SOUND_MENU）。
    std::vector<std::string> keys;
    if (kind == "variable")       keys = { "VARIABLE" };
    else if (kind == "list")      keys = { "LIST" };
    else if (kind == "broadcast") keys = { "BROADCAST_INPUT", "BROADCAST_OPTION" };
    else if (kind == "costume")   keys = { "COSTUME", "BACKDROP" };
    else if (kind == "sound")     keys = { "SOUND_MENU", "SOUND" };
    else if (kind == "sprite")    return failWith("E_BAD_PATH", "角色重命名请用 `sb project rename-sprite`（牵涉目录与素材）");
    else return failWith("E_BAD_PATH", "kind 支持 variable / list / broadcast / costume / sound");

    EditResult r;
    int total = 0;
    std::string chDir = root + "/character";
    std::error_code ec;
    if (fs::is_directory(fs::u8path(chDir), ec)) {
        for (auto& e : fs::directory_iterator(fs::u8path(chDir), ec)) {
            if (!e.is_directory()) continue;
            std::string bp = e.path().u8string() + "/block.sbcli";
            if (!fs::exists(fs::u8path(bp), ec)) continue;
            EditResult one = editBlockFile(bp, [&](SbcFile& f) -> EditResult {
                int n = 0;
                for (const auto& kk : keys)
                    for (auto& s : f.scripts) n += rewriteBlocks(s.blocks, kk, oldName, newName);
                if (kind == "broadcast") {
                    for (auto& s : f.scripts)
                        if (s.hat == "broadcast" && s.hatArg == oldName) { s.hatArg = newName; ++n; }
                }
                EditResult rr;
                rr.changed = n;
                return rr;
            });
            if (!one.ok) return one;
            total += one.changed;
        }
    }

    auto renameInMeta = [&](const std::string& metaPath) {
        if (!fs::exists(fs::u8path(metaPath), ec)) return;
        meta::RootMeta rm = meta::loadRootMeta(metaPath);
        bool touched = false;
        if (kind == "variable" && rm.variables.count(oldName)) {
            auto v = rm.variables[oldName];
            rm.variables.erase(oldName);
            rm.variables[newName] = v;
            touched = true;
        } else if (kind == "list" && rm.lists.count(oldName)) {
            auto v = rm.lists[oldName];
            rm.lists.erase(oldName);
            rm.lists[newName] = v;
            touched = true;
        } else if (kind == "broadcast" && rm.broadcasts.count(oldName)) {
            rm.broadcasts.erase(oldName);
            rm.broadcasts.insert(newName);
            touched = true;
        }
        // 造型/声音登记在**角色级** meta 的 costumes/sounds 表里（RootMeta 没这两张表），
        // 所以这里只处理根级；角色级由下面的 CharMeta 分支处理。
        if (touched) meta::writeRootMeta(metaPath, rm);
    };
    renameInMeta(root + "/meta.sbcli");
    // 角色级 meta：costume/sound 的登记在 CharMeta.costumes/sounds
    std::string projectRootForAsset = root;
    auto renameInCharMeta = [&](const std::string& metaPath, bool /*unused*/) {
        if (!fs::exists(fs::u8path(metaPath), ec)) return;
        if (kind != "costume" && kind != "sound") return;
        meta::CharMeta cm = meta::loadCharMeta(metaPath);
        auto& table = (kind == "costume") ? cm.costumes : cm.sounds;
        auto it = table.find(oldName);
        if (it == table.end()) return;
        std::string rel = it->second;
        table.erase(it);
        // 同步改 assets/ 下的文件名：<旧名>.<ext> → <新名>.<ext>
        // （否则文件名与登记名脱节，用户看 assets/ 会困惑）
        {
            std::error_code ec2;
            fs::path rp = fs::u8path(rel);
            std::string stem = rp.stem().u8string();
            if (stem == oldName) {
                fs::path np = rp.parent_path() / (newName + rp.extension().u8string());
                std::string npRel = meta::norm(np.u8string());
                std::string absOld = projectRootForAsset;
                absOld += "/" + rel;
                std::string absNew = projectRootForAsset + "/" + np.u8string();
                if (fs::exists(fs::u8path(absOld), ec2) &&
                    !fs::exists(fs::u8path(absNew), ec2)) {
                    fs::rename(fs::u8path(absOld), fs::u8path(absNew), ec2);
                    if (!ec2) rel = npRel;
                }
            }
        }
        table[newName] = rel;
        std::string disp = cm.has("name") ? cm.get("name")
                                          : meta::norm(fs::path(metaPath).parent_path().filename().u8string());
        bool isStage = (low(meta::norm(fs::path(metaPath).parent_path().filename().u8string())) == "stage");
        meta::writeCharMeta(metaPath, cm, disp, isStage);
    };
    if (fs::is_directory(fs::u8path(chDir), ec)) {
        for (auto& e : fs::directory_iterator(fs::u8path(chDir), ec)) {
            if (!e.is_directory()) continue;
            std::string mp = e.path().u8string() + "/meta.sbcli";
            renameInMeta(mp);
            renameInCharMeta(mp, false);
        }
    }

    r.changed = total;
    r.notes.push_back("已重命名 " + kind + "：「" + oldName + "」→「" + newName +
                      "」，改写 " + std::to_string(total) + " 处引用");
    if (total == 0 && (kind == "variable" || kind == "list" || kind == "broadcast"))
        r.notes.push_back("警告：没有找到任何引用，请确认名字拼写（可用 sb refs 查）");
    if (total == 0 && (kind == "costume" || kind == "sound"))
        r.notes.push_back("提示：脚本里没有引用这个素材（只改了登记名与文件名）");
    return r;
}

std::vector<ScriptInfo> sbcScriptInfosEx(const std::string& project,
                                         const std::string& sprite,
                                         std::string& err, std::string& code) {
    std::vector<ScriptInfo> out;
    err.clear();
    code.clear();

    std::string root = meta::findProjectRoot(project);
    if (root.empty()) {
        err = "找不到项目根目录：" + project;
        code = "E_NO_PROJECT";
        return out;
    }
    std::string dir = resolveSpriteDir(root, sprite);
    if (dir.empty()) {
        err = "找不到角色：" + (sprite.empty() ? "(默认)" : sprite);
        code = "E_NO_SPRITE";
        return out;
    }
    std::string bp = dir + "/block.sbcli";
    std::error_code ec;
    if (!fs::exists(fs::u8path(bp), ec)) {
        // 角色存在但没有 block.sbcli：视为"0 个脚本"，不是错误
        return out;
    }

    SbcFile f = sbcParseFile(bp);
    for (const auto& d : f.diags)
        if (d.severity == SbcSeverity::Error) {
            err = "block.sbcli 有结构错误（第 " + std::to_string(d.line) +
                  " 行：" + d.message + "），先 `sb check` 修好再看脚本清单";
            code = "E_STRUCT";
            return {};
        }

    out.reserve(f.scripts.size());
    for (size_t k = 0; k < f.scripts.size(); ++k) {
        const SbcScript& s = f.scripts[k];
        ScriptInfo si;
        si.index       = (int)k + 1;
        si.hat         = s.hat;
        si.hatArg      = s.hatArg;
        si.blockCount  = (int)s.blocks.size();
        si.firstOpcode = s.blocks.empty() ? std::string() : s.blocks[0].opcode;
        out.push_back(std::move(si));
    }
    return out;
}

std::vector<ScriptInfo> sbcScriptInfos(const std::string& project,
                                       const std::string& sprite) {
    std::string err, code;
    return sbcScriptInfosEx(project, sprite, err, code);
}

std::vector<std::string> sbcListScripts(const std::string& project,
                                        const std::string& sprite) {
    std::vector<std::string> out;
    for (const auto& si : sbcScriptInfos(project, sprite)) {
        std::string head = "@script " + si.hat;
        if (!si.hatArg.empty()) head += " " + si.hatArg;
        std::string first = si.firstOpcode.empty() ? "(空脚本)" : si.firstOpcode;
        out.push_back("  " + std::to_string(si.index) + ") " + head +
                      "   [" + std::to_string(si.blockCount) + " 块]  首块: " + first);
    }
    return out;
}


// ---------------------------------------------------------------- 子栈路径编辑

EditPath sbcParseEditPath(const std::string& spec) {
    EditPath p;
    if (spec.empty()) { p.error = "路径为空"; return p; }
    std::vector<int> nums;
    size_t i = 0;
    while (i < spec.size()) {
        while (i < spec.size() && (spec[i] == ' ')) ++i;
        size_t j = i;
        while (j < spec.size() && spec[j] != '.') ++j;
        std::string part = spec.substr(i, j - i);
        if (part.empty()) { p.error = "路径有空段：" + spec; return p; }
        try { nums.push_back(std::stoi(part)); }
        catch (...) { p.error = "路径段不是数字：" + part; return p; }
        i = (j < spec.size()) ? j + 1 : j;
    }
    if (nums.empty()) { p.error = "路径为空"; return p; }
    p.script = nums[0];
    // 后续按 (块, 子栈) 成对解析
    size_t k = 1;
    while (k + 1 < nums.size()) {
        p.steps.push_back({ nums[k], nums[k + 1] });
        k += 2;
    }
    // 末尾单个数字 = 最后一级的块序号，子栈用 0（约定：直接指定块时进入它的第 0 子栈）
    if (k < nums.size()) p.steps.push_back({ nums[k], 0 });
    p.ok = true;
    return p;
}

namespace {

// 按路径找到「叶子块」以及它所在的子栈（返回子栈指针）。
// 返回 null 表示路径中间层不存在。
std::vector<SbcBlock>* resolveSubstack(SbcFile& f, const EditPath& path,
                                       std::string& err) {
    if (path.script < 1 || (size_t)path.script > f.scripts.size()) {
        err = "脚本序号越界：只有 " + std::to_string(f.scripts.size()) + " 个脚本";
        return nullptr;
    }
    std::vector<SbcBlock>* cur = &f.scripts[(size_t)path.script - 1].blocks;
    for (size_t si = 0; si < path.steps.size(); ++si) {
        int blockIdx = path.steps[si].first;
        int subIdx   = path.steps[si].second;
        if (blockIdx < 1 || (size_t)blockIdx > cur->size()) {
            err = "第 " + std::to_string(si + 1) + " 层块序号越界（该层有 " +
                  std::to_string(cur->size()) + " 个块）";
            return nullptr;
        }
        SbcBlock& blk = (*cur)[(size_t)blockIdx - 1];
        if (subIdx < 0 || (size_t)subIdx >= blk.substacks.size()) {
            err = "块「" + blk.opcode + "」没有第 " + std::to_string(subIdx) +
                  " 个子栈（它有 " + std::to_string(blk.substacks.size()) + " 个）";
            return nullptr;
        }
        cur = &blk.substacks[(size_t)subIdx];
    }
    return cur;
}

} // namespace

EditResult sbcEditInsertAt(const std::string& project, const std::string& sprite,
                           const std::string& path, const std::string& blockText,
                           int afterBlock) {
    std::string root = meta::findProjectRoot(project);
    if (root.empty()) return failWith("E_NO_PROJECT", "找不到项目根目录：" + project);
    std::string dir = resolveSpriteDir(root, sprite);
    if (dir.empty()) return failWith("E_NO_SPRITE", "找不到角色：" + (sprite.empty() ? "(默认)" : sprite));

    EditPath ep = sbcParseEditPath(path);
    if (!ep.ok) return failWith("E_BAD_PATH", "路径无效：" + ep.error);

    SbcBlock nb;
    std::string perr;
    if (!parseOneBlock(blockText, nb, perr)) return failWith("E_PARSE", "积木文本解析失败：" + perr);

    return editBlockFile(dir + "/block.sbcli", [&](SbcFile& f) -> EditResult {
        std::string err;
        std::vector<SbcBlock>* sub = resolveSubstack(f, ep, err);
        if (!sub) return failWith("E_OUT_OF_RANGE", err);
        int pos = (afterBlock < 0) ? (int)sub->size() : afterBlock;
        if (pos < 0 || (size_t)pos > sub->size())
            return failWith("E_OUT_OF_RANGE", "插入位置越界：该子栈有 " + std::to_string(sub->size()) + " 个块");
        sub->insert(sub->begin() + pos, nb);
        EditResult r;
        r.changed = 1;
        r.notes.push_back("已在子栈 " + path + " 的第 " + std::to_string(pos) +
                          " 位置插入：" + blockText);
        return r;
    });
}

EditResult sbcEditDeleteAt(const std::string& project, const std::string& sprite,
                           const std::string& path) {
    std::string root = meta::findProjectRoot(project);
    if (root.empty()) return failWith("E_NO_PROJECT", "找不到项目根目录：" + project);
    std::string dir = resolveSpriteDir(root, sprite);
    if (dir.empty()) return failWith("E_NO_SPRITE", "找不到角色：" + (sprite.empty() ? "(默认)" : sprite));

    // 删除需要"父容器 + 索引"：把最后一段拆出来
    EditPath ep = sbcParseEditPath(path);
    if (!ep.ok) return failWith("E_BAD_PATH", "路径无效：" + ep.error);
    if (ep.steps.empty()) return failWith("E_BAD_PATH", "删除需要定位到具体块（如 1.2 或 1.2.0.3）");
    auto last = ep.steps.back();
    ep.steps.pop_back();

    return editBlockFile(dir + "/block.sbcli", [&](SbcFile& f) -> EditResult {
        std::string err;
        std::vector<SbcBlock>* container = nullptr;
        if (ep.steps.empty()) {
            if (ep.script < 1 || (size_t)ep.script > f.scripts.size())
                return failWith("E_OUT_OF_RANGE", "脚本序号越界");
            container = &f.scripts[(size_t)ep.script - 1].blocks;
        } else {
            container = resolveSubstack(f, ep, err);
            if (!container) return failWith("E_OUT_OF_RANGE", err);
        }
        int idx = last.first;
        if (idx < 1 || (size_t)idx > container->size())
            return failWith("E_OUT_OF_RANGE", "块序号越界：该层有 " + std::to_string(container->size()) + " 个块");
        std::string op = (*container)[(size_t)idx - 1].opcode;
        container->erase(container->begin() + (idx - 1));
        EditResult r;
        r.changed = 1;
        r.notes.push_back("已删除 " + path + " 处的块（" + op + "）");
        return r;
    });
}

EditResult sbcEditReplaceAt(const std::string& project, const std::string& sprite,
                            const std::string& path, const std::string& blockText) {
    std::string root = meta::findProjectRoot(project);
    if (root.empty()) return failWith("E_NO_PROJECT", "找不到项目根目录：" + project);
    std::string dir = resolveSpriteDir(root, sprite);
    if (dir.empty()) return failWith("E_NO_SPRITE", "找不到角色：" + (sprite.empty() ? "(默认)" : sprite));

    EditPath ep = sbcParseEditPath(path);
    if (!ep.ok) return failWith("E_BAD_PATH", "路径无效：" + ep.error);
    if (ep.steps.empty()) return failWith("E_BAD_PATH", "替换需要定位到具体块（如 1.2 或 1.2.0.3）");
    auto last = ep.steps.back();
    ep.steps.pop_back();

    SbcBlock nb;
    std::string perr;
    if (!parseOneBlock(blockText, nb, perr)) return failWith("E_PARSE", "积木文本解析失败：" + perr);

    return editBlockFile(dir + "/block.sbcli", [&](SbcFile& f) -> EditResult {
        std::string err;
        std::vector<SbcBlock>* container = nullptr;
        if (ep.steps.empty()) {
            if (ep.script < 1 || (size_t)ep.script > f.scripts.size())
                return failWith("E_OUT_OF_RANGE", "脚本序号越界");
            container = &f.scripts[(size_t)ep.script - 1].blocks;
        } else {
            container = resolveSubstack(f, ep, err);
            if (!container) return failWith("E_OUT_OF_RANGE", err);
        }
        int idx = last.first;
        if (idx < 1 || (size_t)idx > container->size())
            return failWith("E_OUT_OF_RANGE", "块序号越界：该层有 " + std::to_string(container->size()) + " 个块");
        std::string oldOp = (*container)[(size_t)idx - 1].opcode;
        (*container)[(size_t)idx - 1] = nb;
        EditResult r;
        r.changed = 1;
        r.notes.push_back("已替换 " + path + " 处的块（" + oldOp + " → " + nb.opcode + "）");
        return r;
    });
}

} // namespace sb
