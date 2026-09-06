# 《红色警戒 2 / 尤里的复仇》调色盘文件与 SHP 自动识别规则

> 调研文档（洁净室复刻用）。本文只记录格式事实与规格，不包含任何代码实现。

---

## 0. 信息来源

| 来源 | 说明 | URL |
|---|---|---|
| 实测 | RA2 1.006 / YR 1.001 中文版 `ra2.mix`、`ra2md.mix` 递归反查 | 本地游戏目录 |
| ModEnc | PAL 页面 | https://modenc.renegadeprojects.com/PAL |

---

## 1. PAL 文件格式

- 原始调色盘：768 字节 = 256 × RGB 三元组（每通道 6 位：0–63，显示时 ×4 展开）。
- JASC-PAL 文本格式：首行 `JASC-PAL`，其后 `0100`（版本）、`256`（颜色数），
  每行 `R G B`（0–255 十进制）。
- 个别调色盘文件为 1024 字节（前 256 字节可能是额外数据），按 768 字节主体处理。

## 2. RA2 / YR 已知调色盘全集（实测反查结果）

| 文件 | 用途 | 出现位置 |
|---|---|---|
| `ISOTEM.PAL` | 温和（temperate）剧场地形 | ra2.mix（嵌套） |
| `ISOSNO.PAL` | 雪地剧场地形 | ra2.mix |
| `ISOURB.PAL` | 城市剧场地形 | ra2.mix |
| `ISOLUN.PAL` | 月球剧场地形 | ra2md.mix（YR 追加） |
| `ISODES.PAL` | 沙漠剧场地形 | ra2md.mix（YR 追加） |
| `UNITTEM.PAL` / `UNITSNO.PAL` / `UNITURB.PAL` | 单位/建筑（对应剧场） | ra2.mix |
| `UNITLUN.PAL` / `UNITDES.PAL` | 单位（月球/沙漠，YR） | ra2md.mix |
| `ANIM.PAL` | 通用动画（conquer.mix / cache.mix） | ra2.mix |
| `CAMEO.PAL` | 侧栏图标 | ra2.mix |
| `PALETTE.PAL` | 菜单/杂项 | ra2.mix |
| `TEMPERAT.PAL` / `SNOW.PAL` | 剧场总调色盘（RA2 两剧场） | ra2.mix |
| `URBANN.PAL` / `LUNAR.PAL` / `DESERT.PAL` | 剧场总调色盘（YR 三剧场） | ra2md.mix |

> 注意：探测过但不存在的名字——`MOUSE.PAL`、`ISOUNI.PAL`、`URBANG.PAL`、`URBAN2.PAL`、
> `UNITSMO.PAL`、`CACHER.PAL`（保留在候选表里以便其它版本兼容）。

## 3. SHP 自动识别调色盘（mixbrowser 实现的启发式）

SHP 文件本身**不携带调色盘信息**；游戏按“哪个系统在画它”选盘。识别用两级上下文：

1. **SHP 条目名**；
2. **所在 MIX 的路径**（如 `ra2.mix / ISOURB.MIX`）。

规则（按优先级）：

| # | 条件 | 选择 |
|---|---|---|
| 1 | SHP 名或路径含 `CAMEO` / `MOUSE` | `CAMEO.PAL` / `MOUSE.PAL` |
| 2 | 路径含剧场 token：`SNO` > `URB` > `LUN` > `DES`（默认 `TEM`） | 记作 `<剧场>` |
| 3 | 路径含 `ISO` | `ISO<剧场>.PAL`（地形） |
| 4 | 路径含 `CACHE` / `ANIM` / `CONQ` | `ANIM.PAL`（动画） |
| 5 | 其余（ra2 / ra2md / local / localmd / expand 等顶层） | `UNIT<剧场>.PAL`（单位） |
| 6 | 候选盘在池中不存在 | 回退灰度 |

### 调色盘池

- 从**所有已打开会话的根 MIX** 递归查找 §2 的候选名（最深 3 层）。
- 若仍有缺失，扫描根 MIX 同目录下 `RA2*` / `RA2MD*` / `EXPAND*` / `LOCAL*` 兄弟 MIX
  （调色盘载体实测为 ra2.mix + ra2md.mix）。
- 用户可在预览面板手动改选，自动结果仅作初始选择。

## 4. 实测验证

- `ra2.mix`（加密）池构建：11 个调色盘全中；兄弟扫描 `ra2md.mix` 补上 7 个 YR 盘（18/18）。
- 嵌套导航进 `ISOURB.MIX`（1257 条目，id 80E03363）选 SHP → 自动选 `ISOURB.PAL`；
  截图统计确认预览出现非灰彩色像素（urban 地形色），证明非灰度渲染。
- 单文件打开无调色盘载体时正确回退灰度并提示“池中缺失”。
