# block.sbcli 解析器设计说明

`src/sbcli_parser.hpp` / `.cpp`。契约是 [format.md](format.md)；本文只记录
**规范里没写清楚、我做决定的地方**，以及给 check / fix / pack 的接口说明。

---

## 1. 分层

```
文本 ──[sbcLex]──> SbToken 流 ──[sbcParse]──> SbcFile ──[sbcUnparse]──> 文本
```

行形态是主表示：先把源码切成物理行（保留 raw 原文，`@script` 的行参数要用），
再逐行词法化。积木不可能跨行（缩进即结构），所以逐行词法不会漏括号；
反而能让"字符串/括号没闭合"被天然限制在一行内报错，定位更准。

`sbcLex()` 是给需要 token 流的场景（比如 fix 做原地改写）准备的，
主流程走 `sbcParse()`。

---

## 2. AST 结构

```
SbcFile
  scripts: [SbcScript]          # 按 @script 切分
    hat / hatArg / line
    blocks: [SbcBlock]          # 顶层块序列
      opcode / line
      params: [SbcParam]        # 有序；key=原文名, canon=SB3 真名
        value: SbcValue         # Scalar | Reporter | List
      substacks: [[SbcBlock]]   # [0]=SUBSTACK  [1]=SUBSTACK2(否则)
      elseLine                  # 0 = 没有 else
  diags: [SbcDiag]              # severity / line / col / code / message
```

几个刻意的选择：

- **params 用 vector 而非 map**：保序（重建要用），且重复 KEY 保留两份，
  交给 check 报"重复参数"，而不是解析阶段悄悄去重。
- **SbcValue 三态**：裸标量 / reporter / 列表。pack 时能直接区分
  `VALUE=0`（数字常量，生成 shadow `[4,"0"]`）和
  `VALUE=(data_variable ...)`（块引用，生成 `[3, id, [4,"0"]]`）。
- **substacks 是二维数组**：`control_if_else` 写了 else 时 `[0]`=那么、
  `[1]`=否则；其余 opcode 最多一个元素。用 `elseLine != 0` 判断有没有 else
  （比"substacks.size()>1"可靠——空分支也会占一个元素）。
- **reporter 的 opcode 复用 `scalar.text`**：少一个字段，重建时直接拼。

---

## 3. 参数名：简写 → SB3 真名（双名制）

format.md §2 用了一批简写，与 `src/sb3_tables.hpp` 的 `SB3_T` 模板名冲突：

| opcode | format.md | SB3_T 真名 |
|---|---|---|
| `control_if` 等 5 个条件块 | `COND` | `CONDITION` |
| `operator_gt/lt/equals/and/or` | `A` / `B` | `OPERAND1` / `OPERAND2` |
| `operator_add/subtract/multiply/divide/mod` | `A` / `B` | `NUM1` / `NUM2` |
| `operator_join/contains` | `A` / `B` | `STRING1` / `STRING2` |
| `control_wait` | `SECS` | `DURATION` |

**现状（2026-10 更新）**：docs/format.md §2/§6 已由 lead 统一改成真名。
这些别名现在只用于**向后兼容**旧手写文件，新代码请直接写真名。
解析器两边都接受，对外统一给 `canon`。

- `SbcParam::key` = 源码原文（重建用，保证 round-trip 字面一致）
- `SbcParam::canon` = 归一化到 SB3 真名（check / pack 用）

映射只在"该 opcode 的 `SB3_T` 模板确实含有目标字段"时才发生，绝不凭空造字段；
`procedures_*` 不在 `SB3_T` 表里（`PROCCODE` / `ARGS` / `ARG1..N` 一律原样）。
别名静默接受，不产生诊断——这是文档演进的兼容，不是用户的错。

### 对外工具（唯一事实来源，check / pack 复用，别再抄一份）

| 函数 | 用途 |
|---|---|
| `sbcCanonKey(opcode, key)` | 简写 → 真名；无别名时原样返回 |
| `sbcAliasTable()` | 完整别名表 `{opcode, 简写, 真名}`，供提示文案 / 反查 |
| `sbcOpcodeFields(opcode)` | 该 opcode 全部合法字段名 → check 的"未知参数"判断 |
| `sbcIsKnownOpcode(opcode)` | 是否已知（含保留 opcode）→ "未收录"判断 |
| `sbcOpcodeTemplate(opcode)` | SB3_T 原始模板串（`移动 {STEPS} 步`） |

别名表带**逐条自校验**：`tests/test_parser2.cpp` 会断言每条别名的目标字段
确实存在于该 opcode 的 `SB3_T` 模板、且 `sbcCanonKey` 真能映射过去。
将来改了 `SB3_T` 而别名没跟上，测试直接挂掉，不会静默失效。

**`sbcIsKnownOpcode` 很重要**：`procedures_definition` / `procedures_call` /
`argument_reporter_*` 都不在 `SB3_T` 表里。check 若按 §5.2 字面执行
"不在表 → 报未收录"，会把自定义积木全误报，必须用这个函数。

---

## 4. 错误处理

- **不抛异常**。`sbcParse()` 外层兜一层 `catch` 转 `E_PARSE`，保证任何输入
  都能拿回一个（可能残缺的）AST。check 因此可以一次报全部错误，
  而不是"修一个、跑一次、再冒出下一个"。
- 每条诊断带 **稳定 code**（`E_UNCLOSED_STRING` / `W_ODD_INDENT` …），
  测试按 code 断言，不依赖中文文案。
- 解析**尽量不丢内容**：遇到坏行记录错误后继续，能救回来的块照样进 AST。

错误码（`E_` = Error，`W_` = Warning）：

| code | 触发 |
|---|---|
| `E_UNCLOSED_STRING` / `W_BAD_ESCAPE` | 引号没闭合 / 未知转义 |
| `E_UNCLOSED_PAREN` / `E_UNCLOSED_ANGLE` | `()` `<>` 没闭合，或括号类型混用 |
| `E_UNCLOSED_BRACKET` / `E_NESTED_LIST` | 列表没闭合 / 列表里嵌了列表或 reporter |
| `E_VALUE_EXPECTED` / `E_PARAM_EXPECTED` | 值位置没值 / 参数没写成 `KEY=值` |
| `E_BAD_INDENT` / `W_ODD_INDENT` / `W_TAB_INDENT` | 缩进回落不匹配 / 非 2 的倍数 / 用了 Tab |
| `E_ELSE_COND` / `E_ELSE_INDENT` / `E_ELSE_ORPHAN` / `E_ELSE_TRAILING` | else 各类错误用法 |
| `E_HAT_UNKNOWN` / `E_HAT_MISSING` / `E_HAT_ARG_MISSING` | `@script` 帽子问题 |
| `E_EMPTY_REPORTER` / `E_DEEP_REPORTER` / `E_QUOTED_OPCODE` | reporter 问题（>100 层防爆） |
| `E_NOT_A_BLOCK` / `E_DIRECTIVE_UNKNOWN` / `W_NO_SCRIPT_HEADER` | 行首不是积木 / 未知 `@` 指令 / 缺 `@script` |

---

## 5. 重建（unparse）

`sbcUnparse()` 输出缩进统一 2 格的规范文本，**语义无损**（再解析回来 AST 相同），
但**不保留注释和原始空格**——AST 不存注释，fix 回写会丢掉手写注释。

引号策略是这里最容易出错的地方，规则是：

> 重建是否加引号，取决于 **token 原本是否带引号**（`quoted`），
> 而不是"内容看起来像不像数字"。

因为 `MESSAGE="123"`（看起来是数字、其实要当文本）的引号是用户明确写的，
必须保留；反之 `VALUE=0` 若加上引号就变成字符串常量，语义直接变了。
只有"内容含空白/分隔符、或为空串"时才**强制**加引号（否则会被切成多个 token）。

自测里有两条不变量断言专门盯着这个（`VALUE=0` 不加引号、`MESSAGE="123"` 带引号）。

---

## 6. 规范里没写清、我做了决定的地方

1. **`KEY=值` 连写时的切分**：`MESSAGE="你好"` 中间无空格，词法上要能正确
   切成 `PARAM(MESSAGE)` + `STRING(你好)`。实现是裸词允许吃到 `=`，
   然后在第一个 `=` 处切开、把后半段插回缓冲区重新词法化。
2. **`COLOR=#ff0000`**：`#` 紧跟在 `KEY=` 后面时**是值不是注释**。
   但值里不能有空格（否则 `#` 之后算注释）。更稳的写法是 `COLOR="#ff0000"`。
3. **缩进宽容度**：缩进只会"变浅"时结束一层；"突然变深"按新一层子栈处理，
   不报错。是否合法（比如 `looks_hide` 不该有子栈）留给 check 判断——
   解析器不知道哪些 opcode 有 SUBSTACK，报了就容易误报。
4. **缺 `@script` 头**：归到一个隐式脚本并给 `W_NO_SCRIPT_HEADER`（警告，非错误），
   这样手写片段也能解析，不硬卡。
5. **`sensing_touchingcolor` 的 `COLOR`**：按 §2 是裸写 `#ff0000`，
   但见上条——解析器两种都接受。
6. **UTF-8 路径**：`sbcParseFile()` 在 Windows 下把路径转 wide 再打开，
   与 `src/common.cpp` 的 `wpath()` 一致，否则中文目录打不开。
   注意 MinGW 的 `ifstream` 没有 `wstring` 重载，要传 `const wchar_t*`。

---

## 7. 自测

```
build\sb_parser_test.exe    # 真实样例 + 11 条负例 + round-trip
build\sb_parser_test2.exe   # 边界用例 + 类型保真 + 别名表自校验 + SB2 白名单
```

两条都是退出码 0 = 全通过。真实样例用
`D:\文档\scratch作品\character\1\block.sbcli`，解析出 3 个脚本、0 条诊断。

---

## 8. sb fix（发现即声明）

`src/sbcli_fix.cpp`，docs/format.md §4。

**fix 只写 meta.sbcli，绝不碰 block.sbcli**（手写源文件 + 重建丢注释，见 §5）。
结构类问题（procedures_call 缺 ARG、else 缺分支）只报告，交给人改。

### 登记规则

| 引用 | 去向 |
|---|---|
| `VARIABLE=` | 角色 meta `variables`；**舞台脚本里 → 根 meta** `[variables]` |
| `LIST=` | 同上（角色 `lists` / 根 `[lists]`） |
| `BROADCAST_INPUT=` / `BROADCAST_OPTION=` | 一律根 meta `[broadcasts]` |
| `COSTUME=` | 角色 meta `costumes`（舞台不登记——舞台用 BACKDROP） |
| `SOUND_MENU=` | 角色 meta `sounds` |

- 变量初值 = 第一次 `data_setvariableto` 的 VALUE（§4.3）
- 素材：文件在 `assets/` 就自动接上路径；**不存在就报错，不臆造**
- `TOUCHINGOBJECTMENU=` 是角色名不是造型，不登记

### 写回策略

已存在的 meta **保留原顺序、原值**，只重写 5 个集合字段；缺失的 §3.2
属性才补默认值。手写的 `x: 42`、`rotation_style: left_right` 不会被覆盖。

写盘条件：文件不存在 / 有新登记 / 缺默认属性，三者之一。
第三条不能漏——否则"只缺属性、没有新引用"的角色永远补不齐。

**幂等**：再跑一次 0 新增、文件内容不变（有测试盯着）。

### id：不落盘

`sbcVarId(scope, name)` / `sbcBroadcastId(name)` 是纯函数（FNV-1a），
同名同 scope 必得同 id。meta 格式没有 id 字段，硬塞一段会动公共格式、
连带影响 check 的 meta 解析；改成纯函数后 fix 与 pack 调同一个即可。

### 自测

```
build\sb_fix_test.exe   # 35 条，临时目录里搭项目，不碰真项目
```

覆盖：变量/广播登记、舞台变量归根、初值、幂等、手写字段保留、
缺素材报错且不臆造、素材存在时接路径、id 确定性。
