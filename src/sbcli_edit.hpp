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
    std::string code;                 // 稳定错误码（仅 ok=false 时有意义）：
                                      //   E_NO_PROJECT / E_NO_SPRITE / E_BAD_PATH /
                                      //   E_OUT_OF_RANGE / E_PARSE / E_STRUCT / E_IO
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

// ---------------------------------------------------------------- 子栈路径编辑

// 路径语法："N" 或 "N.K" 或 "N.K.S"
//   N = 脚本序号（1 起）
//   K = 该层第几个块（1 起）
//   S = 进入该块的子栈（0 = 第一个/那么分支，1 = else 分支）
// 例："1.2.0.1" = 脚本1 → 第2块 → 第1个子栈 → 第1块
struct EditPath {
    int script = 0;
    std::vector<std::pair<int,int>> steps;   // (块序号 1起, 子栈序号 0起)
    bool ok = false;
    std::string error;
};
EditPath sbcParseEditPath(const std::string& spec);

// 在路径指向的**子栈内部**插入/删除/替换。
// path 形如 "1.2.0"（脚本1 → 第2块的子栈0），新块插在该子栈末尾（或 after 指定位置）。
EditResult sbcEditInsertAt(const std::string& project, const std::string& sprite,
                           const std::string& path, const std::string& blockText,
                           int afterBlock = -1);
EditResult sbcEditDeleteAt(const std::string& project, const std::string& sprite,
                           const std::string& path);
EditResult sbcEditReplaceAt(const std::string& project, const std::string& sprite,
                            const std::string& path, const std::string& blockText);

// ---------------------------------------------------------------- 辅助

// 脚本清单的结构化表示（--list --json 用；sbcListScripts 也基于它格式化）
struct ScriptInfo {
    int         index = 0;        // 脚本序号（1 起，供 --script-index 用）
    std::string hat;              // 帽子类型，如 flag / broadcast / key
    std::string hatArg;           // 帽子的参数（广播名 / 按键名），可为空
    int         blockCount = 0;   // 顶层块数
    std::string firstOpcode;      // 首块 opcode；空脚本为空串
};

// 列出某角色所有脚本的结构化摘要。
// 项目根/角色无效时返回空 vector（要看具体原因请用 sbcScriptInfosEx）。
std::vector<ScriptInfo> sbcScriptInfos(const std::string& project,
                                       const std::string& sprite);

// 同上，但失败时通过 err/code 说明原因（E_NO_PROJECT / E_NO_SPRITE / E_IO）。
std::vector<ScriptInfo> sbcScriptInfosEx(const std::string& project,
                                         const std::string& sprite,
                                         std::string& err, std::string& code);

// 列出某角色所有脚本的摘要（编辑前先看 index）。
// 复用 cmd_view 的渲染，返回每脚本的首行描述。
std::vector<std::string> sbcListScripts(const std::string& project,
                                        const std::string& sprite);

} // namespace sb
