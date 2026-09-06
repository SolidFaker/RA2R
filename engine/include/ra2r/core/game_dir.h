#pragma once
// RA2R — 游戏安装目录自动发现（GUI 工具"双击可运行"约束的入口依赖）
//
// 查找顺序：
//   1. 注册表 InstallPath（HKLM/HKCU，RA2 与 YR 两个产品名、32/64 位视图）
//   2. exe 目录及其上两级下的常见子目录（Yuri / RA2 / Red Alert 2 …）
//   3. 当前目录与当前目录的 Yuri 子目录
// 判定条件：目录存在，且含 ra2md.mix 或 ra2.mix（Windows 大小写不敏感）。
#include <string>

namespace ra2r::core {

// 返回第一个"看起来像游戏目录"的绝对路径；找不到返回空串。
std::string find_game_dir();

} // namespace ra2r::core
