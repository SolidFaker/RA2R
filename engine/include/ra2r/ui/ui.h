#pragma once
// RA2R — GUI 工具统一引导（项目 GUI 约束的实现底座）
//
// 约束：1. 双击可运行（配合 core/game_dir 自动发现游戏目录）
//       2. 中文界面（setup_cjk_font 加载系统 CJK 字体，界面字符串用 UTF-8 中文）
//       3. 统一 DPI 缩放（enable_dpi_awareness + scale_window_to_dpi + 布局乘子）
//
// 用法约定（见 docs/design/code-organization.md §GUI 工具约定）：
//   ra2r::ui::enable_dpi_awareness();           // SDL 窗口创建前
//   ra2r::ui::console_utf8();                   // 可选：控制台输出 UTF-8
//   float s = ra2r::ui::scale_window_to_dpi(window, 1440, 900); // 窗口物理尺寸=逻辑×s
//   ra2r::ui::setup_cjk_font(18.0f * s);        // ImGui 建上下文后
//   ImGui::GetStyle().ScaleAllSizes(s);         // 控件间距/内边距随 DPI
//   布局中的固定尺寸（面板宽、列表尺寸等）一律乘以 s。
struct SDL_Window;

namespace ra2r::ui {

// 进程级 DPI 感知（Per-Monitor V2，旧系统回退 System Aware）。
// 必须在 SDL_CreateWindow 之前调用，否则 Windows 会对窗口做位图拉伸。
void enable_dpi_awareness();

// Windows 控制台切换到 UTF-8（代码页 65001）；无控制台（双击运行）时无害。
void console_utf8();

// 加载系统中文字体（Windows 黑体/雅黑/宋体/等线；macOS 苹方/冬青黑体/黑体/宋体；
// Linux Noto CJK/文泉驿/思源，含拉丁字形）为 ImGui 默认字体。
// pixel_size 建议 18×DPI 缩放。成功返回 true；无可用中文字体时退回
// ImGui 内置字体（中文显示为占位符）并返回 false。
bool setup_cjk_font(float pixel_size);

// 把窗口从逻辑尺寸缩放到物理尺寸（逻辑 × 显示器缩放），返回缩放系数 s。
// 之后 ImGui DisplaySize 为物理像素，布局固定尺寸应乘 s。
float scale_window_to_dpi(SDL_Window* window, int logical_w, int logical_h);

} // namespace ra2r::ui
