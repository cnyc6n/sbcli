// src/sbcli_pack.hpp
#pragma once
#include "commands.hpp"

namespace sb {
// `sb pack <项目目录> <输出.sb3>` —— 把 sbcli 项目目录重新打包成 Scratch 3 (.sb3)。
// 详见 docs/format.md（§1.6/@script、§1.7 缩进、§2 opcode、§3 meta 格式）。
int cmd_pack(Args& a);
} // namespace sb
