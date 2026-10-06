// src/sb3.hpp
#pragma once
#include "common.hpp"
#include "zip.hpp"
#include "jdoc.hpp"
#include "sapi.hpp"
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace sb {

// 已解析的 simdjson DOM + 压缩包。data 引用 p 的解析缓冲区（必须一起存活）。
struct Sb3File {
    std::string kind;  // "project" / "sb2" / "sprite"
    std::string path;  // 源文件路径（ext 加载用）
    std::shared_ptr<mzip::Reader> zip;
    std::shared_ptr<simdjson::dom::parser> p;   // data 所在文档的解析器
    simdjson::dom::element data;                // 顶层元素（json 命令 dump 用）
    std::shared_ptr<simdjson::dom::parser> pT;  // sprite 时：包装的 targets 数组文档
    Elem targets;                               // targets 数组视图（总有效）
};

Sb3File sb3LoadAny(const std::string& path);
// 用已打开的 Reader 加载（共享所有权；find 预筛后复用）
Sb3File sb3LoadFromReader(std::shared_ptr<mzip::Reader> zip,
                          const std::string& path);

// 某个目标的 blocks 元素（不拷贝；不是对象时返回无效 Elem）
Elem sb3BlocksOf(const Elem& target);
// 顶层脚本数量（轻量计数）
int sb3TopScriptCount(const Elem& target);
// 顶层脚本（按 y 降序 x 升序；id + 块视图，不拷贝）
std::vector<std::pair<std::string, Elem>>
sb3TopScripts(const Elem& target);

// 翻译一个目标
class Renderer {
public:
    explicit Renderer(const Elem& target);
    std::string render(const std::string& bid);
    // scripts(maxLinesPerScript, maxScriptCount)：行级预算 0=不限；
    // maxScriptCount>0 时只取前 N 个完整脚本（--limit 整脚本截断）。
    std::vector<std::vector<std::string>> scripts(int budget = 0,
                                                  size_t maxScriptCount = 0);

    std::map<std::string, int> missing;  // opcode → 出现次数

    // 供 sb3CollectText 等外部逻辑使用
    std::string inputText(const Elem& b, const std::string& key, bool menu=false);
private:
    const Elem m_t;                                        // 目标视图（拷贝廉价）
    std::unordered_map<std::string_view, Elem> m_blocks;  // id → 块视图（键引用解析缓冲区，不拷贝）
    std::map<std::string, std::string> m_varNames, m_listNames, m_bcastNames;
    int m_renderDepth = 0;   // 渲染递归深度，防止块引用成环时栈溢出
    std::unordered_map<std::string, std::string> m_renderCache;  // id → 已渲染文本（reporter 记忆化）

    // 扩展积木渲染（内置/自定义）：type 0=COMMAND 1=REPORTER 2=BOOLEAN 3=HAT
    std::string extBlockRender(const Elem& b, int type, const std::string& fullOp);

public:
    // 运行时发现的扩展积木表（opcode → 类型），来自自动联网解析（sb ext）
    // 渲染时先查静态表（EXT_BLOCK_TYPES/CUSTOM_EXT_BLOCK_TYPES），再查此动态表
    std::map<std::string, int> dynExtTypes;
    std::map<std::string, std::string> dynExtNames;   // 扩展id → 名称

    std::string inputValue(const Elem& arr, bool menu=false);
    std::string prim(const Elem& x, bool menu=false);
    std::string valueOf(const Elem& b, const std::string& key, bool menu=false);
    std::string customSignature(const Elem& b);
    std::string customCall(const Elem& b);
    std::string generic(const Elem& b);
    std::string fieldTextOf(const Elem& b, const std::string& key);
    std::string rawPrim(const Elem& b, const std::string& key);
    // 造型/背景按编号查名字（1 起；越界/非数字 → ""）
    std::string costumeNameByNumber(long long n) const;
    std::string sb2Call(const Elem& b);
    void stack(const std::string& bid, int depth,
               std::vector<std::string>& lines, int budget);
};

// 素材候选条目名
std::vector<std::string> sb3AssetFile(const Elem& obj, const std::string& kind);
std::string sb3ResolveEntry(const std::map<std::string, std::string>& lower,
                            const std::vector<std::string>& candidates);

// 提取文字：对话 / 注释 / 积木里的普通文字
struct Sb3Collect {
    std::vector<std::pair<std::string, std::string>> dialog;   // (说/思考/询问, 内容)
    std::vector<std::string> comments;
    std::vector<std::tuple<std::string, std::string, std::string>> strings; // (opcode, key, 文字)
};
Sb3Collect sb3CollectText(const Elem& target, bool wantStrings);

// 目标汇总行（sprites / info 用）
Json sb3TargetRows(const Elem& targets);

// 单个文件的摘要（ls 用）
Json sb3Summarize(const std::string& path);

// 扫描目录里的 Scratch 2/3 文件
std::vector<std::string> findSb3Files(const std::string& root);
// 扫描 sbcli 项目目录，返回 character/*/block.sbcli 路径（供 find 搜索）
std::vector<std::string> findSbcliBlockFiles(const std::string& root);

// 命令
struct Args;
int s3_cmd_sprites(const Args& a);
int s3_cmd_text  (const Args& a);
int s3_cmd_script(const Args& a);
int s3_cmd_blocks(const Args& a);
int s3_cmd_vars  (const Args& a);
int s3_cmd_assets(const Args& a);
int s3_cmd_json  (const Args& a);
int s3_cmd_ls    (const Args& a);
int s3_cmd_find  (const Args& a);
int s3_cmd_refs  (const Args& a);   // 变量/列表读写交叉引用
int s3_cmd_events(const Args& a);   // 广播拓扑（谁发谁收）
int s3_cmd_diff  (const Args& a);   // 版本对比
int s3_cmd_dup   (const Args& a);   // 相似角色检测

} // namespace sb