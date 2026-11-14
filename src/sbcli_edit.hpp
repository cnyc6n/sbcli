// src/sbcli_edit.hpp —— 脚本级编辑 + 重命名（P0 / P2）
//
// 设计：所有编辑都走「解析 → 改 AST → sbcUnparse 写回」，
// 不直接操作文本行，避免缩进/引号/转义出错。
//
// ⚠️ 已知取舍：sbcUnparse 会丢弃注释行（如 unpack 为未知积木输出的
// 「# 未知积木: xxx」）。编辑写回后这些注释会消失 —— 但注释不参与语义，
// 且编辑本身就是"重写这个文件"，可接受。
#pragma once

#include <string>
#include <vector>

namespace sb {

struct EditResult {
    bool        ok = true;
    std::string error;
    std::vector<std::string> notes;   // 编辑摘要
    int         changed = 0;          // 改动的块数/行数
};

// ---------------------------------------------------------------- P0 编辑

// 在 <角色> 的第 scriptIdx 个脚本（1 起）的第 afterLine 个顶层块之后插入一行积木。
// blockText 形如 "looks_say MESSAGE=你好"。
EditResult sbcEditInsert(const std::string& project, const std::string& sprite,
                         int scriptIdx, int afterBlock, const std::string& blockText);

// 删除第 scriptIdx 个脚本的第 blockIdx 个顶层块（1 起）。
EditResult sbcEditDelete(const std::string& project, const std::string& sprite,
                         int scriptIdx, int blockIdx);

// 替换第 scriptIdx 个脚本的第 blockIdx 个顶层块。
EditResult sbcEditReplace(const std::string& project, const std::string& sprite,
                          int scriptIdx, int blockIdx, const std::string& blockText);

// 删除整个脚本。
EditResult sbcEditDropScript(const std::string& project, const std::string& sprite,
                             int scriptIdx);

// 追加一个新脚本（hat 如 "flag" / "broadcast 消息1"，body 为多行积木文本）。
EditResult sbcEditAddScript(const std::string& project, const std::string& sprite,
                            const std::string& hat, const std::string& bodyText);

// ---------------------------------------------------------------- P2 重命名

// 重命名变量/列表/广播/角色，并同步所有 block.sbcli 里的引用 + meta 声明。
// kind: "variable" / "list" / "broadcast" / "sprite"
EditResult sbcRename(const std::string& project, const std::string& kind,
                     const std::string& oldName, const std::string& newName);

// ---------------------------------------------------------------- 辅助

// 列出某角色所有脚本的摘要（编辑前先看 index）。
// 复用 cmd_view 的渲染，返回每脚本的首行描述。
std::vector<std::string> sbcListScripts(const std::string& project,
                                        const std::string& sprite);

} // namespace sb
