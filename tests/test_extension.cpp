// tests/test_extension.cpp
//
// 扩展模块端到端测试（docs/format.md §8）：
//   1. 注册    —— sb project add-extension（本地文件）→ extensions/<id>.js + meta [extensions]
//   2. 解析    —— 扩展 getInfo 解析出正确块数（tests/sb3 样例 + 自建最小扩展）
//   3. check 通过 —— 正确参数的扩展积木脚本 → 0 错误
//   4. check 报错 —— 错误参数名 → unknown-param
//   5. round-trip —— unpack(sb3 内嵌 data: URL) → pack → extensionURLs 源码逐字节一致；
//                    本地 extensions/*.js → pack → data: URL 解码后与源文件一致
//   6. remove   —— sb project remove-extension：meta 与文件同步；残留文件不被 pack 采纳
//   7. URL 登记 —— meta [extensions] 只写 URL（data: 离线 + 坏 https 离线）：
//                  可加载 → check 识别积木；下载失败 → ext-unloaded 警告而非 unknown-opcode 错误；
//                  .sbcli-cache/<id>.js 缓存落盘、二次 check 命中缓存
//
// 设计（与 tests/test_pack_roundtrip.cpp 一致）：**API 级测试**，直接链接源码
// 而不是跑子进程。依赖：
//   · CWD 必须能解析 tools/fetch_tw_extension.js（main() 自动 chdir 到项目根）
//   · node 在 PATH 中（用于解析扩展 getInfo()）
//   · 测试全在临时目录跑（$TEMP，默认 F:/temp）
//
// 构建：通过 CMakeLists.txt 的 if(EXISTS tests/test_extension.cpp) 接入，
//       目标名 sb_extension_test。

#include "sbcli_pack.hpp"
#include "sbcli_unpack.hpp"
#include "sbcli_check.hpp"
#include "sbcli_project.hpp"
#include "ext_loader.hpp"
#include "sbcli_parser.hpp"
#include "sbcli_meta.hpp"
#include "sb3.hpp"
#include "zip.hpp"
#include "jdoc.hpp"
#include "common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace cm = sb::meta;

// ===========================================================================
// 极简测试框架（与 test_pack_roundtrip.cpp 同款）
// ===========================================================================

static int g_pass = 0;
static int g_fail = 0;
static std::string g_curCase;

#define CHECK(cond) do {                                               \
    if (!(cond)) {                                                     \
        ++g_fail;                                                      \
        std::fprintf(stderr, "  ✗ [%s] %s:%d  CHECK 失败: %s\n",       \
                     g_curCase.c_str(), __FILE__, __LINE__, #cond);    \
    }                                                                  \
} while (0)

#define CHECK_MSG(cond, msg) do {                                      \
    if (!(cond)) {                                                     \
        ++g_fail;                                                      \
        std::fprintf(stderr, "  ✗ [%s] %s:%d  %s\n",                    \
                     g_curCase.c_str(), __FILE__, __LINE__, (msg));    \
    }                                                                  \
} while (0)

static void beginCase(const std::string& name) {
    g_curCase = name;
    std::printf("• %s\n", name.c_str());
}
static void endCase() { ++g_pass; }

// ===========================================================================
// 路径 / 临时目录 / 文件工具
// ===========================================================================

// 项目根（node 工具的 tools/ 从这里解析；可用环境变量 SBCLI_ROOT 覆盖）
static const char* kProjRoot = "D:/scratch_tool";
static std::string projRoot() {
    const char* r = std::getenv("SBCLI_ROOT");
    return (r && *r) ? std::string(r) : std::string(kProjRoot);
}

static std::string makeTempRoot() {
    std::string base = "F:/temp";
    const char* t = std::getenv("TEMP");
    if (t && *t) base = t;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\'))
        base.pop_back();
    std::error_code ec;
    fs::create_directories(fs::u8path(base), ec);
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
#ifdef _WIN32
    long pidVal = _getpid();
#else
    long pidVal = (long)::getpid();
#endif
    std::string dir = base + "/exttest_" + std::to_string(now) + "_" +
                      std::to_string(pidVal);
    fs::create_directories(fs::u8path(dir), ec);
    return dir;
}
static void rmTree(const std::string& p) {
    std::error_code ec;
    fs::remove_all(fs::u8path(p), ec);
}

static void writeFile(const std::string& path, const std::string& content) {
    fs::path fp = fs::u8path(path);
    std::error_code ec;
    if (fp.has_parent_path()) fs::create_directories(fp.parent_path(), ec);
    std::ofstream f(fp, std::ios::binary | std::ios::trunc);
    f << content;
}
static std::string readFile(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static bool fileExists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(p), ec);
}

// ===========================================================================
// 读回 .sb3 的小工具
// ===========================================================================

static sb::Sb3File loadSb3(const std::string& path) {
    return sb::sb3LoadAny(path);
}

// 顶层 extensions 数组
static std::vector<std::string> sb3Extensions(const std::string& sb3Path) {
    std::vector<std::string> out;
    auto f = loadSb3(sb3Path);
    sb::Elem top(f.data);
    sb::Elem exts = top.at("extensions");
    if (exts.is_array())
        for (auto e : exts.arr()) {
            sb::Elem x(e);
            if (x.is_string()) out.push_back(std::string(x.sv()));
        }
    return out;
}

// extensionURLs[id]（无 → 空串）
static std::string sb3ExtUrl(const std::string& sb3Path, const std::string& id) {
    auto f = loadSb3(sb3Path);
    sb::Elem top(f.data);
    sb::Elem urls = top.at("extensionURLs");
    if (!urls.is_object()) return {};
    sb::Elem v = urls.at(id);
    return v.is_string() ? std::string(v.sv()) : std::string();
}

// meta.platform.name（无 → 空串）
static std::string sb3PlatformName(const std::string& sb3Path) {
    auto f = loadSb3(sb3Path);
    sb::Elem top(f.data);
    sb::Elem metaEl = top.at("meta");
    if (!metaEl.is_object()) return {};
    sb::Elem plat = metaEl.at("platform");
    if (!plat.is_object()) return {};
    sb::Elem n = plat.at("name");
    return n.is_string() ? std::string(n.sv()) : std::string();
}

// ===========================================================================
// 测试素材
// ===========================================================================

// 自建最小扩展（本地 .js，add-extension 直接用它）
static const char* kMiniExt = R"JS(class MiniExt {
  getInfo() {
    return {
      id: 'mini',
      name: 'Mini Ext',
      blocks: [
        { opcode: 'ping', blockType: Scratch.BlockType.REPORTER, text: 'ping [msg]', arguments: { msg: { type: Scratch.ArgumentType.STRING } } },
        { opcode: 'echo', blockType: Scratch.BlockType.COMMAND, text: 'echo [msg]', arguments: { msg: { type: Scratch.ArgumentType.STRING } } },
      ],
    };
  }
}
Scratch.extensions.register(new MiniExt());)JS";

// 仓库 fixtures
static const std::string kFixtureCutem = "D:/scratch_tool/tests/sb3/cutem color.sb3";
static const std::string kFixtureEncode = "D:/scratch_tool/tests/sb3/encode.sb3";

// ===========================================================================
// 用例 1：注册 —— add-extension（本地文件）
// ===========================================================================

static void testRegister(const std::string& tmp) {
    beginCase("注册：sbcliAddExtension(本地 js) → extensions/<id>.js + meta [extensions]");
    std::string proj = tmp + "/reg";
    auto init = sb::sbcliProjectInit(proj, "注册测试");
    CHECK_MSG(init.ok, "sb project init 应成功");

    // 写一个本地扩展源码（自建最小扩展）
    std::string srcJs = tmp + "/mini_ext.js";
    writeFile(srcJs, kMiniExt);

    auto r = sb::sbcliAddExtension(proj, srcJs);
    CHECK_MSG(r.ok, "add-extension 应成功");

    // extensions/<id>.js 落盘（id 来自 getInfo().id）
    std::string dst = proj + "/extensions/mini.js";
    CHECK_MSG(fileExists(dst), "extensions/mini.js 应存在");
    CHECK_MSG(readFile(dst) == kMiniExt, "落盘的源码应与本地文件逐字节一致");

    // meta [extensions] 登记 id = 相对路径
    cm::RootMeta rm = cm::loadRootMeta(proj + "/meta.sbcli");
    CHECK_MSG(rm.extensions.count("mini") && rm.extensions["mini"] == "extensions/mini.js",
              "meta [extensions] 应登记 mini = extensions/mini.js");

    // 幂等/重复登记：再次 add 同一文件，不应报错
    auto r2 = sb::sbcliAddExtension(proj, srcJs);
    CHECK_MSG(r2.ok, "重复 add-extension 不应报错");
    endCase();
}

// ===========================================================================
// 用例 2：解析 —— getInfo 解析出正确块数（fixtures + 自建扩展）
// ===========================================================================

static void testParseBlockCounts(const std::string& tmp) {
    beginCase("解析：getInfo 块数（mini=2 / yc6ncolormaster=13 / Encoding=URL 不落盘）");

    // 2a. 自建扩展（add-extension 后的项目）
    std::string projMini = tmp + "/parse_mini";
    CHECK(sb::sbcliProjectInit(projMini, "解析mini").ok);
    std::string srcJs = tmp + "/mini_ext2.js";
    writeFile(srcJs, kMiniExt);
    CHECK(sb::sbcliAddExtension(projMini, srcJs).ok);
    {
        sb::ExtInfo ext;
        int n = sb::loadExtensionsFromDir(projMini, ext);
        CHECK_MSG(n > 0, "应成功解析本地扩展");
        CHECK_MSG(ext.blockTypes.size() == 2,
                  "mini 扩展应有 2 个积木（blockTypes 2 条）");
        CHECK_MSG(ext.blockTypes.count("mini_ping") && ext.blockTypes.count("mini_echo"),
                  "应包含 mini_ping / mini_echo");
        CHECK_MSG(ext.blockParams["mini_ping"].size() == 1 &&
                  ext.blockParams["mini_ping"][0] == "msg",
                  "mini_ping 的参数表应含 msg");
        CHECK_MSG(ext.extNames["mini"] == "Mini Ext", "应解析出扩展名");
    }

    // 2b. cutem color.sb3（内嵌 yc6ncolormaster）：unpack 落盘源码 → 解析出 13 块
    std::string projCutem = tmp + "/parse_cutem";
    auto ur = sb::sbcliUnpack(kFixtureCutem, projCutem, true);
    CHECK_MSG(ur.ok, "unpack cutem color.sb3 应成功");
    CHECK_MSG(fileExists(projCutem + "/extensions/yc6ncolormaster.js"),
              "unpack 应把 data: 内嵌源码落盘为 extensions/yc6ncolormaster.js");
    CHECK_MSG(cm::loadRootMeta(projCutem + "/meta.sbcli").extensions["yc6ncolormaster"] ==
                  "extensions/yc6ncolormaster.js",
              "meta [extensions] 应登记相对路径");
    {
        sb::ExtInfo ext;
        int n = sb::loadExtensionsFromDir(projCutem, ext);
        CHECK_MSG(n > 0, "应成功解析 yc6ncolormaster");
        CHECK_MSG(ext.blockTypes.size() == 13,
                  "yc6ncolormaster 应有 13 个积木");
        CHECK_MSG(ext.blockTypes.count("yc6ncolormaster_adjustBrightness") &&
                  ext.blockTypes.count("yc6ncolormaster_blendMultipleColors"),
                  "应包含 adjustBrightness / blendMultipleColors");
        auto it = ext.blockParams.find("yc6ncolormaster_adjustBrightness");
        CHECK_MSG(it != ext.blockParams.end() &&
                  std::find(it->second.begin(), it->second.end(), "COLOR") != it->second.end() &&
                  std::find(it->second.begin(), it->second.end(), "PERCENT") != it->second.end(),
                  "adjustBrightness 的参数表应含 COLOR / PERCENT");
    }

    // 2c. encode.sb3（URL 引用 Encoding）：unpack 后 meta 存 URL，不落盘源码
    std::string projEnc = tmp + "/parse_enc";
    auto ur2 = sb::sbcliUnpack(kFixtureEncode, projEnc, true);
    CHECK_MSG(ur2.ok, "unpack encode.sb3 应成功");
    CHECK_MSG(cm::loadRootMeta(projEnc + "/meta.sbcli").extensions["Encoding"] ==
                  "https://extensions.turbowarp.org/encoding.js",
              "URL 扩展应原样登记 URL");
    std::error_code ec;
    CHECK_MSG(!fs::exists(fs::u8path(projEnc + "/extensions"), ec),
              "URL 扩展没有本地源码，不应生成 extensions/ 目录");
    endCase();
}

// ===========================================================================
// 用例 3：check 通过 —— 正确参数的扩展积木脚本 → 0 错误
// ===========================================================================

static void testCheckPass(const std::string& tmp) {
    beginCase("check 通过：正确参数（msg）的扩展积木脚本 → 0 错误");
    std::string proj = tmp + "/check_ok";
    CHECK(sb::sbcliProjectInit(proj, "checkok").ok);
    std::string srcJs = tmp + "/mini_ext3.js";
    writeFile(srcJs, kMiniExt);
    CHECK(sb::sbcliAddExtension(proj, srcJs).ok);
    CHECK(sb::sbcliAddSprite(proj, "玩家").ok);

    // 脚本：扩展 reporter 嵌进原生块
    writeFile(proj + "/character/1/block.sbcli", R"(
@script flag
event_whenflagclicked
looks_sayforsecs MESSAGE=(mini_ping msg="你好") SECS=1
looks_sayforsecs MESSAGE=(mini_echo msg="echo") SECS=2
)");

    sb::CheckReport rep = sb::sbcliCheck(proj);
    CHECK_MSG(rep.errors == 0, "扩展积木 + 正确参数应 0 错误");
    CHECK_MSG(rep.warnings == 0, "应无警告");
    for (const auto& d : rep.diags)
        CHECK_MSG(d.code != "unknown-opcode" && d.code != "unknown-param",
                  (std::string("不应报 opcode/参数类错误，实际：") + d.code).c_str());
    endCase();
}

// ===========================================================================
// 用例 4：check 报错 —— 错误参数名 → unknown-param
// ===========================================================================

static void testCheckError(const std::string& tmp) {
    beginCase("check 报错：错误参数名（WRONG）→ unknown-param");
    std::string proj = tmp + "/check_bad";
    CHECK(sb::sbcliProjectInit(proj, "checkbad").ok);
    std::string srcJs = tmp + "/mini_ext4.js";
    writeFile(srcJs, kMiniExt);
    CHECK(sb::sbcliAddExtension(proj, srcJs).ok);
    CHECK(sb::sbcliAddSprite(proj, "玩家").ok);

    // 参数名写错（WRONG 不是 getInfo 里的参数）
    writeFile(proj + "/character/1/block.sbcli", R"(
@script flag
event_whenflagclicked
looks_sayforsecs MESSAGE=(mini_ping msg="你好" WRONG=1) SECS=1
)");

    sb::CheckReport rep = sb::sbcliCheck(proj);
    CHECK_MSG(rep.errors >= 1, "错误参数名应报错");
    bool hasUnknownParam = false;
    for (const auto& d : rep.diags)
        if (d.code == "unknown-param" && d.message.find("WRONG") != std::string::npos)
            hasUnknownParam = true;
    CHECK_MSG(hasUnknownParam, "应存在针对 WRONG 的 unknown-param 诊断");

    // 参数名大小写敏感：getInfo 定义的是小写 msg，写大写 MSG 应报错
    writeFile(proj + "/character/1/block.sbcli", R"(
@script flag
event_whenflagclicked
looks_sayforsecs MESSAGE=(mini_ping MSG="hi") SECS=1
)");
    sb::CheckReport rep2 = sb::sbcliCheck(proj);
    CHECK_MSG(rep2.errors >= 1, "扩展参数名大小写敏感（MSG ≠ msg）应报错");
    endCase();
}

// ===========================================================================
// 用例 5：round-trip —— unpack → pack → extensionURLs 源码逐字节一致
// ===========================================================================

static void testRoundTrip(const std::string& tmp) {
    beginCase("round-trip：内嵌 data: URL ↔ extensions/*.js ↔ 重打包 data: URL");

    // 5a. 真实 fixture：unpack(cutem color.sb3) → pack → extensionURLs 与原始逐字节一致
    std::string unp = tmp + "/rt_cutem";
    auto ur = sb::sbcliUnpack(kFixtureCutem, unp, true);
    CHECK_MSG(ur.ok, "unpack 应成功");
    CHECK_MSG(ur.projectName == "cutem color", "项目名应保留");

    std::string out1 = tmp + "/rt_cutem_out.sb3";
    {
        sb::Args a;
        a.cmd = "pack";
        a.file = unp;
        a.extra.push_back(out1);
        CHECK_MSG(sb::cmd_pack(a) == 0, "pack 应成功");
    }
    // extensions 数组
    auto exts = sb3Extensions(out1);
    CHECK_MSG(exts.size() == 1 && exts[0] == "yc6ncolormaster",
              "pack 后 extensions 应列出 yc6ncolormaster");
    // extensionURLs：与原始 data: URL 逐字节一致
    std::string origUrl = sb3ExtUrl(kFixtureCutem, "yc6ncolormaster");
    std::string repackUrl = sb3ExtUrl(out1, "yc6ncolormaster");
    CHECK_MSG(!origUrl.empty(), "fixture 应含内嵌 data: URL");
    CHECK_MSG(origUrl == repackUrl, "round-trip 后 data URL 应逐字节一致");
    // 解码后与 unpack 落盘的源码一致（剥掉 unpack 的每行 \n 造成的末尾换行）
    std::string decoded = cm::decodeDataUrl(repackUrl);
    std::string fileSrc = readFile(unp + "/extensions/yc6ncolormaster.js");
    while (!fileSrc.empty() && fileSrc.back() == '\n') fileSrc.pop_back();
    CHECK_MSG(!decoded.empty() && decoded == fileSrc,
              "data URL 解码后应与 extensions/*.js 源码一致");
    // meta.platform 保留
    CHECK_MSG(sb3PlatformName(out1) == "TurboWarp", "meta.platform.name 应保留为 TurboWarp");

    // 5b. 自建项目：本地 extensions/mini.js → pack → data: URL 解码后与源文件一致
    std::string proj = tmp + "/rt_mini";
    CHECK(sb::sbcliProjectInit(proj, "rtmini").ok);
    std::string srcJs = tmp + "/mini_ext5.js";
    writeFile(srcJs, kMiniExt);
    CHECK(sb::sbcliAddExtension(proj, srcJs).ok);
    CHECK(sb::sbcliAddSprite(proj, "玩家").ok);
    writeFile(proj + "/character/1/block.sbcli", R"(
@script flag
event_whenflagclicked
looks_sayforsecs MESSAGE=(mini_ping msg="hi") SECS=1
)");
    std::string out2 = tmp + "/rt_mini_out.sb3";
    {
        sb::Args a;
        a.cmd = "pack";
        a.file = proj;
        a.extra.push_back(out2);
        CHECK_MSG(sb::cmd_pack(a) == 0, "pack 应成功");
    }
    std::string u2 = sb3ExtUrl(out2, "mini");
    CHECK_MSG(!u2.empty() && u2.rfind("data:application/javascript,", 0) == 0,
              "本地扩展应编码为 data:application/javascript,<urlencoded>");
    CHECK_MSG(cm::decodeDataUrl(u2) == kMiniExt,
              "重打包的 data URL 解码后应与本地源码逐字节一致");
    endCase();
}

// ===========================================================================
// 用例 6：remove —— 移除后 meta 与文件同步；残留文件不被 pack 采纳
// ===========================================================================

static void testRemove(const std::string& tmp) {
    beginCase("remove：删 meta 条目 + 删文件 → 打包不再输出该扩展");
    std::string proj = tmp + "/remove";
    CHECK(sb::sbcliProjectInit(proj, "rmtest").ok);
    std::string srcJs = tmp + "/mini_ext6.js";
    writeFile(srcJs, kMiniExt);
    CHECK(sb::sbcliAddExtension(proj, srcJs).ok);
    CHECK(sb::sbcliAddSprite(proj, "玩家").ok);
    // 脚本只用原生块，不用扩展块（移除扩展后 check/pack 均不应报未知 opcode）
    writeFile(proj + "/character/1/block.sbcli", R"(
@script flag
event_whenflagclicked
looks_sayforsecs MESSAGE="你好" SECS=1
)");

    std::string metaPath = proj + "/meta.sbcli";
    std::string jsPath = proj + "/extensions/mini.js";
    CHECK_MSG(fileExists(jsPath), "移除前 extensions/mini.js 应存在");
    CHECK_MSG(cm::loadRootMeta(metaPath).extensions.count("mini"), "移除前 meta 应有 mini 条目");

    // 移除：sb project remove-extension（不带 --keep-file → 同时删 meta 条目 + 源码文件）
    auto rr = sb::sbcliRemoveExtension(proj, "mini", false);
    CHECK_MSG(rr.ok, "remove-extension 应成功");
    CHECK_MSG(!fileExists(jsPath), "extensions/mini.js 应已删除");
    CHECK_MSG(cm::loadRootMeta(metaPath).extensions.empty(),
              "meta [extensions] 应已清空（与文件同步）");

    // 打包：不应再输出该扩展
    std::string out1 = tmp + "/remove_out1.sb3";
    {
        sb::Args a;
        a.cmd = "pack";
        a.file = proj;
        a.extra.push_back(out1);
        CHECK_MSG(sb::cmd_pack(a) == 0, "pack 应成功");
    }
    CHECK_MSG(sb3Extensions(out1).empty(), "移除后打包不应再有 extensions 数组");
    CHECK_MSG(sb3ExtUrl(out1, "mini").empty(), "移除后打包不应再有 extensionURLs");

    // 残留文件场景：文件还在但 meta 没有条目 → pack 不应采纳（meta 是唯一依据）
    writeFile(jsPath, kMiniExt);
    std::string out2 = tmp + "/remove_out2.sb3";
    {
        sb::Args a;
        a.cmd = "pack";
        a.file = proj;
        a.extra.push_back(out2);
        CHECK_MSG(sb::cmd_pack(a) == 0, "pack 应成功");
    }
    CHECK_MSG(sb3Extensions(out2).empty() && sb3ExtUrl(out2, "mini").empty(),
              "meta 未登记的残留文件不应被打包（meta 与文件需同步）");
    endCase();
}

// ===========================================================================
// 用例 7：URL 登记 —— meta [extensions] 只写 URL（task-11 修复点）
//   · data: URL（离线）→ 加载成功，check 识别积木，缓存落盘 .sbcli-cache/<id>.js
//   · 坏 https URL（离线）→ 不崩，ext-unloaded 警告而非 unknown-opcode 错误
// ===========================================================================

static void testUrlRegistration(const std::string& tmp) {
    beginCase("URL 登记：data: 加载成功 + 坏 URL 降级警告 + 缓存命中");
    std::string proj = tmp + "/urlonly";
    CHECK(sb::sbcliProjectInit(proj, "urlonly").ok);
    CHECK(sb::sbcliAddSprite(proj, "玩家").ok);

    // 手工写 meta [extensions]：mini 用 data: URL（离线可加载），bogus 用坏 https（离线必失败）
    {
        std::string meta = "name: urlonly\n"
                           "\n"
                           "[extensions]\n"
                           "mini = data:application/javascript," +
                           cm::encodeDataUrlBody(kMiniExt) +
                           "\n"
                           "bogus = https://127.0.0.1:1/nope.js\n"
                           "\n";
        writeFile(proj + "/meta.sbcli", meta);
    }
    CHECK_MSG(cm::loadRootMeta(proj + "/meta.sbcli").extensions.count("mini") &&
              cm::loadRootMeta(proj + "/meta.sbcli").extensions.count("bogus"),
              "meta [extensions] 应读到两条登记");

    writeFile(proj + "/character/1/block.sbcli", R"(
@script flag
event_whenflagclicked
looks_sayforsecs MESSAGE=(mini_ping msg="你好") SECS=1
looks_sayforsecs MESSAGE=(bogus_foo) SECS=1
)");

    // 第一次 check：mini 联网（data: 解码）加载成功；bogus 失败 → 警告而非错误
    sb::CheckReport rep1 = sb::sbcliCheck(proj);
    CHECK_MSG(rep1.errors == 0,
              "URL 登记项目不应有 unknown-opcode 错误（坏 URL 降级为警告）");
    bool warnUnloaded = false;
    for (const auto& d : rep1.diags)
        if (d.code == "ext-unloaded" && d.message.find("bogus") != std::string::npos)
            warnUnloaded = true;
    CHECK_MSG(warnUnloaded, "坏 URL 扩展应有 ext-unloaded 警告");
    for (const auto& d : rep1.diags)
        CHECK_MSG(d.code != "unknown-opcode",
                  (std::string("不应报 unknown-opcode 错误，实际：") + d.message).c_str());

    // 缓存：mini 落盘 .sbcli-cache/mini.js（内容 = 解码后的源码）；bogus 失败不落缓存
    CHECK_MSG(fileExists(proj + "/.sbcli-cache/mini.js"),
              "data: URL 应缓存到 .sbcli-cache/mini.js");
    CHECK_MSG(readFile(proj + "/.sbcli-cache/mini.js") == kMiniExt,
              "缓存内容应为解码后的源码");
    CHECK_MSG(!fileExists(proj + "/.sbcli-cache/bogus.js"),
              "下载失败的扩展不应写缓存");

    // 第二次 check：mini 命中缓存（不再联网），结果一致
    sb::CheckReport rep2 = sb::sbcliCheck(proj);
    CHECK_MSG(rep2.errors == 0, "缓存命中后 check 仍应 0 错误");

    // 证明确实加载到了定义：给 mini_ping 传错误参数 → 应报 unknown-param
    writeFile(proj + "/character/1/block.sbcli", R"(
@script flag
event_whenflagclicked
looks_sayforsecs MESSAGE=(mini_ping msg="你好" WRONG=1) SECS=1
)");
    sb::CheckReport rep3 = sb::sbcliCheck(proj);
    bool hasUnknownParam = false;
    for (const auto& d : rep3.diags)
        if (d.code == "unknown-param" && d.message.find("WRONG") != std::string::npos)
            hasUnknownParam = true;
    CHECK_MSG(hasUnknownParam,
              "URL 加载成功后，错误参数名应仍报 unknown-param（证明定义真的加载了）");
    endCase();
}

// ===========================================================================
// main
// ===========================================================================

int main() {
    // CWD 切到项目根：ext_loader / add-extension 用相对路径
    // 调 node tools/fetch_tw_extension.js（tools/ 相对 CWD 解析）。
    std::string root = projRoot();
    std::error_code ec;
    fs::current_path(fs::u8path(root), ec);
    if (ec) {
        std::fprintf(stderr, "❌ 无法 chdir 到项目根 %s（node 工具解析依赖 CWD）\n",
                     root.c_str());
        return 1;
    }
    if (!fileExists("tools/fetch_tw_extension.js")) {
        std::fprintf(stderr, "❌ %s/tools/fetch_tw_extension.js 不存在\n", root.c_str());
        return 1;
    }

    std::string tmp = makeTempRoot();
    std::printf("临时根：%s\n（CWD=%s，node 工具解析依赖它）\n", tmp.c_str(), root.c_str());

    testRegister(tmp);
    testParseBlockCounts(tmp);
    testCheckPass(tmp);
    testCheckError(tmp);
    testRoundTrip(tmp);
    testRemove(tmp);
    testUrlRegistration(tmp);

    rmTree(tmp);

    std::printf("\n==== 测试结果 ====\n");
    std::printf("通过用例(组): %d\n", g_pass);
    std::printf("失败断言(CHECK 失败次数): %d\n", g_fail);
    if (g_fail == 0) {
        std::printf("✅ 全部通过\n");
        return 0;
    }
    std::printf("❌ 有 %d 处断言失败\n", g_fail);
    return 1;
}
