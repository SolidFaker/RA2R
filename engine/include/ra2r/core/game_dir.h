#pragma once
// RA2R — 游戏安装目录自动发现（GUI 工具"双击可运行"约束的入口依赖）
//
// 查找顺序：
//   1. 注册表 InstallPath（HKLM/HKCU，RA2 与 YR 两个产品名、32/64 位视图；仅 Windows）
//   2. 环境变量 RA2R_GAME_DIR（跨平台显式指定）
//   3. exe 目录及其上两级下的常见子目录（Yuri / RA2 / Red Alert 2 …）
//   4. 当前目录与当前目录的 Yuri 子目录
// 判定条件：目录存在，且含 ra2md.mix 或 ra2.mix（**文件名比较大小写不敏感**，
// Linux 上目录里可能是 RA2MD.MIX）。候选目录名同样按大小写不敏感匹配
// （如 `yuri` 目录也能命中 `Yuri` 候选）。
#include <string>

namespace ra2r::core {

// 返回第一个"看起来像游戏目录"的绝对路径；找不到返回空串。
std::string find_game_dir();

} // namespace ra2r::core
