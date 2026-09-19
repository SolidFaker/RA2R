# 《红色警戒 2 / 尤里的复仇》SHP 精灵动画格式 与 Westwood LCW (Format80) 压缩算法规格

> 调研文档（洁净室复刻用）。本文只记录格式事实与规格，不包含任何代码实现。
> 所有整数均为**小端序（little-endian）**，除非特别说明。
> 文中「待验证」「来源分歧」处为无法多源交叉确认或各实现互不一致的点。

---

## 0. 信息来源（Source URLs）

| 来源 | 说明 | URL |
|---|---|---|
| XCC Utilities 源码（Olaf van der Spek，XCC Mixer 作者） | `misc/shp_ts_file.{h,cpp}`、`misc/cc_structures.h`、`misc/shp_file.{h,cpp}`、`misc/shp_decode.{h,cpp}`、`misc/shp_images.cpp`、`misc/shp_dune2_file.{h,cpp}` | https://github.com/OlafvdSpek/xcc |
| Westwood 官方 LCW.CPP（EA 开源 CnC_Red_Alert 仓库） | LCW_Uncomp 原始实现 | https://git.datalore.sh/datalore/CnC_Red_Alert/src/branch/main/CODE/LCW.CPP |
| OpenRA | `OpenRA.Mods.Cnc/FileFormats/LCWCompression.cs`、`SpriteLoaders/ShpTDLoader.cs`、`SpriteLoaders/ShpD2Loader.cs` | https://github.com/OpenRA/OpenRA |
| cnc-formats（Rust，Iron Curtain 引擎） | `src/shp_ts/mod.rs`、`src/lcw/mod.rs`、`src/xor_delta.rs`、`src/shp/mod.rs`、`src/shp_d2/mod.rs` | https://github.com/iron-curtain-engine/cnc-formats |
| ModEnc（当前版） | SHP (TS) 页面（2025 年修订） | https://modenc.renegadeprojects.com/SHP |
| ModEnc²（镜像） | SHP 旧版（2005 修订）、Remapable（重映射索引 16–31） | https://modenc2.markjfox.net/index.php?title=SHP&oldid=3621 、 https://modenc2.markjfox.net/index.php?title=Remapable |
| ModdingWiki | Westwood LCW 命令表 | https://moddingwiki.shikadi.net/wiki/Westwood_LCW |
| XCC Forum（Olaf 本人的格式讨论） | xhp.xwis.net | https://xhp.xwis.net/ |

---

## 0.1 重要勘误（先读）

任务描述中的两个前提与源码事实不符，本文已按权威来源修正，差异如下：

1. **「头部前 3 个 uint16 为 0」不正确。**
   TS/RA2 SHP 文件头是 **8 字节**：`[0, width, height, framecount]`，**只有第 1 个 uint16 为 0**。
   三个来源一致（XCC `t_shp_ts_header`、cnc-formats、ModEnc 当前版）。「3 个 0」的形态更接近 **TD/RA1（C&C1/红警 1）SHP** 的 14 字节头（见附录 A），勿混淆。

2. **「压缩类型 0=未压缩、1=LCW、2/3=XOR delta 链式引用」是把 TD/RA1 SHP 的语义张冠李戴到 TS/RA2 SHP 上了。**
   - **TS/RA2 SHP 实际不用 LCW，也不用 XOR delta。** 它的帧头里只有一个 **1 字节标志位**，其中 **bit 1（0x02）= 是否压缩**；压缩时用的是**逐扫描线 RLE**（`decode3`，见 §3.2）。
   - **LCW (Format80)** 与 **XOR delta (Format40)** 属于 **TD/RA1 SHP**（帧索引项的格式号 0x20/0x40/0x80，见附录 A），以及 CPS、TMP（地形）、Dune2 SHP、VQA 等其他格式。
   - 任务提到的「**0x8000 标志表示引用 vs 绝对数据**」实为 **Format40 (XOR delta)** 的扩展命令中 16 位计数字的 **bit 15** 判别位（见附录 B §B.1）。

因此本文主体按「TS/RA2 SHP（真实格式）+ LCW（独立算法，供 TMP/CPS 等复用）+ Format40（XOR delta，供理解 TD/RA1 链式引用）」组织，并单独给出 TD/RA1 SHP 差异附录。

---

## 1. TS/RA2 SHP 文件头

TS 与 RA2 使用同一种 SHP 容器（俗称「SHP (TS)」「SHP v2」）。整体布局：

```
[文件头 8 字节]
[帧头 × framecount]（每帧 24 字节）
[帧数据 …]（各帧按帧头内 offset 定位）
```

### 1.1 文件头（8 字节）

| 偏移 | 大小 | 类型 | 字段 | 含义 |
|---|---|---|---|---|
| 0x00 | 2 | u16 | `zero` | **必须为 0**（相当于魔数校验，XCC/OpenRA/cnc-formats 都据此识别该格式） |
| 0x02 | 2 | u16 | `width` | 全局画布宽（所有帧共享） |
| 0x04 | 2 | u16 | `height` | 全局画布高（所有帧共享） |
| 0x06 | 2 | u16 | `framecount` | 帧数（帧头数量） |

要点：
- 文件头之后紧跟 `framecount × 24` 字节的帧头表（帧索引表），**数据区的起始位置 = 8 + 24 × framecount**。
- 各帧像素被裁剪成 `(x, y, cx, cy)` 的矩形，放入全局 `(width × height)` 画布；`x + cx ≤ width`、`y + cy ≤ height`（XCC 校验）。

---

## 2. 帧索引表布局与帧头结构

### 2.1 帧头（每帧 24 字节）

| 偏移 | 大小 | 类型 | 字段 | 含义 |
|---|---|---|---|---|
| 0x00 | 2 | u16 | `x` | 帧矩形在画布内的水平偏移 |
| 0x02 | 2 | u16 | `y` | 帧矩形在画布内的垂直偏移 |
| 0x04 | 2 | u16 | `cx` | 帧宽（裁剪后的实际像素列数） |
| 0x06 | 2 | u16 | `cy` | 帧高（实际像素行数） |
| 0x08 | 1 | u8 | `flags` | 压缩/格式标志字节（见 §3；**bit 1（0x02）= 压缩**，其余位被 XCC 忽略） |
| 0x09 | 3 | u8×3 | `align` | 对齐填充（使下一字段 dword 对齐），通常为 0 |
| 0x0C | 4 | u32 | `color` | ModEnc 称「Some color (Can be Transparent color)」；XCC 视作 `unknown` 并忽略。**待验证**其确切用途 |
| 0x10 | 4 | u32 | `reserved` | 保留，XCC 校验必须为 0（`zero`） |
| 0x14 | 4 | u32 | `offset` | **帧数据在文件内的绝对字节偏移**（从文件起始算起）；`offset == 0` 表示空帧（NULL frame，无需解码） |

> 注：XCC 把 0x08 处整体当作一个 `u32 compression`（实际只用低字节的 bit 1），把 0x0C 当 `unknown`、0x10 当 `zero`。cnc-formats 读 0x08 处单字节 `compression` 与 0x14 处 `file_offset`。ModEnc 的字段命名如上表。三者字节布局一致，仅命名不同。

### 2.2 帧数据定位

- 每帧数据起始 = 帧头 `offset`（绝对文件偏移）。
- 空帧（`cx==0 || cy==0 || offset==0`）无数据、不渲染。

---

## 3. 压缩类型语义（TS/RA2 SHP）

TS/RA2 SHP 的压缩仅由帧头 `flags` 字节的 **bit 1（0x02）** 决定（XCC：`compression & 2`；ModEnc：「second bit」）：

| `flags` 值 | bit 1 | 含义 | 帧数据格式 |
|---|---|---|---|
| 0x00 | 0 | 未压缩 | 原始 `cx × cy` 字节（逐像素调色板索引） |
| 0x01 | 0 | 未压缩（bit 0 置位；XCC 忽略 bit 0） | 同上。实际文件罕见 |
| 0x02 | 1 | **压缩** | 逐扫描线 RLE（`decode3`，§3.2） |
| 0x03 | 1 | 压缩（bit 0 也置位） | 同上 |
| 其他 | 视 bit 1 | bit 1 决定压缩与否 | bit 0 及高位含义不明，**待验证** |

> **来源分歧（务必留意）**：cnc-formats 的 `shp_ts` 模块把该字段当作**枚举**（0=原始、1/2=扫描线 RLE、3=LCW）。这与 XCC 源码和 ModEnc 当前版的「**bit 1 = 压缩**」位标志解释冲突（按位标志，3=0b11 的 bit 1 已置位，应走 RLE 而非 LCW）。XCC Mixer 是实际编辑 TS/RA2 SHP 的事实标准工具，且其 `shp_ts_file.cpp` 的压缩分支**只有** `decode3`（扫描线 RLE），**没有任何 LCW 路径**。因此本文以 **XCC + ModEnc 为准：TS/RA2 SHP 不使用 LCW**；cnc-formats 的「3=LCW」判为错误/待验证。

### 3.1 未压缩帧（bit 1 = 0）

帧数据就是 `cx × cy` 个字节，按行优先（row-major）排列，每字节一个调色板索引。

### 3.2 扫描线 RLE（bit 1 = 1，即 XCC 的 `decode3`）

精确规则（源自 XCC `decode3`，与 ModEnc 描述一致）：

对 `cy` 行中的每一行：

1. 读 **u16 LE `line_len`**：本行 RLE 段的总字节数，**含这 2 个长度字节本身**。因此本行 RLE 载荷 = `line_len - 2` 字节。
2. 逐字节处理 RLE 载荷，直到本行累计输出 `cx` 个像素：
   - 读 1 字节 `v`：
     - **`v != 0`**：字面像素，输出 1 个像素，值为 `v`（消耗 1 字节）。
     - **`v == 0`**：再读 1 字节 `n`（透明游程长度）；输出 `n` 个透明像素（值 0）。若 `n` 会越过行尾（`x + n > cx`），截断为 `cx - x`（XCC 做了此钳制）。此游程共消耗 **2 字节**（0x00 + 计数）。
3. 若本行 RLE 载荷提前耗尽，行剩余像素补 0（透明）。

> 语义：这是「0x00 + 计数」的透明游程 RLE，专为精灵图大量透明背景而设计；非零字节一律是字面调色板索引。

---

## 4. LCW (Format80) 解压算法完整规格（本文件最重要一节）

> 来源：Westwood 官方 `LCW.CPP`（`LCW_Uncomp`，EA 开源）为准；XCC `decode80`、OpenRA `LCWCompression.DecodeInto`、cnc-formats `lcw::decompress`、ModdingWiki「Westwood LCW」四方交叉一致。
> LCW 又名 **Format80**，OpenRA 注释称其为「Lempel-Castle-Welch algorithm (aka Format80)」。它**不属于 TS/RA2 SHP 帧数据**，但被 TD/RA1 SHP、CPS、TD/RA TMP（含 RA2 地形 TMP）、Dune2 SHP、VQA 等大量使用，是复刻 YR 引擎必须实现的通用解压器。

### 4.1 总则

- 解压为**流式**：维护 `src`（压缩流指针）与 `dst`（输出指针/输出缓冲）。
- 每轮读 1 个**命令字节 `op`**，按其高位分派（`op & 0x80`、`op & 0x40`）。
- 所有 16 位字段（count/offset）均为 **小端**（先低字节后高字节）。
- 原始引擎**预分配并清零**输出缓冲，因此「从尚未写入的区域回拷」读到的是 0（实现者应把越界/未写位置按 0 处理，cnc-formats 与 OpenRA 均如此处理）。

### 4.2 命令总表

| 命令字节 `op` | 名称 | 长度/偏移来源 | 附加字节 | 字节流总消耗 | 行为 |
|---|---|---|---|---|---|
| `0x00–0x7F`（bit7=0） | **短回拷（相对）** | `count = (op>>4)+3`（3…10）；`offset = ((op&0x0F)<<8) \| b`（12 bit） | 1（`b`=偏移低 8 位） | **2** | 从 `dst - offset` 逐字节拷贝 `count` 字节（允许重叠） |
| `0x80` | **流结束** | — | 0 | **1** | 停止解压 |
| `0x81–0xBF`（bit7=1, bit6=0, count≠0） | **中等字面量拷贝（从源）** | `count = op & 0x3F`（1…63） | `count` | **1 + count** | 从 `src` 逐字节拷贝 `count` 字节到 `dst`（唯一从源读原始数据的命令） |
| `0xC0–0xFD`（bit7=1, bit6=1, 低6位<0x3E） | **中等回拷（绝对）** | `count = (op&0x3F)+3`（3…66）；`offset` = 后续 u16 | 2（u16 绝对 offset） | **3** | 从 `dst_base + offset` 拷贝 `count` 字节 |
| `0xFE` | **长填充** | `count` = 后续 u16；`value` = 后续 1 字节 | 2（u16 count）+ 1（value） | **4** | 用 `value` 填充 `count` 字节 |
| `0xFF` | **长回拷（绝对）** | `count` = 后续 u16；`offset` = 后续 u16 | 2（u16 count）+ 2（u16 offset） | **5** | 从 `dst_base + offset` 拷贝 `count` 字节 |

### 4.3 逐命令精确说明

#### 命令 0 —— 短回拷（相对偏移） `0b0xxx_xxxx`

- 判别：`(op & 0x80) == 0`。
- `count = (op >> 4) + 3`，范围 **3…10**。
- 读下一字节 `b`；`offset = ((op & 0x0F) << 8) | b`，范围 **0…4095**（12 位）。
- `src_ptr_copy = dst_ptr - offset`（**相对当前输出位置**）。
- 逐字节 `*dst++ = *src_ptr_copy++`，重复 `count` 次（重叠区域安全，天然支持 RLE 式重复）。
- 消耗：命令字节 + 1 偏移字节 = **2 字节**。
- 边界：`offset == 0` 时指向当前写位置，读到的均为未写入区 → 实际表现为写 `count` 个 0（cnc-formats 显式处理此情形）。

#### 命令 1 —— 中等字面量拷贝（从源） `0b10xx_xxxx`

- 判别：bit7=1 且 bit6=0（即 `op & 0xC0 == 0x80`），且 `op != 0x80`。
- `count = op & 0x3F`，范围 **1…63**。
- 从 `src` 拷贝 `count` 字节到 `dst`（`*dst++ = *src++`）。
- 消耗：命令字节 + `count` 字节 = **1 + count 字节**。
- 特殊：`count == 0`（即 `op == 0x80`）即**流结束**命令，不拷贝。

#### 命令 2 —— 中等回拷（绝对偏移） `0b11xx_xxxx`（`0xC0…0xFD`）

- 判别：bit7=1 且 bit6=1，且低 6 位 `< 0x3E`（即 `op & 0x3F != 0x3E` 且 `!= 0x3F`）。
- `count = (op & 0x3F) + 3`，范围 **3…66**。
- 读 u16 LE `offset`；`src_ptr_copy = dst_base + offset`（**相对输出缓冲起始的绝对偏移**）。
- 逐字节拷贝 `count` 字节。
- 消耗：命令字节 + 2 偏移字节 = **3 字节**。

#### 命令 3 —— 长填充 `0xFE`（`11111110`）

- 判别：`op == 0xFE`（低 6 位 == 0x3E）。
- 读 u16 LE `count`；再读 1 字节 `value`。
- 用 `value` 连续填充 `count` 字节。
- 消耗：命令字节 + 2 + 1 = **4 字节**。
- 注：官方 `LCW.CPP` 内有按 4 字节对齐的写优化，但语义与「逐字节填 `count` 个 `value`」完全一致。

#### 命令 4 —— 长回拷（绝对偏移） `0xFF`（`11111111`）

- 判别：`op == 0xFF`（低 6 位 == 0x3F）。
- 读 u16 LE `count`；再读 u16 LE `offset`。
- `src_ptr_copy = dst_base + offset`（绝对偏移）。
- 逐字节拷贝 `count` 字节。
- 消耗：命令字节 + 2 + 2 = **5 字节**。

#### 命令 5 —— 流结束 `0x80`

- 判别：`op == 0x80`（bit7=1, bit6=0, 低 6 位 = 0）。
- 停止解压，返回已输出字节数。

### 4.4 分派优先级（实现顺序）

1. `op & 0x80 == 0` → 命令 0（短回拷）。
2. `op == 0x80` → 命令 5（结束）。
3. `op & 0x40 == 0`（此时 `op` 在 `0x81…0xBF`）→ 命令 1（字面量）。
4. `op == 0xFE` → 命令 3（长填充）。
5. `op == 0xFF` → 命令 4（长回拷）。
6. 其余（`0xC0…0xFD`）→ 命令 2（中等回拷）。

### 4.5 最小可行示例

压缩流（十六进制）：`82 41 42  10 02  80`

| 步骤 | 字节 | 解释 | 输出累计 |
|---|---|---|---|
| 1 | `82 41 42` | `0x82`：bit7=1, bit6=0，count=`0x82&0x3F`=2，字面量拷贝 `41 42` | `AB` |
| 2 | `10 02` | `0x10`：bit7=0 → 短回拷；count=`(0x10>>4)+3`=4；offset=`((0x10&0x0F)<<8)|0x02`=2；从 `dst-2` 拷 4 字节 → `A B A B` | `ABABAB` |
| 3 | `80` | 流结束 | — |

最终输出：`ABABAB`（6 字节）。注意第 2 步拷贝跨越了「已写区域 + 刚写出的区域」，这是短回拷重叠 RLE 的典型用法。

---

## 5. 帧数据解压后的像素语义

### 5.1 8-bit 调色板索引

- 解压后得到 `cx × cy` 字节，每字节是**外部调色板（.pal）的索引**，行优先。
- 调色板为 256 色 6-6-6 RGB（VGA 18-bit，各分量 0–63），由 `.pal` 文件提供（本文不展开 PAL 格式）。
- **索引 0 = 透明**：渲染时跳过。
- **索引 1 = 阴影色**（**已实测修正**，见下）。渲染时不是取调色盘里该索引的深蓝，而是重映射为**半透明黑 ARGB(140,0,0,0)**，按 alpha 混合叠到已画内容上。依据：
  - 全库实测：扫描 1200 个 SHP、20589 帧，其中「非零像素索引集合只有 1 个元素」的单色帧共 6363 个，**6292 个（1025 个文件）是索引 1**；其余索引各只出现 1 帧（个别单色美术）。即 SHP 阴影帧（建筑/动画/树木等的影子段）统一用索引 1 绘制。
  - OpenRA 引擎 `PaletteFromFile` 的 `ShadowIndex` 语义即「把这些索引重映射为阴影」：`ImmutablePalette.LoadFromStream` 里 `colors[i] = 140u << 24`（= ARGB(140,0,0,0)），而 RA2 的单位/地形/资源调色盘定义（`ShadowIndex: 1`）把索引 1 交给该重映射。
  - **勘误**：XCC 的 `shp_split_shadows`/`combine_shadows` 把索引 **4** 当阴影，这只适用于其他 Westwood 作品（TD/RA1 时期惯例）；RA2/TS 实测为索引 1。用索引 4 会把影子画成调色盘里的杂色，用索引 1 直接取色则会把影子画成深蓝（UNITURB.PAL 索引 1 = (0,0,49)）。
- **索引 16–31 = 阵营/玩家色重映射区**：这是 RA2/YR 的单位颜色重映射范围（16 个索引），渲染时按所属阵营/玩家替换为对应颜色（ModEnc「Remapable」页明确：*「color indexes 16–31 … are always remapped」*）。

### 5.2 帧合成

- 每帧解压成 `cx × cy` 后，放置到全局画布 `(width × height)` 的 `(x, y)` 处；画布其余区域为透明（0）。
- 动画由 `framecount` 帧依次播放。

### 5.3 帧段布局（建筑本体 / 配件动画，实测归纳）

原版建筑本体与 `ActiveAnim` 系动画 SHP 的帧按**等长段**排布，段数由帧内容（后半是否全为阴影/空帧）与循环接缝判定：

| 用途 | 布局 | 段长 | 阴影段起点 |
|---|---|---|---|
| 建筑本体 | `[空闲][受损][建造/临界][…]` + `[同数阴影]` | — | **n/2**（6 帧 → 3、8 帧 → 4） |
| 配件动画（四段） | `[空闲 L][受损 L][空闲阴影 L][受损阴影 L]` | n/4 | n/2 |
| 配件动画（两段） | `[动画 L][阴影 L]` | n/2 | n/2 |
| 配件动画（单段） | `[动画 n]`（无阴影段） | n | — |

实测要点（全部由游戏自身 SHP 帧内容与 artmd 键归纳，不依赖外部表）：

1. **受损变体在同一 SHP 的第 2 段**，不在第 3 段。`artmd.ini` 的 `ActiveAnimDamaged=<名>_AD` 指向的文件（NATSLA_AD / GAPRIS_BD / CAOILD_AD …）**在原版数据里并不存在**；受损动画实际是同一 SHP 的 `[L, 2L)` 段。把第 2 段当阴影画（旧实现）会让光棱塔/磁暴塔的**空闲动画与受损动画同时出现、叠在一起**。
2. **阴影段恒在后半**（起点 n/2），每帧非零像素只有索引 1。动画的受损阴影段在 `[3L, 4L)`。
3. 四段/两段的区分靠**循环接缝**：空闲段自闭环时，`L = n/4` 的接缝（第 `n/4-1` 帧 vs 第 0 帧的画布差异率）小于 `L = n/2` 的接缝。例：NATSLA_A（n=40）接缝 0.37 vs 0.84 → L=10（四段）；CAOILD_F 旗子（n=32）0.53 vs 0.32 → L=16（两段）；GAREFNOR（n=40）1.13 vs 1.06 → L=20（两段）。
4. 判据只看**帧内容**：后半全为「纯索引 1 帧或空帧」才有阴影段；否则整段视为一个循环动画。

> 例：CAOILD.SHP（n=8）本体 = 4 彩色帧 + 4 阴影帧（第 4 帧是建造脚手架彩色帧，**不是**影子）；CAOILD_A.SHP（n=128）动画 = 32 帧摇臂 + 32 帧受损摇臂 + 64 帧阴影；CAOILD_F.SHP（n=32）旗帜 = 16 帧 + 16 空帧。
>
> `SpecialAnim`（光棱塔棱镜充能 GAPRIS_A、磁暴塔放电 NATSLA_B、维修厂吊臂 GADEPT_A 等）是**动作动画**：artmd 的 `IsAnimDelayedFire=yes` / `DelayedFireDelay=28` 表明它在开火前播放，待机时不绘制。

### 5.4 NewTheater 文件名代号（artmd `NewTheater=yes`）

`NewTheater=yes` 的建筑，其 SHP 名字的**第 2 个字母是剧场代号**；找不到对应变体时回退到通用（`G`）：

| 剧场 | 代号 | 示例（建筑 GTGCAN / YAGGUN） |
|---|---|---|
| TEMPERATE | **A** | GAGCAN.SHP、YAGGUN.SHP |
| SNOW | **T** | GTGCANMK.SHP |
| URBAN | **U** | YU GGUN 系列（YUGGUN.SHP）、GUGCANMK.SHP |
| DESERT | **D** | YDGGUN.SHP、GDGCANMK.SHP |
| LUNAR | **L** | YLGGUN.SHP、GLGCANMK.SHP |
| NEWURBAN | **N** | GNGCANMK.SHP |
| 通用回退 | **G** | GGGCAN.SHP、YGGGUN.SHP、GGPOWR.SHP |

实测依据：全库 SHP 名第 2 字母分布中只有 A(590)/D(301)/G(315)/L(316)/N(335)/T(328)/U(351) 是高频，其余字母均个位数；rulesmd 类型名（GAPOWR/YAGGUN/NATSLA…）第 2 字母为 A 即温和剧场。
典型反例：**巨炮 GTGCAN 的底座并不叫 GTGCAN.SHP**（不存在），而是温和变体 **GAGCAN.SHP**；只按原名加载会整块底座缺失、只剩体素炮塔。
解析顺序：原名 → 剧场代号变体 → `G` 通用 → `A`（见 `render::resolve_art_name`）。

---

## 附录 A —— TD/RA1（C&C1/红警 1）SHP 头部差异

任务要求「与 RA1 SHP 头部的差异说明」。RA1 与 C&C1 共用一种 SHP（「SHP (TD)」/「SHP v1」），与 TS/RA2 差异如下：

### A.1 文件头（14 字节，XCC `t_shp_header`）

| 偏移 | 大小 | 类型 | 字段 | 含义 |
|---|---|---|---|---|
| 0x00 | 2 | u16 | `c_images` | 帧数（**TD 头以帧数开头**，TS 头以 0 开头） |
| 0x02 | 2 | u16 | `unknown1` | 保留，0 |
| 0x04 | 2 | u16 | `unknown2` | 保留，0 |
| 0x06 | 2 | u16 | `cx` | 宽 |
| 0x08 | 2 | u16 | `cy` | 高 |
| 0x0A | 4 | u32 | `unknown3` | 保留，0 |

### A.2 帧索引表（每项 8 字节 = 2 × u32）

共有 `c_images + 2` 项（多出的 2 项为 EOF 哨兵与全零哨兵）。第 `i` 项：

| 字段 | 位置 | 含义 |
|---|---|---|
| `offset \| format` | 项内 u32[0] | 低 28 位 = 帧数据**偏移**（掩码 `0x0FFFFFFF`）；高 4 位（bit 28–31）= **格式号** |
| `ref_offset \| ref_format` | 项内 u32[1] | 低 28 位 = **引用帧偏移**（仅格式 4 用）；高 4 位 = 引用格式 |

### A.3 格式号（高 4 位，XCC `get_format = u32 >> 28`）

| 格式号（nibble） | 字节形态（bit 24–31） | 名称 | 含义 |
|---|---|---|---|
| 0x2 | `0x20` | XOR-Prev | **XOR delta (Format40)**，引用**上一帧**（`i-1`） |
| 0x4 | `0x40` | XOR-Ref | **XOR delta (Format40)**，引用 `ref_offset` 指向的那一帧（**链式引用**） |
| 0x8 | `0x80` | LCW | **LCW (Format80)**，绝对数据、自包含 |

- 这就是任务所述的「XOR delta 链式引用」：格式 2 引用前一帧、格式 4 引用任意帧，形成依赖链；解码时先解码被引用帧、复制其像素，再用 Format40 流在其上 XOR 出本帧。
- TD/RA1 SHP 的「0=未压缩」虽见于部分文档，但 OpenRA 只接受 0x20/0x40/0x80 三种，XCC 也只实现 decode80/decode40 两条路径；「格式 0 未压缩」**待验证**。
- OpenRA `ShpTDLoader` 用字节值 `0x20/0x40/0x80` 表示同一组格式号（与 XCC nibble 2/4/8 一致）。

---

## 附录 B —— Format40 (XOR delta) 规格

> 来源：XCC `decode40`、cnc-formats `xor_delta.rs`、OpenRA `XORDeltaCompression` 交叉一致。
> 用于 TD/RA1 SHP 格式 2/4、WSA 动画帧等。调用前目标缓冲必须**已复制引用帧像素**；解码过程对目标做 XOR，只在「变化处」翻位。

### B.1 命令表

| 首字节 | 名称 | 附加字节 | 行为 |
|---|---|---|---|
| `0x81–0xFF` | 小跳过 | 0 | 前进 `op & 0x7F` 个像素（引用像素保持原样 = 透明/不变） |
| `0x80` + u16 `w`（`w==0`） | 流结束 | 2 | 停止 |
| `0x80` + u16 `w`（bit15=0） | 大跳过 | 2 | 前进 `w` 个像素（不变） |
| `0x80` + u16 `w`（bit15=1, bit14=0） | 大 XOR 拷贝 | 2 + `w&0x3FFF` | 从流中读 `w & 0x3FFF` 字节，逐字节 XOR 到目标 |
| `0x80` + u16 `w`（bit15=1, bit14=1）+ 1 字节 `v` | 大 XOR 填充 | 2 + 1 | 用 `v` 对 `w & 0x3FFF` 个目标字节逐个 XOR |
| `0x01–0x7F` | 小 XOR 拷贝 | `op` 字节 | 从流读 `op` 字节，逐字节 XOR 到目标 |
| `0x00` + 1 字节 `n` + 1 字节 `v` | 重复 XOR | 2 | 用 `v` 对 `n` 个目标字节逐个 XOR |

### B.2 关于「0x8000 标志」

在 `0x80` 扩展命令里，后续 u16 字（小端）的 **bit 15（0x8000）** 是判别位：

- **bit 15 = 0** → 「跳过」（引用/未变化区域，目标像素保持引用帧内容）；
- **bit 15 = 1** → 「本命令携带 XOR 差异数据」，再由 **bit 14（0x4000）** 区分是「从流拷贝差异字节」（bit14=0）还是「单字节填充差异」（bit14=1）。

这正对应任务描述里「以 0x8000 标志表示引用（跳过/不变）vs 绝对（携带差异）数据」。

---

## 附录 C —— Dune2 SHP（简述，供对照）

另有一种「SHP (Dune 2)」格式（XCC `t_shp_dune2_header`、OpenRA `ShpD2Loader`），用于 Dune 2 / Dune 2000，**不是** RA2 的 SHP。要点：

- 头 2 字节 = 帧数；随后是帧偏移表（每项 2 或 4 字节，靠探测区分）。
- 每帧头：`flags u16`（bit0=调色板表、bit1=非 LCW、bit2=变长表）、保留 1、宽 u16、高 u8、`dataLen u16`、`dataSize u16`、[可选调色板表]、帧数据。
- 帧数据：`flags` 的 bit1 清 → **LCW (Format80)**；置位 → RLEZeros（`0x00 + count` 透明游程，逐帧整体而非逐扫描线）。

列此仅为防混淆：LCW 确实用于 SHP 家族（TD/RA1 SHP 与 Dune2 SHP），但**不用于 TS/RA2 SHP**。

---

## 关键发现总结（3–5 条）

1. **TS/RA2 SHP 头部是 8 字节 `[0, width, height, framecount]`（仅 1 个零 u16），不是「3 个零」**；帧头固定 24 字节：`x, y, cx, cy, flags(u8)+3 对齐, color(u32), reserved(u32), offset(u32)`。三来源（XCC、cnc-formats、ModEnc）布局一致。

2. **TS/RA2 SHP 的压缩标志是帧头 `flags` 字节的 bit 1（0x02）**：0 = 原始 `cx×cy`；1 = 逐扫描线 RLE（`decode3`：每行 u16 长度 + 「0x00+计数=透明游程 / 非零=字面像素」）。**TS/RA2 SHP 不使用 LCW、也不使用 XOR delta**；cnc-formats 声称「3=LCW」与 XCC/ModEnc 冲突，判为存疑（倾向 XCC+ModEnc）。

3. **LCW (Format80) 共 5 种有效命令 + 1 结束标志**，判别与字节消耗精确如下（四方一致，含官方 `LCW.CPP`）：短回拷 `0x00–0x7F`（2 字节，count=(op>>4)+3，相对偏移 12bit）；字面量 `0x81–0xBF`（1+count 字节）；中等回拷 `0xC0–0xFD`（3 字节，count=(op&0x3F)+3，绝对偏移 u16）；长填充 `0xFE`（4 字节）；长回拷 `0xFF`（5 字节）；`0x80` 结束。**短回拷用相对偏移，中等/长回拷用绝对偏移**，这是最易实现错的地方。

4. **「0x8000 标志」属于 Format40 (XOR delta)**（TD/RA1 SHP 的链式引用），不是 TS/RA2 的东西：`0x80` 扩展命令后 u16 字的 bit15=0 表示「跳过/引用不变」，bit15=1 表示「携带 XOR 差异数据」（bit14 再分拷贝/填充）。

5. **像素语义**：索引 0 = 透明，**索引 1 = 阴影**（实测 6363 个单色帧中 6292 个为索引 1；OpenRA 亦把该索引重映射为 ARGB(140,0,0,0) 半透明黑；XCC 文档所称「索引 4」不适用于 RA2），索引 16–31 = 阵营/玩家色重映射区（ModEnc 明确）。帧段布局见 §5.3：建筑本体 `[状态帧…][阴影帧 n/2]`，配件动画四段 `[空闲 L][受损 L][空闲阴影 L][受损阴影 L]`。
