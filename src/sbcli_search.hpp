// src/sbcli_search.hpp
// `sb search <关键词>` —— block.sbcli 语法查找手册（format.md 的配套工具）。
//
// 输入中文概念（广播 / 变量 / 如果 / 移动 …）或英文 opcode（looks_say），
// 输出匹配的语法模板：opcode + 参数清单（字段名 + 类型）+ 一行示例。
//
// 数据来源：
//   · 中文概念 → opcode 的映射表（覆盖 format.md §2 高频块 + 扩展）
//   · SB3_T / SB2_T 模板表（src/sb3_tables.hpp）抽取参数占位符 {X}
//   · 参数类型用 FIELD_MAP + 命名启发式判定（menu / boolean / number / text / input）
#pragma once
#include <string>
#include <vector>
#include "common.hpp"

namespace sb {

struct Args;   // 前向声明（命令入口参数）

struct SearchParam {
    std::string name;   // SB3 字段名（STEPS / MESSAGE …）
    std::string type;   // text / number / boolean / menu / input
};

struct SearchMatch {
    std::string opcode;
    std::string category;     // 事件 / 动作 / 外观 / 数据 … / Scratch 2
    std::string templateText; // SB3_T/SB2_T 的中文翻译模板
    std::vector<SearchParam> params;
    std::string example;      // 一行示例块行
};

struct SearchResult {
    std::string keyword;
    std::vector<SearchMatch>  matches;
    std::vector<std::string>  suggestions;  // 无匹配时的相近建议
};

// 主查询：关键词可中文概念或英文 opcode 子串（不区分大小写）。
// extDir 非空时（项目目录），额外加载该项目的 extensions/*.js 扩展积木并入结果。
SearchResult sbSearch(const std::string& keyword, const std::string& extDir = {});

// 命令入口（commands.cpp → main.cpp 分发）。
int cmd_search(Args& a);

} // namespace sb
