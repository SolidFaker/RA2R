#pragma once
// RA2R — M3 寻路：引擎格空间 4 邻接 A*（确定性、纯整数代价）
//
// 格空间 = 砖墙渲染格 (col,row)（与 MapFile.cell 一致）；4 邻接步 =
// (col±1,row)、(col,row±1)。砖墙排布中这 4 个邻居恰为共享斜边的相邻
// 菱形（奇偶行 30px 错位已含在格坐标里）；地图空间的对角步 = 连续两个
// 行步（路径经中间格），代价自然为 2，与 RA2 格网寻路语义一致。
#include <cstdint>
#include <utility>
#include <vector>

namespace ra2r::sim {

// 4 邻接 A*：返回途经格序列（不含起点，含终点；起点=终点时返回 {终点}）。
// blocked 为格栅（1 = 不可通过），w/h 为格栅尺寸；起终点越界/被阻挡或
// 不可达时返回空。确定性：整数代价 + 固定邻序 + 堆内按 (f,g,插入序) 决胜。
std::vector<std::pair<int, int>> find_path(const std::vector<uint8_t>& blocked, int w, int h,
                                           int sc, int sr, int tc, int tr);

} // namespace ra2r::sim
