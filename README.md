# sbcli —— Scratch 作品命令行查看器

读取 Scratch 1.4（.sb/.sprite）、Scratch 2（.sb2）、Scratch 3（.sb3/.sprite3，
以及改名的 .sb/.zip）作品，把舞台、角色、积木脚本、变量、文字等内容
翻译成可读的中文文本或 JSON 输出。

JSON 解析基于 [simdjson](https://github.com/simdjson/simdjson)（只读 DOM，
性能优先），已完全移除对 nlohmann json 的依赖。

除只读查看外，1.1 起还支持**项目级编辑工具链**：把作品拆成可读可写的
文本项目目录，用文本编辑器改脚本，再打包回 .sb3（见下方「项目级工具链」）。

## 命令

| 命令     | 作用 |
|----------|------|
| `info`   | 概要：格式、目标数、工程信息 |
| `sprites`| 列出所有角色和舞台 |
| `text`   | 提取作品里的全部文字（对话 / 名称 / 注释 / 积木文字） |
| `script` | 把积木脚本翻译成可读文字 |
| `vars`   | 列出变量和列表 |
| `assets` | 列出造型和声音，可导出 |
| `media`  | 同 `assets`；1.4 造型导出 PNG |
| `blocks` | 积木使用统计 |
| `json`   | 输出原始 project.json（`--raw` 输出未格式化文本） |
| `raw`    | 输出原始对象树（1.4 调试用，`--depth N` 控制深度） |
| `ls`     | 扫描目录下所有作品 |
| `find`   | 在所有作品里搜索文字 |
| `refs`   | 变量/列表读写交叉引用（谁在哪个脚本里设置/读取/追加；**同名变量标注 全局/局部·角色**） |
| `events` | 广播拓扑（谁发、谁收；孤儿广播标注 ⚠） |
| `diff`   | 版本对比（角色/积木/变量差异；默认折叠摘要，`--detail`/`--by-sprite` 展开明细） |
| `dup`    | 相似角色检测（复制粘贴拼贴作品：按积木结构 Jaccard 相似度分组，`--threshold N` 设阈值 %，`--limit N` 限每组条数） |

### 项目级工具链

把作品拆成**可读可写的文本项目目录**（`sbcli project` 格式），
改完再打包回 .sb3 —— 即「拆开 → 手改 → 装回」的完整闭环。

| 命令     | 作用 |
|----------|------|
| `project`| 项目脚手架：`init` / `add-sprite` / `add-costume` / `add-sound` / `add-variable` / `add-list` / `add-broadcast` |
| `unpack` | 把 .sb3/.sb2 作品拆成 sbcli 项目目录（含素材导出） |
| `check`  | 检查项目的脚本语法 / 结构 / 引用（四类；`--json` 结构化输出，有错退出码 1） |
| `fix`    | 发现即声明：把脚本引用到的变量/列表/广播/造型/声音自动登记进 meta（**不改动 block.sbcli**，幂等） |
| `view`   | 浏览项目：角色清单 + 脚本中文渲染 |
| `pack`   | 把 sbcli 项目目录重新打包成 .sb3（`sb pack <目录> <输出.sb3>`） |
| `search` | 语法查找手册：中文概念 / 英文 opcode → 模板 + 参数 + 示例 |

> 项目目录结构：`meta.sbcli`（元数据）+ `character/*/block.sbcli`（手写脚本）+ `assets/`。
> `block.sbcli` 用缩进的英文 opcode 语法，参数用 SB3 真实字段名
> （如 `data_setvariableto VARIABLE=分数 VALUE=0`），`(...)`/`<...>` 表示 reporter 嵌套。
> 完整格式规范见 **`docs/format.md`**（词法/作用域/meta/round-trip 规则）。
> 只有 `block.sbcli` 需要人写，`meta.sbcli` 交给 `sb fix` 自动维护。
> `info`/`sprites`/`script`/`vars`/`refs`/`events`/`text`/`blocks` 都能**直接读项目目录**（不必先打包），
> `find --dir` 也会搜索项目目录里的 block.sbcli。
> 示例：`sb unpack 作品.sb3 我的项目` → 手改 `block.sbcli` → `sb check 我的项目`
> → `sb fix 我的项目` → `sb pack 我的项目 新版.sb3`。

> `refs`/`events`/`diff`/`dup` 支持 Scratch 2/3（1.4 回显提示）。
> `refs` 过滤参数是**空格分隔多关键词 OR**（`GRID COLUMNS` 命中 GRID/GRID COLUMNS/GRID ROWS）；
> 加 `--regex` 则按正则匹配（如 `^Sprite1$` 精确锚定）。反查用法：`sb refs 作品.sb3 变量名`
> 即输入变量名、输出谁在哪个脚本读写（--sprite 可聚焦单角色）。
> `events` 标注两类孤儿：⚠ 无发送者（可能变量广播/残留）、⚠ 无接收者（发出没人监听）。
> `diff` 明细为 script 风格真实译文（真实参数），`--json` 始终带 `opcodeDelta` 等完整明细。
> `dup` 默认 60% 阈值，`--threshold N` 调阈值（0-100，防呆超界）；
> `--limit N` 限制每组显示条数（默认显示全部）。
> `refs --by-sprite` 反查角色读/写；`--regex` 正则匹配；`0xC0000005` 已修（dup --json）。

通用选项（**可放在文件/目录/关键字前后任意位置**）：

```
--json              输出 JSON
--limit N           显示条数上限
--sprite NAME       只看某角色
--extract DIR       导出目录
--dir DIR           find/ls 的搜索目录
--jobs N            find 的并行任务数（默认 4）
--strings            text 额外收集积木里的文字
--dialog-only       text 只输出对话
--drop-numbers      text 过滤纯数字/符号串（数值噪音）
--full              vars 显示完整值 / 列表全部内容
--show-unknown      script 显示全部未收录积木
--by-sprite         blocks 按角色统计
--raw               json 输出原始文本 / info-table 输出信息表
--depth N           raw 的递归深度
--file 关键字       find 只搜文件名包含关键字的作品
--sort 名字|大小|时间|积木   ls 排序
--script            find 同时搜索脚本译文
--threshold N       dup 相似度阈值 %（0-100）
```

示例：

```bash
sb info 作品.sb3 --json
sb script 作品.sb3 --json --sprite 角色名
sb text 作品.sb2 --limit 20 --drop-numbers
sb find 你好 --dir 我的作品 --jobs 8
sb assets 作品.sb3 --extract ./导出
sb ls 我的作品 --sort size
sb refs 作品.sb3 系统日志          # 只查「系统日志」列表的读写
sb refs 作品.sb3 --sprite 角色名    # 只查某角色的变量读写
sb refs 作品.sb3 --regex "^Sprite1$"  # 正则锚定精确匹配
sb events 作品.sb3                 # 广播拓扑图
sb diff 旧版.sb3 新版.sb3           # 折叠摘要
sb diff 旧版.sb3 新版.sb3 --detail   # 展开每个角色的积木明细
sb dup 作品.sb3                    # 相似角色检测（复制粘贴拼贴）
sb dup 作品.sb3 --threshold 80      # 阈值调到 80%
sb dup 作品.sb3 --limit 5           # 每组只显示前 5 条
```

项目级工具链示例：

```bash
sb project init 我的项目 打砖块      # 建项目骨架
sb unpack 作品.sb3 我的项目          # 或直接从作品拆出来
sb view 我的项目                    # 看角色清单 + 脚本译文
sb check 我的项目                   # 查语法/结构/引用
sb check 我的项目 --json            # 结构化输出（有错退出码 1）
sb fix 我的项目                     # 补齐 meta（不改 block.sbcli）
sb pack 我的项目 新版.sb3            # 装回 .sb3
sb search 广播                      # 查语法模板 + 示例
sb search looks_say --json
```

## 构建

需要 C++17 编译器（开发环境为 w64devkit 的 g++ 15 + Ninja）：

```bash
cmake -S . -B build
cmake --build build
# 产物：build/sb.exe
```

`third_party/` 下的 simdjson（amalgamated）与 miniz 均为源码内置，无外部依赖。

## 架构

simdjson 的 DOM 是只读、且借用解析缓冲区；而工具还需要构造输出 JSON、
以及 sb2→sb3 的转换构造，因此拆成两层：

- **读取层（simdjson DOM）**：`src/sapi.hpp/cpp` 提供 `Elem` 包装
  （`simdjson::dom::element` 的值语义封装，带有效性保护），所有读取
  （目标、积木、字段、输入、素材……）都直接在 simdjson 磁带上遍历，
  不建树、不拷贝。`Sb3File` 持有 `parser + element` 保证缓冲区存活。
- **输出层（自有 Json）**：`src/jdoc.hpp/cpp` 提供 `sb::Json` 值类型
  （`std::map`/`std::vector`，键序与旧版一致），负责命令输出与
  sb2→sb3 转换构造。`parse()` 内部仍由 simdjson 完成。

sb2 文件走「simdjson 解析 → `Json` 中转转换 → 序列化回灌 simdjson」的桥接；
sprite 单文件则包装成单元素 targets 数组。

## 性能

解析走 simdjson、读取不建树，大工程明显提速（Release 构建，Windows）：

| 命令 | 旧（nlohmann） | 新（simdjson） | 加速 |
|------|---------------|----------------|------|
| info  | 555ms | 177ms | x3.1 |
| blocks | 490ms | 167ms | x2.9 |
| sprites | 499ms | 134ms | x3.7 |
| script | 533ms | 235ms | x2.3 |

（示例：17MB project.json 的 Minecraft 3D 作品。）

## 目录结构

```
src/
  main.cpp        参数解析与命令分发
  commands.cpp    统一命令层（格式探测 / ls / find）
  sb1_*.cpp       Scratch 1.4（Squeak 对象表 + 位图解码 + PNG 写出）
  squeak*.cpp     Squeak 二进制格式解析
  sb2.cpp         sb2 → sb3 转换
  sb3_*.cpp       sb3 加载 / 渲染 / 汇总 / 命令
  sbcli_parser.*  block.sbcli 语法解析（AST）
  sbcli_check.*   项目检查（语法 / 结构 / 引用）
  sbcli_fix.*     发现即声明（补齐 meta）
  sbcli_view.*    项目浏览（角色清单 + 脚本渲染）
  sbcli_project.* 项目脚手架（init / add-*）
  sbcli_pack.*    项目目录 → .sb3（块链重建 + mutation）
  sbcli_unpack.*  .sb3/.sb2 → 项目目录
  sbcli_search.*  语法查找手册
  sbcli_meta.hpp  项目 meta 读写
  sapi.*          simdjson DOM 访问层
  jdoc.*          Json 值类型（输出与转换用）
third_party/
  simdjson.h/.cpp 解析引擎（amalgamated）
  miniz-zip.hpp   zip 读取（读取 + Writer）
```
