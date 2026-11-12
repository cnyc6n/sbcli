# sbcli 项目格式规范（block.sbcli / meta.sbcli）

本文档是 sbcli 编辑能力的契约。AI/人用文本编辑器写 `block.sbcli`（脚本积木），
`sb check` 验证语法，`sb fix` 补齐结构并自动登记元数据，`sb pack` 打包回 .sb3。

## 0. 目录结构

```
项目目录/
  meta.sbcli           # 项目级元数据（全局变量/列表/广播索引）  ← sb project init 创建
  character/
    stage/             # 舞台（固定存在）
      meta.sbcli       # 舞台属性                              ← sb project init 创建
      block.sbcli      # 舞台脚本（可选，需要就自己建）           ← 手写（init 不创建）
    {id}/              # 角色，id 从 1 递增
      meta.sbcli       #                                        ← sb project add-sprite 创建
      block.sbcli      #                                        ← 手写
  assets/              # 造型/声音文件（引用指向这里）              ← sb project add-costume/sound 放入
  extensions/          # 扩展源码（可选）<id>.js                    ← sb project add-extension / sb unpack 放入
```

- `character/stage` 固定是舞台；其余角色用数字目录 `character/1/`、`character/2/`…
- **手写的只有 `block.sbcli`**（含舞台的——`stage/block.sbcli` 需要脚本时自己建）；
  `meta.sbcli` 由 `sb project init` / `sb add-*` 创建、`sb fix` 自动维护，**不要手改**。
- `assets/` 里的素材由 `sb project add-costume` / `add-sound` 放入并登记。
- `extensions/` 里的扩展源码由 `sb project add-extension` 放入，或 `sb unpack`
  从 .sb3 的内嵌 `data:` URL 解码导出（见 §8）。

### 0.1 从零建项目的命令流水

```
sb project init <目录> [项目名]                    # 建 meta.sbcli + character/stage/ + assets/
sb project add-sprite <项目> <角色名>               # 建 character/{下一个id}/ + 角色 meta
sb project add-costume <项目> <角色> <文件> <造型名>  # 拷素材进 assets/ + 登记 costumes
sb project add-sound <项目> <角色> <文件> <声音名>    # 同上，登记 sounds
sb project add-variable <项目> <名字> <初值> [角色]   # 省略角色 = 全局（根 meta）
sb project add-list <项目> <名字> <元素...> [角色]    # 同上
sb project add-broadcast <项目> <名字>              # 广播一律登记根 meta
# ↓ 此后手写各 character/{id}/block.sbcli
sb check <项目>      # 语法/结构/引用检查（有错返回码 1）
sb fix <项目>        # 补齐 + 把脚本引用到的名字自动登记进 meta（幂等）
sb pack <项目> <输出.sb3>   # 打包回 .sb3
sb view <项目>       # 浏览（角色清单 + 脚本中文）
```

---

## 1. block.sbcli 词法规则

### 1.1 注释
- `#` 开头是注释（整行或行尾都行），与 Python 一致
- **引号内的 `#` 不是注释**（先解析字符串，再处理注释）

### 1.2 字符串
- 简单标识符（无空格、无 `=`、无 `"`、无 `#`）**可省略引号**裸写
- 否则**必须用双引号**包起来
- **中文文本一律推荐加引号**（`MESSAGE="你好"`），即使无空格——避免与变量名/枚举值混淆
- 转义：`\"` 引号、`\\` 反斜杠、`\n` 换行、`\t` 制表
```
VARIABLE=分数                # 英文/简单 → 裸写
MESSAGE="你好 世界"          # 含空格 → 引号
MESSAGE="你好"               # 中文 → 引号（推荐）
MESSAGE="他说 \"你好\""     # 转义引号
```

### 1.2.1 变量/列表的作用域前缀（同名消歧）

Scratch 允许**舞台与角色各有一个同名变量**。存在这种冲突时，光写名字无法确定用哪个，
用作用域前缀指定：

```
data_setvariableto VARIABLE=@local:血条 VALUE=1     # 强制：当前角色的私有 血条
data_setvariableto VARIABLE=@global:血条 VALUE=2    # 强制：舞台的全局 血条
data_changevariableby VARIABLE=血条 VALUE=1         # 无前缀：按下面的隐式规则
```

**名字含空格/特殊字符**时，前缀后跟**引号字符串**（两者都可解析）：
```
data_setvariableto VARIABLE=@local:"Stage: ii" VALUE=1   # 含空格与冒号的名字
data_changevariableby VARIABLE=@global:"我的 变量" VALUE=1
```

**隐式规则**（不写前缀时）：
1. 名字在**当前角色 meta 里声明过** → 用角色的私有变量
2. 否则名字在**根 meta 声明过** → 用全局变量
3. 都没有 → 兜底用当前角色作用域（`sb fix` 会先把它登记好）

**何时需要写前缀**：只有同名冲突存在时。`sb unpack` 检测到冲突会**自动**给引用加前缀
（含空格的名字会输出为 `@local:"名"` 形式）；无冲突的文件保持裸名不受影响。
列表同理（`@local:名` / `@global:名`）。

> 前缀只对**值位置**生效：裸标识符 `@local:名` 或引号 `@local:"名"` 都识别；
> 引号内的 `@`（如普通文本 `MESSAGE="@..."`）是普通字符，不解析为前缀。

### 1.3 数字 / 布尔 / 列表
- 数字裸写：`10`、`-5`、`0.5`
- 布尔裸写：`true` / `false`
- 列表用 `[a, b, "c"]`（只允许标量，**不支持嵌套列表**）

### 1.4 块行 = opcode + 命名参数
```
<opcode> KEY1=value1 KEY2=value2
```
- opcode 用 **Scratch 3 原生英文名**（如 `event_whenflagclicked`、`motion_move`）
- 参数名用 **Scratch 3 字段名**（VARIABLE/VALUE/COSTUME/STOP_OPTION…），
  与 sb3 的 fields/inputs 键一一对应
- 参数顺序无关；缺参数由 `sb check` 报告

### 1.5 reporter 嵌套
- `(...)` = 普通 reporter（数值/文本）
- `<...>` = 布尔 reporter（条件）
- 括号/尖括号**递归配对**；参数值可以是 reporter，reporter 的参数里也能再嵌 reporter
```
control_if COND=<operator_gt A=(data_variable VARIABLE=分数) B=100>
```

### 1.6 脚本分隔：@script 行
```
@script flag                     # 绿旗帽子
@script broadcast 游戏开始        # 广播接收（第一个 token 是类型，剩余整行是参数）
@script key space                # 按键
@script clone                    # 克隆
@script click                    # 角色被点击
```
- `@script` 后**第一个空格切分**：第一个 token = 帽子类型，**剩余整行 trim 后**是参数
- 帽子类型固定词：`flag` / `broadcast` / `key` / `clone` / `click`
- 广播名/键名是后续独立 token，含空格/`-` 无歧义

### 1.7 缩进 = 嵌套
- 缩进表示嵌套块（SUBSTACK），用**空格**（建议 2 格）
- 缩进栈严格匹配，错缩进报语法错误
- `control_if_else` 的第二个分支用 `else` 行分隔（缩进与 `control_if_else` 同级）
```
control_if_else COND=A
  looks_say MESSAGE="对" SECS=1
else
  looks_say MESSAGE="错" SECS=1
```
- `else` 属于**最近一个缩进层级相同、未闭合的 control_if_else**
- **`else COND=...` 是语法错误**（Scratch 不支持 else if）

---

## 2. 常用 opcode 与参数（块行参考）

> 完整模板用 `sb search <关键词>` 查询；这里列高频的。

### 事件
```
event_whenflagclicked                                  # 绿旗（无参数，@script flag 后）
event_whenbroadcastreceived BROADCAST_OPTION=游戏开始
event_whenkeypressed KEY_OPTION=space
event_whenthisspriteclicked
event_broadcast BROADCAST_INPUT=游戏开始
event_broadcastandwait BROADCAST_INPUT=游戏开始
```

### 动作
```
motion_movesteps STEPS=10
motion_turnright DEGREES=15
motion_glidesecstoxy SECS=0.5 X=0 Y=0
looks_say MESSAGE="你好"
looks_sayforsecs MESSAGE="你好" SECS=2
looks_switchcostumeto COSTUME=造型1       # 造型名（字符串）或编号（数字）
looks_hide
looks_show
sound_play SOUND_MENU=音效1
```

> 注：opcode 一律用 **Scratch 3 原生名**（`motion_movesteps`，不是 `motion_move`）。
> 参数名也用 **SB3 真实字段名**（见下）。不确定时用 `sb search <中文概念或 opcode>` 查准确 opcode 与参数。
> **秒数字段名因 opcode 而异**：`control_wait` 用 `DURATION`，`looks_sayforsecs` 用 `SECS`，
> `motion_glidesecstoxy` 也用 `SECS`——以 `sb search <opcode>` 给出的模板为准。
> `looks_say` **没有** SECS 参数（要定时请用 `looks_sayforsecs`），多写会被 check 判为"值会被丢弃"。

### 控制
```
control_wait DURATION=1
control_repeat TIMES=10
control_forever
control_if CONDITION=<...>
control_if_else CONDITION=<...>
control_stop STOP_OPTION=all             # all / this script / other scripts in sprite
control_create_clone_of CLONE_OPTION=_myself_
```

### 数据
```
data_setvariableto VARIABLE=分数 VALUE=0
data_changevariableby VARIABLE=分数 VALUE=1
data_variable VARIABLE=分数              # reporter，读变量
data_addtolist LIST=道具 ITEM=苹果
data_deleteoflist LIST=道具 INDEX=1
data_itemoflist LIST=道具 INDEX=1        # reporter，取列表项
data_lengthoflist LIST=道具              # reporter，列表长度
```

### 运算（reporter）
```
operator_add NUM1=... NUM2=...
operator_subtract NUM1=... NUM2=...
operator_gt OPERAND1=... OPERAND2=...
operator_lt OPERAND1=... OPERAND2=...
operator_equals OPERAND1=... OPERAND2=...
operator_and OPERAND1=<...> OPERAND2=<...>
operator_or OPERAND1=<...> OPERAND2=<...>
operator_not OPERAND=<...>
operator_random FROM=1 TO=10
operator_join STRING1=... STRING2=...
operator_letter_of LETTER=1 STRING=...
operator_length STRING=...
```

### 侦测
```
sensing_touchingobject TOUCHINGOBJECTMENU=苹果    # 角色名 / _mouse_ / _edge_
sensing_touchingcolor COLOR=#ff0000
sensing_keypressed KEY_OPTION=space
sensing_distanceto DISTANCETOMENU=_mouse_
sensing_timer
```

### 自定义积木（procedures）
```
# 定义（无参）——必须放 @script flag 或独立脚本里
procedures_definition PROCCODE="加速" ARGS=[]
  data_changevariableby VARIABLE=速度 VALUE=2

# 定义（带参）：%s = 字符串/数字参数，%n = 数字，%b = 布尔
procedures_definition PROCCODE="移动 %s 步" ARGS=[步数]
  argument_reporter_string_number VALUE=步数
  motion_movesteps STEPS=(argument_reporter_string_number VALUE=步数)

# 调用（不写 ARGS，用 ARG1..N 对应占位符顺序）
procedures_call PROCCODE="移动 %s 步" ARG1=10
```
- `procedures_call` 可出现在**任何块位置**（顶层或嵌套内，如 control_if 里）
- 一个文件可有**多个同帽子 @script**（如两个 `@script flag`：一个装主逻辑、一个装定义），各自独立
- `procedures_definition` 内必须至少一个 `procedures_definition` 对应的定义体（其下的块在定义被调用时执行）
- **参数名含空格/特殊字符**时在 `ARGS=[...]` 里加引号（列表元素规则，见 §1.3）：
  ```
  procedures_definition PROCCODE="Get Mouse Turn %s" ARGS=["mouse x"]
  ```
  否则会被当两个参数（`mouse` + `x`），导致 definition 的参数名错乱。

---

## 3. meta.sbcli 规范

### 3.1 项目级（根 meta.sbcli）
```
name: 接水果
author: 作者名              # 可选
description: 接水果小游戏    # 可选
platform: TurboWarp        # 可选：Scratch / TurboWarp / Gandi（空 = 不写 platform）
agent: sbcli               # 可选：导出工具标识（空 = 默认 scratch-www）
notes: 本地备注             # 可选：只留在本地，不写入 sb3

[extensions]
yc6ncolormaster = extensions/yc6ncolormaster.js      # 本地源码（相对路径）
Encoding = https://extensions.turbowarp.org/encoding.js   # 在线 URL

[variables]
分数 = 0
速度 = 1
角色名 = "玩家"

[lists]
道具 = [苹果, 香蕉]

[broadcasts]
游戏开始
游戏结束
```
- 变量带初始值（数字/引号字符串/true/false）；列表带初始内容
- 广播只有名字——id 由 `sb fix` 在 pack 时统一生成
- 元数据字段（`author` / `description` / `platform` / `agent` / `notes`）**均可选**：
  写了才输出，删掉即不写入 sb3。pack 时 `name`/`author`/`description`/`platform`/`agent`
  映射进 sb3 的 `meta` 段，`notes` 是纯本地备注**不写入 sb3**。
  `platform` 的合法值：`Scratch` / `TurboWarp` / `Gandi`（见 §8.3）。
- `[extensions]` 段：`扩展id = 相对路径 | URL`，见 §8。格式与变量段一致：
  值含 `=` 等特殊字符时加引号（URL 可能含 `=`，解析只切第一个）。

### 3.1.1 sb project init 生成的模板注释

`sb project init` 生成的根 meta.sbcli 自带一段注释说明（元数据字段均可选）：
```
# <项目名> —— sbcli 项目定义
# 元数据字段均可选，删掉即不写入 sb3：
#   name         项目名（写入 sb3 meta.name 由平台决定）
#   author       作者
#   description  项目描述
#   platform     目标平台：Scratch / TurboWarp / Gandi
#   agent        导出工具标识（默认留空＝不写）
#   notes        本地备注（不写入 sb3）
```

### 3.2 角色级（character/{id}/meta.sbcli）
```
name: player
is_stage: false
x: 0
y: 0
size: 100
direction: 90
visible: true
current_costume: 1              # 当前造型（编号或名字）
rotation_style: all_around      # all_around / left_right / dont_rotate
layer_order: 1                  # 图层顺序
draggable: false                # 可拖动
costumes: [造型1: assets/p1.svg, 造型2: assets/p2.svg]
sounds: [音效1: assets/s1.mp3]
variables: {局部血量 = 100}      # 角色级变量（带初始值）
lists: {}
broadcasts: []
```

### 3.3 类型推断
- 数字裸写 → number
- 引号字符串 → string
- `true`/`false` → bool
- `[...]` → list（仅标量）
- **空值语义**（round-trip 保真关键）：
  - `VALUE=""`（带引号）→ 空**文本**槽 `[10,""]`（渲染 ∅）
  - `VALUE=` 后**裸空**（无值）→ 空**数字**槽 `[4,""]`（渲染空白，如 `( + 180)` 的 NUM1）
- 解析器**不做类型转换**：`COSTUME=1` 是数字 1，`COSTUME=造型1` 是字符串

### 3.4 广播的作用域

**广播是全局唯一的**（Scratch 没有私有广播）。因此：
- 广播只在**根 meta** 的 `[broadcasts]` 声明；`sb fix` 只往根 meta 登记
- 角色 meta 的 `broadcasts` 字段**不表示声明**——留空即可（`sb unpack` 输出 `[]`）
- 脚本里的 `event_broadcast BROADCAST_INPUT=名` / `event_whenbroadcastreceived BROADCAST_OPTION=名`
  只是引用，不需要、也不应该在角色 meta 重复登记

---

## 4. sb fix 的"发现即声明"规则

agent 只需写 block.sbcli，meta 自动维护：

1. 扫描 block.sbcli 里的 `VARIABLE=`/`LIST=`/`BROADCAST_*=`/`COSTUME=`/`SOUND_MENU=` 引用
2. 引用名在 meta 未声明 → 自动登记：
   - 变量/列表 → 角色 meta 或根 meta（变量出现在舞台脚本 → 根 meta）
   - 广播 → 根 meta [broadcasts]
   - 造型/声音 → 角色 meta costumes/sounds（缺素材文件则报错）
3. 变量初始值 = 第一次 `data_setvariableto` 的 VALUE（若未在 meta 显式声明）

**作用域与去重**：变量/列表先看根 meta（全局）。**已全局声明的名字，fix 不再往角色 meta 建同名条目**
（避免隐式影子）；只有非全局的名字才落到角色 meta。
- 如果你**确实想要**一个与全局同名的角色私有变量，用
  `sb project add-variable <项目> <名> <初值> <角色id>` **显式声明**——显式声明优先，
  之后脚本里用 `@local:名` 引用它。
- `sb pack` 的作用域解析（§1.2.1）：显式前缀 > 角色声明（局部优先）> 全局 > 兜底当前角色。
- 无前缀且无冲突时，行为与 1.0 一致（全局优先）。

**广播**：广播是**全局唯一**的（Scratch 没有私有广播）。角色 meta 的 `broadcasts` 字段
**不表示声明**，`fix` 只把广播登记到根 meta；角色 meta 该字段保持为空。

### 4.1 实现说明（sb fix）

- fix **只写 meta.sbcli，绝不改 block.sbcli**（手写源文件；且重建会丢注释，
  见 [parser-notes.md](parser-notes.md) §5）。结构类问题只报告，交给人改。
- **输出形态**：逐条 `文件级：提示 [registered-variable] 变量「X」登记到角色 meta` /
  `[registered-broadcast] 广播「Y」登记到根 meta`；没有新引用时输出「没有需要登记的新引用」。
- 已存在的 meta 保留原顺序与原值，只重写 costumes/sounds/variables/lists/
  broadcasts 五个集合字段；缺失的 §3.2 属性才补默认值。
- 素材文件不在 `assets/` 时**报错而非臆造路径**。
- 变量/广播的 id 由 `sbcVarId(scope,name)` / `sbcBroadcastId(name)` 纯函数生成，
  **不落盘**（meta 格式没有 id 字段；fix 与 pack 调同一个函数即一致）。
- 幂等：重复运行不新增条目、不改变文件内容。

---

## 5. sb check 检查项（实现清单）

### 5.1 词法
- 引号配对、转义合法
- 未闭合的 `(...)` / `<...>`
- 缩进：建议 2 格（任意宽度均可）；制表符 = 错误；宽度为奇数 = 警告；
  缩进层级回不到已打开的层级 = 错误（缩进栈严格匹配）

### 5.2 语法
- opcode 不在 SB3_T/SB2_T 表 → 报"未收录"（**例外：`procedures_definition`/`procedures_call` 是保留 opcode，不报**）
- 命名参数名不在该 opcode 模板 → 报"未知参数"（别名表内的简写如 `COND`→`CONDITION` 静默通过）
- 必填参数缺失（如 `data_setvariableto` 缺 VALUE）→ 报"缺参数"
- `else` 缩进不匹配 / `else COND=` → 报错
- `@script` 帽子类型非法 → 报错

### 5.3 结构
- `procedures_call` 的 ARG 数量 vs proccode 占位符数不一致 → 报错
- 重复的 `procedures_definition` → 报错
- reporter 嵌套超过 100 层 → 报错（防爆）

### 5.4 引用
- 变量/列表/广播名未在 meta 声明 → 警告（可 fix 自动登记）
- 造型/声音引用的文件在 assets 不存在 → 报错
- 广播只有发送没接收 / 只有接收没发送 → 警告（孤儿广播）

---

## 6. 完整示例

### character/1/block.sbcli
```
# ===== 角色 1（player）脚本 =====
@script flag
event_whenflagclicked
data_setvariableto VARIABLE=分数 VALUE=0
looks_switchcostumeto COSTUME=造型1
control_forever
  motion_glidesecstoxy SECS=0.5 X=0 Y=0
  control_if CONDITION=<operator_gt OPERAND1=(data_variable VARIABLE=分数) OPERAND2=100>
    event_broadcast BROADCAST_INPUT=游戏结束
    control_stop STOP_OPTION=all

@script broadcast 游戏开始
event_whenbroadcastreceived BROADCAST_OPTION=游戏开始
control_repeat TIMES=10
  data_changevariableby VARIABLE=速度 VALUE=1
  control_if_else CONDITION=<sensing_touchingobject TOUCHINGOBJECTMENU=苹果>
    data_changevariableby VARIABLE=分数 VALUE=10
    looks_sayforsecs MESSAGE="好耶" SECS=1
  else
    data_changevariableby VARIABLE=分数 VALUE=-5
    looks_sayforsecs MESSAGE="接住!" SECS=0.5

@script key space
event_whenkeypressed KEY_OPTION=space
procedures_call PROCCODE="移动 %s 步" ARG1=10

@script flag
procedures_definition PROCCODE="移动 %s 步" ARGS=[步数]
  argument_reporter_string_number VALUE=步数
  motion_movesteps STEPS=(argument_reporter_string_number VALUE=步数)
```

### character/1/meta.sbcli
```
name: player
is_stage: false
x: 0
y: 0
size: 100
direction: 90
visible: true
current_costume: 1
rotation_style: all_around
layer_order: 1
draggable: false
costumes: [造型1: assets/p1.svg, 造型2: assets/p2.svg]
sounds: []
variables: {}
lists: {}
broadcasts: []
```

### 根 meta.sbcli
```
name: 接水果

[variables]
分数 = 0
速度 = 1

[lists]
道具 = [苹果, 香蕉]

[broadcasts]
游戏开始
游戏结束
```

---

## 附录 A：旧写法兼容（参数名简写）

> **本附录只是存档。** 正文 §2/§6 用的都是 SB3 真实字段名（CONDITION /
> OPERAND1 / NUM1 / DURATION …），**新写的代码请一律用真名**。
>
> 早期版本的本文档用过一批简写参数名。解析器**仍然接受**这些简写，
> 以便旧手写文件不用改；它们会被静默归一化成真名，不报错、不警告。
> 这张表的唯一事实来源是代码里的 `sbcAliasTable()`
> （见 `src/sbcli_parser.cpp`），本附录与它同步，改动请一并更新。

| opcode | 旧简写 | 现用真名 |
|---|---|---|
| `control_if` | `COND` | `CONDITION` |
| `control_if_else` | `COND` | `CONDITION` |
| `control_wait_until` | `COND` | `CONDITION` |
| `control_repeat_until` | `COND` | `CONDITION` |
| `control_while` | `COND` | `CONDITION` |
| `operator_gt` | `A` / `B` | `OPERAND1` / `OPERAND2` |
| `operator_lt` | `A` / `B` | `OPERAND1` / `OPERAND2` |
| `operator_equals` | `A` / `B` | `OPERAND1` / `OPERAND2` |
| `operator_and` | `A` / `B` | `OPERAND1` / `OPERAND2` |
| `operator_or` | `A` / `B` | `OPERAND1` / `OPERAND2` |
| `operator_add` | `A` / `B` | `NUM1` / `NUM2` |
| `operator_subtract` | `A` / `B` | `NUM1` / `NUM2` |
| `operator_multiply` | `A` / `B` | `NUM1` / `NUM2` |
| `operator_divide` | `A` / `B` | `NUM1` / `NUM2` |
| `operator_mod` | `A` / `B` | `NUM1` / `NUM2` |
| `operator_join` | `A` / `B` | `STRING1` / `STRING2` |
| `operator_contains` | `A` / `B` | `STRING1` / `STRING2` |
| `operator_not` | `COND` | `OPERAND` |
| `control_wait` | `SECS` | `DURATION` |

共 31 条映射。实现细节与 AST 的双名制（`SbcParam::key` 原文 +
`SbcParam::canon` 归一化名）见 [parser-notes.md](parser-notes.md) §3。

> 另注：`procedures_definition` / `procedures_call` /
> `argument_reporter_string_number` / `argument_reporter_boolean` **不在**
> `SB3_T` 翻译表里，但它们是合法 opcode。`sb check` 判断"opcode 是否收录"
> 时必须用 `sbcIsKnownOpcode()`，不能直接查表，否则自定义积木会被误报。

---

## 7. 未知积木的处理规则

`sb unpack` 会把 .sb3 拆成项目目录，`sb pack` 再打包回去。遇到 `SB3_T`/`SB2_T` 表里没有的
opcode（扩展积木、新版积木、别人的自定义块）时，工具链的规则是 **原样保留，不丢弃**：

| 环节 | 行为 |
|---|---|
| `sb unpack` | 块行原样写出 `opcode KEY=value …`（含全部 fields/inputs 键值），行前加注释 `# 未知积木: <opcode>` |
| `sb check` | 报**警告**「opcode 未收录」（`W_UNKNOWN_OPCODE`），**不阻断**、不影响退出码判定为"有错误"以外的流程 |
| `sb pack` | 按原样的键值重建该块，写回 .sb3；打包末尾汇总提示「遇到未知 opcode（已照原样写入）」 |
| 嵌套 | 未知块内部的 SUBSTACK / reporter 引用同样按通用规则递归处理 |

**原则**：*不支持 ≠ 会坏* —— 未知积木只是"不翻译成中文"，结构上无损保留。
这样 `unpack → pack` 才是**无损往返**（唯一会变的是块 id 与脚本坐标，Scratch 打开时自动重排）。

> 注意：未知积木的参数名不会被别名归一化，也不做类型推断——
> 原样进出，适合"先拆开看看、原封不动装回去"的备份场景。
> 要真正理解某个扩展积木的语义，需要人工补 `SB3_T` 模板条目。
>
> **例外**：项目里 `extensions/<id>.js` **已注册**的扩展积木不算未知
> （`sb check` 加载源码识别它们、`sb pack` 原样写回），见 §8。

### 7.1 menu 影子块（`*_menu`）的展开

Scratch 里很多**菜单字段**（`KEY_OPTION`/`COSTUME`/`TOUCHINGOBJECTMENU` 等）在 .sb3 中
不是内联值，而是引用一个 `*_menu` 影子块（如 `sensing_of_object_menu`、`pen_menu_colorParam`）。
`sb unpack` 会**沿引用展开**这些 menu 块、直接输出菜单字段值，而不是保留块结构：

```
# unpack 前（.sb3 内）：COLOR_PARAM 引用 pen_menu_colorParam 块
# unpack 后（block.sbcli）：
pen_changePenColorParamBy COLOR_PARAM="brightness" VALUE=...
```

这样写手看到的是**可读的菜单值**而非块引用。`sb pack` 重建时按普通字段写回
（内联值，语义等价）。**已知边界**：个别扩展 menu 块在原始 .sb3 的渲染上可能与
重打包后不同（数据无损，仅显示形态差异）。

---

## 8. 扩展（extensions）

TurboWarp/Gandi 生态允许作品引用**第三方扩展**（如 `yc6ncolormaster`、
`Encoding`）。扩展以「源码」形式存在：`.sb3` 里要么**内嵌**（`data:` URL 存源码），
要么**在线引用**（`https://extensions.turbowarp.org/xxx.js`）。sbcli 把它们拆成
独立的 `extensions/<id>.js` 源码文件，并在根 meta 的 `[extensions]` 段登记。

### 8.1 extensions/ 目录约定

- 每个扩展一个文件：`extensions/<id>.js`，`<id>` 取扩展 `getInfo().id`
  （不是文件名，add-extension 时由源码解析出来）。
- **手写不用碰这里**：`sb project add-extension` 负责放文件 + 登记；
  `sb unpack` 从 .sb3 内嵌 `data:` URL **解码落盘**。
- 扩展积木在脚本里的 opcode 是 `扩展id_积木opcode`（如 `yc6ncolormaster_adjustBrightness`、
  `Encoding_encode`），参数名**原样取自 getInfo() 的 arguments 键**（大小写敏感，见 §8.5）。

### 8.2 注册：sb project add-extension / remove-extension

```
sb project add-extension <项目> <扩展.js | URL>
sb project remove-extension <项目> <扩展id> [--keep-file]
```

| 命令 | 行为 |
|---|---|
| `add-extension` + 本地 `.js` 文件 | 读源码 → 交给 node 工具解析 `getInfo()`（语法检查 + 取 id）→ 复制到 `extensions/<id>.js` → meta `[extensions]` 登记 `id = extensions/<id>.js` |
| `add-extension` + `https://` URL | 下载源码 → 解析校验 → 存为 `extensions/<id>.js` 并登记相对路径 |
| `add-extension` + `data:` URL | 解码 → 解析校验 → 存为 `extensions/<id>.js` 并登记相对路径 |
| `remove-extension <id>` | 从 meta `[extensions]` 摘掉登记；默认同时删除 `extensions/<id>.js`（`--keep-file` 保留源码文件） |

- 重复登记同一扩展：重新解析校验，不报错（幂等）。
- 解析失败（语法错误 / `getInfo()` 抛错 / 没有合法 id）→ 报错、不落盘。
- 只**登记在线 URL**（不想下载源码）怎么办：直接编辑 meta `[extensions]` 写
  `id = https://...`，或者从 .sb3 unpack（见 §8.4）——unpack 对 `https:` 引用
  **原样登记 URL、不下载源码**。

### 8.3 元数据字段（根 meta 顶部）

| 字段 | 含义 | pack 时写入 sb3 |
|---|---|---|
| `author` | 作者 | `meta.author` |
| `description` | 项目描述 | `meta.description` |
| `platform` | 目标平台：`Scratch` / `TurboWarp` / `Gandi` | `meta.platform = {name, url}`（空则不写） |
| `agent` | 导出工具标识 | `meta.agent`（空 = 默认 `scratch-www`） |
| `notes` | 本地备注 | **不写入**，仅本地文档 |

`platform` 的 URL 映射：`TurboWarp → https://turbowarp.org/`、
`Gandi → https://getgandi.com/`、`Scratch → https://scratch.mit.edu/`。

### 8.4 [extensions] 段格式与 round-trip 语义

`[extensions]` 段每行 `扩展id = 相对路径 | URL`（`=` 只切第一个，URL 里的 `=` 安全）：

```
[extensions]
yc6ncolormaster = extensions/yc6ncolormaster.js      # 本地源码
Encoding = https://extensions.turbowarp.org/encoding.js   # 在线 URL
```

**round-trip（本地文件 ↔ sb3 的 data: URL）逐字节无损**：

| 环节 | 行为 |
|---|---|
| `sb unpack` | `extensionURLs[id]` 是 `data:application/javascript,...` → 解码 → 落盘 `extensions/<id>.js`，meta 登记**相对路径**；是 `https:` → meta 原样登记 URL（不下载、不落盘） |
| `sb pack` | 读 meta `[extensions]`：相对路径 → 读 `extensions/<id>.js` → `encodeURIComponent` 编码为 `data:application/javascript,<urlencoded>` 写入 `extensionURLs`；URL 原样写入 |
| 产出 | sb3 顶层 `extensions` 数组列出全部 id；`extensionURLs` 存 id → URL；`meta.platform` 写目标平台 |

- **源码逐字节一致**：本地文件 ↔ data URL 互相转换不丢字节
  （pack 读取时会剥掉文件末尾一个 `\n`，与 unpack 逐行落盘补换行的行为对称）。
- **meta 是唯一依据**：pack 只打包 `[extensions]` 里登记的扩展；
  `extensions/` 下有文件但没登记 → 不打包。移除扩展用
  `sb project remove-extension <项目> <id>`（删 meta 条目 + 删文件，两边同步）；
  残留文件（meta 没登记）不会被 pack 采纳。
- `extensions` 列表有、`extensionURLs` 没有的 id（如只有名单的旧作品）：
  unpack 会登记 `id = `（空值），pack 只写 `extensions` 数组、不写该 id 的 URL。

### 8.5 sb check 的扩展积木校验

`sb check` 加载扩展定义，来源**两处互补**：

| 来源 | 说明 |
|---|---|
| `extensions/*.js` | unpack 落盘 / add-extension 拷贝的本地源码，直接解析 |
| meta `[extensions]` 里值是 URL 的登记 | 联网下载解析，**缓存到 `<项目>/.sbcli-cache/<id>.js`**，之后每次 check 命中缓存不再联网 |

行为：

1. **opcode 收录**：`扩展id_积木opcode` 视为已知积木，不再报"未收录"
   （未注册的扩展积木仍按 §7 的未知积木规则处理）。
2. **参数名校验**：允许的参数名 = `getInfo().blocks[].arguments` 的键，
   **原样大小写**（如 `Encoding_encode` 的参数是小写 `string` / `code`，
   `yc6ncolormaster_adjustBrightness` 的参数是大写 `COLOR` / `PERCENT`）。
   写错/大小写不符 → `unknown-param` 错误，消息里列出该积木允许的参数。
3. **URL 下载失败（断网 / 坏 URL / 超时）优雅降级**：扩展记入"未加载"，其积木
   报 `ext-unloaded` **警告**（提示用 `sb project add-extension` 拉源码），
   **不报 unknown-opcode 错误**——那可能是合法扩展积木，只是没拿到定义。
4. **登记了相对路径但源码缺失**：同样记入"未加载"，相关积木降级为警告。

> 缓存可手动清理（删 `<项目>/.sbcli-cache/`）；改 URL 后旧缓存不会自动失效，
> 可删掉对应缓存文件强制重新下载。

示例（Encoding 扩展，注意参数小写）：
```
@script flag
event_whenflagclicked
looks_sayforsecs MESSAGE=(Encoding_encode string="你好" code="Base64") SECS=1
```
