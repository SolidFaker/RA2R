# 《红色警戒 2 / 尤里的复仇》FNT Unicode 位图字体格式

> 调研文档（洁净室复刻用）。本文只记录格式事实与规格，不包含任何代码实现。
> 所有整数均为**小端序（little-endian）**。

---

## 0. 信息来源（Source URLs）

| 来源 | 说明 | URL |
|---|---|---|
| ModdingWiki | Westwood Unicode BitFont Format（权威规格） | https://moddingwiki.shikadi.net/wiki/Westwood_Unicode_BitFont_Format |
| Phobos ra2fnt 工具 | game.fnt ↔ PNG 转换工具 | https://github.com/Phobos-developers/ra2fnt |
| 实测 | YR 1.001 中文版 `game.fnt`（1,792,690 字节）逐字段走通 | 本地 |

---

## 1. 文件头（0x1C + 65536×2 字节）

| 偏移 | 大小 | 字段 | 实测值 |
|---|---|---|---|
| 0x00 | 4 | 魔数 `"fonT"` | `fonT` |
| 0x04 | 4 | IdeographWidth（表意空格 0x3000 的宽度覆盖） | 20 |
| 0x08 | 4 | Stride（每行图像数据字节数，用于全部字形） | 3 |
| 0x0C | 4 | Lines（每个符号存储的像素行数） | 16 |
| 0x10 | 4 | FontHeight（实际字高，可含纵向 padding） | 17 |
| 0x14 | 4 | Count（字形图像块数量） | 33910 |
| 0x18 | 4 | SymbolDataSize（= 1 + Stride×Lines，须恒等） | 49 |
| 0x1C | 65536×2 | UnicodeTable：全 Unicode 16 位范围 → 字形块索引 **+1**（0 = 空） | — |

## 2. 图像数据

自偏移 `0x2001C` 起，Count × SymbolDataSize 字节：

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 1 | SymbolWidth（显示宽度，可小于 Stride×8） |
| 1 | Stride×Lines | 1bpp 位图（每字节最高位 = 最左像素） |

- 码点 → 字形：`index = UnicodeTable[cp] - 1`；index = -1（表值 0）= 空。
- 符号间无内嵌 padding，游戏渲染时自动加 1 像素间距。
- 块内被 SymbolWidth 裁掉的部分可能藏有数据（原版有 width=0 的"隐形"字形）。

## 2. TS 旧式 FNT（cache.mix / local.mix 内 6point/8point/editfnt 等）

实测 RA2 的 TS 系字体为 **21 字节头**（注意：XCC `t_fnt_header` 是 20 字节，
其 `zero` 为 1 字节；实测 RA2 字体 zero 为 **u16**，多一字节）：

| 偏移 | 大小 | 字段 | 实测 |
|---|---|---|---|
| 0 | 2 | size = 文件大小 | ✓ |
| 2 | 2 | id1 = 0x0002（TS/RA2） | ✓ |
| 4 | 2 | id2 = 0x000E | ✓ |
| 6 | 2 | id3 = 0x0014 | ✓ |
| 8 | 2 | cx_ofs（每字符宽表，n 字节） | 530 |
| 10 | 2 | image_ofs（字形位图区起点） | 1295 |
| 12 | 2 | cy_ofs（每字符 [y<<8\|cy] 表，n×2 字节） | 785 |
| 14 | 2 | id4 = 0 | ✓ |
| 16 | 2 | zero = 0 | ✓ |
| 18 | 1 | c_chars（字符数 - 1） | 如 8point=11 |
| 19 | 1 | cy（整字体高度，i8） | 如 8point=10 |
| 20 | 1 | cmax_x（最大宽） | 如 8point=64 |

- 字形偏移表位于 **偏移 20**（紧随头后）：n × u16，值为**相对 image_ofs 的偏移**。
- 字形位图 1bpp，行宽 = (宽+7)/8 字节取整，行数 = cy 表高字节。

## 3. 'fonT' 紧凑变体（huge.fnt / efnt.fnt / map.fnt 等编辑器字体）

- 魔数 **`"FONt"`（大写 F/N）**，头字段同 game.fnt，但**无全 64K Unicode 表**（表被截断），
  文件只有几十 KB。当前按"头一致即接受"处理（字形表留空，校验器用途）。

## 4. 实测验证

- `0x2001C + 33910 × 49 = 1,792,690` 与文件大小精确一致。
- `SymbolDataSize = 1 + 3×16 = 49` 与头字段一致。
- 中文版 game.fnt 的 `'A'`、`'中'`、表意空格 0x3000 等码点字形可查。
