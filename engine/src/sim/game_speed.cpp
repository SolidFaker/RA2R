// RA2R — 游戏速度实现（接口见 game_speed.h）
#include "ra2r/sim/game_speed.h"

#include <algorithm>

namespace ra2r::sim {

namespace {
// 档位 → 目标逻辑帧率（索引 0 最慢、6 最快，原版游戏内滑条语义）。
// 索引 3 = 本项目 15Hz 基准（普通档，默认）；向快端到 60、慢端到 8。
// 原版相对时间倍率（见 game_speed.h：建造换算系数）更极端（最快/最慢 ≈ 12×），
// 这里按 15Hz 基准收敛为可实用的 7 档（最快/最慢 ≈ 7.5×）。
constexpr int kFps[7] = {8, 10, 12, 15, 20, 30, 60};
constexpr const char* kNames[7] = {"最慢", "更慢", "慢", "普通（基准）", "快", "更快", "最快"};
} // namespace

int clamp_game_speed(int index) {
    return std::clamp(index, kGameSpeedMin, kGameSpeedMax);
}

int game_speed_fps(int index) { return kFps[clamp_game_speed(index)]; }

int game_speed_interval_ms(int index) {
    // 整数除法：默认档 1000/15 = 66，与既有 15Hz 驱动间隔一致
    return 1000 / game_speed_fps(index);
}

const char* game_speed_name(int index) { return kNames[clamp_game_speed(index)]; }

} // namespace ra2r::sim
