// src/sbcli_unpack.hpp
// `sb unpack <作品.sb3> <输出目录>` —— 把 Scratch 3 作品拆成 sbcli 项目目录
// （`sb pack` 的逆操作）。契约见 docs/format.md。
#pragma once

#include <string>

namespace sb {

struct UnpackResult {
    bool        ok      = false;
    std::string error;                    // 失败原因（ok=false 时有意义）
    std::string outDir;                   // 归一化后的输出目录
    std::string projectName;              // 根 meta 的 name
    int         spriteCount = 0;          // 角色数（不含舞台）
    int         scriptCount = 0;          // block 数（顶层 + 嵌套）
    int         assetCount  = 0;          // 导出的素材文件数
    int         unknownBlocks = 0;        // 未知 opcode 块数
    std::string log;                      // 逐行提示（写入的文件、告警）
};

// 读 .sb3（或 .sb2 / .sprite3 —— 走 sb3LoadAny 统一的 targets 视图），
// 在 outDir 生成：
//   meta.sbcli                 项目级（name + 全局变量/列表/广播）
//   character/stage/...        舞台
//   character/{1,2,...}/...    角色
//   assets/<md5.ext>           造型/声音素材
UnpackResult sbcliUnpack(const std::string& sb3Path, const std::string& outDir,
                         bool overwrite = false);

} // namespace sb
