// src/sb1.hpp
#pragma once
#include "common.hpp"
#include "squeak.hpp"
#include "jdoc.hpp"

namespace sb {

struct Target {
    bool isStage = false;
    const SqueakObject* obj = nullptr;
    std::string name;
};

std::vector<Target> findTargets(const Value& root);
std::string targetName(const Target& t);

// 造型/声音
struct Media { std::string name; const SqueakObject* obj; };
void targetMedia(const Target& t,
                 std::vector<Media>& images, std::vector<Media>& sounds);

// 变量
struct VarEntry { std::string name; Value value; };
std::vector<VarEntry> targetVariables(const Target& t);

// 脚本
std::vector<Value> targetScripts(const Target& t);

// 收集字符串。withPath=false 时跳过路径拼接（find 只需要文本，更快）
std::vector<std::pair<std::string, std::string>>
collectStrings(const Value& root, size_t limit = 4000, bool withPath = true);

// 脚本翻译
std::vector<std::string> renderScript(const Value& script, int budget = 600);

// ---- Squeak 位图 ----
struct RGBAImage { int w, h; std::vector<uint8_t> rgba; };
bool formToRGBA(const SqueakObject& form, RGBAImage& out);
bool writePNG(const std::string& path, int w, int h,
              const std::vector<uint8_t>& rgba);

// ---- 命令 ----
int s1_cmd_info  (const struct Args& a);
int s1_cmd_sprites(const struct Args& a);
int s1_cmd_text  (const struct Args& a);
int s1_cmd_script(const struct Args& a);
int s1_cmd_vars  (const struct Args& a);
int s1_cmd_media (const struct Args& a);
int s1_cmd_raw   (const struct Args& a);
int s1_cmd_refs  (const struct Args& a);   // 变量/列表读写交叉引用
int s1_cmd_events(const struct Args& a);   // 广播拓扑

} // namespace sb