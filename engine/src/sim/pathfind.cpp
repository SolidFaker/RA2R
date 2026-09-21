// RA2R — 寻路实现：多源流场（接口与语义见 pathfind.h）
#include "ra2r/sim/pathfind.h"

#include <cstdint>
#include <cstdlib>
#include <queue>

namespace ra2r::sim {

int nav_neighbours(int row, int par, NavStep out[8]) {
    const bool odd = ((row + par) & 1) != 0;
    int k = 0;
    out[k++] = {1, 0, 14};  // 屏幕右（地图对角步 √2 格）
    out[k++] = {-1, 0, 14}; // 屏幕左
    out[k++] = {0, 2, 14};  // 屏幕下
    out[k++] = {0, -2, 14}; // 屏幕上
    if (odd) { // n 奇（n = X−Y−min_d = 2col+1）：+X=(1,1) −Y=(1,−1) +Y=(0,1) −X=(0,−1)
        out[k++] = {1, 1, 10};
        out[k++] = {1, -1, 10};
        out[k++] = {0, 1, 10};
        out[k++] = {0, -1, 10};
    } else { // n 偶：+X=(0,1) −Y=(0,−1) +Y=(−1,1) −X=(−1,−1)
        out[k++] = {0, 1, 10};
        out[k++] = {0, -1, 10};
        out[k++] = {-1, 1, 10};
        out[k++] = {-1, -1, 10};
    }
    return k;
}

namespace {
constexpr int32_t kUnreachable = INT32_MAX;

// 小根堆节点：代价 + 插入序（确定性决胜）
struct HeapNode {
    int32_t cost = 0;
    uint32_t seq = 0;
    int32_t idx = 0;
};
struct Cmp {
    bool operator()(const HeapNode& a, const HeapNode& b) const {
        if (a.cost != b.cost) return a.cost > b.cost;
        return a.seq > b.seq;
    }
};
} // namespace

bool build_flow_field(const std::vector<uint8_t>& blocked, int w, int h, int par,
                      const std::vector<std::pair<int, int>>& sources, FlowField& out) {
    out = FlowField{};
    if (w <= 0 || h <= 0) return false;
    const size_t n = static_cast<size_t>(w) * h;
    out.w = w;
    out.h = h;
    out.cost.assign(n, kUnreachable);
    out.next.assign(n, -1);
    out.goal.assign(n, 0);
    const auto in_bounds = [&](int c, int r) { return c >= 0 && r >= 0 && c < w && r < h; };
    const auto index = [&](int c, int r) { return static_cast<size_t>(r) * w + c; };
    const auto blocked_at = [&](size_t i) { return i < blocked.size() && blocked[i] != 0; };

    std::priority_queue<HeapNode, std::vector<HeapNode>, Cmp> open;
    uint32_t seq = 0;
    int source_count = 0;
    for (const auto& s : sources) {
        if (!in_bounds(s.first, s.second)) continue;
        const size_t i = index(s.first, s.second);
        if (blocked_at(i)) continue; // 被阻挡的目标不入场（调用方应传可走槽位）
        if (out.goal[i]) continue;
        out.goal[i] = 1;
        out.cost[i] = 0;
        ++source_count;
        open.push({0, seq++, static_cast<int32_t>(i)});
    }
    if (source_count == 0) return false;

    while (!open.empty()) {
        const HeapNode cur = open.top();
        open.pop();
        const int cc = cur.idx % w;
        const int cr = cur.idx / w;
        if (cur.cost != out.cost[static_cast<size_t>(cur.idx)]) continue; // 过期堆项
        NavStep steps[8];
        const int k = nav_neighbours(cr, par, steps);
        for (int j = 0; j < k; ++j) {
            const int nc = cc + steps[j].dc;
            const int nr = cr + steps[j].dr;
            if (!in_bounds(nc, nr)) continue;
            const size_t ni = index(nc, nr);
            if (blocked_at(ni)) continue; // 障碍格不参与（保持不可达，走逃逸分支）
            const int32_t ng = cur.cost + steps[j].cost;
            if (ng >= out.cost[ni]) continue;
            out.cost[ni] = ng;
            open.push({ng, seq++, static_cast<int32_t>(ni)});
        }
    }

    // 方向：可达格沿代价严格下降的邻格走（Dijkstra 松弛关系，平手取邻序靠前
    // 者）；不可达格（含被阻挡格）取"逃逸方向"——邻格中 (代价 + 步代价) 最小者，
    // 卡在障碍里的单位可先迈出来再沿场前进。
    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c) {
            const size_t i = index(c, r);
            if (out.goal[i]) {
                out.next[i] = -1; // 目标格：终点
                continue;
            }
            NavStep steps[8];
            const int k = nav_neighbours(r, par, steps);
            const int32_t ci = out.cost[i];
            int32_t best_idx = -1;
            if (ci == kUnreachable) {
                int32_t best = kUnreachable;
                for (int j = 0; j < k; ++j) {
                    const int nc = c + steps[j].dc;
                    const int nr = r + steps[j].dr;
                    if (!in_bounds(nc, nr)) continue;
                    const size_t ni = index(nc, nr);
                    if (blocked_at(ni)) continue;
                    if (out.cost[ni] == kUnreachable) continue;
                    const int32_t cand = out.cost[ni] + steps[j].cost;
                    if (cand < best) {
                        best = cand;
                        best_idx = static_cast<int32_t>(ni);
                    }
                }
            } else {
                for (int j = 0; j < k; ++j) {
                    const int nc = c + steps[j].dc;
                    const int nr = r + steps[j].dr;
                    if (!in_bounds(nc, nr)) continue;
                    const size_t ni = index(nc, nr);
                    if (blocked_at(ni)) continue;
                    if (out.cost[ni] != kUnreachable && out.cost[ni] + steps[j].cost == ci) {
                        best_idx = static_cast<int32_t>(ni);
                        break;
                    }
                }
            }
            out.next[i] = best_idx;
        }
    }
    out.valid = true;
    return true;
}

std::vector<std::pair<int, int>> flow_path(const FlowField& f, int sc, int sr, int max_steps) {
    std::vector<std::pair<int, int>> out;
    if (!f.valid || sc < 0 || sr < 0 || sc >= f.w || sr >= f.h) return out;
    size_t i = static_cast<size_t>(sr) * f.w + sc;
    if (i < f.goal.size() && f.goal[i]) return out; // 已在目标
    int steps = 0;
    while (i < f.next.size() && steps < max_steps) {
        const int32_t ni = f.next[i];
        if (ni < 0) return {}; // 无路（或分量的边界）
        out.emplace_back(ni % f.w, ni / f.w);
        i = static_cast<size_t>(ni);
        ++steps;
        if (i < f.goal.size() && f.goal[i]) return out; // 到达任一目标格
    }
    return {}; // 未到目标（超步数/断链）→ 视为不可达
}

std::vector<std::pair<int, int>> find_path(const std::vector<uint8_t>& blocked, int w, int h,
                                           int sc, int sr, int tc, int tr, int par) {
    std::vector<std::pair<int, int>> out;
    if (w <= 0 || h <= 0) return out;
    if (sc < 0 || sr < 0 || sc >= w || sr >= h) return out;
    if (tc < 0 || tr < 0 || tc >= w || tr >= h) return out;
    const size_t si = static_cast<size_t>(sr) * w + sc;
    const size_t ti = static_cast<size_t>(tr) * w + tc;
    if (si == ti) {
        out.emplace_back(tc, tr);
        return out;
    }
    if (ti < blocked.size() && blocked[ti]) return out; // 目标被阻挡（严格语义）
    FlowField field;
    if (!build_flow_field(blocked, w, h, par, {{tc, tr}}, field)) return out;
    return flow_path(field, sc, sr);
}

} // namespace ra2r::sim
