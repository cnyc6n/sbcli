// src/sbcli_merge.hpp —— 跨作品操作（P1）
//
// 只在「项目目录」之间工作（先各自 unpack，再合并），
// 避免直接改 sb3 二进制带来的块 id / 变量 id 冲突处理复杂度。
#pragma once

#include <string>
#include <vector>

namespace sb {

struct MergeResult {
    bool        ok = true;
    std::string error;
    std::string code;                 // 稳定错误码（仅 ok=false 时有意义）：
                                      //   E_NO_PROJECT / E_NO_SPRITE / E_IO
    int         sprites = 0;          // 合并/导出的角色数
    int         renamed = 0;          // 因重名而改名的次数
    std::vector<std::string> notes;
};

// 把 src 项目里的角色合并进 dst 项目。
// spriteFilter 为空 = 全部非舞台角色；否则只合并名字匹配的角色。
// 重名时自动改名为「名字2」「名字3」…，并同步 block.sbcli 里的角色引用
//（sensing_of_object_menu / motion_goto_menu 等的 OBJECT 字段）。
MergeResult sbcMergeProject(const std::string& dstProject,
                            const std::string& srcProject,
                            const std::string& spriteFilter);

// 从 src 项目导出单个角色到独立项目目录（含它引用到的素材与私有变量）。
MergeResult sbcExportSprite(const std::string& srcProject,
                            const std::string& spriteName,
                            const std::string& outProject);

} // namespace sb
