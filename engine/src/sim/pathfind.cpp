// RA2R — M3 寻路实现（接口见 pathfind.h）
#include "ra2r/sim/pathfind.h"

#include <cstdint>
#include <cstdlib>
#include <queue>

namespace ra2r::sim {

namespace {
// 开放堆节点：f 总代价 / g 已走代价 / seq 插入序（确定性决胜）
struct HeapNode {
    int32_t f = 0;
    int32_t g = 0;
    uint32_t seq = 0;
    int32_t idx = 0; // 格序号 = row*w + col
};
// 小根堆比较（priority_queue 默认大根 → 反转语义）
struct Cmp {
    bool operator()(const HeapNode& a, const HeapNode& b) const {
        if (a.f != b.f) return a.f > b.f;
        if (a.g != b.g) return a.g < b.g; // f 相等取 g 更大（更接近目标）优先
        return a.seq > b.seq;             // 同 f/g 按插入序（确定性）
    }
};
} // namespace

std::vector<std::pair<int, int>> find_path(const std::vector<uint8_t>& blocked, int w, int h,
                                           int sc, int sr, int tc, int tr, int par) {
    std::vector<std::pair<int, int>> out;
    if (w <= 0 || h <= 0) return out;
    const size_t n = static_cast<size_t>(w) * h;
    const auto at = [&](int c, int r) { return static_cast<size_t>(r) * w + c; };
    const auto in_bounds = [&](int c, int r) { return c >= 0 && r >= 0 && c < w && r < h; };
    if (!in_bounds(sc, sr) || !in_bounds(tc, tr)) return out;
    const int si = static_cast<int>(at(sc, sr));
    const int ti = static_cast<int>(at(tc, tr));
    if (si == ti) {
        out.emplace_back(tc, tr);
        return out;
    }
    const auto blocked_at = [&](int c, int r) -> bool {
        const size_t i = at(c, r);
        return i < blocked.size() && blocked[i] != 0;
    };
    // 允许起点被阻挡（水面单位/紧邻地基出生格），仅目标阻挡即放弃
    if (blocked_at(tc, tr)) return out;

    const int32_t kAxial = 10;     // 地图轴向步（屏幕 45°，1 格）
    const int32_t kDiagonal = 14;  // 地图对角步（屏幕水平/垂直，√2 格）
    std::vector<int32_t> g(n, INT32_MAX);
    std::vector<int32_t> came(n, -1);
    std::vector<uint8_t> closed(n, 0);
    std::priority_queue<HeapNode, std::vector<HeapNode>, Cmp> open;
    uint32_t seq = 0;
    // 可采纳启发：每步最多推进 |Δcol| ≤ 1、|Δrow| ≤ 2（轴向步 10 为最小代价）
    const auto hval = [&](int c, int r) {
        const int dc = std::abs(c - tc);
        const int dr = (std::abs(r - tr) + 1) / 2;
        return static_cast<int32_t>(dc > dr ? dc : dr) * kAxial;
    };
    // 8 邻接步表：屏幕水平/垂直（地图对角步）+ 按行奇偶的 4 个 45° 步
    struct Step {
        int dc, dr, cost;
    };
    const auto neighbours = [&](int cr, int out_dc[8], int out_dr[8], int out_cost[8]) {
        const bool odd = ((cr + par) & 1) != 0;
        int k = 0;
        out_dc[k] = 1; out_dr[k] = 0; out_cost[k++] = kDiagonal;   // 屏幕右
        out_dc[k] = -1; out_dr[k] = 0; out_cost[k++] = kDiagonal;  // 屏幕左
        out_dc[k] = 0; out_dr[k] = 2; out_cost[k++] = kDiagonal;   // 屏幕下
        out_dc[k] = 0; out_dr[k] = -2; out_cost[k++] = kDiagonal;  // 屏幕上
        if (odd) { // n 奇（n = X−Y−min_d = 2col+1）：+X=(1,1) −Y=(1,−1) +Y=(0,1) −X=(0,−1)
            out_dc[k] = 1; out_dr[k] = 1; out_cost[k++] = kAxial;
            out_dc[k] = 1; out_dr[k] = -1; out_cost[k++] = kAxial;
            out_dc[k] = 0; out_dr[k] = 1; out_cost[k++] = kAxial;
            out_dc[k] = 0; out_dr[k] = -1; out_cost[k++] = kAxial;
        } else { // n 偶：+X=(0,1) −Y=(0,−1) +Y=(−1,1) −X=(−1,−1)
            out_dc[k] = 0; out_dr[k] = 1; out_cost[k++] = kAxial;
            out_dc[k] = 0; out_dr[k] = -1; out_cost[k++] = kAxial;
            out_dc[k] = -1; out_dr[k] = 1; out_cost[k++] = kAxial;
            out_dc[k] = -1; out_dr[k] = -1; out_cost[k++] = kAxial;
        }
    };
    g[si] = 0;
    open.push({hval(sc, sr), 0, seq++, si});
    bool found = false;
    while (!open.empty()) {
        const HeapNode cur = open.top();
        open.pop();
        if (closed[cur.idx]) continue;
        closed[cur.idx] = 1;
        if (cur.idx == ti) {
            found = true;
            break;
        }
        const int cc = cur.idx % w;
        const int cr = cur.idx / w;
        int dc[8], dr[8], cost[8];
        neighbours(cr, dc, dr, cost);
        for (int k = 0; k < 8; ++k) {
            const int nc = cc + dc[k];
            const int nr = cr + dr[k];
            if (!in_bounds(nc, nr)) continue;
            const size_t ni = at(nc, nr);
            if (closed[ni] || (ni < blocked.size() && blocked[ni])) continue;
            const int32_t ng = cur.g + cost[k];
            if (ng >= g[ni]) continue;
            g[ni] = ng;
            came[ni] = cur.idx;
            open.push({ng + hval(nc, nr), ng, seq++, static_cast<int32_t>(ni)});
        }
    }
    if (!found) return out;
    std::vector<int> rev;
    for (int i = ti; i != si; i = came[i]) {
        if (i < 0) return out; // 防御：came 链断裂
        rev.push_back(i);
    }
    rev.push_back(si);
    out.reserve(rev.size());
    for (auto it = rev.rbegin(); it != rev.rend(); ++it)
        out.emplace_back(*it % w, *it / w);
    if (!out.empty()) out.erase(out.begin()); // 不含起点
    return out;
}

} // namespace ra2r::sim
