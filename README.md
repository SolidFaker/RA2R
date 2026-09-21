# RA2R — 《尤里的复仇》现代引擎复刻

**RA2R** 是用现代 C++ 从零重新实现的 *Command & Conquer: Red Alert 2 — Yuri's Revenge*
引擎（洁净室逆向，代码全部自研）。游戏素材不随仓库分发，运行需玩家自备正版游戏拷贝。

> English: A from-scratch, clean-room reimplementation of the Red Alert 2 / Yuri's Revenge engine.

## 当前进度

- **资产管线**：MIX（Blowfish 解密）、SHP/VXL/HVA/PAL/PCX/TMP、AUD/WAV、CSF、LCW/LZO、
  地图（`.map/.yrm/.yro/.mmx`）、剧场瓦片集、rulesmd/artmd 规则库。
- **渲染与地图**：等距地形（高度/悬崖/斜坡/雷达色小地图）、对象层（建筑帧语义、建造动画、
  载具体素、步兵序列）、迷雾、地图场景合成。
- **模拟**：15Hz 确定性世界——建造/电力/经济/采矿、防御建筑、多单位编队（流场寻路 +
  格占用约束）、1v1 遭遇战（基地车展开、科技树、阵营色）。
- **工具**：`mixbrowser`（MIX 浏览/预览/导出）、`mixdump`、`shpview`、`vxlview`、`mapview`、
  `stage`（模拟/渲染测试台）、`assetcheck`、`cachebuild`、`smoketest`。

## 构建

Windows（MinGW-w64 GCC 13+）：SDL3 3.4.x 解压后由 `CMAKE_PREFIX_PATH` 指定
（ImGui/GoogleTest 已 vendored，无需额外安装）。

```powershell
cmake -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=<SDL3 目录>
cmake --build build -j
```

Linux（GCC 13+，SDL3 用发行版包）：

```bash
sudo pacman -S cmake ninja sdl3        # Debian/Ubuntu：libsdl3-dev
cmake -B build -G Ninja -DRA2R_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

- 游戏目录：设 `RA2R_GAME_DIR=<正版安装目录>` 即可（不设则按 exe 相对路径/注册表探测；
  目录与文件名大小写不敏感）。
- 可选：社区名称库 `tools/fetch_references.ps1`；BIK 视频播放需 FFmpeg 8.x DLL 放到 exe 同目录。
- 运行：`build\tools\mixbrowser.exe <mix 或游戏目录>`、`build\tools\stage.exe --test`。

## 测试

```powershell
cmake -B build -DRA2R_BUILD_TESTS=ON
cmake --build build --target ra2r_tests -j
ctest --test-dir build --output-on-failure
```

GoogleTest（已 vendored，离线可构建）；缺少游戏素材时资产/渲染用例自动 SKIP。
CI 覆盖 Linux/Windows 构建与测试、ASan+UBSan、cppcheck/clang-tidy（见 docs/DEBUGGING.md §8）。

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/formats/](docs/formats/) | 格式规格：mix/shp/vxl/map/pcx/tmp/aud/csf/fnt/hva/palettes 等 |
| [docs/PLAN.md](docs/PLAN.md) | 项目规划、架构决策、里程碑 M0–M9 |
| [docs/REFERENCE.md](docs/REFERENCE.md) | 社区项目、参考资料与工具索引 |
| [docs/DEBUGGING.md](docs/DEBUGGING.md) | 调试方法、实测勘误与踩坑记录 |

提交信息规范见 `.gitmessage`（`git config commit.template .gitmessage` 启用）。

## 许可与法律

- 本仓库代码以 **MIT** 发布（见 [LICENSE](LICENSE)）。
- 不包含、不分发任何 EA 版权素材（SHP/VXL/音频/文本/地图等）；运行需自持正版
  《尤里的复仇》并指定其安装目录。
- 第三方参考数据（如 XCC 名称库，GPLv3）经 `tools/fetch_references.ps1` 获取，仅存本地，不入库。
