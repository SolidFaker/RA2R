# RA2R — 《尤里的复仇》现代引擎复刻

**RA2R** 是用现代 C++ 与工程实践从零重新实现的 *Command & Conquer: Red Alert 2 — Yuri's Revenge* 游戏引擎。
引擎代码全部自研（洁净室逆向）；游戏素材不随仓库分发，运行需玩家自备正版游戏拷贝。

> English: A from-scratch, clean-room reimplementation of the Red Alert 2 / Yuri's Revenge engine.
> Asset pipelines decode the original game files; no copyrighted assets are distributed.

## 当前进度

- **M0 资产管线**：MIX（含 Blowfish 加密解密）、SHP、VXL、PAL、PCX、TMP 地形瓦片、AUD/WAV、CSF、HVA、LCW/LZO 压缩、地图（`.map/.yrm/.yro/.mmx`）、剧场瓦片集、规则库（rulesmd/artmd）——全部格式有实测驱动规格文档。
- **M1/M2 渲染与地图**：等距地形渲染（雷达色小地图、高度/悬崖/斜坡）、对象放置层（建筑帧语义/受损切换/建造动画、载具体素、步兵 SHP）、迷雾与探明、地图场景合成；`tools/mapview`、`tools/stage` 可视化。
- **M3 模拟**：15Hz 确定性模拟世界（建筑/单位/步兵、矿车采集循环、建筑建造、路径寻路 A*、阵营经济、基础开火与爆炸）、1v1 演示、多标签步进调试。
- **工具链**：`mixdump`（MIX 解包/递归查找/扫描）、`shpview`、`vxlview`、`mapview`、`stage`、`assetcheck`、`cachebuild`、`smoketest`。

### mixbrowser — MIX 资源浏览器

打开一个 `.mix` 或整个游戏目录（自动批量加载为标签页），支持：

- **条目浏览**：排序、名称查找（内置规则 INI 推导名库 + 可选 XCC 社区名库）、嵌套 MIX 下钻、右键导出到文件 / 复制 ID；
- **预览**：SHP（帧动画/调色板自动识别/缩放）、VXL（可交互旋转 3D 光栅 + HVA 动画）、PAL 色板、PCX 位图（24 位与 8 位索引色）、TMP 地形瓦片（帧/缩放/地形色板）、地图（雷达色小地图）、文本/十六进制、CSF 字串；
- **音视频**：AUD/WAV 播放（内置解码器）、BIK 视频（FFmpeg 运行时绑定，可选）；
- **体验**：耗时操作（调色盘池搜索/地图扫描/地图缩略图）后台线程执行，底部状态栏实时进度；HiDPI 缩放、中文路径支持。

## 构建

工具链：**Windows + MinGW-w64 GCC 13+**（开发用 GCC 16.2）、CMake 3.25+。

```powershell
# 1. 准备依赖
#    SDL3（3.4.x，MinGW 版）：https://github.com/libsdl-org/SDL/releases ，
#    解压后在配置时通过 CMAKE_PREFIX_PATH 指定；
#    ImGui 已 vendored（third_party/imgui），无需额外安装。

# 2. 配置与构建
cmake -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=<SDL3 目录>
cmake --build build -j

# 3.（可选）社区名称库与调研参考（XCC 名库为 GPLv3 数据，不入库）
powershell -File tools/fetch_references.ps1

# 4.（可选）BIK 视频播放需要 FFmpeg 8.x 运行库
#    将 avutil-60/avcodec-62/avformat-62/swscale-9/swresample-6.dll
#    放到 mixbrowser.exe 同目录（头文件已 vendored，运行时动态加载）。
```

运行各工具：

```powershell
build\tools\mixbrowser.exe <mix 或游戏目录>     # 资源浏览器
build\tools\mixdump.exe <mix> list              # MIX 清单/提取/查找
build\tools\mapview.exe <地图文件>              # 地图查看
build\tools\stage.exe --test                    # 模拟/渲染测试台
```

## 格式规格文档

洁净室逆向、本机实测驱动，见 [docs/formats/](docs/formats/)：

mix（含 Blowfish/RSA 密钥派生）、shp、vxl、map、pcx、tmp、aud、csf、fnt、hva、palettes、
[skirmish](docs/formats/skirmish.md)（遭遇战流程：出生点/基地车展开/科技树/阵营色）等。

## 架构与规划

| 文档 | 内容 |
| --- | --- |
| [docs/PLAN.md](docs/PLAN.md) | 项目规划、架构决策、里程碑 M0–M9、风险 |
| [docs/REFERENCE.md](docs/REFERENCE.md) | 社区项目、格式文档与工具索引 |
| [docs/design/](docs/design/) | 代码组织约定、缓存层设计 |
| [docs/DEBUGGING.md](docs/DEBUGGING.md) | 调试方法、踩坑记录（含 SHP 阴影/帧段、HVA、NewTheater 剧场代号等实测勘误） |

## 许可与法律

- 本仓库代码以 **MIT** 许可发布（见 [LICENSE](LICENSE)）。
- 仓库不包含、不分发任何 EA 版权素材（SHP/VXL/音频/文本/地图等）——运行引擎需玩家自持正版
  《尤里的复仇》并指定其安装目录；游戏安装目录、解包产物均不入库。
- XCC 社区名称库等第三方参考数据（GPLv3）经 `tools/fetch_references.ps1` 获取，仅存本地，不入库。
