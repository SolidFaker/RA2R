// RA2R 模拟层测试：移动（8 向/恒速/朝向/重下令/idle）、建造（队列/工期/电力/
// 修理出售/精炼采矿）、防御建筑攻击、确定性哈希。全部纯逻辑（不依赖游戏资产）。
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "ra2r/render/isometric.h"
#include "ra2r/sim/sim_world.h"

using namespace ra2r;

namespace {
// 空世界 + 一辆车（speed 68）
sim::SimWorld make_world(int w = 64, int h = 64, int speed = 68, int kind = 1) {
    sim::SimWorld s;
    s.w = w;
    s.h = h;
    s.blocked.assign(static_cast<size_t>(w) * h, 0);
    s.spawn_unit("Player", kind == 2 ? "E1" : "HTNK", kind, w / 2, h / 2, 0, {}, false, 0,
                 speed);
    return s;
}

// 单位屏幕位置（格中心 + 段内插值；与 stage 的线性近似一致）
void unit_screen_pos(const render::IsometricGrid& g, const sim::SimUnit& u, int& px, int& py,
                     bool smooth = false) {
    int tx, ty, nx, ny;
    g.cell_to_pixel(u.col, u.row, tx, ty);
    g.cell_to_pixel(u.next_col, u.next_row, nx, ny);
    int ox = ((nx - tx) * u.frac) / sim::kFracMax;
    int oy = ((ny - ty) * u.frac) / sim::kFracMax;
    if (smooth && (u.next_col != u.col || u.next_row != u.row)) {
        int qx, qy;
        g.cell_to_pixel(u.prev_col, u.prev_row, qx, qy);
        if (u.prev_col == u.col && u.prev_row == u.row) {
            qx = 2 * tx - nx;
            qy = 2 * ty - ny;
        }
        int wx, wy;
        if (!u.path.empty())
            g.cell_to_pixel(u.path.front().first, u.path.front().second, wx, wy);
        else {
            wx = 2 * nx - tx;
            wy = 2 * ny - ty;
        }
        const int m0x = qx + tx, m0y = qy + ty;
        const int m1x = tx + nx, m1y = ty + ny;
        const int m2x = nx + wx, m2y = ny + wy;
        const long long f = u.frac;
        long long sx2, sy2;
        if (f < 128) {
            const long long v = f + 128, a = 256 - v, b = v;
            sx2 = (a * a * m0x + 2 * a * b * (2LL * tx) + b * b * m1x) / 65536;
            sy2 = (a * a * m0y + 2 * a * b * (2LL * ty) + b * b * m1y) / 65536;
        } else {
            const long long v = f - 128, a = 256 - v, b = v;
            sx2 = (a * a * m1x + 2 * a * b * (2LL * nx) + b * b * m2x) / 65536;
            sy2 = (a * a * m1y + 2 * a * b * (2LL * ny) + b * b * m2y) / 65536;
        }
        ox = static_cast<int>((sx2 - 2LL * tx) / 2);
        oy = static_cast<int>((sy2 - 2LL * ty) / 2);
    }
    px = tx + ox + g.tile_w / 2;
    py = ty + oy + g.tile_h / 2;
}
} // namespace

// ── 移动：8 向 / 恒速 / 朝向 ─────────────────────────────────────────────────

TEST(SimMove, EightDirectionsWithCorrectFacing) {
    // 8 个地图方向步 → 屏幕朝向索引（0=右上 1=右 2=右下 3=下 4=左下 5=左 6=左上 7=上）
    struct Case {
        int dc, dr, expect;
    };
    const int r = 32;
    const bool odd = (r & 1) != 0; // par=0 时 n 奇偶 = 行奇偶
    const Case cases[8] = {
        {odd ? 1 : 0, 1, 2},  {odd ? 0 : -1, -1, 6}, {odd ? 0 : -1, 1, 4}, {odd ? 1 : 0, -1, 0},
        {0, 2, 3},            {0, -2, 7},           {1, 0, 1},            {-1, 0, 5},
    };
    for (const auto& c : cases) {
        sim::SimWorld s = make_world();
        const int tc = 32 + c.dc, tr = 32 + c.dr;
        ASSERT_TRUE(s.issue_move(0, tc, tr));
        s.tick();
        // dir 为 0..255 全圆周：8 向扇区中心 = 索引 × 32
        EXPECT_EQ(s.units[0].dir, c.expect * 32) << "目标 (" << tc << "," << tr << ")";
    }
}

TEST(SimMove, StraightRunsAreCollinearAndConstantSpeed) {
    // 四个地图主轴方向：整段轨迹应共线（旧 4 邻接会阶梯摆动），单帧位移近似恒定
    // 屏幕下/右/上/左 + 两条 45°（+X/+Y）：都应走直线
    const int targets[6][2] = {{32, 44}, {44, 32}, {32, 20}, {20, 32}, {38, 44}, {26, 44}};
    for (const auto& tg : targets) {
        sim::SimWorld s = make_world();
        render::IsometricGrid g;
        ASSERT_TRUE(s.issue_move(0, tg[0], tg[1]));
        std::vector<std::pair<int, int>> pts;
        double sum = 0;
        int n = 0;
        int lpx = 0, lpy = 0;
        for (int t = 0; t < 200; ++t) {
            int px, py;
            unit_screen_pos(g, s.units[0], px, py);
            if (t > 0) {
                sum += std::hypot(px - lpx, py - lpy);
                ++n;
            }
            pts.emplace_back(px, py);
            lpx = px;
            lpy = py;
            s.tick();
            if (s.units[0].order == sim::kOrderNone) break;
        }
        ASSERT_GE(pts.size(), 10u);
        const double dx = pts.back().first - pts.front().first;
        const double dy = pts.back().second - pts.front().second;
        const double len = std::hypot(dx, dy);
        ASSERT_GT(len, 0.0);
        double max_dev = 0;
        for (const auto& p : pts)
            max_dev = std::max(max_dev,
                               std::fabs(dx * (p.second - pts.front().second) -
                                         dy * (p.first - pts.front().first)) /
                                   len);
        EXPECT_LT(max_dev, 1.5) << "目标 " << tg[0] << "," << tg[1]; // 直线偏差
        const double avg = sum / std::max(1, n);
        EXPECT_GT(avg, 6.0);  // 单帧 ≈8.7px（speed 68）
        EXPECT_LT(avg, 12.0);
    }
}

TEST(SimMove, ArrivesAndStopsAtTargetCell) {
    sim::SimWorld s = make_world();
    ASSERT_TRUE(s.issue_move(0, 32 + 10, 32));
    for (int t = 0; t < 200 && s.units[0].order != sim::kOrderNone; ++t) s.tick();
    EXPECT_EQ(s.units[0].col, 42);
    EXPECT_EQ(s.units[0].row, 32);
    EXPECT_EQ(s.units[0].frac, 0);
    EXPECT_FALSE(s.unit_moving(0));
}

TEST(SimMove, RepathKeepsProgressAndDoesNotSnapBack) {
    // 移动中反复重下令（右键连点）：不丢段内进度、位置单调、无复位跳变
    sim::SimWorld s = make_world();
    render::IsometricGrid g;
    ASSERT_TRUE(s.issue_move(0, 44, 44));
    s.tick();
    s.tick();
    const int frac_before = s.units[0].frac;
    const int next_c = s.units[0].next_col, next_r = s.units[0].next_row;
    ASSERT_TRUE(s.issue_move(0, 46, 42)); // 改目标
    EXPECT_EQ(s.units[0].frac, frac_before); // 段内进度保留
    EXPECT_EQ(s.units[0].next_col, next_c);  // 当前段不动
    EXPECT_EQ(s.units[0].next_row, next_r);
    // 连续重下令 4 次，位置应连续（单帧位移 < 半个格）
    int lpx = 0, lpy = 0, max_step = 0;
    for (int t = 0; t < 60; ++t) {
        if (t == 5) s.issue_move(0, 45, 45);
        if (t == 10) s.issue_move(0, 46, 44);
        if (t == 15) s.issue_move(0, 45, 43);
        if (t == 20) s.issue_move(0, 47, 45);
        int px, py;
        unit_screen_pos(g, s.units[0], px, py);
        if (t > 0) max_step = std::max(max_step, std::abs(px - lpx) + std::abs(py - lpy));
        lpx = px;
        lpy = py;
        s.tick();
    }
    EXPECT_LT(max_step, 24); // 复位到格心会出现 45~60px 的曼哈顿跳变
}

TEST(SimMove, FractionNeverExceedsOneStep) {
    // 跨格在同帧结算：frac 恒 < 256（否则渲染会越过格心再拉回）
    sim::SimWorld s = make_world();
    ASSERT_TRUE(s.issue_move(0, 32 + 20, 32 + 20));
    for (int t = 0; t < 200; ++t) {
        s.tick();
        if (s.units[0].order == sim::kOrderNone) break;
        EXPECT_LT(s.units[0].frac, sim::kFracMax);
    }
}

TEST(SimMove, UnreachableTargetStopsWithoutMoving) {
    sim::SimWorld s = make_world();
    for (int r = 0; r < 64; ++r) s.blocked[r * 64 + 40] = 1; // 整列墙（含上下边界，彻底封死）
    EXPECT_FALSE(s.issue_move(0, 50, 32));
    for (int t = 0; t < 10; ++t) s.tick();
    EXPECT_EQ(s.units[0].col, 32); // 原地
    EXPECT_EQ(s.units[0].row, 32);
}

// 编队（多选）重下令：移动中的单位保留段内进度 —— 不再"闪现"弹回格心。
// 回归：issue_move_group 曾对每个单位按"从当前格重新起步"布置路径（frac 归零、
// next 改回当前格），段中反复改目的地时单位视觉上反复弹回。
TEST(SimMove, GroupRepathKeepsMidSegmentProgress) {
    sim::SimWorld s = make_world();
    ASSERT_TRUE(s.issue_move(0, 50, 32));
    int t = 0;
    while (t < 30 && s.units[0].frac == 0) { // 推进到段中
        s.tick();
        ++t;
    }
    ASSERT_GT(s.units[0].frac, 0);
    const int col = s.units[0].col, row = s.units[0].row;
    const int nc = s.units[0].next_col, nr = s.units[0].next_row;
    const int frac = s.units[0].frac;
    const int n2c = s.units[0].next2_col, n2r = s.units[0].next2_row;
    // 反向改目的地（编队 API）：段起点/终点/进度都必须保持
    EXPECT_EQ(s.issue_move_group({0}, 20, 32), 1u);
    EXPECT_EQ(s.units[0].col, col);
    EXPECT_EQ(s.units[0].row, row);
    EXPECT_EQ(s.units[0].next_col, nc);
    EXPECT_EQ(s.units[0].next_row, nr);
    EXPECT_EQ(s.units[0].frac, frac);
    // 渲染转角平滑的控制点也必须不变（否则当前段的画面会跳变＝闪现）
    EXPECT_EQ(s.units[0].next2_col, n2c);
    EXPECT_EQ(s.units[0].next2_row, n2r);
    EXPECT_FALSE(s.units[0].path.empty());
}

// 停止指令：清指令/路径并原地驻停（多选"停止"用）
TEST(SimMove, StopUnitClearsOrderAndPath) {
    sim::SimWorld s = make_world();
    ASSERT_TRUE(s.issue_move(0, 40, 32));
    for (int t = 0; t < 5; ++t) s.tick();
    ASSERT_TRUE(s.stop_unit(0));
    EXPECT_EQ(s.units[0].order, sim::kOrderNone);
    EXPECT_TRUE(s.units[0].path.empty());
    EXPECT_EQ(s.units[0].next_col, s.units[0].col);
    const int c = s.units[0].col, r = s.units[0].row;
    for (int t = 0; t < 10; ++t) s.tick();
    EXPECT_EQ(s.units[0].col, c); // 原地
    EXPECT_EQ(s.units[0].row, r);
    EXPECT_FALSE(s.stop_unit(99)); // 越界
}

namespace {
// 三辆车间隔摆放（同一 House）；返回车下标
std::vector<size_t> spawn_three(sim::SimWorld& s, int c0, int r0) {
    s.spawn_unit("Player", "HTNK", 1, c0, r0, 0, {}, false, 0, 68);
    s.spawn_unit("Player", "HTNK", 1, c0 + 2, r0 + 2, 0, {}, false, 0, 68);
    s.spawn_unit("Player", "HTNK", 1, c0, r0 + 4, 0, {}, false, 0, 68);
    return {0, 1, 2};
}
bool any_moving(const sim::SimWorld& s) {
    for (size_t i = 0; i < s.units.size(); ++i)
        if (s.unit_moving(i)) return true;
    return false;
}
} // namespace


// ── 格占用：一格 1 载具 或 ≤3 步兵（子格 0..2 等腰三角）──────────────────────

// 动态障碍：前车占住目标格 → 被挡单位立即绕行到相邻格（不是原地等位死等）
TEST(SimMove, BlockedUnitReplansImmediatelyAroundDynamicObstacle) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    EXPECT_GT(s.spawn_unit("Player", "HTNK", 1, 20, 20, 0, {}, false, 0, 68), 0u); // A（先到）
    EXPECT_GT(s.spawn_unit("Player", "HTNK", 1, 10, 20, 0, {}, false, 0, 68), 0u); // B（后到）
    ASSERT_TRUE(s.issue_move(0, 30, 20));
    ASSERT_TRUE(s.issue_move(1, 30, 20)); // 同一目标格 → A 占住后 B 必须另找落点
    int t = 0;
    while (t < 1200 && any_moving(s)) {
        s.tick();
        ++t;
    }
    ASSERT_FALSE(any_moving(s));
    EXPECT_EQ(s.units[0].col, 30); // A 占住目标
    EXPECT_EQ(s.units[0].row, 20);
    EXPECT_TRUE(s.units[1].col != 30 || s.units[1].row != 20) << "B 不得与 A 同格";
    EXPECT_LE(std::abs(s.units[1].col - 30) + std::abs(s.units[1].row - 20), 3)
        << "B 应绕行到目标附近";
}

// 绕行重规划保留段内进度：被动态障碍挡住而重算路径时，不弹回格心（无闪现）
TEST(SimMove, ReplanKeepsMidSegmentProgressNoSnapBack) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    EXPECT_GT(s.spawn_unit("Player", "HTNK", 1, 30, 20, 0, {}, false, 0, 68), 0u); // A
    EXPECT_GT(s.spawn_unit("Player", "HTNK", 1, 16, 20, 0, {}, false, 0, 68), 0u); // B
    ASSERT_TRUE(s.issue_move(0, 30, 20));
    ASSERT_TRUE(s.issue_move(1, 30, 20)); // A 先占住 → B 半路被挡后重算
    int t = 0;
    bool replanned = false;
    while (t < 1200 && any_moving(s)) {
        const int bc = s.units[1].col, br = s.units[1].row;
        const int bnc = s.units[1].next_col, bnr = s.units[1].next_row;
        const int bpc = s.units[1].prev_col, bpr = s.units[1].prev_row;
        s.tick();
        ++t;
        const auto& b = s.units[1];
        // 重规划的特征：同一格内 next 被换掉（正常跨格时 col 会变）
        if (!replanned && b.col == bc && b.row == br &&
            (b.next_col != bnc || b.next_row != bnr)) {
            replanned = true;
            EXPECT_GT(b.frac, 0) << "重算后不得弹回格心（frac 应为投影保留值）";
            EXPECT_EQ(b.prev_col, bpc) << "来向应保留（转角平滑用）";
            EXPECT_EQ(b.prev_row, bpr);
        }
    }
    ASSERT_FALSE(any_moving(s));
    EXPECT_TRUE(replanned) << "应发生一次绕行重规划";
    EXPECT_TRUE(s.units[1].col != 30 || s.units[1].row != 20) << "B 绕行到别的格";
}

// 重下令不改变渲染位置：段中改目的地后，单位绘制像素位置逐像素不变
//（不闪现）；其后一帧只按步长前进。
TEST(SimMove, ReorderKeepsRenderedPixelPosition) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    EXPECT_GT(s.spawn_unit("Player", "HTNK", 1, 10, 20, 0, {}, false, 0, 68), 0u);
    EXPECT_GT(s.spawn_unit("Player", "HTNK", 1, 10, 24, 0, {}, false, 0, 68), 0u);
    EXPECT_EQ(s.issue_move_group({0, 1}, 40, 22), 2u);
    int t = 0;
    while (t < 20 && s.units[0].frac == 0) { // 推进到段中
        s.tick();
        ++t;
    }
    ASSERT_GT(s.units[0].frac, 0);
    int o0x = 0, o0y = 0;
    sim::unit_render_offset(s.units[0], o0x, o0y);
    EXPECT_EQ(s.issue_move_group({0, 1}, 30, 44), 2u); // 段中重下令
    int o1x = 0, o1y = 0;
    sim::unit_render_offset(s.units[0], o1x, o1y);
    EXPECT_EQ(o1x, o0x) << "重下令后绘制位置不得变化";
    EXPECT_EQ(o1y, o0y);
    s.tick(); // 下一帧：位移应 ≤ 一步（而非弹回格心）
    int o2x = 0, o2y = 0;
    sim::unit_render_offset(s.units[0], o2x, o2y);
    EXPECT_LE(std::abs(o2x - o1x) + std::abs(o2y - o1y), 12);
}

// ── 运动物理（rulesmd Accelerates/AccelerationFactor/DeaccelerationFactor/ROT）──

namespace {
sim::SimWorld make_veh_with_motion(const sim::UnitMotion& mo) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    s.spawn_unit("Player", "HTNK", 1, 10, 20, 0, {}, false, 0, 0, mo);
    return s;
}
} // namespace

// Accelerates=yes：从静止按 AccelerationFactor 加速到 Speed 上限
TEST(SimMotion, AcceleratesRampsToSpeedLimit) {
    sim::UnitMotion mo;
    mo.max_speed = 100;
    mo.accel_step = 10; // 每帧 +10 → 10 帧到上限
    sim::SimWorld s = make_veh_with_motion(mo);
    EXPECT_EQ(s.units[0].vel, 0) << "加速型应从静止起步";
    ASSERT_TRUE(s.issue_move(0, 40, 20));
    s.tick();
    EXPECT_LE(s.units[0].vel, 10);
    for (int i = 0; i < 12; ++i) s.tick();
    EXPECT_EQ(s.units[0].vel, 100); // 达到上限（不超）
}

// DeaccelerationFactor=0：不减速、到达后瞬间停止（vel=0）
TEST(SimMotion, DeaccelFactorZeroStopsInstantly) {
    sim::UnitMotion mo;
    mo.max_speed = 100; // accel=0 → 立即上限；decel=0 → 不减速
    sim::SimWorld s = make_veh_with_motion(mo);
    EXPECT_EQ(s.units[0].vel, 100);
    ASSERT_TRUE(s.issue_move(0, 12, 20)); // 相邻格：很快到达
    int t = 0;
    while (t < 200 && any_moving(s)) {
        s.tick();
        ++t;
    }
    ASSERT_FALSE(any_moving(s));
    EXPECT_EQ(s.units[0].vel, 0) << "到达后应瞬间停止";
}

// 减速系数 > 0：末段（无后续路径）逐渐减速且仍能到达
TEST(SimMotion, DeceleratesOnFinalSegmentButStillArrives) {
    sim::UnitMotion mo;
    mo.max_speed = 120;
    mo.decel_step = 12;
    sim::SimWorld s = make_veh_with_motion(mo);
    ASSERT_TRUE(s.issue_move(0, 12, 20));
    int t = 0;
    int min_vel = 1 << 20;
    while (t < 400 && any_moving(s)) {
        s.tick();
        ++t;
        min_vel = std::min(min_vel, s.units[0].vel);
    }
    ASSERT_FALSE(any_moving(s));
    EXPECT_EQ(s.units[0].col, 12); // 仍然到达目的地
    EXPECT_EQ(s.units[0].row, 20);
    EXPECT_EQ(min_vel, 0); // 到达后归零
}

// ROT + 转向/移动互斥（用户更正）：偏差 >16 时原地转向、不推进；
// 偏差 ≤ 16（半个扇区）后才开始/继续移动；每帧转向不超 rot_step。
TEST(SimMotion, TurnsInPlaceUntilAlignedThenMoves) {
    sim::UnitMotion mo;
    mo.max_speed = 100;
    mo.rot_step = 8;
    sim::SimWorld s = make_veh_with_motion(mo); // 初始 dir=0（屏幕右上），东行目标 dir=32
    ASSERT_TRUE(s.issue_move(0, 40, 20));
    const int c0 = s.units[0].col, r0 = s.units[0].row;
    int prev = s.units[0].dir;
    bool turned = false, moved_while_misaligned = false, moved_after_aligned = false;
    for (int i = 0; i < 200 && s.units[0].col == c0 && s.units[0].row == r0; ++i) {
        s.tick();
        const int now = s.units[0].dir;
        int d = now - prev;
        if (d > 128) d -= 256;
        if (d < -128) d += 256;
        EXPECT_LE(std::abs(d), 8) << "每帧转向不得超 ROT";
        if (now != prev) turned = true;
        int dev = 32 - now; // 行进方向为东（dir=32）
        if (dev > 128) dev -= 256;
        if (dev < -128) dev += 256;
        const bool aligned = std::abs(dev) <= 16;
        const bool moved = s.units[0].frac > 0;
        if (moved && aligned) moved_after_aligned = true;
        if (moved && !aligned) moved_while_misaligned = true;
        prev = now;
    }
    EXPECT_TRUE(turned) << "应发生转向";
    EXPECT_FALSE(moved_while_misaligned) << "偏差 >16 时不得推进（原地转向）";
    EXPECT_TRUE(moved_after_aligned) << "对准后应开始移动";
}

// Accelerates=0（或 AccelerationFactor=0）：立即达到上限速度并正常移动（不是不能移动）
TEST(SimMotion, ZeroAccelCoefficientStillMoves) {
    sim::UnitMotion mo;
    mo.max_speed = 100;
    mo.accel_step = 0; // Accelerates=0
    mo.rot_step = 5;   // 真实车辆有 ROT：不得因此卡住
    sim::SimWorld s = make_veh_with_motion(mo);
    EXPECT_EQ(s.units[0].vel, 100) << "系数 0 = 立即到位";
    ASSERT_TRUE(s.issue_move(0, 14, 20));
    for (int t = 0; t < 600 && any_moving(s); ++t) s.tick();
    EXPECT_FALSE(any_moving(s));
    EXPECT_EQ(s.units[0].col, 14) << "Accelerates=0 的单位必须能移动";
    EXPECT_EQ(s.units[0].row, 20);
}

// TurretROT：炮塔独立按自己的转速追赶行进方向
TEST(SimMotion, TurretTurnsTowardHeadingAtOwnRate) {
    sim::UnitMotion mo;
    mo.max_speed = 100;
    mo.rot_step = 0;    // 车身立即转向
    mo.turret_rot = 4;  // 炮塔每帧 4
    sim::SimWorld s = make_veh_with_motion(mo);
    ASSERT_TRUE(s.issue_move(0, 40, 20));
    s.tick();
    EXPECT_EQ(s.units[0].dir % 32, 0); // 车身已对准
    const int t0 = s.units[0].turret_dir;
    for (int i = 0; i < 100 && s.units[0].turret_dir != s.units[0].dir; ++i) s.tick();
    EXPECT_EQ(s.units[0].turret_dir, s.units[0].dir) << "炮塔应追上行进朝向";
    if (t0 == s.units[0].dir) GTEST_SKIP() << "初始已同向（本测例无意义）";
}

// 生成规则：同格最多 3 步兵（子格各异）；载具与步兵互斥、载具独占整格
TEST(SimUnitCell, ThreeInfantryPerCellAndVehicleExclusive) {
    sim::SimWorld s;
    s.w = 32;
    s.h = 32;
    s.blocked.assign(32 * 32, 0);
    const auto spawn = [&](const char* type, int kind) {
        return s.spawn_unit("Player", type, kind, 10, 10, 0, {}, false, 0, 0);
    };
    EXPECT_GT(spawn("E1", 2), 0u); // 步兵 1 → 子格 0
    EXPECT_GT(spawn("E1", 2), 0u); // 步兵 2 → 子格 1
    EXPECT_GT(spawn("E1", 2), 0u); // 步兵 3 → 子格 2
    EXPECT_EQ(spawn("E1", 2), 0u); // 第 4 个步兵：生成失败
    EXPECT_EQ(spawn("HTNK", 1), 0u); // 载具不能与步兵同格
    ASSERT_EQ(s.units.size(), 3u);
    EXPECT_EQ(s.units[0].subcell, 0);
    EXPECT_EQ(s.units[1].subcell, 1);
    EXPECT_EQ(s.units[2].subcell, 2);
    EXPECT_EQ(s.free_subcell(10, 10, 2), -1); // 步兵格已满
    EXPECT_EQ(s.free_subcell(10, 10, 1), -1); // 载具不可入
    // 载具独占：一格 1 辆，第二辆失败
    EXPECT_GT(s.spawn_unit("Player", "HTNK", 1, 12, 12, 0, {}, false, 0, 0), 0u);
    EXPECT_EQ(s.free_subcell(12, 12, 1), -1);
    EXPECT_EQ(s.units.back().subcell, 0);
    EXPECT_EQ(s.free_subcell(12, 12, 2), -1); // 步兵也不可入载具格
    EXPECT_TRUE(s.free_subcell(13, 13, 2) >= 0); // 空地可入
}

// 移动落位：3 个步兵先后走到同一格 → 各占一个子格；第 4 个在格边等位后
// 自动绕行到邻近空格（不会挤进第 4 个）
TEST(SimUnitCell, ArrivingInfantryTakesFreeSubcellsAndOverflowReroutes) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    for (int i = 0; i < 4; ++i)
        s.spawn_unit("Player", "E1", 2, 10 + i * 2, 10, 0, {}, false, 0, 51);
    for (size_t i = 0; i < 4; ++i) EXPECT_TRUE(s.issue_move(i, 30, 30));
    int t = 0;
    while (t < 3000 && any_moving(s)) {
        s.tick();
        ++t;
    }
    ASSERT_FALSE(any_moving(s));
    // 同格步兵数 ≤3，且 (30,30) 上的 3 人子格互不相同
    int at_target = 0;
    bool slot[3] = {false, false, false};
    for (const auto& u : s.units) {
        if (u.col == 30 && u.row == 30) {
            ++at_target;
            ASSERT_LT(u.subcell, 3);
            EXPECT_FALSE(slot[u.subcell]) << "同格步兵子格不能重复";
            slot[u.subcell] = true;
        }
    }
    EXPECT_EQ(at_target, 3);
    // 第 4 人（绕行）不得与目标格重叠，且离目标不远
    for (const auto& u : s.units) {
        if (!(u.col == 30 && u.row == 30)) {
            EXPECT_LE(std::abs(u.col - 30) + std::abs(u.row - 30), 4);
        }
    }
}

// 编队目标分配：每个单位拿到"尽量接近点击格、且不违反格占用约束"的格子
//（步兵同格 ≤3、载具独占、互斥）；到达后约束仍成立
TEST(SimMove, GroupMoveAssignsDestinationsRespectingOccupancy) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    for (int i = 0; i < 3; ++i) // (30,30) 已有 3 个步兵（满格）
        EXPECT_GT(s.spawn_unit("Player", "E1", 2, 30, 30, 0, {}, false, 0, 51), 0u);
    EXPECT_GT(s.spawn_unit("Player", "E1", 2, 10, 10, 0, {}, false, 0, 51), 0u);
    EXPECT_GT(s.spawn_unit("Player", "HTNK", 1, 12, 12, 0, {}, false, 0, 68), 0u);
    ASSERT_EQ(s.units.size(), 5u);
    EXPECT_EQ(s.issue_move_group({3, 4}, 30, 30), 2u);
    // 目的地：避开被占的 (30,30)；步兵与载具不能同格
    const int full = 30 * 64 + 30;
    EXPECT_NE(s.units[3].dest_col * 64 + s.units[3].dest_row, full);
    EXPECT_NE(s.units[4].dest_col * 64 + s.units[4].dest_row, full);
    EXPECT_TRUE(s.units[3].dest_col != s.units[4].dest_col ||
                s.units[3].dest_row != s.units[4].dest_row);
    int t = 0;
    while (t < 3000 && any_moving(s)) {
        s.tick();
        ++t;
    }
    ASSERT_FALSE(any_moving(s));
    std::map<int, int> inf, veh;
    for (const auto& u : s.units) {
        const int k = u.row * 64 + u.col;
        if (u.kind == 2) {
            ++inf[k];
        } else {
            veh[k] = 1;
        }
    }
    for (const auto& [k, n] : inf) {
        EXPECT_LE(n, 3) << "同格步兵超过 3";
        EXPECT_EQ(veh.count(k), 0u) << "步兵与载具同格";
    }
    for (const auto& [k, n] : veh) {
        (void)n;
        EXPECT_EQ(inf.count(k), 0u) << "载具与步兵同格";
    }
}

// ── 编队移动（多单位共享流场）────────────────────────────────────────────────

// 一条编队指令 → 所有选中单位都接到移动指令并各自落位（就近槽位）
TEST(SimMove, GroupMoveOrdersAllSelectedUnits) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    const auto idx = spawn_three(s, 10, 10);
    const size_t ordered = s.issue_move_group(idx, 40, 40);
    EXPECT_EQ(ordered, 3u);
    for (const size_t i : idx) {
        EXPECT_EQ(s.units[i].order, sim::kOrderMove);
        EXPECT_FALSE(s.units[i].path.empty()); // 都有路径（而不是只动第一辆）
    }
    int t = 0;
    while (t < 3000 && any_moving(s)) {
        s.tick();
        ++t;
    }
    ASSERT_FALSE(any_moving(s)) << "编队未在限时内全部到位";
    for (const size_t i : idx) {
        const auto& u = s.units[i];
        // 到位：落在目标格或其邻格槽位（编队散开）
        EXPECT_LE(std::abs(u.col - 40) + std::abs(u.row - 40), 3);
    }
}

// 绕障：一道带缺口的墙 → 编队路径不得穿墙，且全部到达
TEST(SimMove, GroupMoveDetoursAroundWall) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    for (int r = 0; r < 40; ++r) s.blocked[r * 64 + 30] = 1; // 竖墙（下半留缺口）
    const auto idx = spawn_three(s, 10, 10);
    EXPECT_EQ(s.issue_move_group(idx, 40, 10), 3u);
    for (const size_t i : idx) {
        ASSERT_FALSE(s.units[i].path.empty());
        for (const auto& c : s.units[i].path)
            EXPECT_EQ(s.blocked[static_cast<size_t>(c.second) * 64 + c.first], 0)
                << "路径不得经过阻挡格";
    }
    int t = 0;
    while (t < 6000 && any_moving(s)) {
        s.tick();
        ++t;
    }
    ASSERT_FALSE(any_moving(s));
    for (const size_t i : idx) {
        // 目标 = 点击格 + 周围槽位（编队各自一格）：落在目标附近且已绕过墙
        EXPECT_LE(std::abs(s.units[i].col - 40) + std::abs(s.units[i].row - 10), 2);
        EXPECT_GE(s.units[i].col, 38);
    }
}

// 目标格被建筑占据 → 单位走到旁边可走格停驻（不穿建筑、不再报"不可达"）
TEST(SimMove, GroupMoveApproachesBlockedTargetCell) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    const auto idx = spawn_three(s, 10, 10);
    for (int dr = -1; dr <= 1; ++dr)
        for (int dc = -1; dc <= 1; ++dc) s.blocked[(30 + dr) * 64 + (30 + dc)] = 1;
    EXPECT_EQ(s.issue_move_group(idx, 30, 30), 3u);
    int t = 0;
    while (t < 3000 && any_moving(s)) {
        s.tick();
        ++t;
    }
    for (const size_t i : idx) {
        const auto& u = s.units[i];
        EXPECT_EQ(s.blocked[static_cast<size_t>(u.row) * 64 + u.col], 0) << "不得停在障碍格上";
        EXPECT_LE(std::abs(u.col - 30) + std::abs(u.row - 30), 4) << "应停在目标附近";
    }
}

// ── 步兵 idle 动作（IdleActionFrequency 语义）────────────────────────────────

TEST(SimIdle, TriggersInExpectedWindowAndIsDeterministic) {
    auto run = [](std::vector<std::pair<int, int>>& log) {
        sim::SimWorld s = make_world(32, 32, 51, 2); // 步兵
        s.idle_freq_ticks = 135;
        int last = 0;
        for (int t = 0; t < 900; ++t) {
            s.tick();
            const int k = s.units[0].idle_kind;
            if (k != last) {
                log.emplace_back(t, k);
                last = k;
            }
        }
    };
    std::vector<std::pair<int, int>> a, b;
    run(a);
    run(b);
    EXPECT_EQ(a, b); // 确定性（每单位 LCG 流）
    ASSERT_GE(a.size(), 2u);
    EXPECT_GE(a[0].second, 1);        // 首条日志 = idle 动作开始（0→1/2）
    EXPECT_LE(a[0].second, 2);
    EXPECT_GE(a[0].first, 60);        // 等待 ∈ 0.5~2× freq = [67,337]（±1 帧结算误差）
    EXPECT_LE(a[0].first, 345);
    // 动作类型 ∈ {1,2}，忙期 78 帧后结束
    bool saw_action = false;
    for (size_t i = 0; i + 1 < a.size(); ++i) {
        if (a[i].second != 0 && a[i + 1].second == 0) {
            EXPECT_GE(a[i].second, 1);
            EXPECT_LE(a[i].second, 2);
            EXPECT_GE(a[i + 1].first - a[i].first, 70); // ≈ kIdleAnimBusyTicks
            saw_action = true;
        }
    }
    EXPECT_TRUE(saw_action);
}

TEST(SimIdle, MovingInterruptsIdleAction) {
    sim::SimWorld s = make_world(32, 32, 51, 2);
    s.idle_freq_ticks = 30; // 缩短等待，尽快触发
    bool saw = false;
    for (int t = 0; t < 600 && !saw; ++t) {
        s.tick();
        if (s.units[0].idle_kind != 0) saw = true;
    }
    ASSERT_TRUE(saw);
    ASSERT_TRUE(s.issue_move(0, 26, 26));
    bool cleared = false;
    for (int t = 0; t < 60 && !cleared; ++t) {
        s.tick();
        if (s.unit_moving(0) && s.units[0].idle_kind == 0) cleared = true;
    }
    EXPECT_TRUE(cleared) << "开始移动应打断 idle 动作";
}

// ── 建造：放置 / 工期 / 电力 / 队列 ──────────────────────────────────────────

TEST(SimBuild, SpawnBlocksFootprintAndCompletesOnTime) {
    sim::SimWorld s;
    s.w = 32;
    s.h = 32;
    s.blocked.assign(32 * 32, 0);
    const uint32_t id = s.spawn_building("Player", "GAPOWR", 10, 10, 2, 2, 300, 100, true, 54,
                                         750);
    ASSERT_NE(id, 0u);
    // 地基 2×2（地图空间）→ 引擎格菱形：顶格 (10,10) + 左 (9,11) / 右 (10,11) + 底 (10,12)
    // （引擎网格矩形是沿行错位的平行四边形，不是原版地基形状）
    const std::pair<int, int> want[] = {{10, 10}, {10, 11}, {9, 11}, {10, 12}};
    for (const auto& c : want) EXPECT_EQ(s.blocked[c.second * 32 + c.first], 1);
    EXPECT_EQ(s.blocked[11 * 32 + 10], 1);     // (col 10, row 11)
    EXPECT_EQ(s.blocked[11 * 32 + 11], 0);     // 引擎矩形位置不应阻挡
    EXPECT_EQ(s.blocked[10 * 32 + 9], 0);
    EXPECT_FALSE(s.can_place(10, 12, 1, 1)); // 底格被占
    EXPECT_TRUE(s.can_place(11, 11, 1, 1));  // 原矩形角点其实是空的
    // 奇数行锚点：菱形镜像（顶格 (11,11)，右/下 (12,12)，左 (11,12)，底 (11,13)）
    {
        sim::SimWorld s2;
        s2.w = 32;
        s2.h = 32;
        s2.blocked.assign(32 * 32, 0);
        ASSERT_NE(s2.spawn_building("Player", "GAPOWR", 11, 11, 2, 2, 300, 100, true, 54, 750),
                  0u);
        for (const auto& c : {std::pair<int, int>{11, 11}, {12, 12}, {11, 12}, {11, 13}})
            EXPECT_EQ(s2.blocked[c.second * 32 + c.first], 1);
        EXPECT_EQ(s2.blocked[12 * 32 + 12], 1); // (col 12, row 12)
        EXPECT_EQ(s2.blocked[11 * 32 + 12], 0); // (col 12, row 11) 不是地基
    }
    // 建造中：hp=1 且不计电力
    const auto& b = s.buildings[0];
    EXPECT_TRUE(b.under_construction);
    EXPECT_EQ(b.hp, 1);
    EXPECT_TRUE(s.power_net.empty());
    for (int t = 0; t < 53; ++t) s.tick();
    EXPECT_TRUE(s.buildings[0].under_construction) << "54 帧工期未到";
    s.tick();
    EXPECT_FALSE(s.buildings[0].under_construction) << "第 54 帧应完工";
    EXPECT_EQ(s.buildings[0].hp, 750);
    EXPECT_EQ(s.power_net["Player"], 100);
}

TEST(SimBuild, FoundationShapeIsMapSpaceRectangle) {
    // 现场放置与地图装载必须产生同一地基形状：地图空间矩形 → 引擎格菱形。
    // 用非零 min_d/min_s（含奇偶偏移）覆盖换算的奇偶分支。
    sim::SimWorld s;
    s.w = 40;
    s.h = 40;
    s.min_d = -15; // 与夹具地图一致（rx-ry ∈ [-15,15]）
    s.min_s = 0;
    s.blocked.assign(40 * 40, 0);
    // 锚点 (10,10) 的引擎格 → 地图空间，再按地图空间枚举 → 应与 foundation_cells 一致
    int rx = 0, ry = 0;
    s.cell_to_map(10, 10, rx, ry);
    for (int fw = 1; fw <= 4; ++fw)
        for (int fh = 1; fh <= 3; ++fh) {
            std::vector<std::pair<int, int>> cells;
            s.foundation_cells(10, 10, fw, fh, cells);
            ASSERT_EQ(cells.size(), static_cast<size_t>(fw) * fh);
            // 逐格与地图空间换算对齐
            for (int j = 0; j < fh; ++j)
                for (int i = 0; i < fw; ++i) {
                    int c = 0, r = 0;
                    s.map_to_cell(rx + i, ry + j, c, r);
                    const auto& got = cells[static_cast<size_t>(j) * fw + i];
                    EXPECT_EQ(got.first, c) << "fw=" << fw << " fh=" << fh;
                    EXPECT_EQ(got.second, r) << "fw=" << fw << " fh=" << fh;
                }
            // 顶格 = 锚点（行号最小且在锚行）
            EXPECT_EQ(cells[0].first, 10);
            EXPECT_EQ(cells[0].second, 10);
        }
    // 引擎格 ↔ 地图空间往返
    for (int row = 0; row < 40; ++row)
        for (int col = 0; col < 40; ++col) {
            int mx = 0, my = 0, c = 0, r = 0;
            s.cell_to_map(col, row, mx, my);
            s.map_to_cell(mx, my, c, r);
            ASSERT_EQ(c, col);
            ASSERT_EQ(r, row);
        }
}

TEST(SimBuild, PlacementRejectsUnitsOverlaysAndOre) {
    // 建造只能在空地上：建筑地基（blocked）、覆盖物（no_build）、矿石（ore）、
    // 单位占格都会拒绝；ignore_unit_id 用于基地车展开忽略自身。
    sim::SimWorld s;
    s.w = 16;
    s.h = 16;
    s.blocked.assign(16 * 16, 0);
    s.no_build.assign(16 * 16, 0);
    s.ore.assign(16 * 16, 0);
    const uint32_t uid = s.spawn_unit("Player", "MTNK", 1, 5, 5, 0, {}, false, 0, 68);
    ASSERT_NE(uid, 0u);
    EXPECT_FALSE(s.cell_buildable(5, 5));                    // 单位占格
    EXPECT_FALSE(s.can_place(5, 5, 1, 1));                   // 单格地基同样拒绝
    EXPECT_TRUE(s.can_place(5, 5, 1, 1, uid));               // 展开：忽略自身
    EXPECT_EQ(s.spawn_building("Player", "GAPOWR", 5, 5, 1, 1, 0, 0, false, 1, 100), 0u);
    s.no_build[6 * 16 + 6] = 1;                              // 覆盖物（桥/墙/栅栏）
    EXPECT_FALSE(s.cell_buildable(6, 6));
    s.ore[7 * 16 + 7] = 50;                                  // 矿石：有矿不可建
    EXPECT_FALSE(s.cell_buildable(7, 7));
    s.ore[7 * 16 + 7] = 0;                                   // 采完可建
    EXPECT_TRUE(s.cell_buildable(7, 7));
    s.blocked[8 * 16 + 8] = 1;                               // 建筑地基/地形
    EXPECT_FALSE(s.cell_buildable(8, 8));
    EXPECT_FALSE(s.cell_buildable(-1, 0));                   // 越界
    EXPECT_FALSE(s.cell_buildable(16, 0));
}

TEST(SimBuild, NoBuildupInstantComplete) {
    sim::SimWorld s;
    s.w = 16;
    s.h = 16;
    s.blocked.assign(16 * 16, 0);
    // build_total = 1 = 无动画即放即完成（stage 对无 Buildup 美术传 1）
    const uint32_t id = s.spawn_building("Player", "GACNST", 4, 4, 4, 4, 2000, 0, false, 1, 1000);
    ASSERT_NE(id, 0u);
    s.tick();
    EXPECT_FALSE(s.buildings[0].under_construction);
    EXPECT_EQ(s.buildings[0].hp, 1000);
}

TEST(SimBuild, QueueDeductsRefundsAndBecomesReady) {
    sim::SimWorld s;
    s.w = 16;
    s.h = 16;
    s.blocked.assign(16 * 16, 0);
    s.credits["Player"] = 1000;
    EXPECT_TRUE(s.queue_build("Player", "GAPOWR", 300, 10));
    EXPECT_EQ(s.credits["Player"], 700);
    EXPECT_FALSE(s.queue_build("Player", "GACNST", 2000, 10)); // 单队列占用
    EXPECT_FALSE(s.build_ready("Player"));
    for (int t = 0; t < 10; ++t) s.tick();
    EXPECT_TRUE(s.build_ready("Player"));
    std::string type;
    EXPECT_TRUE(s.take_ready_build("Player", &type));
    EXPECT_EQ(type, "GAPOWR");
    EXPECT_FALSE(s.build_ready("Player"));
    // 取消退款 50%
    s.credits["Player"] = 1000;
    ASSERT_TRUE(s.queue_build("Player", "GAREFN", 400, 100));
    EXPECT_EQ(s.credits["Player"], 600);
    EXPECT_TRUE(s.cancel_build("Player", 50));
    EXPECT_EQ(s.credits["Player"], 800);
}

TEST(SimBuild, IssueBuildNeedsCreditsAndFreeGround) {
    sim::SimWorld s;
    s.w = 16;
    s.h = 16;
    s.blocked.assign(16 * 16, 0);
    s.credits["Player"] = 500;
    EXPECT_TRUE(s.issue_build("Player", "GAPOWR", 4, 4, 2, 2, 300, 54, 100));
    EXPECT_EQ(s.credits["Player"], 200);
    EXPECT_FALSE(s.issue_build("Player", "GAREFN", 8, 8, 4, 3, 2000, 54, -30)); // 资金不足
    EXPECT_FALSE(s.issue_build("Player", "GAPOWR", 5, 5, 2, 2, 300, 54, 100));  // 地基被占
}

// ── 修理 / 出售 ──────────────────────────────────────────────────────────────

TEST(SimBuild, RepairHealsAndChargesThenSellUnblocks) {
    sim::SimWorld s;
    s.w = 16;
    s.h = 16;
    s.blocked.assign(16 * 16, 0);
    s.credits["Player"] = 1000;
    const uint32_t id = s.spawn_building("Player", "GAPOWR", 4, 4, 2, 2, 300, 100, false, 1, 750);
    ASSERT_NE(id, 0u);
    s.buildings[0].hp = 300;
    s.buildings[0].repair_step_hp = 2;
    ASSERT_TRUE(s.toggle_repair(0));
    for (int t = 0; t < 50; ++t) s.tick();
    EXPECT_GT(s.buildings[0].hp, 300);
    EXPECT_LT(s.credits["Player"], 1000); // 修理扣款
    const int64_t refund = s.sell_building(0, 50);
    EXPECT_GT(refund, 0);
    s.tick(); // 死亡整批结算：解除阻挡并移除建筑实体
    EXPECT_TRUE(s.buildings.empty());
    EXPECT_EQ(s.blocked[4 * 16 + 4], 0);
    EXPECT_EQ(s.blocked[5 * 16 + 5], 0);
}

// ── 精炼厂 + 采矿车（经济闭环）──────────────────────────────────────────────

TEST(SimEconomy, MinerUnloadsAtRefineryCredits) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    s.ore.assign(64 * 64, 0); // 无矿区（卸货场景），但格栅尺寸必须正确
    // 精炼厂（configure_building 注入 is_refinery —— 玩家自建的精炼厂同样生效）
    const uint32_t bid = s.spawn_building("Player", "GAREFN", 12, 12, 4, 3, 2000, -30, false, 1,
                                          1000);
    ASSERT_NE(bid, 0u);
    ASSERT_TRUE(s.configure_building(bid, 0, {}, true));
    // 满载矿车
    const uint32_t uid = s.spawn_unit("Player", "CMIN", 1, 30, 30, 0, {}, true, 20, 68);
    ASSERT_NE(uid, 0u);
    ASSERT_EQ(s.units.size(), 1u);
    s.units[0].cargo = 20;
    s.credits["Player"] = 0;
    for (int t = 0; t < 600; ++t) s.tick();
    EXPECT_EQ(s.units[0].cargo, 0) << "应完成卸货";
    EXPECT_EQ(s.credits["Player"], 20) << "卸货入账";
}

TEST(SimEconomy, MinerHarvestsOreIntoCapacity) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    s.ore.assign(64 * 64, 0);
    for (int r = 28; r < 34; ++r)
        for (int c = 28; c < 34; ++c) s.ore[r * 64 + c] = 50; // 矿石格
    const uint32_t uid = s.spawn_unit("Player", "CMIN", 1, 30, 30, 0, {}, true, 20, 68);
    ASSERT_NE(uid, 0u);
    for (int t = 0; t < 300; ++t) s.tick();
    EXPECT_GT(s.units[0].cargo, 0) << "空闲矿车应自动采集";
}

// ── 防御建筑攻击（手动 / 自动索敌 / 炮塔转向 / Neutral 不开火）──────────────

TEST(SimDefense, ManualAttackTurnsTurretAndDamages) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    const uint32_t bid = s.spawn_building("Player", "GAPILL", 32, 32, 1, 1, 500, -50, false, 1,
                                          500);
    ASSERT_NE(bid, 0u);
    ASSERT_TRUE(s.configure_building(bid, 0, {40, 20, 5}));
    const uint32_t uid = s.spawn_unit("Opponent", "E1", 2, 32, 36, 0, {}, false, 0, 51);
    ASSERT_NE(uid, 0u);
    ASSERT_TRUE(s.issue_build_attack(0, 0));
    for (int t = 0; t < 60; ++t) s.tick();
    EXPECT_LT(s.units[0].hp, 256) << "射程内应开火扣血";
    EXPECT_EQ(s.buildings[0].turret_dir, 96) << "炮塔应转向目标（正下方 = 字节 96）";
    // 无武器建筑不可攻击
    const uint32_t bid2 = s.spawn_building("Player", "GAPOWR", 20, 20, 2, 2, 300, 100, false, 1,
                                           750);
    ASSERT_NE(bid2, 0u);
    EXPECT_FALSE(s.issue_build_attack(1, 0));
}

TEST(SimDefense, OutOfRangeAimsWithoutFiring) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    const uint32_t bid = s.spawn_building("Player", "GAPILL", 32, 32, 1, 1, 500, -50, false, 1,
                                          500);
    ASSERT_TRUE(s.configure_building(bid, 0, {40, 20, 5}));
    s.spawn_unit("Opponent", "E1", 2, 32, 42, 0, {}, false, 0, 51);
    ASSERT_TRUE(s.issue_build_attack(0, 0));
    for (int t = 0; t < 80; ++t) s.tick();
    EXPECT_EQ(s.units[0].hp, 256) << "射程外不开火";
    EXPECT_EQ(s.buildings[0].turret_dir, 96) << "但应先转向目标";
}

TEST(SimDefense, AutoAcquireFiresButNeutralDoesNot) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    const uint32_t b1 = s.spawn_building("Player", "GAPILL", 32, 32, 1, 1, 500, -50, false, 1,
                                         500);
    const uint32_t b2 = s.spawn_building("Neutral", "GAPILL", 20, 20, 1, 1, 500, 0, false, 1, 500);
    ASSERT_TRUE(s.configure_building(b1, 0, {40, 20, 5}));
    ASSERT_TRUE(s.configure_building(b2, 0, {40, 20, 5}));
    s.spawn_unit("Opponent", "E1", 2, 32, 36, 0, {}, false, 0, 51);
    for (int t = 0; t < 90; ++t) s.tick();
    EXPECT_LT(s.units[0].hp, 256) << "玩家建筑应自动索敌开火";
    EXPECT_EQ(s.buildings[1].target, -1) << "Neutral 陈设不自动开火";
}

TEST(SimDefense, StopAttackResumesAutoAcquire) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    const uint32_t bid = s.spawn_building("Player", "GAPILL", 32, 32, 1, 1, 500, -50, false, 1,
                                          500);
    ASSERT_TRUE(s.configure_building(bid, 0, {40, 20, 5}));
    s.spawn_unit("Opponent", "E1", 2, 32, 35, 0, {}, false, 0, 51);
    ASSERT_TRUE(s.issue_build_attack(0, 0));
    s.tick();
    EXPECT_GE(s.buildings[0].target, 0);
    EXPECT_TRUE(s.stop_build_attack(0));
    EXPECT_EQ(s.buildings[0].target, -1);
    for (int t = 0; t < 30; ++t) s.tick();
    EXPECT_GE(s.buildings[0].target, 0) << "停止后应恢复自动索敌";
}

// ── 单位战斗与死亡 ──────────────────────────────────────────────────────────

TEST(SimCombat, UnitAttackKillsTargetAndRemovesIt) {
    sim::SimWorld s;
    s.w = 32;
    s.h = 32;
    s.blocked.assign(32 * 32, 0);
    s.spawn_unit("A", "HTNK", 1, 10, 10, 0, {90, 5, 3}, false, 0, 68);
    s.spawn_unit("B", "HTNK", 1, 12, 10, 0, {1, 100, 1}, false, 0, 68);
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    for (int t = 0; t < 300 && s.units.size() > 1; ++t) s.tick();
    ASSERT_EQ(s.units.size(), 1u) << "目标应被击杀并移除";
    EXPECT_FALSE(s.explosions.empty()); // 死亡爆炸
}

// ── 确定性（同操作序列 → 状态与视觉哈希一致）────────────────────────────────

TEST(SimDeterminism, IdenticalSequencesProduceIdenticalState) {
    auto run = []() {
        sim::SimWorld s;
        s.w = 64;
        s.h = 64;
        s.blocked.assign(64 * 64, 0);
        s.idle_freq_ticks = 60;
        s.spawn_unit("Player", "HTNK", 1, 10, 10, 0, {90, 5, 3}, false, 0, 68);
        s.spawn_unit("Opponent", "E1", 2, 30, 30, 0, {}, false, 0, 51);
        s.spawn_building("Player", "GAPOWR", 20, 20, 2, 2, 300, 100, false, 1, 750);
        s.issue_move(0, 22, 22);
        for (int t = 0; t < 400; ++t) {
            if (t == 120) s.issue_attack_unit(0, 1);
            if (t == 200) s.issue_move(0, 40, 12);
            s.tick();
        }
        return s;
    };
    const sim::SimWorld a = run();
    const sim::SimWorld b = run();
    EXPECT_EQ(a.visual_hash(), b.visual_hash());
    ASSERT_EQ(a.units.size(), b.units.size());
    for (size_t i = 0; i < a.units.size(); ++i) {
        EXPECT_EQ(a.units[i].col, b.units[i].col);
        EXPECT_EQ(a.units[i].row, b.units[i].row);
        EXPECT_EQ(a.units[i].frac, b.units[i].frac);
        EXPECT_EQ(a.units[i].dir, b.units[i].dir);
        EXPECT_EQ(a.units[i].hp, b.units[i].hp);
        EXPECT_EQ(a.units[i].idle_kind, b.units[i].idle_kind);
    }
}
