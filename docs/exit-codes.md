# sbcli 退出码规范与实测矩阵

本文是 `sb` 的退出码契约：脚本 / CI / AI 可以靠退出码判断**失败类型**，
而不必去 grep 中文输出。

> **本版最重要的约定：没有改变任何已有行为。**
> 本版只做「裸数字 → 具名常量」和文档，未改任何分支走向，
> 所以下表「实测」列**就是改动前的行为**（改动前后 52 个场景逐条比对，退出码 100% 一致）。
> 规范与现状不一致的地方，一律如实标注为「未分类」或「预留」，不做偷偷改码。

代码位置：

- 枚举定义：`src/common.hpp`（`sb::ExitCode` + `sb::exitInt()`）
- 命令分发与顶层错误：`src/main.cpp`
- 各命令实现：`src/commands.cpp`（`cmd_*` 函数）

---

## 1. 退出码表

| 码 | 含义 | 典型场景 | 现状 |
|---|---|---|---|
| 0 | 成功 | 正常完成 | ✅ 已在使用 |
| 1 | 通用失败 | 未分类错误（文件读不出 / 解压失败 / 搜索零命中 `/` / `check` 发现 error） | ✅ 已在使用 |
| 2 | 用法错误 | 参数缺失 / 未知选项 / 未知命令 / 目标不支持该命令 | ✅ 已在使用 |
| 3 | 输入无效 | 文件不存在 / 格式不支持 / 压缩包损坏 | ⚠️ **预留，当前仍返回 1** |
| 4 | 校验失败 | `check` 发现 error 级问题 | ⚠️ **预留，当前仍返回 1** |
| 5 | 编辑冲突 | 序号越界 / 重复登记 / 状态不允许 | ⚠️ **预留，当前仍返回 2** |
| 6 | 依赖缺失 | 需要 node 但找不到 / 扩展源码缺失 | ⚠️ **预留，当前仍返回 2** |
| 7 | 权限或 IO | 读写失败 / 磁盘满 | ⚠️ **预留，当前多数仍返回 1** |

### 1.1 为什么 3/4/5/6/7 只是「预留」

任务的目标表定义了 8 个码，但**铁律是不改变已有行为**，而下面这些现状都不能在不改行为的前提下
挪动，所以本版**只登记、不启用**：

- `check` 发现问题：目标是 4（校验失败），但现在返回 **1**。
  `README.md`、`docs/format.md`、`--help` 和下游脚本都在按「1 = 有错」用，
  改成 4 属于**行为变更**。→ 保留 1，语义记为「1（通用失败）：check 发现 error 级问题」。
- 文件不存在 / 压缩包损坏：目标是 3，但现在返回 **1**（`信息：找不到文件` 走的是异常路径）。
- 扩展源码缺失（`project ext-info` / `add-extension`）：目标是 6，但现在返回 **2**。
- 序号越界 / 状态不允许：目标是 5，但现在返回 **2**（且 `cmd_edit` / `cmd_rename` / `cmd_merge`
  三个函数本版不改，归别的任务）。

因此 **3/4/5/6/7 目前没有任何命令会返回**。脚本里请仍按 0/1/2 判断；
将来启用 3~7 时会是**破坏性变更**，需要同步改 README、CI 和这五个测试套件。

---

## 2. 实测退出码矩阵

**测量方法**（可复现）：

```powershell
$env:TEMP="F:\temp"; $env:TMP="F:\temp"
cd D:\scratch_tool
pwsh -NoProfile -File F:\temp\ec_probe2.ps1     # 逐条跑真实命令并记录 $LASTEXITCODE
```

夹具：

| 夹具 | 说明 |
|---|---|
| `fixture_origin.sb3` | 仓库自带的合法 sb3（990 B，1 角色 / 3 脚本） |
| `fixture_v2.sb3` | 同上的另一版，用于 diff |
| `F:\temp\ec-proj` | `sb unpack fixture_origin.sb3 F:\temp\ec-proj` 生成的干净 sbcli 项目 |
| `F:\temp\ec-bad` | ec-proj 的副本，另在 `character/1/block.sbcli` 追加一行 `@bogus_block_opcode foo bar`，制造 1 个 error 级诊断 |
| `F:\temp\ec-corrupt.sb3` | 手写的 6 字节假 PK 头，制造「压缩包损坏」 |
| `D:\scratch_tool\docs` | 一个**不是** sbcli 项目的普通目录 |
| `F:\temp\nonexistdir` | 不存在的目录 |

下表的「退出码」列全部来自**实际执行**，不是推断。

### 2.1 顶层（`src/main.cpp`）

| ID | 命令 | 场景 | 退出码 | 判断依据（首行输出） |
|---|---|---|---|---|
| E01 | `sb --help` | 打印帮助 | **0** | `sbcli —— 读取 Scratch 作品…` |
| E02 | `sb --version` | 打印版本 | **0** | `sbcli 1.3.0` |
| E03 | `sb` | 无参数 → 打印帮助 | **0** | `sbcli —— 读取 Scratch 作品…` |
| E04 | `sb nosuchcmd` | 未知命令 | **2** | `未知命令：nosuchcmd` |
| E05 | `sb info x.sb3 --bogus` | 未知选项 | **2** | `未知选项：--bogus` |
| E06 | `sb info -h` | 子命令位置给 `-h` | **0** | 打印帮助 |

### 2.2 作品读取类（info / sprites / text / script / vars / assets / media / blocks / json / raw）

| ID | 命令 | 场景 | 退出码 | 判断依据 |
|---|---|---|---|---|
| E10 | `sb info fixture_origin.sb3` | 正常 .sb3 | **0** | `文件：fixture_origin.sb3（990B，project）` |
| E11 | `sb info nope.sb3` | 文件不存在 | **1** | `错误：找不到文件：nope.sb3` |
| E12 | `sb info ec-corrupt.sb3` | 压缩包损坏 | **1** | `错误：不是有效的压缩包，文件可能损坏` |
| E13 | `sb info F:\temp\ec-proj` | sbcli 项目目录 | **0** | `文件：ec-proj（sbcli 项目）` |
| E14 | `sb info … --json` | JSON 输出 | **0** | `{` |
| E20 | `sb sprites fixture_origin.sb3` | 正常 | **0** | `文件：fixture_origin.sb3…` |
| E21 | `sb sprites nope.sb3` | 文件不存在 | **1** | `错误：找不到文件` |
| E22 | `sb text fixture_origin.sb3` | 正常 | **0** | `文件：fixture_origin.sb3…` |
| E23 | `sb text nope.sb3` | 文件不存在 | **1** | `错误：找不到文件` |
| E24 | `sb script fixture_origin.sb3` | 正常 | **0** | `文件：fixture_origin.sb3…` |
| E25 | `sb script nope.sb3` | 文件不存在 | **1** | `错误：找不到文件` |
| E26 | `sb vars fixture_origin.sb3` | 正常 | **0** | `文件：fixture_origin.sb3…` |
| E27 | `sb vars ec-proj` | sbcli 项目 | **0** | `文件：ec-proj（sbcli 项目）` |
| E28 | `sb vars D:\scratch_tool\docs` | 普通目录（当作文件） | **1** | `错误：找不到文件：D:\scratch_tool\docs` |
| E29 | `sb assets fixture_origin.sb3` | 正常 | **0** | `文件：…` |
| E30 | `sb media fixture_origin.sb3` | 正常 | **0** | `文件：…` |
| E31 | `sb blocks fixture_origin.sb3` | 正常 | **0** | `文件：…` |
| E32 | `sb json fixture_origin.sb3` | 正常 | **0** | `{` |
| E33 | `sb raw fixture_origin.sb3` | 正常 | **0** | `{` |
| E34 | `sb json ec-proj` | **sbcli 项目目录**（json 不支持） | **1** | `错误：找不到文件：F:\temp\ec-proj` |
| E35 | `sb info nope.sb` | `.sb` 后缀但文件不存在 | **1** | `错误：找不到文件` |

> **E34 是一个已知的不一致**：`sb json <项目目录>` 实际走的是 sb3 分支，
> 把目录当成文件读，于是报「找不到文件」并返回 1。语义上它更像「格式不支持」（应为 2 或 3），
> 但本版**保持原样**，登记为「未分类」。

### 2.3 扫描 / 搜索类（ls / find / refs / events / diff / dup / search）

| ID | 命令 | 场景 | 退出码 | 判断依据 |
|---|---|---|---|---|
| E40 | `sb ls .` | 目录有作品 | **1** | `错误：解压失败：project.json`（见下方说明） |
| E41 | `sb ls F:\temp\nonexistdir` | 目录无作品 | **2** | `这里没有找到 .sb / .sprite / .sb3 / .sb2 / .sprite3 文件` |
| E42 | `sb find 你好 --dir . --jobs 1` | 有命中 | **0** | `搜索「你好」，命中 2 处` |
| E43 | `sb find zzz绝不存在zzz --dir . --jobs 1` | 零命中 | **0** | `搜索「zzz绝不存在zzz」，命中 0 处` |
| E50 | `sb refs fixture_origin.sb3` | 正常 .sb3 | **0** | `文件：…` |
| E51 | `sb refs ec-proj` | sbcli 项目 | **0** | `变量 分数` |
| E52 | `sb refs D:\scratch_tool\docs` | 非项目目录 | **1** | `错误：找不到文件` |
| E53 | `sb events fixture_origin.sb3` | 正常 .sb3 | **0** | `文件：…` |
| E54 | `sb events ec-proj` | sbcli 项目 | **0** | `文件：ec-proj（sbcli 项目）` |
| E55 | `sb events D:\scratch_tool\docs` | 非项目目录 | **1** | `错误：找不到文件` |
| E56 | `sb diff fixture_origin.sb3 fixture_v2.sb3` | 正常两文件 | **0** | `对比：fixture_origin.sb3 → fixture_v2.sb3` |
| E57 | `sb dup fixture_origin.sb3` | 正常 | **0** | `文件：fixture_origin.sb3（相似度阈值 60%）` |
| E58 | `sb search 广播` | 有命中 | **0** | `「广播」匹配 3 个语法块：` |
| E59 | `sb search zzz绝不存在zzz` | 零命中 | **1** | `没有匹配「…」的语法块。` |

> **E40**：仓库根目录里的 `test_open.sb3` / `test_closed.sb3` 并不是真的 sb3（内容是乱码 / 不是 zip），
> `sb ls .` 扫到它们时其中一个抛「解压失败」，于是整体返回 1。
> 这是**已存在**的行为：`ls` 遇到坏文件会打印错误并返回 1，本版未改动。
>
> **零命中的不一致**：`find` 零命中返回 **0**，`search` 零命中返回 **1**。
> 两者语义都不算「失败」，但现状不同，本版**各自保持原样**。

### 2.4 项目检查类（check / fix / view）

| ID | 命令 | 场景 | 退出码 | 判断依据 |
|---|---|---|---|---|
| E60 | `sb check F:\temp\ec-proj` | 干净项目 | **0** | `检查项目：F:/temp/ec-proj` |
| E61 | `sb check F:\temp\ec-proj --json` | 干净项目 --json | **0** | `{` |
| E62 | `sb check F:\temp\ec-bad` | **有 error 级问题** | **1** | `共 1 个错误、2 个警告。` |
| E63 | `sb check F:\temp\ec-bad --json` | 有 error 级问题 --json | **1** | `{`（`"ok": false`） |
| E64 | `sb check D:\scratch_tool\docs` | 非项目目录 | **2** | `错误：找不到项目根目录…` |
| E70 | `sb fix F:\temp\ec-proj --dry-run` | 干净项目 --dry-run | **0** | `修复项目：…（--dry-run，不写文件）` |
| E71 | `sb fix D:\scratch_tool\docs` | 非项目目录 | **2** | `错误：找不到项目根目录…` |
| E75 | `sb view F:\temp\ec-proj` | 正常项目 | **0** | `项目：F:/temp/ec-proj（fixture_origin）` |
| E76 | `sb view D:\scratch_tool\docs` | 非项目目录 | **2** | `错误：找不到项目根目录…` |
| E77 | `sb view ec-proj --sprite 不存在` | 角色不存在 | **2** | `错误：没有找到角色「不存在」` |

> **E62 / E63 是最需要注意的一条**：`check` 发现 error 级问题时返回 **1**，不是 4。
> 铁律要求「现在非 0，改完必须仍非 0」——本版保持 **1**，行为不变。
> 文本模式与 `--json` 模式的判定一致（都看 `r.errors`）。
>
> **E77**：`--sprite` 找不到角色返回 2（用法错误）。语义上更像「输入无效 / 编辑冲突」，
> 但本版**保持 2**。

### 2.5 脚手架类（project）

| ID | 命令 | 场景 | 退出码 | 判断依据 |
|---|---|---|---|---|
| E80 | `sb project` | 无子命令 | **2** | `用法：sb project <子命令> <项目目录> [参数...]` |
| E81 | `sb project nosuchsub` | 未知子命令 | **2** | `用法：sb project <子命令> …`（见下方说明） |
| E82 | `sb project add-sprite <项目>` | 缺名字参数 | **2** | `用法：sb project add-sprite <项目> <名字>` |
| E83 | `sb project list-extensions <项目>` | 项目没有扩展 | **0** | `（项目没有登记任何扩展）` |
| E84 | `sb project ext-info <项目> nope` | 扩展源码缺失 | **2** | `错误：找不到扩展源码 extensions/nope.js` |
| E85 | `sb project add-extension <项目>` | 缺参数 | **2** | `用法：sb project add-extension <项目> <扩展.js \| URL>` |
| E86 | `sb project add-extension <项目> <不存在.js>` | 扩展文件不存在 | **2** | `错误：读不到扩展文件：…` |
| E87 | `sb project remove-extension <项目>` | 缺参数 | **2** | `用法：sb project remove-extension <项目目录> <扩展id>` |

> **E81 的实际输出是「用法：…」而不是「未知 project 子命令：…」**：
> `main()` 只把第一个位置参数塞进 `a.file`，`nosuchsub` 于是被当成项目路径，
> 走的是「子命令为空」那条分支。分支里那条「未知 project 子命令」提示要保持
> `a.file` 是真目录、`a.extra[0]` 是坏子命令的写法才打得出。**本版未改。**

### 2.6 打包 / 解包（pack / unpack）

| ID | 命令 | 场景 | 退出码 | 判断依据 |
|---|---|---|---|---|
| E90 | `sb pack F:\temp\ec-proj` | 缺输出文件名 | **2** | `错误：缺少输出文件名。用法：sb pack <项目目录> <输出.sb3>` |
| E91 | `sb pack <不存在目录> out.sb3` | 项目目录不存在 | **2** | `错误：项目目录不存在：…` |
| E92 | `sb pack F:\temp\ec-proj F:\temp\ec_out.sb3` | 正常 | **0** | `已打包：F:\temp\ec_out.sb3` |
| E93 | `sb unpack` | 缺参数 | **2** | `用法：sb unpack <作品.sb3> <输出目录> [--force]` |
| E94 | `sb unpack <不存在.sb3> <目录>` | 源文件不存在 | **2** | `错误：找不到文件：…` |
| E95 | `sb unpack fixture_origin.sb3 F:\temp\ec-unpA` | 正常 | **0** | `已解包：F:/temp/ec-unpA` |
| E96 | `sb unpack fixture_origin.sb3 F:\temp\ec-nonempty` | 输出目录非空且无 `--force` | **2** | `错误：输出目录已存在且非空：…（加 --force 覆盖）` |

> **E91 / E94 / E96 的「2」是 `pack`/`unpack` 自己的判定，不是 `main.cpp` 的用法错误**。
> 语义上它们更接近「输入无效（3）」和「编辑冲突（5）」，但本版**保持 2**。

### 2.7 本版未改动的命令（edit / rename / merge）

这三个命令的退出码逻辑归别的任务，本版**一行未动**，仅记录现状：

| ID | 命令 | 场景 | 退出码 |
|---|---|---|---|
| E97 | `sb edit <项目>`（无操作） | 缺 `--script-index` | **2** |
| E98 | `sb rename` | 缺参数 | **2** |
| E99 | `sb merge` | 缺参数 | **2** |

---

## 3. 未分类清单（语义拿不准 / 有权衡，一律保持原样）

| 位置 | 现状退出码 | 目标语义（如果将来要归口） | 为什么本版不动 |
|---|---|---|---|
| `check` 发现 error | **1** | 4 校验失败 | 已写进 README/format.md/--help，且五个测试套件与下游脚本按「1=有错」判断 |
| 文件不存在（info/sprites/text/…） | **1** | 3 输入无效 | 统一走 `Sb3Error` 异常 → `main()` catch 返回 1，改动面波及所有命令 |
| 压缩包损坏 | **1** | 3 输入无效 | 同上 |
| `sb json <项目目录>` | **1** | 2 或 3 | 目录被当文件读，是历史实现细节 |
| `sb ls` 扫到坏文件 | **1** | 1（通用失败） | 已是 1，无需动；但注意它是「部分成功也返回 1」 |
| `sb find` 零命中 | **0** | 0 | 与 `search` 零命中（1）不一致，但两者都属既有行为 |
| `sb search` 零命中 | **1** | 0 或 1 | 同上 |
| `view --sprite` 不存在 | **2** | 3 / 5 | 归「用法错误」也能自圆其说 |
| `project ext-info` 扩展缺失 | **2** | 6 依赖缺失 | 与其它 `project` 失败同为 2，拆开就要改 `show()` 的统一返回 |
| `project add-extension` 读不到文件 | **2** | 3 / 6 | 同上 |
| `pack` 项目目录不存在 | **2** | 3 | `pack` 内部自己返回 2（不在 `commands.cpp`） |
| `unpack` 输出目录非空 | **2** | 5 编辑冲突 | `unpack` 内部自己返回 2 |
| `blocks`/`json`/`diff`/`dup` 对 1.4 格式 | **2** | 2 用法错误 | 「目标不支持该命令」本来就归 2，属正确分类 |

---

## 4. 代码是怎么用这套常量的

`src/common.hpp`：

```cpp
enum class ExitCode : int {
    Ok = 0, Fail = 1, Usage = 2, BadInput = 3,
    CheckFailed = 4, Conflict = 5, DepMissing = 6, IoError = 7
};
// 转 int：命令返回类型是 int，避免每个 return 都写 (int)ExitCode::X
inline constexpr int exitInt(ExitCode c) noexcept { return static_cast<int>(c); }
```

> `exitInt` 不叫 `ec`：`commands.cpp` 里大量存在局部变量 `std::error_code ec`，
> 同名函数会被局部变量遮蔽，调用处直接编译不过。

`src/main.cpp` / `src/commands.cpp` 的写法：

```cpp
return (int)ExitCode::Usage;          // main.cpp：函数返回 int，直接强转
return exitInt(ExitCode::Ok);         // commands.cpp：省掉强转
```

**本版没有任何 `return exitInt(ExitCode::BadInput)` 之类的「升级」**——
`commands.cpp` 里 70 处替换全部是
`return 0;` → `return exitInt(ExitCode::Ok);`、
`return 2;` → `return exitInt(ExitCode::Usage);`，
数值一模一样，所以行为必然不变。

---

## 5. 验收怎么跑

```powershell
$env:TEMP="F:\temp"; $env:TMP="F:\temp"
cmake --build "F:\temp\sb-build2" --target sb -j 1

# 退出码矩阵（逐条跑真实命令）
pwsh -NoProfile -File F:\temp\exitcode_check.ps1

# 五套测试必须全 exit=0
foreach ($t in 'sb_parser_test','sb_parser_test2','sb_fix_test','sb_pack_test','sb_extension_test') {
    & "F:\temp\sb-build2\$t.exe" | Out-Null
    "$t => exit=$LASTEXITCODE"
}
```
