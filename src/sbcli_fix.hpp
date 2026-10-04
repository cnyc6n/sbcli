// src/sbcli_fix.hpp
// `sb fix` 的「发现即声明」引擎（docs/format.md §4）。
//
// 分工（与 check 互补，两者共享 parser 的 AST，各自独立）：
//   check → 报告问题，不改任何文件
//   fix   → 能自动补的补上（meta 声明），补不了的**报告**但不改 block.sbcli
//
// 为什么 fix 不碰 block.sbcli：它是手写源文件，且 pack 不回写（见
// docs/parser-notes.md §5）——重建会丢注释。所以结构类问题（procedures_call
// 缺 ARG、else 错位）fix 只报告，交给人改。
#pragma once

#include <string>
#include <vector>

namespace sb {

struct FixNote {
    std::string file;    // 项目相对路径；非文件级为空
    int         line = 0;
    std::string level;   // "info" / "warn" / "error"
    std::string code;    // registered-variable / asset-missing / call-arg-count …
    std::string message;
};

struct FixReport {
    std::string              root;
    std::vector<std::string> files;     // 扫描过的 block.sbcli（相对路径）
    std::vector<std::string> written;   // 新建或更新的 meta（相对路径）
    std::vector<FixNote>     notes;
    int  registered = 0;   // 新登记的条目数（变量+列表+广播+造型+声音）
    int  errors     = 0;
    bool dryRun     = false;
    std::string error;     // 目录本身不可用时的顶层错误
};

// 扫描项目里的 block.sbcli，把引用到的名字登记进 meta。
// dryRun=true 时只报告不写文件（用于 --dry-run / 自测）。
FixReport sbcliFix(const std::string& rootOrDir, bool dryRun = false);

// 确定性 id：给变量 / 列表 / 广播生成 sb3 用的 id。
//
// 刻意**不落盘**：meta.sbcli 的格式（§3.1/3.3）没有 id 字段，硬塞一个 [ids]
// 段会动到公共格式、连带影响 check 的 meta 解析。改成"名字 → id"的纯函数，
// fix 和 pack 各自调用同一函数即可，跨命令一致且幂等。
// scope 用来隔离同名的全局变量与角色变量（"stage" / "1" / …）。
std::string sbcVarId(const std::string& scope, const std::string& name);
std::string sbcBroadcastId(const std::string& name);

} // namespace sb
