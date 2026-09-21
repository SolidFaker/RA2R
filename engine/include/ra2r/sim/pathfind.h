#pragma once
// RA2R — 寻路：引擎格空间 **多源流场**（flow field）+ 单目标便捷封装
//
// 格空间 = 砖墙渲染格 (col,row)（与 MapFile.cell 一致）。砖墙格与地图格同构
// （(col,row) ↔ 地图 (X,Y)），其 **8 邻接恰为屏幕 8 向 / 地图 8 向单步**：
//   · 地图轴向步（1 格，屏幕 45°，33.5px）：(0,±1) 与按行奇偶二选一的对角步
//   · 地图对角步（√2 格）：(±1,0) 屏幕水平 60px、(0,±2) 屏幕垂直 30px
// 行奇偶 p(r) = (r + par) & 1（par = (min_s+min_d)&1，随地图固定）；p=0 时对角
// 步为 (−1,+1)/(+1,−1)，p=1 时为 (+1,+1)/(−1,−1)。参考 OpenRA 8 向寻路
// （PathSearch：对角步 + octile 距离启发）与原版 TS/RA2 8 向寻路。
//
// 流场：从**目标集合**（一个或一组目标格）出发做一次 Dijkstra，得到每格到最近
// 目标的代价 `cost` 与"下一步"指针 `next`；随后每个单位沿 `next` 拎出自己的
// 路径。多单位同目标只需构建一次场（N 个单位共享 O(w·h log w·h) 计算，而不是
// 每个单位一次 A*），且天然支持"目标格被建筑挡住 → 走到最近的可达邻格"。
// 代价：轴向步 10、对角步 14（与原 A* 一致）；确定性：整数代价 + 固定邻序 +
// 堆内按 (cost, 插入序) 决胜 + 方向选择平手取邻序靠前者。
#include <cstdint>
#include <utility>
#include <vector>

namespace ra2r::sim {

// 邻居步（砖墙格 8 邻接；row 的奇偶决定对角步集合）
struct NavStep {
    int dc = 0, dr = 0, cost = 0;
};
// 写入 row 行的邻居步（返回个数，恒为 8）
int nav_neighbours(int row, int par, NavStep out[8]);

// 流场（目标集合可为多格；目标格自身允许被阻挡，由调用方决定集合）
struct FlowField {
    int w = 0, h = 0;
    std::vector<int32_t> cost; // 到最近目标的代价（INT32_MAX = 不可达）
    std::vector<int32_t> next; // 下一步格序号（row*w+col；-1 = 目标格/无路）
    std::vector<uint8_t> goal; // 目标集合标记
    bool valid = false;
};

// 构建多源流场：sources 为目标格集合（越界项忽略；须至少一个在图内）。
// 返回 false = 无有效目标（out 保持为空场）。blocked（1 = 不可通过）尺寸不足
// 的格视为可通行。
bool build_flow_field(const std::vector<uint8_t>& blocked, int w, int h, int par,
                      const std::vector<std::pair<int, int>>& sources, FlowField& out);

// 沿流场从 (sc,sr) 取路径（不含起点，到任一目标格即含该格结束）。已达目标返回
// 空；不可达/超步数上限（防环）返回空。起点允许被阻挡（先迈出到最佳邻格）。
std::vector<std::pair<int, int>> flow_path(const FlowField& f, int sc, int sr,
                                           int max_steps = 8192);

// 单目标封装（保持旧 API；即 build_flow_field(单目标) + flow_path）。
// 起点=终点时返回 {终点}；目标被阻挡/越界/不可达返回空。
std::vector<std::pair<int, int>> find_path(const std::vector<uint8_t>& blocked, int w, int h,
                                           int sc, int sr, int tc, int tr, int par = 0);

} // namespace ra2r::sim
