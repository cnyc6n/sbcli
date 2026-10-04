// src/sbcli_check.hpp
// `sb check` 的检查引擎接口。
//
// 分工（与 src/sbcli_parser.hpp 的约定一致）：
//   解析器只做「文本 → 结构」，报结构类诊断（括号/缩进/else 匹配）；
//   本模块在其 AST 上做**语义**检查：opcode 是否收录、参数名与必填、
//   ARG 数量 vs proccode 占位符、重复定义、reporter 深度、以及对 meta 的引用。
#pragma once

#include <string>
#include <vector>

namespace sb {

enum class CheckLevel { Error, Warning };

struct CheckDiag {
    std::string file;                  // 项目相对路径
    int         line = 0;              // 1 起；非行级为 0
    int         col  = 0;              // 1 起；未定位为 0
    CheckLevel  level = CheckLevel::Error;
    std::string category;              // lexical / syntax / structure / reference
    std::string code;                  // 机器可读短码
    std::string message;
};

struct CheckReport {
    std::string              root;             // 项目根目录
    std::vector<std::string> files;            // 检查过的 block.sbcli（项目相对路径）
    std::vector<CheckDiag>   diags;
    int                      errors = 0;
    int                      warnings = 0;
    bool                     rootMetaFound = false;
    std::string              error;            // 目录本身不可用时的顶层错误
};

// 检查一个 sbcli 项目目录（也可以直接指向 character/{id} 或某个 block.sbcli）。
CheckReport sbcliCheck(const std::string& rootOrDir);

} // namespace sb
