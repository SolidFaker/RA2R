# 《红色警戒 2 / 尤里的复仇》AUD / WAV 音频格式

> 调研文档（洁净室复刻用）。本文只记录格式事实与规格，不包含任何代码实现。
> 所有整数均为**小端序（little-endian）**。

---

## 0. 信息来源（Source URLs）

| 来源 | 说明 | URL |
|---|---|---|
| XCC Utilities 源码 | `misc/aud_file.{h,cpp}`、`misc/cc_structures.h` | https://github.com/OlafvdSpek/xcc |
| OpenRA | `OpenRA.Mods.Common/AudioLoaders/AudLoader.cs` | https://github.com/OpenRA/OpenRA |
| 微软 WAVEFORMATEX 文档 | WAV fmt 结构（含 IMA ADPCM 扩展） | https://learn.microsoft.com/windows/win32/api/mmreg/ns-mmreg-waveformatex |
| 实测 | YR 1.001 语言包 459 个 WAV 全量解码；mod 提取的 AUD（ra_kaboom15/ra_tank5） | 本地 |

---

## 0.1 重要勘误（先读）

1. **RA2 AUD 头是 12 字节，不是 TD 文档常见的 14 字节。**
   实测 mod 仓库提取自游戏的 `.aud`：`[rate:2][size:4][uncomp:4][flags:1][comp:1]` 共 12 字节，
   `size` **从偏移 12 起算**，且 `12 + size == 文件大小` 精确成立。
2. **RA2 语言包 WAV 是 IMA ADPCM（fmt=17）**，不是 PCM。459 个语音 WAV 全部为
   `format=17, bits=4, blockAlign=512, samplesPerBlock=1017`（fmt 扩展 2 字节 cbSize + 2 字节 samples/block）。

---

## 1. AUD

### 1.1 文件头（12 字节）

| 偏移 | 大小 | 内容 |
|---|---|---|
| 0 | 2 | 采样率（如 22050） |
| 2 | 4 | 数据区大小（从偏移 12 起算） |
| 6 | 4 | 未压缩大小（字节） |
| 10 | 1 | flags：bit0=立体声，bit1=16bit |
| 12 | 1 | compression：0=无压缩，1=ZAP，99=ADPCM |

### 1.2 块（从偏移 12 起，连续排布，直到 size 耗尽）

| 字段 | 大小 | 内容 |
|---|---|---|
| fsize | 2 | 本块压缩后字节数 |
| dsize | 2 | 本块解压后字节数 |
| magic | 4 | `0x0000DEAF` = WW-ADPCM 压缩；其他（通常 0）= 原样数据 |
| data | fsize | 块数据 |

- `fsize == dsize`（magic≠DEAF）时：数据原样拷贝。
- magic=0xDEAF：WW-ADPCM 解码（见 §3）。

## 2. WAV（RA2 语言包）

标准 RIFF 容器。RA2 语言包实测：

- `fmt ` 块：`format=17`（IMA ADPCM）、1 声道、22050 Hz、`bits=4`、`blockAlign=512`、
  扩展 2 字节 cbSize + 2 字节每块样本数（=1017）。
- `fact` 块存在（样本总数）。
- `data` 块：长度 = 块数 × 512。

### 2.1 IMA ADPCM 块结构（每 512 字节一块）

| 偏移 | 大小 | 内容 |
|---|---|---|
| 0 | 2 | 预测值（i16） |
| 2 | 1 | 步长索引（0–88） |
| 3 | 1 | 保留 |
| 4 | 508 | 半字节流：1016 个 4bit 样本（低半字节先） |

每块解出 1 + 1016 = 1017 个样本。多声道时：先排每声道 4 字节头，再按**声道顺序**排各声道的半字节流。

## 3. WW-ADPCM / IMA ADPCM 解码

两种容器共用同一套 IMA ADPCM 核心（Westwood 的 AUD 压缩即 IMA ADPCM）：

- 步长表 `kStepTab[89]` 与索引表 `kIndexTab[8] = {-1,-1,-1,-1,2,4,6,8}`（标准 IMA）。
- 每个样本 4 bit：**低半字节在前**。
- 样本递推：
  ```
  diff = step>>3 + (n&4?step:0) + (n&2?step>>1:0) + (n&1?step>>2:0)
  if (n&8) diff = -diff
  sample = clamp16(predicted + diff)
  index = clamp(index + kIndexTab[n&7], 0, 88); step = kStepTab[index]
  ```
- AUD 多声道：压缩字节按声道平面排布（每声道 `fsize/ch` 字节连续），输出按声道交错。
- AUD 8bit 输出时，每样本 1 字节，取高 8 位并异或 0x80 转无符号。
- AUD 中 ZAP（compression=1）为游戏内极少使用的查表压缩，M1 按原样拷贝降级处理。

## 4. 实测验证

- 语言包 459/459 个 WAV（fmt=17）解码通过，样本数 = 块数 × 1017 精确一致。
- `ra_kaboom15.aud`（15748B，31 块）、`ra_tank5.aud`（16036B，31 块）：12 字节头，
  块链精确走通至文件尾，解码样本数 = uncomp/2 精确一致。
- 参考仓库 PCM WAV（`umenucl1.wav` 等）原样路径通过。
