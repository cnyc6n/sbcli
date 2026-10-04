// src/sb3_internal.hpp
// sb3 拆分后的内部共享声明（不对外公开）。
#pragma once
#include "sb3.hpp"

namespace sb {

// 名称清单（sb3_name_lists；s3_cmd_text 使用）
std::map<std::string, std::vector<std::string>>
sb3NameLists(const Elem& targets);

// 命令内用的小工具
void sb3JsonOut(const Json& j);
std::string sb3View(const Json& v);
std::string sb3View(const Elem& v);
int sb3ScriptBudget(const struct Args& a);

} // namespace sb