# M0 实测验证备忘（对四篇格式文档的勘误与确认）

> 本文件记录 M0 阶段用真实资产（`Yuri/` 安装 + `starcity.yrm` 等地图）验证格式规格时
> 得到的结论。**以本文为准**：文中标注"勘误"处表示对应文档的结论有误。

## 1. MIX（mix.md）

- ✅ 确认：加密布局 `[4B 旗标][80B RSA 封装 Blowfish 密钥][Blowfish-ECB 头+索引]`；
  密钥用公开 RSA 参数（e=65537、320 位公开模数）`m^e mod n` 解封，**无需读取 exe**。
- ✅ 确认：正文文件**不加密**（嵌套 MIX 文件头直接明文可见）。
- ✅ 确认：真实 MIX 精确满足 `body_start + data_size + (校验和?20:0) == 文件大小`
  ——这是嵌套遍历时判别"真 MIX vs 恰好以 00 00 03 00 开头的数据"的可靠判据。
- ✅ 确认：文件 id = 标准 CRC-32（**init 0xFFFFFFFF / xorout 0xFFFFFFFF**，与 zlib 一致）
  作用于"大写 + 4 字节对齐混淆"后的名字。混淆规则：长度非 4 的倍数时先追加 `(len&3)`
  字节，再从索引 `(len&~3)` 复制字符补齐。
  - **勘误**：初版误用 init-0/xorout-0 变体，导致 C++ 名字查找全部失效；
    锚点验证：`crc("LOCAL MIX DATABASE.DAT" 混淆后) == 0x366E051F`。
- ✅ 实测：ra2.mix(21 条)/ra2md.mix(25 条) 顶层几乎全是嵌套 MIX；LOCAL.MIX(607)/LOCALMD(187)
  才是规则 INI 与单位资产所在；ISOURB/ISODES/ISOLUN 等 theater 包在顶层。
- 注：`local mix database.dat`（XCC 名称库）**不在** ra2/ra2md.mix 中；资产名以
  rules/art INI 的节名与 `Image=` 值为准（已实测 100 个名字全部命中）。

## 2. SHP（shp.md）

- ✅ 确认：8 字节头 `[0,w,h,frames]` + 24 字节帧头 + `flags bit1` 逐扫描线 RLE，
  与 `sidenc01.mix` 内实际文件逐字节吻合（帧表偏移数学精确验证）。
- ✅ 确认：TS/RA2 SHP **不使用 LCW/XOR delta**。

## 3. VXL（vxl.md）

- ✅ 确认：802 字节头、魔数 `"Voxel Animation"`、内嵌调色板 @0x22、
  span 布局 `SKIP|COUNT|COUNT×(color,normal)|COUNT`、列序 x 最快。
- ✅ 确认：RA2 244 条 / TS 36 条双法线表。
- ✅ 新发现：**RA2 资产中 `normal_type=2`（TS 表）与 `=4`（RA2 表）并存**
  （如 LTNK.VXL/TNKD.VXL 为 2，HARV/ZEP/CRUISE 为 4），渲染器必须按类型选表。

## 4. 地图（map.md）

- ✅ 确认：IsoMapPack5 用**标准 Base64**；但**必须把整节所有值拼接成一条流再解码**
  （逐行解码会因填充跨行而失败）。
- ✅ 确认：pack 流 = `[u16 压缩长][u16 解压长]` 块序列 + LZO1X 解压；
  （starcity.yrm 实测 10 块全部成功、流长精确吻合）。
- ✅ 确认：每瓦片 11 字节 = `rx,ry,tilenum,zero1,subtile,z,zero2`。
- ✅ 确认：单元格映射 `dx = rx-ry+W-1, dy = rx+ry-W-1, mx = dx/2, my = dy`，
  W/H = `[Map] Size` 的 (right-left, bottom-top)。
- ✅ **新发现（关键）**：**瓦片记录流跨块连续**——每块解压长度（典型 8192）不是
  11 的倍数，块尾余字节是下一记录的开头；必须拼接全部块后再解析。
  逐块解析会导致块间对齐错位（实测：3643 单元格 → 732，oob 翻倍）。
- ✅ 确认：OverlayPack/OverlayDataPack 用 Format80(LCW)（尚未在引擎中接入，M2 做）。
- 注：LZO1X 实现要点（canonical 语义）：首字节**窥视不消耗**（仅 >17 时消耗）；
  `match_done` 尾部扩展字节来自**输入流**（非匹配位置）；匹配允许与写位置重叠
  （逐字节前进拷贝）。

## 5. 引擎工具链备注

- MinGW（WinLibs UCRT）+ pip CMake + SDL3 mingw 预编译包，全部免管理员安装。
- 工具使用 `wmain`（`-municode`）支持中文路径（玩家地图目录）。
- 参考实现对照：`tools/debug_mappack.py`（Python 版 LZO1X+地图包解码，调试用）。
