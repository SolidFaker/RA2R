#pragma once
// RA2R — 游戏速度（对齐原版规则）
//
// 原版有两套相反的表示，务必区分：
//   • 游戏内设置滑条（玩家实际看到的档位）：**0 = 最慢，6 = 最快**。
//     实测换算系数（GameFAQs "YR Time Study" / Cheatbook，建造时间→真实秒）：
//       0:1.50(最慢) 1:1.25 2:1.00 3:0.75(普通) 4:0.50(快) 5:0.25 6:0.125(最快)
//   • rulesmd.ini [MultiplayerDialogSettings] GameSpeed 标志值：注释原文
//       "; GameSpeed = starting game speed. For some wacky reason, 0=fastest, 6=slowest. (def=0)"
//     即与游戏内滑条**相反**（ModEnc 亦注明 "inversed from the actual selection"）。
// 本模块采用**游戏内滑条约定（0 最慢、6 最快）**，因为那是玩家视角。
//
// 原版把速度当作"时间倍率"：档位越高，单位/建造/超武计时等一切按真实时间推进越快
// （世界/渲染各自独立）。本项目逻辑恒为逐帧确定性，故档位只映射为**喂帧间隔**。
// 基准逻辑帧为 15Hz（见 sim_world.h）：为不改既有手感，把 15Hz 定为普通档（索引 3）。
namespace ra2r::sim {

inline constexpr int kGameSpeedMin = 0;     // 最慢
inline constexpr int kGameSpeedMax = 6;     // 最快
inline constexpr int kGameSpeedDefault = 3; // 默认档（= 本项目 15Hz 基准）

// 档位 → 目标逻辑帧率（fps）
int game_speed_fps(int index);

// 档位 → 每逻辑帧的墙钟间隔（ms，= 1000 / fps）。驱动方按此间隔喂逻辑帧，
// 逻辑本身仍是逐帧确定性模拟（改速度只改真实时间推进速度，不改结果）。
int game_speed_interval_ms(int index);

// 档位中文名（越界回退到边界档）
const char* game_speed_name(int index);

// 夹取到合法档位 [kGameSpeedMin, kGameSpeedMax]
int clamp_game_speed(int index);

} // namespace ra2r::sim
