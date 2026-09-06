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
- 每个 section 的变换是 12 个 float：**3×4 矩阵（行主序）**，即前三行是 3×3 旋转/缩放，
  第 4 列为平移；第 4 行恒为 `[0,0,0,1]` 不存储。

```
[ m0  m1  m2  m3 ]
[ m4  m5  m6  m7 ]
[ m8  m9 m10 m11 ]
[ 0   0   0   1  ]
```

## 4. 用法

渲染 VXL 第 f 帧时：对 VXL 的每个 section，取其 HVA 对应矩阵，作用到该 section 的体素坐标上。
单位矩阵（恒等变换）出现在静止单 section 模型中（实测 `MTNK.HVA`）。
HVA 中的 section 名应与 VXL 中同名 section 配对（按名字匹配，VXL 内无对应 section 时跳过）。

## 5. 实测验证

- 文件名与 VXL 同名同目录（`.HVA` 扩展名），可选——无 HVA 时视为全单位矩阵。
- `MTNK.HVA`：1 帧 × 1 section，矩阵为单位矩阵，名字 `"SEC_MTNK"` 与 VXL 头一致。
