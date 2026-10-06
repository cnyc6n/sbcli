// src/sbcli_project.hpp
// 项目脚手架：sb project init / add-* 系列（docs/format.md §0 / §3）。
//
// 与 fix 的分工：
//   init / add-*  → **建结构**（目录、meta 骨架、素材拷贝、显式登记）
//   fix           → **补引用**（扫描 block.sbcli 里引用到但没声明的名字）
// 两者写的是同一批 meta 文件，格式规则一致（见 sbcli_fix.cpp 的实现说明）。
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sb {

struct ProjResult {
    bool        ok = true;
    std::string error;                  // 失败原因（ok=false 时有效）
    std::vector<std::string> created;   // 新建/更新的路径（项目相对，便于回显）
    std::vector<std::string> notes;     // 提示信息（如"已存在，跳过"）
};

// ---- sb project init <目录> ----
// 建 character/stage/ + assets/ + 根 meta.sbcli + stage 的 meta.sbcli。
// 已存在的项目不覆盖：只在缺什么补什么，并在 notes 里说明。
ProjResult sbcliProjectInit(const std::string& dir, const std::string& name = "");

// ---- sb add-sprite <项目> <名字> ----
// 在 character/ 下开下一个数字 id 目录，写角色 meta。
// 返回时 error 为空；新角色目录名通过 created 回显。
ProjResult sbcliAddSprite(const std::string& project, const std::string& spriteName);

// ---- sb add-costume <项目> <角色id或名> <素材文件> <造型名> ----
// 把素材拷进 assets/（同名覆盖），并在角色 meta 的 costumes 里登记。
ProjResult sbcliAddCostume(const std::string& project, const std::string& sprite,
                           const std::string& assetFile, const std::string& costumeName);

// ---- sb add-sound <项目> <角色> <素材文件> <声音名> ----
// 与 add-costume 同理，只是落在 sounds。
ProjResult sbcliAddSound(const std::string& project, const std::string& sprite,
                         const std::string& assetFile, const std::string& soundName);

// ---- sb add-variable / add-list / add-broadcast ----
// scope 为空 → 根 meta；否则是角色 id 或角色名 → 角色 meta。
// 广播只在根 meta（format.md §4.2：广播是全局的）。
ProjResult sbcliAddVariable (const std::string& project, const std::string& name,
                             const std::string& init, const std::string& scope = "");
ProjResult sbcliAddList     (const std::string& project, const std::string& name,
                             const std::string& items,   const std::string& scope = "");
ProjResult sbcliAddBroadcast(const std::string& project, const std::string& name);

// ---- sb project add-extension <项目> <扩展.js | URL> ----
// 从本地文件或 URL 取扩展源码 → getInfo() 语法检查 → 存 extensions/<id>.js
// 并在 meta.sbcli 的 [extensions] 段登记。
ProjResult sbcliAddExtension(const std::string& project, const std::string& src);

// ---- 扩展管理（task-7）----

// 一个已注册扩展的概要（list-extensions 用）
struct ExtEntry {
    std::string id;
    std::string name;       // getInfo().name（拿不到时用 id）
    int         blocks = 0; // 块数
    std::string source;     // meta 里登记的值：本地相对路径 或 URL
    bool        isUrl = false;
    bool        hasFile = false;   // extensions/<id>.js 是否真的存在
    bool        parsed = false;    // getInfo() 是否解析成功
};

// 一个扩展积木的明细（ext-info 用）
struct ExtBlockInfo {
    std::string opcode;          // 短 opcode（不含扩展 id 前缀）
    std::string fullOpcode;      // <extId>_<opcode>
    int         type = 0;        // 0=命令 1=报告 2=布尔 3=帽子
    std::string text;            // getInfo() 里的 text 模板
    std::vector<std::pair<std::string, std::string>> args;  // (参数名, 类型)
    std::map<std::string, std::string> menus;               // 参数名 → 菜单 id
};

// sb project list-extensions <项目>
// 列出 meta 的 [extensions] 登记 + extensions/*.js 解析出的名称与块数。
ProjResult sbcliListExtensions(const std::string& project,
                               std::vector<ExtEntry>& out);

// sb project remove-extension <项目> <id> [--keep-file]
// 从 [extensions] 摘掉登记；keepFile=false 时同时删除 extensions/<id>.js。
ProjResult sbcliRemoveExtension(const std::string& project, const std::string& id,
                                bool keepFile);

// sb project ext-info <项目> <id>
// 展开该扩展的全部积木：opcode / 类型 / 参数（含菜单）。
ProjResult sbcliExtInfo(const std::string& project, const std::string& id,
                        std::vector<ExtBlockInfo>& out);

// ---------------------------------------------------------------- 辅助

// 列出 character/ 下已有的角色目录（按 id 数字升序；stage 排最前）。
// 每项 = {目录名, meta 里的 name}
std::vector<std::pair<std::string, std::string>> sbcliListSprites(const std::string& project);

// 下一个可用的角色 id（已有 1、2 → 返回 3）。stage 不占数字 id。
int sbcliNextSpriteId(const std::string& project);

// 把"角色 id 或角色名"解析成 character 下的目录名；
// 找不到时 ok=false 并在 error 里给出可选列表，便于报错提示。
ProjResult sbcliResolveSprite(const std::string& project, const std::string& sprite,
                              std::string& outDir);

} // namespace sb
