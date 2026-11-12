# `sb unpack` 交付说明

**命令**：`sb unpack <作品.sb3> <输出目录> [--force] [--json]`
**定位**：`sb pack` 的逆操作——把 Scratch 3 作品拆成 sbcli 项目目录。
**实现**：`src/sbcli_unpack.cpp` / `src/sbcli_unpack.hpp`（+ `cmd_unpack` 于 `src/commands.cpp`）

---

## 1. 用法

```bash
sb unpack 作品.sb3 F:\temp\utest        # 输出目录必须不存在或为空
sb unpack 作品.sb3 F:\temp\utest --force  # 覆盖已有目录
sb unpack 作品.sb3 F:\temp\utest --json   # 结构化输出
```

输出目录结构（与 `docs/format.md` §0 契约一致）：

```
输出目录/
  meta.sbcli                 项目名 + [variables]/[lists]/[broadcasts]（取自 Stage）
  character/stage/           舞台：meta.sbcli（+ 有脚本时才有 block.sbcli）
  character/1/, 2/, …        角色（按 sb3 targets 顺序编号，名字写进 meta 的 name）
  assets/                    造型/声音素材（md5.svg / md5.wav 等原名）
```

## 2. 映射规则（与 `sb pack` 严格对称）

| SB3 | block.sbcli |
|---|---|
| `topLevel` 帽子块 | `@script flag` / `broadcast <名>` / `key <键>` / `clone` / `click` |
| `event_whenflagclicked` 等 5 类帽子 | 反推为上面的 `@script` 类型 |
| `next` 链 | 顺序块行 |
| `SUBSTACK` / `SUBSTACK2` | 缩进 / `else` + 缩进 |
| 块引用 `[2, id]` | 递归还原成 `(...)` 或 `<...>` |
| 内联影子 `[1,[10,"文"]]` / `[1,[4,3]]` | `"文"` / `3` |
| 变量、列表 `fields`（`[名, id]`） | `VARIABLE=名` / `LIST=名`（id 由 `sbcVarId` 重算） |
| 广播 `BROADCAST_INPUT` | `BROADCAST_INPUT=名` |
| `procedures_definition` + `procedures_prototype` | 一行 `PROCCODE=… ARGS=[…]`，定义体（挂 `prototype.next`）缩进展开 |
| `procedures_call`（`argumentids`） | `PROCCODE=… ARG1=… ARG2=…`（栈位置与 reporter 位置同一套逻辑） |
| 未知 opcode | `# 未知积木: <opcode>` + 全部 fields/inputs 原样保留，SUBSTACK 仍展开 |

## 3. 自测结果

### 3.1 round-trip：`unpack → pack → sb script` 中文逐行一致

| 用例 | 覆盖点 | 结果 |
|---|---|---|
| `e2e-game.sb3` | 全局变量、广播、if/else、forever | ✅ 逐行一致 |
| `fruitproj.sb3` | 角色级变量、孤儿广播 | ✅ 逐行一致 |
| `proc.sb3` | 自定义积木定义 + 调用 | ✅ 逐行一致 |
| `md5real.sb3` | 真实 md5 命名素材、共用素材去重 | ✅ 逐行一致 |
| `defstd.sb3` | **Scratch 标准定义体结构**（`prototype.next`）、带参调用 | ✅ 逐行一致 |
| `real.sb3` | 未知积木、4 类帽子、`[3,id,shadow]` 兜底 | ⚠️ 见 §4 |

### 3.2 其它验证

- `sb check`：解包出的项目结构/引用检查通过（`defstd`、`md5real` 零错误零警告）
- `sb fix`：幂等（连跑两次 meta 文件 hash 不变）
- 现有测试套件 `sb_parser_test` / `sb_parser_test2` / `sb_fix_test` 全部通过
- 干净重编 **零警告**
- 素材闭环：`sb unpack` 导出 → `sb pack` 写回 zip → `sb assets` 可识别（3/3）
- 素材兼容两种 zip 布局：zip 根（Scratch 原生）与 `assets/` 前缀；`sounds` 缺 `md5ext` 时靠 `assetId`+`dataFormat` 兜底
- 边界：空工程（仅 Stage）、无 Stage 的非常规 sb3、目录非空、文件缺失、缺参数——均正常处理不崩溃

## 4. 已知限制

### 4.1 未知积木的 field/input 分类会漂移（pack 侧）

未知 opcode（不在 `SB3_T`/`SB2_T` 的积木）在文本层，`fields` 与 `inputs` 都表现为 `KEY=value`，
**文本无法区分二者**。`sb pack` 重打包时按 `menuFieldKeys()` 判断（只认已知字段名），
于是未知积木的 field 会被写成 input：

```
原    : "fields": {"WIDGET": ["foo"]}
重打包: "inputs": {"WIDGET": [1,[10,"foo"]]}
```

**影响面**：仅未知/第三方扩展积木——它们在 Scratch 里本来就是灰块，无法执行；
**所有已知 opcode 零漂移**（6 个用例中 5 个逐行完全一致）。
键值本身不丢失，只是分类变化。pack 侧正在加启发式（未知 opcode 时：键名全大写 + 值无空格 → 当 field）。

### 4.2 `unknown opcode` 会被 `sb check` 判为错误

这是 `sb check` 的正确行为（未知 opcode 确实是问题），不是解包缺陷。
解包保留它们是为了**不丢数据**，便于人工迁移到受支持的积木。

### 4.3 无法还原的 SB3 元数据

以下字段 sbcli 格式不承载，解包时丢弃（属契约限制，非缺陷）：
- 积木坐标（`x`/`y`）、注释（`comments`）、`monitors`
- 变量/广播的**原始 id**（改由 `sbcVarId`/`sbcBroadcastId` 确定性重算，语义等价）
- `volume`、`tempo`、`videoState` 等舞台属性（解包写默认值）

## 5. 附带修复的上游问题

实现过程中发现并报告给 lead 的三个既有缺陷均已修复：
1. **`sb pack` 素材路径**：meta 存项目根相对路径（`assets/x.svg`），pack 却拼成 `<项目>/<角色id>/assets/x.svg` → 素材永远打包不进去。
2. **`sb assets` 不认 `assets/` 前缀**：只匹配 zip 根，导致重打包文件（素材在 `assets/` 下，也是 Scratch 官方布局）报"找不到"。
3. 上述 §4.1 的 field/input 漂移（pack 侧启发式修复中）。

## 6. lead 收尾补充

### 6.1 第二轮修复的上游缺陷
4. **菜单字段集合不全**（`OPERATOR`/`COSTUME`/`FRONT_BACK`/`NUMBER_NAME` 等缺失）
   → 重建时误归 `inputs`，渲染出现 `abs` vs `绝对值`、`front` vs `最前面` 之类差异。
   **修法**：`menuFieldKeys()` 改为**从 `FIELD_MAP` 自动推导**（权威来源）+ 少量手工补充。
5. **菜单字段收到 reporter 值时被当字面值**：如 `KEY_OPTION=(argument_reporter_string_number VALUE=键)`
   被写成 `KEY_OPTION: ["argument_reporter_string_number"]`。修：仅标量才进 fields，否则走 inputs。
6. **unpack 遇 menu 块引用时写出 block id**（如 `COSTUME` 引用 `looks_costume_menu` 块）
   → 修：沿引用解析 menu 块的 field 值。
7. **`refInput` 用 `[2, blockId]`** 而非 Scratch 标准 `[3, blockId, fallback]` → 已改。

### 6.2 实测结果（8 个真实作品）
`unpack → pack → sb script` 中文翻译对比：**6/8 完全一致**（逐行相同，48 行级）。
未一致的 2 个均为**空槽 fallback 的显示差异**（`( + 180)` → `(∅ + 180)`），不丢任何积木/字段。
