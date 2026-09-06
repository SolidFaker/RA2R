# 社区参考与资源索引

> 用途：M0 调研入口。注意区分"可读文档/规格"（洁净室可用）与"GPL/受许可约束代码"（仅 D1 决策为 B 路线时可吸收）。
>
> **本地参考副本**：`third_party/reference/`（已 gitignore，不入库）存放部分开源参考的克隆/抓取
> （Chronoshift、OpenRA YR mod、ra2-remixer、OpenRA LZOCompression、SDL3 对话框源码等）。
> 获取/恢复方式：`tools/fetch_references.ps1`（需本机 SOCKS5 代理）。

## 1. 同类开源项目

| 项目 | 语言 | 说明 | 许可 | 参考价值 |
| --- | --- | --- | --- | --- |
| [Chronoshift](https://github.com/TheAssemblyArmada/Chronoshift) | C++ | RA2/TS 引擎复刻（2020 公开源码） | GPL-3.0 | MIX/SHP/VXL/INI/地图解析与等距渲染的最大代码参考 |
| [vera20k](https://github.com/Yrvera/vera20k) | 待评估 | 面向 YR 的游戏引擎 | 待 M0 确认 | 现代 YR 引擎实现，M0 期间评估 |
| [ChronoStorm](https://github.com/cookgreen/ChronoStorm) | Python | RA2/YR 引擎（Pygame） | 开源 | 格式处理与快速原型思路 |
| [OpenRA](https://github.com/OpenRA/OpenRA)（含 ra2 分支） | C# | RA/TD/D2K 引擎 | GPL-3.0 | 等距渲染、画家排序、资产管线、确定性模拟架构 |
| [Vanilla-Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer) | C++ | TD/RA1 官方 GPL 源码改造 | GPL-3.0 | 等距引擎架构（无体素）、画格排序 |
| [Phobos](https://github.com/Phobos-developers/Phobos) | C++ | YR 扩展框架（DLL 注入） | LGPL | rules.ini 语义实现的最权威参照 |
| [Ares](https://github.com/Ares-Developers/Ares) | C++ | YR 扩展框架（前辈项目） | LGPL | 同上 |
| [YRpp](https://github.com/CnCNet/yrpp) | C++ | YR 反编译头文件项目 | 开源 | 类结构与游戏机制文档化参考 |
| [CnCNet YR](https://cncnet.org)（ClientPP） | — | 官方替代联机客户端 | 闭源+开源组件 | M8 大厅/联机体验基线 |

> EA 2025 年 GPL 开源的 C&C 源码仅含 Tiberian Dawn / Red Alert / Renegade / Generals: Zero Hour，**不含 RA2/YR**。

## 2. 格式与规则文档

- [ModEnc](https://modenc.renegadeprojects.com) — rules/art/ai INI 全量键位文档 + 文件格式页（**规则语义第一文档源**）；
- XCC 源码（[xhp.xwis.net](https://xhp.xwis.net/)）— MIX/SHP/VXL/PAL/AUD 解码器的事实标准实现；
- PPMSite（[ppmforums.com](https://ppmforums.com)）— mod 社区，格式问答存档；
- C&C Wiki — RA2/YR 文件列表与格式综述；
- 社区 TS/RA2 地图格式文档 + FinalAlert 2 行为基准（地图编辑器对照）；
- RA2/YR 加密 MIX 的 Blowfish 密钥：社区公开资料 + 可从玩家自有 `gamemd.exe` 派生（本项目采用后者）。

## 3. 工具链

| 工具 | 用途 |
| --- | --- |
| XCC Utilities | MIX 解包/查看的对照基准（输出用于 `assetcheck` 校验） |
| Voxel Section Editor III | 体素模型编辑/预览基准 |
| OS SHP Builder | SHP 编辑/预览基准 |
| FinalAlert 2 | 地图编辑器，触发器/脚本编辑的行为基准 |
| CnCNet YR | 联机与平衡性基线（M8 参照） |
| FFmpeg | BIK（Bink1）视频解码，可选集成 |

## 4. 素材格式速查

| 格式 | 用途 | 关键点 |
| --- | --- | --- |
| MIX | 归档容器 | 明文 + Blowfish 加密变体（ra2/ra2md/cache），含文件头/CRC |
| SHP | 2D 精灵动画 | 8bit 调色板，Westwood LCW（Format80）压缩，帧偏移表；建筑/地形/单位/UI |
| TMP/地形 | 60×30 菱形瓦片 | 每 theater 一套瓦片集，含混合/斜坡 |
| VXL | 体素模型 | 含逐体素法线（光照）、多 section；HVA 伴生动画（炮塔/炮管） |
| PAL | 256 色调色板 | 光照等级由调色板斜坡实现；含 JASC 文本变体 |
| INI | 规则/数据 | rulesmd/artmd/aimd/soundmd/thememd/evamd/battlemd/keyboardmd |
| CSF | 文本字串表 | label + value + extra（音效引用），多语言容器 |
| AUD/WAV | 音效 | IMA ADPCM / PCM |
| BIK | 视频 | Bink1，FFmpeg 可解（libavcodec） |
| .map/.yrm/.yro/.mmx | 地图/场景 | INI 外壳 + IsoMapPack5 压缩地形 + 对象/触发数据；.mmx 为多地图场景（M1 确认） |
| FNT | 位图字体 | game.fnt |
| .sav | 存档 | 低优先（本项目用自有格式） |

## 5. 原版安装文件地图（`Yuri/`）

| 文件 | 内容 |
| --- | --- |
| ra2.mix（加密） | RA2 基础资产（SHP/VXL/INI/PAL/音效） |
| ra2md.mix（加密） | YR 增量 + 1.001 补丁资产 |
| language.mix / langmd.mix（加密） | 本地化资产（语言包） |
| multimd.mix | 多人游戏 UI 资产 |
| thememd.mix | 背景音乐 |
| movmd03.mix | 视频（BIK，394 MB） |
| mapsmd03.mix | 战役地图 |
| ra2.csf / ra2md.csf | 字串表 |
| gamemd.exe | YR 主程序（作为行为基准 + Blowfish 密钥来源） |
| map/ | 玩家地图（含大量自制 .yrm，可用作测试语料） |
