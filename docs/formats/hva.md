# 《红色警戒 2 / 尤里的复仇》HVA 体素动画格式

> 调研文档（洁净室复刻用）。本文只记录格式事实与规格，不包含任何代码实现。
> 所有整数均为**小端序（little-endian）**，浮点数为 **IEEE-754 单精度（float32）**。

---

## 0. 信息来源（Source URLs）

| 来源 | 说明 | URL |
|---|---|---|
| XCC Utilities 源码 | `misc/hva_file.{h,cpp}`、`misc/cc_structures.h` | https://github.com/OlafvdSpek/xcc |
| OpenRA | `OpenRA.Mods.Cnc/FileFormats/HvaReader.cs` | https://github.com/OpenRA/OpenRA |
| 实测 | YR 1.001 多份 HVA（含 `MTNK.HVA` 1 帧 × 1 section 单位矩阵） | 本地游戏目录 |

---

## 1. 文件头（24 字节）

| 偏移 | 大小 | 内容 |
|---|---|---|
| 0 | 16 | 动画标识字符串（如 `"VOXEL_ANIM_..."`），NUL 填充 |
| 16 | 4 | 帧数 n_frames |
| 20 | 4 | section 数 n_sections |

## 2. section 名字表

紧随文件头之后，n_sections × 16 字节：每个 section 一个 16 字节名字（ASCII，NUL 填充）。
名字通常形如 `"SEC_A"`、`"SEC_TURR"` 等，对应 VXL 内部的 section 名（VXL 头部的名字表）。

## 3. 帧变换矩阵

名字表之后是 n_frames × n_sections × 12 个 float32，**帧主序（frame-major）**：

- 矩阵下标：`matrix_index = frame * n_sections + section`。
- 每个 section 的变换是 12 个 float = 3×4 矩阵，**文件内按列存储**：每 4 个 float 是一列
  （3 个旋转/缩放分量 + 1 个平移分量），第 4 行恒为 `[0,0,0,1]` 不存储。

```
文件顺序:  [c0r0 c0r1 c0r2 tx] [c1r0 c1r1 c1r2 ty] [c2r0 c2r1 c2r2 tz]
等价的 3×4 行主序矩阵:
[ c0r0  c1r0  c2r0  tx ]
[ c0r1  c1r1  c2r1  ty ]
[ c0r2  c1r2  c2r2  tz ]
[ 0     0     0     1  ]
```

> **勘误（实测）**：文件里存的是**列主序**，不是行主序——把 12 个 float 直接当行主序读，
> 得到的 3×3 是真实旋转的转置（即逆旋转），平移分量下标 3/7/11 恰好不受影响。
> 依据 OpenRA `HvaReader`：`ids = {0,4,8,12, 1,5,9,13, 2,6,10,14}` 把文件序列写入 4×4
> 行主序矩阵的对应下标（等价于转置）。

## 3.1 平移分量的单位（重要）

平移分量（tx/ty/tz）的单位是 **1/16 体素**，渲染时需除以 16 才是体素坐标。
实测依据（`t` 为文件原值）：

| 模型 | 分量 | 原值 | /16 | 几何校验 |
|---|---|---|---|---|
| `YAGGUN.HVA` 炮管 | tz | 479.641 | 30.0 | 炮塔枢轴高度（炮塔节 z 22.6..39.7） |
| `YTNKTUR.HVA` 炮塔 | tz | 256.7 | 16.0 | 车体高 15 体素 → 炮塔正落在车体顶 |
| `1TNKBARL.HVA` 炮管 | tz | 139.254 | 8.7 | 车体高 11 体素，炮管在炮塔高度 |

全库 139 个非零平移 section 按 1/16 换算后均落在模型尺寸范围内；按 `t × VXL节scale`
换算则会把 `YTNKTUR` 抬到车体之上 6 个体素（错误）。不除以 16（早期实现）会让部件
飞出几十到几百体素——表现为"炮管变白色条状物乱飞/闪烁"。

## 4. 用法

渲染 VXL 第 f 帧时：对 VXL 的每个 section，取其 HVA 对应矩阵，作用到该 section 的体素坐标上。
单位矩阵（恒等变换）出现在静止单 section 模型中（实测 `MTNK.HVA`）。
HVA 中的 section 名应与 VXL 中同名 section 配对（按名字匹配，VXL 内无对应 section 时跳过）。

## 5. 实测验证

- 文件名与 VXL 同名同目录（`.HVA` 扩展名），可选——无 HVA 时视为全单位矩阵。
- `MTNK.HVA`：1 帧 × 1 section，矩阵为单位矩阵，名字 `"SEC_MTNK"` 与 VXL 头一致。
- `YAGGUN.HVA`：2 帧 × 3 section（`DUMMY03`/`DUMMY01` 双枪管 + `DUMMY02` 炮塔体）。
  第 0 帧为单位旋转 + 平移；第 1 帧两根枪管分别绕自身轴（x 轴）转 ±60°（枪管旋转动画），
  平移不变。平移 (111.5, ±164.5, 479.6) → /16 = (7.0, ±10.3, 30.0) 体素：炮管自炮塔
  前方伸出、两管横向对称 ✓。
- 全库统计：217 个 HVA，139 个 section 含非零平移，全部符合 1/16 换算。
