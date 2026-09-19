#pragma once
// RA2R — M3 寻路：引擎格空间 8 邻接 A*（确定性、纯整数代价）
//
// 格空间 = 砖墙渲染格 (col,row)（与 MapFile.cell 一致）。砖墙格与地图格同构
// （(col,row) ↔ 地图 (X,Y)），其 **8 邻接恰为屏幕 8 向 / 地图 8 向单步**：
//   · 地图轴向步（1 格，屏幕 45°，33.5px）：(0,±1) 与按行奇偶二选一的对角步
//   · 地图对角步（√2 格）：(±1,0) 屏幕水平 60px、(0,±2) 屏幕垂直 30px
// 行奇偶 p(r) = (r + par) & 1（par = (min_s+min_d)&1，随地图固定）；p=0 时对角
// 步为 (−1,+1)/(+1,−1)，p=1 时为 (+1,+1)/(−1,−1)。参考 OpenRA 8 向 A*
// （PathSearch：对角步 + octile 距离启发）与原版 TS/RA2 8 向寻路；
// 4 邻接无法表达一条笔直的 45° 线（旧实现走出阶梯状"走格子"感）。
#include <cstdint>
#include <utility>
#include <vector>

namespace ra2r::sim {

// 8 邻接 A*：返回途经格序列（不含起点，含终点；起点=终点时返回 {终点}）。
// blocked 为格栅（1 = 不可通过），w/h 为格栅尺寸；起终点越界/被阻挡或
// 不可达时返回空。par = (min_s + min_d) & 1（地图行奇偶补偿；无地图时 0）。
// 代价：轴向步 10、对角步 14（octile 启发，可采纳）；确定性：整数代价 +
// 固定邻序 + 堆内按 (f,g,插入序) 决胜。
std::vector<std::pair<int, int>> find_path(const std::vector<uint8_t>& blocked, int w, int h,
                                           int sc, int sr, int tc, int tr, int par = 0);

} // namespace ra2r::sim
