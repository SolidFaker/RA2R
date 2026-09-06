# RA2R 代码组织与编码规范（M2 起强制执行）

> 背景：本项目目标规模为超大型（12–18 个月、M0–M8）。早期原型阶段（M0/M1）
> 为验证格式把大量逻辑堆在单个工具文件里（mixbrowser/main.cpp 一度 ~1750 行）。
> 自 M2 起执行本规范：**文档与注释完善、文件与函数拆分**，避免技术债在后期爆炸。

---

## 1. 文件组织

### 1.1 模块目录

```
engine/include/ra2r/<module>/   ← 公共头（一个概念一个头）
engine/src/<module>/            ← 实现（与头一一对应）
tools/<tool>/main.cpp           ← 工具入口（只做参数解析/UI 粘合，不含算法）
docs/formats/<format>.md        ← 格式规格（洁净室）
docs/design/<topic>.md          ← 架构/设计稿
docs/DEBUGGING.md               ← 调试经验（问题→根因→手法）
data/                           ← 随仓库分发的数据文件（附 README 说明来源）
third_party/reference/          ← 开源参考（gitignore，只阅不抄）
```

### 1.2 硬性规则

| 规则 | 阈值 |
|---|---|
| 单文件行数上限 | **~500 行**（超过必须拆分；算法细节优先下沉到独立翻译单元） |
| 单函数行数上限 | ~80 行；超出拆成匿名命名空间/静态辅助函数 |
| 一文件一职责 | 一个头文件只声明一个类/一组强相关接口 |
| 头文件自包含 | 每个头可独立 `#include` 编译；只 include 自己需要的 |
| 工具不写算法 | tools/*/main.cpp 只做 UI/IO 粘合；算法一律进 engine |

### 1.3 拆分手法（已验证）

- **匿名命名空间辅助函数**：把大函数切成 `load_x / parse_y / emit_z` 之类的小函数。
- **类化状态组**：一组 lambda 共享大量捕获时（如光栅器的 put_px/fill_tri），
  升级为带成员的小类（构造传依赖），方法各自成函数——见
  `engine/src/render/voxel_raster.cpp` 的 `VoxelRasterizer`（M2 首个重构样板）。
- **"先写接口再拆实现"**：接口（.h）只暴露最少入口；内部细节全部 static/anonymous。

---

## 2. 注释规范

| 位置 | 要求 |
|---|---|
| 文件头 | 模块职责一句话 + **来源参考**（哪个格式文档/开源实现）+ 管线/结构概览 |
| 公共接口 | 每个公开函数/结构：做什么、参数语义、返回/失败约定 |
| 非显然算法 | 一行"为什么"（引用 docs 页码/小节），而不是复述"是什么" |
| 魔数 | 必须命名常量并注明出处（实测/规格/参考实现） |
| 临时代码 | 标注 `// TODO(Mx)` 或 `// ── 临时：...`，绝不静默残留 |

反例（禁止）：`x = 0x2001C; // 偏移`——应写 `constexpr size_t kImageDataOffset = 0x2001C; // 0x1C 表 + 64K×2 表项`。

---

## 3. M2 渲染模块规划（按本规范）

```
engine/include/ra2r/render/
  raster.h          RasterImage（公共帧类型）                       ✅ 已建
  voxel_raster.h    体素光栅（z-buffer）                            ✅ 已建
  palette_lut.h     调色板 6bit×4→8bit LUT + 光照等级              （下一步）
  isometric.h       等距投影基向量/格↔屏坐标变换（camera 并入）      （下一步）
  terrain.h         地形瓦片渲染（TMP 图集 + 混合过渡）              （下一步）
  sprite_frame.h    SHP 帧元数据/绘制                               （下一步）
engine/src/render/  一一对应实现
tools/
  mapviewer/        地图查看器（M2 主工具，组装上述模块）            （下一步）
  cachebuild/       缓存构建（docs/design/cache-layer.md 实现）      （下一步）
```

**每模块交付顺序**：接口头（含注释）→ 实现（≤500 行，否则再拆）→ smoketest/工具接入 → 回归。

---

## 4. 重构纪律

- 重构与行为变更**分开提交**：先无行为差异的搬移（本规范样板 = 体素光栅器提入引擎），
  再在其上做功能迭代。
- 每次重构后跑既有回归（mixbrowser 自测模式 `--test-combo/--test-nested/--shot`）。
- 拆分时顺手补文档注释；发现旧注释过期就修。

---

## 5. GUI 工具约定（M2 起强制执行）

> 背景：`-mwindows` 下无控制台，stderr 用户不可见；双击启动时 CWD 不可靠；
> Windows 显示缩放 100%/125%/150%/200% 混用。三条约束统一落到
> `engine/include/ra2r/ui/ui.h`（`ra2r_ui` 库）与 `engine/include/ra2r/core/game_dir.h`。

| 约束 | 约定 |
|---|---|
| **双击可运行** | 无参数启动不得直接退出：`ra2r::core::find_game_dir()` 自动发现游戏目录（注册表 InstallPath → exe 目录向上几级的 Yuri/RA2 子目录 → 当前目录，判定含 ra2md.mix/ra2.mix）；仍失败弹出 GUI 目录选择窗口（`SDL_ShowOpenFolderDialog` + 文本输入 + 重试），错误信息显示在窗口内 |
| **中文界面** | ImGui 建上下文后调用 `ra2r::ui::setup_cjk_font(18.0f×s)`（黑体/雅黑/宋体/等线候选，含拉丁字形，直接作唯一默认字体）；界面字符串一律 UTF-8 中文；有控制台时经 `ra2r::ui::console_utf8()` 切 65001 |
| **统一 DPI 缩放** | 顺序固定：`ra2r::ui::enable_dpi_awareness()`（Per-Monitor V2，窗口创建前）→ 窗口物理尺寸 = 逻辑尺寸 × `ra2r::ui::scale_window_to_dpi()` 返回值 s → 字体按 `18.0f×s` 加载 → `ImGui::GetStyle().ScaleAllSizes(s)` → 布局中固定尺寸（面板宽、列表尺寸、对话框尺寸等）一律乘 s |
| **可写数据默认路径** | 缓存等可写数据放 exe 目录（`SDL_GetBasePath()` + 子目录），不依赖 CWD |

- 接入现状：`tools/stage`（完整：双击/中文/DPI/目录选择）、`tools/mapview`（纯 SDL 无 ImGui：DPI 缩放 + 游戏目录自动发现 + 双击自动打开第一张地图）。
- 新增 GUI 工具按本表接入，勿另起炉灶；验证高 DPI 布局可用 `--dpi <f>` 强制系数（stage 已支持）。
