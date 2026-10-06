// src/ext_loader.hpp —— 自动联网解析 sb3 扩展
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sb {
struct Sb3File;

struct ExtInfo {
    std::map<std::string, int> blockTypes;        // opcode → 类型（0..3）
    std::map<std::string, std::string> extNames;  // 扩展id → 名称
    // opcode → 允许的参数名集合（来自 getInfo().blocks[].arguments 的键）
    std::map<std::string, std::vector<std::string>> blockParams;
    // opcode → 菜单参数名 → 菜单 id（用于校验菜单值）
    std::map<std::string, std::map<std::string, std::string>> blockMenus;
    // 菜单 id → 该菜单允许的**值**集合（来自 getInfo().menus，每项取 value）
    // 只有在能静态取到菜单项时才填；动态菜单不填 → check 静默跳过。
    // 键带 extId 前缀（"Encoding::encode"），与 blockMenus 里存的裸 menuId
    // 通过 blockOwner 关联。
    std::map<std::string, std::set<std::string>> menuValues;
    // opcode → 所属扩展 id（用于把 blockMenus 的裸菜单 id 解析成 menuValues 的键）
    std::map<std::string, std::string> blockOwner;
    // meta.sbcli [extensions] 段登记的所有扩展 id（权威清单，无论本地/URL/加载成败）
    std::set<std::string> registeredIds;
    // 登记了但**没加载到定义**的扩展 id：
    //   本地相对路径但源码缺失；URL 下载失败/超时；getInfo() 解析失败。
    // 调用方（sb check）据此把这类扩展的积木从 unknown-opcode 错误降级为警告。
    std::set<std::string> failedIds;
    bool tried = false;                            // 是否尝试过（避免重复 spawn）
};

// 从 sb3 的 extensionURLs 自动解析扩展（node tools/fetch_tw_extension.js）。
// 返回 -1 = 无 extensionURLs；0 = 解析失败；N = 扩展数。
int loadExtensionsFromSb3(const Sb3File& sf, ExtInfo& out);

// 从项目目录解析扩展：
//   · extensions/*.js              —— unpack 导出的本地源码
//   · meta.sbcli 的 [extensions]    —— 权威登记：
//       值为相对路径 → 当本地源码（extensions/<id>.js 缺失 → failedIds）
//       值为 URL      → 联网下载解析，缓存到 <项目>/.sbcli-cache/<id>.js
//                      （网络失败/解析失败 → failedIds，绝不崩）
// 返回 -1 = 既无扩展目录也无 meta 登记；0 = 无可用源码；N = 成功解析的扩展数。
int loadExtensionsFromDir(const std::string& projectDir, ExtInfo& out);
}
