# 红色警戒 2 / 尤里的复仇 — MIX 归档容器与 Blowfish 加密格式规格

> 文档状态：v1.0（实测 + 多源交叉核对）· 语言：中文 · 适用：RA2 1.006 / YR 1.001 加密与明文 MIX
> 本文件只记录**格式事实与数据常量**，不含任何代码实现（洁净室要求）。

---

## 0. 信息来源 URL

| 来源 | 关键文件 | 许可 / 性质 |
| --- | --- | --- |
| XCC Utilities 源码（Olaf van der Spek），GitHub 镜像 [OmniBlade/xcc](https://github.com/OmniBlade/xcc) | `xcc/misc/mix_file.cpp`、`mix_file.h`、`cc_structures.h`、`mix_decode.cpp`（`get_blowfish_key`）、`cc_file.cpp`、`misc/crc.h` | 原站 [xhp.xwis.net](https://xhp.xwis.net/)（MIX 事实标准实现） |
| [OpenRA](https://github.com/OpenRA/OpenRA)（`bleed` 分支） | `OpenRA.Mods.Cnc/FileSystem/MixFile.cs`、`FileFormats/BlowfishKeyProvider.cs`、`FileFormats/Blowfish.cs` | GPL-3.0 |
| [cnc-formats](https://github.com/iron-curtain-engine/cnc-formats)（Iron Curtain 引擎） | `src/mix/mod.rs`、`src/mix_crypt/mod.rs`、`src/mix_crypt/tests_vectors.rs` | MIT OR Apache-2.0（洁净室最干净的参照） |
| [Chronoshift](https://github.com/TheAssemblyArmada/Chronoshift)（`develop` 分支） | `src/game/io/mixfile.h`（`HAS_CHECKSUM=0x00010000`、`IS_ENCRYPTED=0x00020000`、SHA1 位于末尾 20 字节）、`src/game/crypto/blowfish.cpp` | GPL |
| [ModEnc](https://modenc.renegadeprojects.com) | MIX 页面、[Unprotecting a protected MIX](https://modenc2.markjfox.net/index.php?title=Unprotecting_a_protected_MIX) | 社区文档 |
| PPM 论坛 | [How to unprotect a protected MIX file](https://ppmforums.com/topic-22967/user-1625/omegabolt/) | 社区讨论 |
| 本机实测 | `I:\ai\RA2R\Yuri\` 下的 `ra2.mix`、`ra2md.mix`、`langmd.mix`、`thememd.mix`、`gamemd.exe` 等 | 只读字节核对 |

> 说明：XCC 源码与 OpenRA、Chronoshift 为 GPL/闭源许可，本文档**只从中摘录格式事实与常量**（字节布局、标志位值、公开密钥常量、文件偏移），不复制其代码。

---

## 1. 总览

MIX 是西木（Westwood）的**扁平归档**格式：只存文件内容与一个 12 字节/条目的索引，**不存文件名**。查找文件时，把文件名（大写）哈希成 32 位 `id`，再在按 `id` 排序的索引里二分查找。

存在两种互不兼容的变体：

1. **基础 MIX**（TD / RA1）：首 2 字节是文件数（> 0），无标志位、无校验、无加密。`thememd.mix` 属于此类。
2. **扩展 MIX**（TS / RA2 / YR）：首 2 字节恒为 `0x0000`（标记），随后是 16 位 `flags`。支持 SHA1 校验和与 Blowfish 加密。`ra2.mix`、`ra2md.mix`、`expandmd01.mix`、`language.mix`、`multimd.mix`、`mapsmd03.mix`、`movmd03.mix` 属于此类。

判定方法：读首 2 字节 `u16`。若 ≠ 0 → 基础 MIX；若 == 0 → 扩展 MIX（XCC 正是用 `union { {u16 c_files; u32 size}; u32 flags; }` 一次性兼容这两种）。

---

## 2. MIX 容器结构

### 2.1 扩展 MIX 文件头（TS / RA2 / YR）

| 偏移 | 大小 | 字段 | 字节序 | 含义 |
| --- | --- | --- | --- | --- |
| `0x00` | 2 | 标记 marker | LE | 恒为 `0x0000`，用于与基础 MIX（首字=文件数>0）区分 |
| `0x02` | 2 | flags | LE | 位 0（`0x0001`）= 含 SHA1 校验和；位 1（`0x0002`）= Blowfish 加密 |
| `0x04` | 2 | file_count | LE | 索引条目数（**仅非加密时可见**；加密时此处开始是 key_source） |
| `0x06` | 4 | body_size | LE | 数据区（文件正文）总字节数 = 文件大小 − 头部 − 索引 −（可选）20 字节校验 |
| `0x0A` | `12 × count` | 索引表 | — | 每条 12 字节，见 §2.2 |
| `0x0A + 12×count` | `body_size` | 数据区 | — | 各文件正文（明文） |
| 文件末尾 | 20 | SHA1 摘要 | — | 仅当 `flags & 0x0001`；位于**整个文件的最后 20 字节**，摘要对象是数据区 |

> **同一字节的两种视角**：`0x00..0x03` 四个字节，既可作为「2 字节标记 + 2 字节 flags」解读（OpenRA / cnc-formats），也可作为「一个 4 字节 u32 flags」解读（XCC / Chronoshift）：
> - u16 视角：`flags & 0x0001` = 校验和，`flags & 0x0002` = 加密；
> - u32 视角：`0x00010000` = 校验和（`HAS_CHECKSUM`），`0x00020000` = 加密（`IS_ENCRYPTED`）。
>
> 本机实测 `ra2.mix` 首 4 字节 = `00 00 03 00`：u16 视角 flags=`0x0003`（校验+加密）；u32 视角 = `0x00030000`。

### 2.2 索引条目（SubBlock，每条 12 字节）

| 偏移 | 大小 | 字段 | 字节序 | 含义 |
| --- | --- | --- | --- | --- |
| `0x00` | 4 | id | LE | 文件名的 32 位哈希（CRC-32，见 §2.3） |
| `0x04` | 4 | offset | LE | **相对数据区起始**的字节偏移（不是相对文件头） |
| `0x08` | 4 | size | LE | 该文件正文字节数 |

要点：

- 三个字段全部**小端（little-endian）**。
- 索引按 `id` **升序**排列，供二分查找。注意西木工具在磁盘上按**有符号 i32** 比较排序（`id` 最高位为 1 的条目会被当作负数排到前面）——cnc-formats 在解析后会改用无符号 u32 重排以配合二分查找。**（此边界细节待验证：对 id ≥ 0x80000000 的条目，有符号/无符号排序结果不同，但游戏原始二分查找具体用哪种比较尚未在本文档确认。）**
- `offset` 是相对「数据区起始」的偏移；XCC 在读取时统一加上头部长度换算成绝对偏移（基础 MIX 加 `6+cb_index`，扩展明文 MIX 加 `10+cb_index`，加密 MIX 加 `92+cb_f`，见 §3.4）。

### 2.3 文件名 id（哈希）与「字节序问题」

- `id` 本身是 4 字节小端整数（**存储字节序**是 LE）。
- 计算方式（TS / RA2 / YR）：
  1. 文件名转**大写**，`/` 替换为 `\`；
  2. 做填充使长度为 4 的倍数（XCC 的填充怪癖：若长度 `l` 不是 4 的倍数，先追加一个字节，值为 `l mod 4`，再用 `name[l & ~3]` 的那个字符补齐到 4 的倍数——**此填充细节各实现不完全一致，标记待验证**）；
  3. 对填充后的字符串计算**标准 CRC-32**（IEEE 802.3，多项式 `0x04C11DB7`，反射，初值/异或出 `0xFFFFFFFF`，即 `boost::crc_32_type`）。
- 注意与 **TD / RA1 基础 MIX** 的哈希**不同**：基础 MIX 用的是「`id = rotl(id,1) + 每组 4 字节大端解释`」的累加哈希（XCC `get_id` 的 `default` 分支），而 TS/RA2/YR 用 CRC-32。**解析 RA2/YR 时必须用 CRC-32。**
- 「字节序问题」的具体含义：哈希在 4 字节分组时按**大端**读取每组（`b0<<24 | b1<<16 | b2<<8 | b3`），但最终 `id` 存盘是**小端**；且排序比较存在上述有符号/无符号差异。二者叠加容易踩坑，实现时需分别对待。

### 2.4 SHA1 校验和（`flags & 0x0001`）

- 位置：**文件末尾的最后 20 字节**（在数据区之后，不是夹在索引与数据区之间）。Chronoshift 明确注释 "the last 20 bytes are a SHA1 digest"；XCC 的长度校验公式也把 `+ (m_has_checksum ? 20 : 0)` 放在 `body_size` 之后。
- 内容：对**数据区（body）**（从数据区起始到文件末尾 − 20 字节）计算的 SHA-1 摘要，标准 20 字节、大端十六进制习惯。
- 本机实测验证（`langmd.mix`，明文+校验，flags=`0x0001`，count=10，body_size=`0x050B8CB0`）：
  - 索引位于 `0x0A..0x82`（10×12=120 字节），数据区 `0x82..len-20`，摘要位于 `len-20..len`；
  - 数据区 SHA1 = `3D209F59129495FA35401C7EF376D16AC86F101E`，与文件末尾 20 字节**完全一致**（详见 §4）。

### 2.5 基础 MIX（TD / RA1，无扩展头）

| 偏移 | 大小 | 字段 | 说明 |
| --- | --- | --- | --- |
| `0x00` | 2 | file_count | LE，> 0 |
| `0x02` | 4 | body_size | LE，数据区字节数 |
| `0x06` | `12 × count` | 索引表 | 同 §2.2 |
| `0x06 + 12×count` | body_size | 数据区 | — |

本机 `thememd.mix`：首 2 字节 `0A 00`（count=10），body_size=`0x02CB02E0`=46,858,976，恰等于 `文件大小 − 6 − 10×12`。

---

## 3. Blowfish 加密（`flags & 0x0002`）

### 3.1 加密 MIX 的完整布局

| 偏移 | 大小 | 字段 | 是否加密 | 说明 |
| --- | --- | --- | --- | --- |
| `0x00` | 2 | 标记 | 否 | `0x0000` |
| `0x02` | 2 | flags | 否 | bit1 置位（`0x0002`） |
| `0x04` | **80** | key_source | **否（明文）** | RSA 密钥派生的输入，见 §3.2 |
| `0x54` | `N` | 加密头 | **是** | Blowfish ECB；含 file_count(u16)+body_size(u32)+索引，`N = ceil((6 + 12×count)/8) × 8` |
| `0x54 + N` | body_size | 数据区 | **否（明文）** | 各文件正文 |
| 末尾 | 20 | SHA1 | 否 | 仅当 `flags & 0x0001`（ra2.mix / ra2md.mix 二者都有） |

### 3.2 密钥来源：**不是 exe 里的固定密钥，也不是单一 56 字节常量**

这是本调研最关键的结论，纠正一个常见误解：

> **Blowfish 密钥不是从 `game.exe` / `ra2.exe` / `gamemd.exe` 里直接抠出的一个固定 56 字节常量，而是「每个加密 MIX 自己内嵌一段 80 字节 key_source，用公开的 RSA 公钥对它做模幂运算派生」得到的、按文件各异的 56 字节密钥。**

唯一的固定常量是一个 **RSA 公钥**（社区公开、二十余年未变）：

| 项 | 值 |
| --- | --- |
| 公钥字符串（base64，无填充） | `AihRvNoIbTn85FZRYNZRcT+i6KpU+maCsEqr3Q5q+LDB5tH7Tz2qQ38V` |
| base64 解码（42 字节，DER 风格） | `02 28 51 BC DA 08 6D 39 FC E4 56 51 60 D6 51 71 3F A2 E8 AA 54 FA 66 82 B0 4A AB DD 0E 6A F8 B0 C1 E6 D1 FB 4F 3D AA 43 7F 15`（`02`=INTEGER 标签，`28`=0x28=40，其后为 40 字节模数） |
| RSA 模数 n（40 字节，大端） | `51 BC DA 08 6D 39 FC E4 56 51 60 D6 51 71 3F A2 E8 AA 54 FA 66 82 B0 4A AB DD 0E 6A F8 B0 C1 E6 D1 FB 4F 3D AA 43 7F 15`（约 319 位） |
| RSA 公钥指数 e | `0x10001` = 65537 |

来源一致性：XCC `mix_decode.cpp` 的 `pubkey_str`、OpenRA `BlowfishKeyProvider.cs` 的 `PublicKeyString`、cnc-formats `mix_crypt/mod.rs` 的 `PUBKEY_STR` **三者逐字符相同**；指数三者均为 `0x10001`。

#### 公钥在 exe 中的位置（本机实测）

`I:\ai\RA2R\Yuri\gamemd.exe`（YR 1.001，4,813,072 字节）内，ASCII 明文命中：

- 偏移 `0x3E1A8E` 起为 base64 串 `AihRvNoIbTn85FZRYNZRcT+i6KpU+maCsEqr3Q5q+LDB5tH7Tz2qQ38V`；
- 前文语境为 `[cKey]` + 换行 + `1=`（即该串是 `gamemd.exe` 内嵌配置 `[cKey]` 段的一个键值，形式 `[cKey]\n1=<base64>\n`，串前的 `[` 位于 `0x3E1A85`，`1=` 位于 `0x3E1A8C`）。

> 本安装中的 `ra2.exe`、`YURI.exe`、`Ra2md.exe`、`Blowfish.dll` **未命中该 ASCII 串**。纯 RA2 安装的 `game.exe` 内确切偏移**待验证**（社区普遍认为 RA2 与 YR 使用同一公钥，XCC 对 RA2/YR 混合也只用这一把公钥）。**提取步骤**：无需任何 RSA 私钥——该公钥连同 `e=0x10001` 直接硬编码进引擎即可，这是 XCC/OpenRA/Chronoshift/cnc-formats 的共同做法。

### 3.3 密钥派生算法（56 字节 Blowfish 密钥）

输入：加密 MIX 偏移 `0x04` 起的 **80 字节 key_source**。输出：**56 字节**（448 位）Blowfish 密钥。

1. 由公钥模数 n 求 `a = (bitlen(n) − 1) / 8`。对本公钥 n（319 位），`a = 39`。
2. 把 80 字节 key_source 切成若干块，每块 `a+1 = 40` 字节。
3. 每块按**小端**解释为**无符号大整数** `m`。
4. 计算 `c = m^e mod n`（RSA 公钥运算，`e = 0x10001`）。
5. 取 `c` 的**低 `a = 39` 字节**（小端）作为该块的输出。
6. 拼接各块输出（2 块 × 39 = 78 字节），取**前 56 字节** = Blowfish 密钥。

XCC 的 `len_predata()` 为 `(55/a + 1) × (a+1)` = `(55/39+1) × 40` = 80，与 key_source 长度一致，印证 `a=39`。

**本机验证**：用 .NET `BigInteger.ModPow` 复现上述派生，先对 cnc-formats 的两个官方测试向量逐一比对：
- `42^0x10001 mod n` = `0x48F15EEE457C6C99154DA38B24DE1251FA72EC8F5625E4C2629348A79C7819EC718A4B81953DD655` — **完全一致**；
- 80 字节已知 key_source → 56 字节密钥 `7F578C161987D9DF1D22EE15D72F9FE35680BAB2CE2E7C7BA068FBB4DD9A5C1C396A3E0F2A526FB73770C90CE81BF2FFE72CAC5B87FA7444` — **完全一致**。

（注：上述两个向量来自 cnc-formats `tests_vectors.rs`，可作为读者实现的自检基线。）

### 3.4 加密范围（回答「哪些加密、哪些不加密」）

| 区域 | 是否加密 |
| --- | --- |
| 标记 + flags（0x00..0x04） | **否**（明文，否则无法得知是加密文件） |
| 80 字节 key_source（0x04..0x54） | **否**（明文，是密钥派生的输入） |
| file_count + body_size + 索引（0x54 起） | **是**，Blowfish ECB 整体加密，按 8 字节块对齐 |
| 数据区（各文件正文） | **否**（明文）——XCC `get_vdata`、OpenRA `GetContent`、cnc-formats 都只解密头部/索引，**不解密文件数据** |
| SHA1 摘要（末尾 20 字节） | **否** |

> 结论：RA2/YR 加密 MIX **只加密「文件头 + 索引」，不加密文件正文**。任务里「每个文件前 80 字节是否加密」的说法对主 MIX（`ra2.mix`/`ra2md.mix`）**不成立**——「80」对应的是文件头部的 80 字节 key_source，而非每个文件。若另有缓存类 MIX（`rmcache/` 等）采用逐文件加密，属另一变体，**待验证**。

### 3.5 解密流程（顺序与块对齐）

1. 读 `0x02` 处 flags，确认 `flags & 0x0002`。
2. 读 `0x04..0x54` 共 80 字节 key_source。
3. 按 §3.3 派生 56 字节 Blowfish 密钥。
4. 初始化 Blowfish（标准 16 轮、56 字节密钥、**ECB 模式**、8 字节块）。
5. 从 `0x54` 起读第一个 8 字节块并解密 → 得到 `file_count`（u16 LE）与 `body_size`（u32 LE）。
6. 计算需要解密的总块数 `ceil((6 + 12×count) / 8)`；继续按 8 字节块解密到完整索引（`count × 12` 字节）。
7. 数据区起始 = `0x54 + 块数 × 8`；此后 `body_size` 字节为明文文件数据。
8. 若 `flags & 0x0001`，读文件末尾 20 字节 SHA1 与数据区 SHA1 比对。

**字节序细节（西木的 `reverse()` 约定）**：每个 8 字节块按两个小端 u32 字读入，先各自字节交换成 big-endian 字，再跑标准 Blowfish Feistel 轮，输出再字节交换回 little-endian。用现成「标准 big-endian Blowfish」实现时，直接喂小端块即可得到等价结果（cnc-formats 使用 `blowfish` crate 的标准 `Blowfish` 类型，注释明确此点）。

---

## 4. 本地文件实测线索（只读，不改动文件）

工作区 `I:\ai\RA2R\Yuri\` 有一份完整 YR 1.001 安装。以下命令均为**只读**（PowerShell），可直接复现本规格的核对过程。

### 4.1 看文件头字节

```powershell
Format-Hex -Path 'I:\ai\RA2R\Yuri\ra2.mix' -Count 96
Format-Hex -Path 'I:\ai\RA2R\Yuri\ra2md.mix' -Count 96
Format-Hex -Path 'I:\ai\RA2R\Yuri\langmd.mix' -Count 160
Format-Hex -Path 'I:\ai\RA2R\Yuri\thememd.mix' -Count 32
```

预期（首 4 字节，扩展头视角 = 标记 + flags）：

| 文件 | 首 4 字节 | 标记 | flags | 含义 |
| --- | --- | --- | --- | --- |
| ra2.mix | `00 00 03 00` | `0x0000` | `0x0003` | 加密 + 校验 |
| ra2md.mix | `00 00 03 00` | `0x0000` | `0x0003` | 加密 + 校验 |
| expandmd01.mix | `00 00 03 00` | `0x0000` | `0x0003` | 加密 + 校验 |
| language.mix | `00 00 03 00` | `0x0000` | `0x0003` | 加密 + 校验 |
| langmd.mix | `00 00 01 00` | `0x0000` | `0x0001` | 仅校验，**未加密** |
| multimd.mix | `00 00 02 00` | `0x0000` | `0x0002` | 仅加密，无校验 |
| mapsmd03.mix | `00 00 02 00` | `0x0000` | `0x0002` | 仅加密 |
| movmd03.mix | `00 00 03 00` | `0x0000` | `0x0003` | 加密 + 校验 |
| thememd.mix | `0A 00 E0 02` | —（基础 MIX） | — | 无扩展头，count=10 |

> 用 `langmd.mix`（明文+校验）与 `thememd.mix`（基础）即可在不涉及 Blowfish 的情况下验证 §2 的头部/索引/校验和布局。

### 4.2 验证 SHA1 校验和（langmd.mix，纯 .NET/只读）

`langmd.mix`（84,643,142 字节）：count=10、body_size=`0x050B8CB0`；数据区在 `0x82` 到 `len−20`，SHA1 在 `len−20..len`。计算数据区 SHA-1 应与末尾 20 字节一致（本机结果为 `3D209F59129495FA35401C7EF376D16AC86F101E`，一致）。可用 `Get-FileHash -Algorithm SHA1` 对截取出的数据区临时文件复核（临时文件用后删除，不改动原文件）。

### 4.3 验证公钥在 exe 中的位置（gamemd.exe，只读）

```powershell
# 在 gamemd.exe 中定位 ASCII 串 "AihRvNoIbTn85FZRYNZRcT"
$b = [System.IO.File]::ReadAllBytes('I:\ai\RA2R\Yuri\gamemd.exe')
$needle = [System.Text.Encoding]::ASCII.GetBytes('AihRvNoIbTn85FZRYNZRcT')
# 顺序扫描命中后打印偏移（本机 = 0x3E1A8E）
```

### 4.4 验证密钥派生（可选，.NET BigInteger，只读）

按 §3.3 用 `[System.Numerics.BigInteger]::ModPow` 对 `ra2.mix` 偏移 `0x04..0x54` 的 80 字节 key_source 做 `m^0x10001 mod n`（n 取 §3.2 的 40 字节模数），取低 39 字节×2、前 56 字节。可先跑 §3.3 的两个官方测试向量自检，再算 ra2.mix / ra2md.mix，二者派生密钥**不同**（key_source 不同）。

---

## 5. 待验证 / 存疑项

1. **纯 RA2 `game.exe` 内公钥串的确切偏移**：本安装（YR）仅在 `gamemd.exe` `0x3E1A8E` 命中；`ra2.exe`/`YURI.exe`/`Blowfish.dll` 未命中 ASCII 串。RA2 原版 exe 偏移**待验证**（但公钥值本身 RA2/YR 通用，已被 XCC 证实）。
2. **文件名 CRC 的精确填充规则**：XCC 的 `name[a]` 填充怪癖与 cnc-formats「零填充」描述不一致（cnc-formats 实际描述的是 TD/RA1 的 rotl+add 哈希）。TS/RA2/YR 用标准 CRC-32 这一点多源一致，但「非 4 倍数长度如何补齐」的字节级细节**待验证**。
3. **索引排序的有符号 vs 无符号比较**：磁盘上西木按有符号 i32 排序（cnc-formats 注释），游戏二分查找用的比较方式**待验证**（影响 id ≥ `0x80000000` 的条目）。
4. **「每文件前 80 字节加密」**：对主 MIX 已证伪（只加密头+索引）；若指 `rmcache/` 缓存 MIX 或其它变体，**待验证**。
5. **key_source 与「Blowfish.dll」的关系**：`Blowfish.dll`（COM 组件，随 RA2 安装附带）内部机制未在本调研中拆解；社区普遍用纯软件重实现（本文档算法）替代之。

---

## 6. 关键结论（3–5 条）

1. **Blowfish 密钥的确切获得方式**：不是 exe 里抠固定常量，而是——读加密 MIX 偏移 `0x04` 的 **80 字节 key_source** → 用公开 RSA 公钥（base64 `AihRvNoIbTn85FZRYNZRcT+i6KpU+maCsEqr3Q5q+LDB5tH7Tz2qQ38V`，模数 `0x51BCDA08…43 7F15`，指数 `0x10001`）做 `m^e mod n` → 拼两段各 39 字节 → 取前 56 字节。**密钥按文件各异**（ra2.mix 与 ra2md.mix 派生结果不同），该公钥常量可硬编码，且在本机 `gamemd.exe` 偏移 `0x3E1A8E` 可查到原文。
2. **加密范围**：只加密「file_count + body_size + 索引」这一整块（Blowfish ECB、8 字节对齐）；flags、key_source、文件正文、SHA1 均为明文。**文件数据不加密**。
3. **容器布局**：扩展 MIX = `[0x0000 标记][u16 flags][u16 count][u32 body_size][索引 12B/条][数据区][末尾可选 20B SHA1]`；flags 位 0=校验、位 1=加密（u16 视角，等价 u32 的 `0x00010000`/`0x00020000`）。
4. **SHA1 校验和位置**：在**文件末尾 20 字节**，摘要对象是数据区；已用 `langmd.mix` 实测吻合。
5. **id 的字节序坑**：id 存盘为小端 u32，但 TS/RA2/YR 用**标准 CRC-32（大写文件名）**计算（与 TD/RA1 的 rotl+add 哈希不同），且索引排序存在有符号/无符号 i32 差异——这是实现二分查找时最易踩的坑。
