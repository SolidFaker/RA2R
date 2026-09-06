# 红色警戒 2 / 尤里的复仇 地图文件格式与 Westwood INI 语法规格

> 洁净室复刻参考文档。本文只记录**事实与规格**，不包含任何 GPL 代码实现。
> 调研方式：阅读公开 GPL 源码获取事实 + 直接解析 `I:\ai\RA2R\Yuri` 下的真实地图做二进制实测。
> 约定：文中所有多字节整数均为**小端序（little-endian, LSB first）**，除非特别说明。
> 置信度标记：`高`=多来源一致且实测验证；`中`=单一来源或逻辑推断；`待验证`=无法多源确认。

---

## 0. 信息来源（URL）

| 来源 | 用途 | 许可 |
|---|---|---|
| FinalAlert 2 / FinalSun 官方源码（EA）<br>https://github.com/electronicarts/CNC_TS_and_RA2_Mission_Editor | 地图读写权威实现（`MissionEditor/MapData.cpp/.h`、`MissionEditor/IniFile.cpp`、`MissionEditorPackLib/`） | GPLv3 |
| ra2ff（Thomas Spurden）<br>https://github.com/tcrs/ra2ff | `ra2ff.txt` 格式文档、`src/Base64.cpp`、`src/MapReader.cpp` | GPLv2 |
| Chronoshift<br>https://github.com/TheAssemblyArmada/Chronoshift | `src/game/common/base64.cpp`、`ini.cpp`、`lcw.cpp`（Westwood 引擎行为复刻） | GPLv2 |
| XCC Utilities（Olaf van der Spek，随 FA2 仓库 `3rdParty/xcc/` 分发） | `mix_file.cpp`、map pack 编解码（encode5/decode5/encode80 等） | GPLv3 |
| ModEnc（旧站）<br>http://modenc.renegadeprojects.com/IsoMapPack5 、/Maps | 各节语义、扩展名含义 | — |
| ModEnc²<br>https://modenc2.markjfox.net/IsoMapPack5 | IsoMapPack5 补充 | — |
| Shikadi Modding Wiki — Westwood LCW<br>https://moddingwiki.shikadi.net/wiki/Westwood_LCW | Format80 命令表 | — |
| 实测样本 | `I:\ai\RA2R\Yuri` 下 `.yrm/.yro/.mmx`（如 `map\网络地图\立交河(2).yrm`、`Ice_Age.yro`、`amazon.mmx`） | — |

---

## 1. 地图文件外壳：INI 结构与四种扩展名

### 1.1 总体结构

RA2/YR 地图是一个 **Westwood INI 文本文件**（`[Section]` + `key=value`），其中若干节的值是**经过 Base64 编码的二进制块**。文本部分与普通 Westwood INI（rules.ini/art.ini）共用同一解析规则（见 §5）。

一个 FA2 生成的多人地图（`.yrm`）的节大致按下列顺序出现（实测自 `立交河(2).yrm`）：

```
; Map created with FinalAlert 2(tm) Mission Editor
[Header] [AircraftTypes] [Preview] [PreviewPack]
[AITriggerTypesEnable]
[Americans] [Alliance] [French] [Germans] [British]
[Africans] [Arabs] [Confederation] [Russians]
[YuriCountry] [GDI] [Nod] [Neutral] [Special]
[Basic] [Houses] [IsoMapPack5] [Lighting] [Map]
[OverlayDataPack] [OverlayPack] [SpecialFlags]
[Structures] [TaskForces] [Terrain] [Units] [Waypoints]
[Digest]
```

### 1.2 各节概览

| 节 | 用途 | 备注 |
|---|---|---|
| `[Header]` | FA2 编辑器元数据：`Width`/`Height`/`StartX`/`StartY`/`Waypoint1..8`/`NumberStartingPoints` | 游戏实际地图尺寸以 `[Map] Size` 为准；`[Header]` 的 Width/Height 与 `[Map]` 数值不一致（见 §2.6 实测），**待验证**其游戏侧作用 |
| `[Basic]` | 地图基本信息：`Name`、`Percent`、`GameMode`、`HomeCell`、`InitTime`、`Official`、`EndOfGame`、`FreeRadar`、`MaxPlayer`、`MinPlayer`、`SkipScore`、`TrainCrate`、`TruckCrate`、`AltHomeCell`、`OneTimeOnly`、`CarryOverCap`、`NewINIFormat`、`NextScenario`、`SkipMapSelect` 等 | 布尔以 `yes/no` 书写（见 §5.4） |
| `[Map]` | 地图尺寸与剧场：`Size=`、`Theater=`、`LocalSize=` | 详规见 §2.6 |
| `[Lighting]` | 环境光照参数（RGB/Ambient 等） | |
| `[IsoMapPack5]` | **地形瓦片数据**（核心，见 §2） | Base64 + LZO |
| `[OverlayPack]` | 覆盖物类型索引（每格 1 字节） | Base64 + Format80，见 §4.1 |
| `[OverlayDataPack]` | 覆盖物附加数据（每格 1 字节） | 同上 |
| `[Preview]` / `[PreviewPack]` | 小地图预览图（`[Preview]` 存尺寸/调色，`[PreviewPack]` 存 24bit RGB 像素） | Base64 + LZO，见 §4.3 |
| `[Houses]` | 参战阵营/玩家列表 | |
| `[Americans]` `[Alliance]` `[French]` `[Germans]` `[British]` `[Africans]` `[Arabs]` `[Confederation]` `[Russians]` `[YuriCountry]` `[GDI]` `[Nod]` `[Neutral]` `[Special]` | 各阵营/玩家配置（IQ、Edge、Color、Allies、Country、Credits、NodeCount、TechLevel、PercentBuilt、PlayerControl 等） | 键名见实样 |
| `[Infantry]` / `[Units]` / `[Aircraft]` / `[Structures]` / `[Terrain]` | 预放置对象：`序号=类型,所属,坐标/属性,...` | 键为递增序号 |
| `[Triggers]` / `[Events]` / `[Actions]` / `[Tags]` | 触发/事件/动作/标签脚本 | 键为唯一 ID（8 位十六进制字符串） |
| `[TaskForces]` / `[TeamTypes]` / `[ScriptTypes]` / `[AITriggerTypes]` / `[AITriggerTypesEnable]` | AI 相关（部队编成/小队类型/脚本/触发启用） | RA2/YR 用 `[TeamTypes]`/`[ScriptTypes]`；旧作（RA1/TS）才叫 `[Teams]`/`[Scripts]` |
| `[Waypoints]` | 航点（`编号=坐标`） | |
| `[CellTags]` | 单元格标签（部分地图存在） | |
| `[Digest]` | 地图校验摘要：`1=<28 字符 base64>`（解码 = 20 字节 = 160 bit，长度与 SHA-1 一致；具体哈希算法 待验证，用途为完整性/防篡改） | `.yro/.mmx` 在 `[Digest]` 后还附二进制尾部，见 §1.3 |
| `[Ranking]` | 官方地图的排名/统计信息（`.yro/.mmx` 常见） | |
| `[SpecialFlags]` | 特殊标志 | |
| `[MultiMaps]` / `[MultiMap[...]]` | **YR 多子地图机制**：`[MultiMaps]` 用 `1=<场景节名>` 列出子地图；`[MultiMap[XXX]]`（如 `[MultiMap[NEGLAMP]`、`[INPURPLAMP]`、`[INYELWLMP]`、`[INBLULMP]`）定义各“灯柱（lamppost）”节点 | 仅在 `.yro/.mmx` 出现，见 §1.3 |

### 1.3 四种扩展名差异

| 扩展名 | 性质 | 说明 |
|---|---|---|
| `.map` | 纯 INI 文本地图 | 通用地图格式（RA2 与 YR 均读取）。单机战役、遭遇战、多人地图均可用。**（本工作区无 `.map` 样本）** |
| `.yrm` | 纯 INI 文本地图 | “Yuri's Revenge Multiplayer”自定义多人地图，行为等价于 `.mpr`（RA1 的多人数图）。整个文件是可直接阅读的 INI 文本，`[Digest]` 值也是文本 base64。 |
| `.yro` | **加密 MIX 壳** | “Yuri's Revenge Official”官方多人地图。整个文件 = 加密 MIX v2（旗标 `0x00030000`，116 字节头 = 4 旗标 + 80 RSA + 32 加密头），内含 `[MultiMaps]` 索引条目 + 地图 INI 条目；尾部 20 字节条目校验和。 |
| `.mmx` | **加密 MIX 壳** | YR 战役/官方地图包，结构与 `.yro` 相同，支持多子地图。 |

**实测确认**（`amazon.mmx`、`EB1.mmx`、`Ice_Age.yro`、`HighExpR.yro`）：

- 文件头部 4 字节固定为 `00 00 03 00`，`[MultiMaps]` 节一律出现在**偏移 116** 处，即二进制头长度固定为 **116 字节**。
- `[MultiMaps]` 内容形如 `1=AMAZON01`（或 `1=Ice_Age`），随后是名为 `[AMAZON01]`/`[Ice_Age]` 的场景节。
- `[Digest]` 节含 `1=<28 字符 base64>`（解码 = 20 字节 = 160 bit 摘要），其后跟随一段二进制数据（文件尾部）。

> **重大更正（实测 + MixFile 解析走通）**：ModEnc `Maps` 页是正确的——`.mmx/.yro` 就是 **MIX 容器**，
> 而且是**加密 MIX v2**（旗标 `0x00030000`）：`[4B 旗标][80B RSA 封装 Blowfish 密钥][32B 加密头(条目数+大小)]`
> = **116 字节头**，正文从偏移 116 开始。此前"不满足标准 MIX 头校验"的分析是用明文 MIX 公式
> 检验加密 MIX 导致的误判；`structure_valid`（`body_start + data_size + 校验和20 == 文件大小`）精确成立。
> 实测 `CrctBrd.yro`：2 个条目——119 字节 `[MultiMaps]` 索引（`1=CrctBrd`）+ 188KB 地图 INI
> （`[Basic]/[Map]/[Terrain]/[OverlayPack]/[IsoMapPack5]/...`）；文件尾部 20 字节为条目校验和（旗标 bit0）。
> 因此 `.yro/.mmx` 的正确解析路径 = 先按 MIX 解壳，取含 `IsoMapPack5` 的条目作为地图 INI。

---

## 2. [IsoMapPack5] 详规（最重要）

### 2.1 解码总流程（高置信度，多源 + 实测一致）

1. 取 `[IsoMapPack5]` 节的所有 `键=值`，**按键名数值升序（1,2,3,…）** 依次把 `值` 拼接成一个长字符串。
2. 对整个字符串做 **Base64 解码**（见 §2.2）。输入长度必为 4 的整数倍；无 `=` 填充。
3. 解码结果是一个 **Pack 字节流**，按下列结构反复读取直到耗尽：

   | 字段 | 大小 | 说明 |
   |---|---|---|
   | `compressedLength` | 2（uint16 LE） | 本块**压缩后**字节数 |
   | `uncompressedLength` | 2（uint16 LE） | 本块**解压后**字节数（通常为 8192，末块为余数） |
   | `packedData` | `compressedLength` 字节 | LZO1X 压缩数据 |

4. 对每块执行 **LZO1X 解压**（`lzo1x_decompress`，见 §2.4），压缩块解出 `uncompressedLength` 字节。
5. 顺序拼接所有解压输出 = **瓦片数组**。其长度必为 **11** 的整数倍；每 11 字节是一个瓦片记录（§2.5）。

### 2.2 Base64 字母表 —— 就是标准 RFC 4648（重要更正）

**RA2/YR 地图的 `[IsoMapPack5]` 使用标准 Base64（RFC 4648）字母表，不存在“自定义字母表”。** 逐字符列出（值 0→63）：

```
值:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
字符: A  B  C  D  E  F  G  H  I  J  K  L  M  N  O  P
值: 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
字符: Q  R  S  T  U  V  W  X  Y  Z  a  b  c  d  e  f
值: 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47
字符: g  h  i  j  k  l  m  n  o  p  q  r  s  t  u  v
值: 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63
字符: w  x  y  z  0  1  2  3  4  5  6  7  8  9  +  /
```

即 `A–Z`(0–25)、`a–z`(26–51)、`0–9`(52–61)、`+`(62)、`/`(63)，与 RFC 4648 完全一致。

**证据（三份源码 + 实测）：**
- ra2ff `src/Base64.cpp` 第 98–104 行的 `alphabet[]` 即上表，注释称 “see RFC4648”。
- Chronoshift `src/game/common/base64.cpp` 第 23 行 `_encoder[]` 同表。
- FA2 的 `FSunPackLib::EncodeBase64/DecodeBase64` 调 XCC 的 `encode64/decode64`（XCC 库，标准字母表）。
- **实测**：对 `立交河(2).yrm` 拼接后的 55 560 字符按标准 base64 解码，得到 41 670 字节，其 Pack 分块头（§2.1）完全合法（11 块，10×8192 + 1×844，累加恰好耗尽），说明标准字母表正确；任意置换字母表都不可能产生这种规整结构。

**与“自定义 base64”传闻的差异说明**：社区流传的“RA2 自定义 base64 字母表”对**地图 `[IsoMapPack5]`/`[OverlayPack]`/`[PreviewPack]` 不成立**——这里就是标准字母表。真正的差异点只有两处非字母表层面的细节：(a) 文件内**没有 `=` 填充**（总长度本身是 4 的倍数）；(b) 值按 **70 字符一行**切分（见 §2.3），行尾余量与下一行连续。若你曾看到“自定义字母表”的说法，大概率来自其它 Westwood 格式（TD/RA 的 map/其它 6-bit 编码），**待验证**其具体出处。

### 2.3 行分割与行内格式

- 每个 `键=值` 中，键为从 1 开始的递增序号，值为 base64 字符。
- 标准做法是**每 70 字符为一行**（最后一行为余数）。FA2 写入时按 70 字符切块；读取方则把整节所有值**无条件顺序拼接**后再解码（ra2ff 与 FA2 均如此）。
- 因此单行的字符数**不必**是 4 的倍数（如实测每行 70 字符），只有**拼接后的总长度**必须是 4 的倍数。`outputLength = (inputLength / 4) * 3`。

### 2.4 压缩方式：LZO1X（Format 5）

- `[IsoMapPack5]` 与 `[PreviewPack]` 使用 **LZO1X**（miniLZO，`lzo1x_decompress`，Oberhumer 的 LZO 库），即 XCC 内部命名的 “format 5”——这也是节名 `IsoMapPack**5**` 的由来。
- 每块独立压缩，`compressedLength` 为压缩后字节数，`uncompressedLength` 为解压后字节数；解压后逐块按顺序拼接。
- 本规格不粘贴 LZO1X 算法（参考 miniLZO 实现即可）；引擎侧直接集成 miniLZO 或等价实现。
- 实测：`立交河(2).yrm` 解压总输出 = 82 764 字节 = **7 524 瓦片 × 11 字节**（恰好整除，验证见 §2.6）。

### 2.5 每瓦片未压缩字节数 = 11（130/132 之问的定论）

**答案：每瓦片记录未压缩 = 11 字节。既不是 130，也不是 132。**

| 证据 | 内容 |
|---|---|
| FA2 官方源码 `MissionEditor/MapData.h:81–90` | `struct MAPFIELDDATA { unsigned short wX; unsigned short wY; WORD wGround; BYTE bData[3]; BYTE bHeight; BYTE bData2[1]; };` 且 `#define MAPFIELDDATA_SIZE 11` |
| ra2ff `src/MapReader.cpp:88–107` | `if(len % 11 != 0) error`；`numEntries = len / 11`；逐字段读 11 字节 |
| ModEnc / ModEnc² `IsoMapPack5` | “total bytes in the pack is **11 bytes × number of tiles** (+4 padding)” |
| 实测 | 82 764 = 7 524 × 11；7 524 = `(2×50−1)×76`，与 `[Map] Size=0,0,50,76` 精确吻合 |

**关于 130/132**：在 RA2/YR 地图**磁盘格式**中找不到 130 或 132 字节/瓦片的任何依据。它很可能与**游戏内存中的 CellClass（单元格对象）大小**或**其它游戏格式**混淆。结论：磁盘 `[IsoMapPack5]` 每瓦片 = **11 字节**；130/132 与本节无关（相关内存结构差异 → `待验证`，不属于地图文件格式）。

### 2.6 瓦片数量与地图尺寸的关系

- `[Map] Size = left,top,right,bottom`（4 个逗号分隔整数，`中`→实测确认由 FA2 按 left/top/right/bottom 解析，`m_IsoSize = right + bottom`）。
- 瓦片总数 = `(2×right − 1) × bottom`（ModEnc 公式，实测验证）。
- 例：`Size=0,0,50,76` → 瓦片数 = `(2×50−1)×76 = 99×76 = 7 524`。实测解压出的 82 764 字节 ÷ 11 = 7 524，**完全一致**。
- 瓦片坐标（§3 的坐标字段）落在等距菱形网格上：列 ∈ [0, 2×right−2]，行 ∈ [0, bottom−1]。`[Map]` 的 `LocalSize` 表示可视/本地子区域（left,top,right,bottom），`待验证`其在引擎中的确切用途。

### 2.7 点阵 → 屏幕砖墙映射（2026-09-05 实测定案，官方图 egypt/kirov + FinalAlert2 对照）

**全部 (2W−1)×H 条记录均为真实瓦片**（无"伴生半格"数据）。屏幕为**砖墙排布**：
水平行内菱形角挨角（60px 间距），奇数行右移半宽 30px、下移半高 15px，相邻行共享斜边。

- `px = (rx − ry − min(rx−ry)) · 30` —— 行内步进 60px，行间天然错位 30px（奇偶相位免费）
- `py = (rx + ry − min(rx+ry)) · 15` —— **行 = 反对角线 rx+ry，不对折**
- 网格列 = `(rx − ry − min)/2`；网格行 = `rx+ry−min`
- **行跨度 ≈ 2H**：`Size` 的 H 是"逻辑行"数（每逻辑行 = 2 条反对角线）。
  实测：egypt `Size=0,0,119,82` → 反对角线行 164 = 2×82；FinalAlert2 显示 119×82 与此一致。
- ⚠️ 两个历史陷阱：行按 H 裁剪 → 丢下半图；行对折+奇偶过滤 → 只剩半数行（可见链 ≈ H/2）。

---

## 3. 瓦片记录（11 字节）各字节含义

每 11 字节一个瓦片记录，物理布局如下（小端）：

| 字节偏移 | 大小 | 字段名 | 含义 | 置信度 |
|---|---|---|---|---|
| 0 | 2 | 坐标字段 A（uint16） | 单元格坐标之一。FA2 源码把**第一个** uint16 命名为 `wX`，但在算线性索引 `pos = wY + wX*IsoSize` 时把它当**行（y）**用（见 MapData.cpp:3288、注释 4011 “x=y and y=x in the mappack”） | 高（字段存在）／坐标轴向约定 中 |
| 2 | 2 | 坐标字段 B（uint16） | 单元格坐标之二。FA2 把它当**列（x）**用 | 同上 |
| 4 | 2 | `wGround` / 瓦片索引（uint16） | 剧场瓦片编号（指向 `<theater>.mix/iso<theater>.mix/*.tmp` 的瓦片，跨 tileset 连续编号）。**`0xFFFF` = 无瓦片**，载入时替换为 Clear 瓦片（索引 0） | 高 |
| 6 | 2 | `bData[0..1]` / 附加数据（WORD） | FA2 记作 `bMapData`（与 §3.1 的 subtile/变体相关）；ra2ff 记为 “zero1 / padding”（常为 0）。精确语义 待验证 | 中 |
| 8 | 1 | `bData[2]` / `bSubTile` | **子瓦片索引**：该瓦片内的 sub-tile 编号（`tiledata[tile].tiles[subTile]`，一个瓦片可跨多个单元格） | 高 |
| 9 | 1 | `bHeight` / 高度层（Z） | 该格高度层级 | 高 |
| 10 | 1 | `bData2` / 附加数据（BYTE） | FA2 记作 `bMapData2`；ra2ff 记为 “zero2 / padding”（常为 0）；ModEnc（TS 语境）记作 `IceGrowth`（仅 TS 雪地）。RA2/YR 语义 待验证 | 中 |

**三份来源对同一 11 字节的不同命名对照**（便于交叉核对）：

| 偏移 | FA2（权威） | ra2ff | ModEnc（偏 TS） |
|---|---|---|---|
| 0–1 | `wX` | `x` (uint16) | `X` (int16) |
| 2–3 | `wY` | `y` (uint16) | `Y` (int16) |
| 4–5 | `wGround` (uint16) | `tile` (uint16) | `TileIndex` 低 16 位 |
| 6–7 | `bData[0..1]`→`bMapData` | `zero1`(2) | `TileIndex` 高 16 位 |
| 8 | `bData[2]`→`bSubTile` | `subTile` | `TileSubIndex` |
| 9 | `bHeight` | `z` | `Level` |
| 10 | `bData2`→`bMapData2` | `zero2` | `IceGrowth` |

> 差异要点：ModEnc 把 4–7 字节整体当 32 位 `TileIndex` 并称 0xFFFF 为“无瓦片”，这与 FA2/ra2ff 的“16 位 `tile` + 2 字节附加/填充”冲突。**对 RA2/YR，以 FA2 为准：瓦片索引是 16 位（4–5 字节），0xFFFF 为哨兵**。ModEnc 该段描述更符合 TS（FinalSun）语义，此处已标注。

### 3.1 相关字段补充（来自 FA2 渲染代码，置信度：中）

- 游戏/FA2 内存里每个“格”会展开成一个较大的 `FIELDDATA` 结构（含 unit/infantry/aircraft/structure/terrain/overlay/overlaydata/wGround/bMapData/bSubTile/bHeight/bMapData2/celltag 等）。**这是内存表示，不是磁盘格式**。
- `wGround` 指向剧场瓦片表（`tiledata[]`）；`bSubTile` 从 0 开始索引该瓦片的多个 sub-tile（`tiledata[ground].tiles[bSubTile]`，`wTileCount` 为该瓦片 sub-tile 总数）。
- `bHeight` 为高度层；FA2 用相邻格 `bHeight` 差值（≥4）判定悬崖等（MapData.cpp:3368）。
- `bMapData`（6–7 字节）与 `bMapData2`（10 字节）在 FA2 中随瓦片读写，但渲染主路径主要依赖 `wGround` + `bSubTile` + `bHeight`；这两处附加数据的精确语义（是否与坡道/悬崖变体/随机图相关）**待验证**。

---

## 4. 其它二进制节（简述）

### 4.1 [OverlayPack] 与 [OverlayDataPack]

- 二者同为 **Base64（标准字母表）→ Format80(LCW) 单流解压**。**没有 Pack 块头**
  （不是 IsoMapPack5 的 2+2 块格式；此前"每块 uncompressedLength 恒为 8192"的描述是错的，
  那是对 LCW 命令流字节的误读）。OpenRA `ImportGen2MapCommand` 权威实现：`UnpackLCW` 直接解压整段。
- 解压结果 = **按格栅线性索引的 1 字节/格数组**，容量上限 256KB（512×512）：
  - `[OverlayPack]`：每格 = 覆盖物类型索引（OverlayTypes 编号），**`0xFF` = 无覆盖物**（不是 0）。
  - `[OverlayDataPack]`：每格 = 覆盖物附加数据（如资源密度/状态），同索引。
- 索引约定（OpenRA 实测）：`index = rx + 512 * ry`（rx/ry 为地图原始格坐标）。
  实测 `CrctBrd.yro`：解压 8198 字节 = 最高覆盖物格 (5,16)；`amazon.mmx`：10 个覆盖物
  （7 个 type=0 位于顶行边界 + (81,12) type=173），data 全 0。

### 4.2 两者与 IsoMapPack5 的对比

| 节 | 压缩 | 解压后内容 | 每格大小 |
|---|---|---|---|
| `[IsoMapPack5]` | LZO1X（format 5） | 稀疏瓦片记录数组 | 11 字节/记录 |
| `[OverlayPack]` / `[OverlayDataPack]` | Format80（LCW） | 密集 512×512 数组 | 1 字节/格 |

### 4.3 [PreviewPack]

- Base64 → Pack → **LZO1X** 解压（与 IsoMapPack5 同压缩）。
- 解压后为 **24 位 RGB 原始像素**，尺寸由 `[Preview]` 的 `Size=` 给出（`previewWidth × previewHeight × 3` 字节）。

---

## 5. Westwood INI 语法要点

依据：Chronoshift `src/game/common/ini.cpp`（Westwood `INIClass` 行为复刻）+ FA2 `MissionEditor/IniFile.cpp` + 实测地图。

### 5.1 节（Section）

- 一行以 `[` 开头且含 `]` 即视为节头；节名 = 第一个 `[` 与第一个 `]` 之间的文本，两端空白被裁剪；`]` 之后的内容忽略。
- 例：`[IsoMapPack5]` → 节名 `IsoMapPack5`。
- 空节可被跳过（无任何键值时，该节可能被丢弃，Chronoshift 如此处理）。

### 5.2 键值（key = value）

- 非节头行，若含 `=`，则在**第一个 `=`** 处切分：左侧 = 键（去首尾空白），右侧 = 值（去首尾空白）。
- 键为空、或值为空的行被忽略。
- 值内可再出现 `=`（如 base64 值，虽地图数据不含 `=` 填充）。
- 一行最多一个键值对。

### 5.3 注释与空行

- **`;` 是唯一注释符**：从行内第一个 `;` 起（含 `;`）到行尾被去除（并回删尾随空白）。
- 整行以 `;` 或 `=` 开头的行被跳过。
- 无 `//`、`#` 注释。空行忽略。
- 实测地图首行即 `; Map created with FinalAlert 2(tm) Mission Editor`。

### 5.4 大小写敏感性

- 节名与键名按**原始字节做 CRC 索引**（`INIClass` 用精确字符串 CRC），因此**大小写敏感**：`Theater` ≠ `theater`、`[Map]` ≠ `[map]`。
- 实践中游戏与 FA2 读写都用固定大小写（如 `Theater`、`IsoMapPack5`、`Size`），实现方应保留/匹配官方大小写。`中高`（以 Chronoshift 复刻为准；原引擎行为与之等价，但未逐字节比对原二进制，故留轻微余地）。

### 5.5 重复键

- 官方 `INIClass` 用“CRC 索引 + 链表”双结构：重复键名会触发 `Duplicate_CRC`（调试期视为错误），索引会指向**后出现**的那条（查表时“后者覆盖前者”）；链表中两条都保留，可被顺序枚举遍历到。
- FA2 的简化解析用 `std::map`，重复键直接覆盖（后者胜）。
- 结论：正常地图不依赖重复键；若必须处理，按“后者覆盖前者（按名查）/顺序保留（按序枚举）”实现并**待验证**游戏对特殊节的精确行为。

### 5.6 布尔值与数值

- 布尔写回形式：`true/false`（`INIClass::Put_Bool`）或地图中常见的 `yes/no`。读取 `Get_Bool` 只看**首字符（大小写不敏感）**：`1/T/Y` → true，`0/F/N` → false。
- 整数：十进制字符串；带 `h` 后缀 = 十六进制（`Get_Int` 检查末尾 `h`）。
- 浮点：`%f`；值末尾带 `%` 表示百分比（`Get_Float/Get_Double` 会除以 100）。
- 列表：值内用**逗号**分隔（如 `Size=0,0,50,76`、`Waypoint1=241,45`、`Allies=Neutral` 前的多值），由各节自行按逗号切分。

### 5.7 二进制节与键名约定

- 二进制节（`IsoMapPack5`/`OverlayPack`/`OverlayDataPack`/`PreviewPack`）用 `1=`,`2=`,`3=`,… 递增整数键存放分块后的 base64 文本，读取方按键名数值升序拼接（§2.1）。

---

## 6. Format80（LCW）命令表（供 [OverlayPack]/[OverlayDataPack] 参考）

Format80 是 Westwood 的 LCW 压缩，用于地图 Overlay 两节与旧 SHP。地图用法为**绝对偏移模式**（数据不以 0x00 开头；若流以 0x00 开头则为“相对偏移”变体，用于 >64KB 数据如 VQA，地图不用）。命令按**首字节**分派：

| 首字节（位） | 命令 | 字节数 | 动作 | 说明 |
|---|---|---|---|---|
| `10xx xxxx` | 短字面拷贝 | 1 | 从输入拷贝 `x`（6 位）字节到输出 | **`x==0`（即字节 `0x80`）= 流结束** |
| `0ccc rrrr` `rrrr rrrr` | 近距回拷 | 2 | 拷贝 `c+3` 字节，源 = 输出当前位置 − `r`（`r` 为 10 位，`c` 为 3 位） | `r==1` 即重复上一字节 |
| `11cc cccc` `pppp pppp` `pppp pppp` | 中长回拷 | 3 | 拷贝 `c+3` 字节，源 = 输出绝对位置 `p`（16 位） | 仅当 `c≠0x3E/0x3F` |
| `1111 1110`(`0xFE`) `cc cccc...`(2B) `ww`(1B) | 填充 | 4 | 向输出写 `c`（16 位）个字节 `w` | |
| `1111 1111`(`0xFF`) `cc...`(2B) `pp...`(2B) | 长回拷 | 5 | 拷贝 `c`（16 位）字节，源 = 输出绝对位置 `p`（16 位） | |

- 结束标记：`0x80`（短字面拷贝、计数 0）。
- 回拷允许源/目标区间重叠（做 `memmove` 语义的逐字节拷贝即可）。
- 依据：ra2ff `ra2ff.txt` §“Format80”与 `src/MapReader.cpp decode80()`、Shikadi `Westwood LCW`、FA2/XCC `encode80/decode5(...,80)`。三方一致。

---

## 7. 关键结论速览

1. **`[IsoMapPack5]` 每瓦片 = 11 字节**（非 130/132）：`坐标(2)+坐标(2)+瓦片号(2)+附加(2)+子瓦片(1)+高度(1)+附加(1)`。
2. **Base64 = 标准 RFC 4648 字母表**（`A–Z a–z 0–9 + /`，无 `=` 填充，70 字符/行），**不是自定义字母表**。
3. 解码管线 = **拼值 → 标准 Base64 → Pack(2+2 头) → LZO1X**；瓦片数 = `(2×width−1)×height`（width/height 取 `[Map] Size` 的第 3/4 字段）。
4. `[OverlayPack]`/`[OverlayDataPack]` 同管线但用 **Format80**，解压为 512×512×1 字节。
5. `.map/.yrm` = 纯 INI；`.yro/.mmx` = **116 字节二进制头** + `[MultiMaps]` + INI + 二进制 `[Digest]` 尾（116 字节头结构待验证）。

## 8. 待验证清单（未达多源确认）

- `.yro/.mmx` 116 字节二进制头（`00 00 03 00` + 112 字节）的确切结构/用途（ModEnc 称 Mix，实测非标准 Mix v1）。
- 瓦片记录 6–7 字节（`bMapData`）与 10 字节（`bMapData2`）的精确语义（RA2/YR）。
- 瓦片坐标字段的引擎侧轴向约定（FA2 存在 X/Y 互换）。
- `[Header]` 节是否被游戏读取及其 `Width/Height` 与 `[Map] Size` 不一致的原因。
- `[OverlayPack]` 512×512 数组的引擎侧行列/轴向约定。
- 原引擎 `INIClass` 是否严格大小写敏感（本文以 Chronoshift CRC 索引复刻为准）。
