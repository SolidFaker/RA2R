// RA2R 模拟层测试：移动（8 向/恒速/朝向/重下令/idle）、建造（队列/工期/电力/
// 修理出售/精炼采矿）、防御建筑攻击、确定性哈希。全部纯逻辑（不依赖游戏资产）。
#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "ra2r/assets/rules_db.h"
#include "ra2r/render/isometric.h"
#include "ra2r/sim/game_speed.h"
#include "ra2r/sim/sim_world.h"
#include "test_util.h"

using namespace ra2r;

namespace {
// 测试用武器（M5.1 后 SimWeapon 字段变多：显式赋值，避免聚合初始化告警）
sim::SimWeapon test_weapon(int damage, int rof, int range) {
    sim::SimWeapon w;
    w.damage = damage;
    w.rof = rof;
    w.range = range;
    return w;
}
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

// ── 格预订（原版"预订-检测"：行进中占据当前格 + 预订的下一格）──────

namespace {
// 每帧渲染屏幕偏移变化量的最大值（px）：正常行进恒定 ~8.9px/帧，
// 超过说明发生了位置跳变（闪现）。
int max_render_step(sim::SimWorld& s) {
    std::vector<std::pair<int, int>> prev(s.units.size(), {INT_MIN, INT_MIN});
    int worst = 0;
    for (int t = 0; t < 1200; ++t) {
        s.tick();
        for (size_t i = 0; i < s.units.size(); ++i) {
            int ox = 0, oy = 0;
            sim::unit_render_offset(s.units[i], ox, oy);
            // 绝对屏幕坐标 = 格心 + 偏移（只跟这个才能检出跨格跳变）
            const int px = s.units[i].col * 60 + (s.units[i].row & 1) * 30 + ox;
            const int py = s.units[i].row * 15 + oy;
            if (prev[i].first != INT_MIN)
                worst = std::max(worst, std::max(std::abs(px - prev[i].first),
                                                  std::abs(py - prev[i].second)));
            prev[i] = {px, py};
        }
        bool moving = false;
        for (size_t i = 0; i < s.units.size(); ++i)
            if (s.unit_moving(i)) moving = true;
        if (!moving) break;
    }
    return worst;
}
} // namespace

// 行进中单位同时占据"当前格 + 已预订的下一格"：预订格对他人视为占用，
// 离开后立即释放（不会出现原版那种"空气墙"）
TEST(SimReserve, MovingUnitReservesItsNextCell) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    ASSERT_GT(s.spawn_unit("Player", "HTNK", 1, 10, 20, 0, {}, false, 0, 0), 0u);
    ASSERT_TRUE(s.issue_move(0, 40, 20));
    for (int i = 0; i < 200 && s.units[0].frac == 0; ++i) s.tick();
    const int nc = s.units[0].next_col, nr = s.units[0].next_row;
    ASSERT_TRUE(nc != s.units[0].col || nr != s.units[0].row) << "应处于行进中";
    EXPECT_LT(s.free_subcell(nc, nr, 1), 0) << "预订格必须视为已占用";
    EXPECT_EQ(s.spawn_unit("Player", "HTNK", 1, nc, nr, 0, {}, false, 0, 0), 0u)
        << "不得在他人预订格上生成单位";
    for (int i = 0; i < 900 && any_moving(s); ++i) s.tick();
    EXPECT_EQ(s.free_subcell(nc, nr, 1), 0) << "离开后应释放预订";
}

// 预订冲突（互指对方格）：不死锁、不重叠、不闪现（用户强调的闪现场景）
TEST(SimReserve, HeadOnOrdersResolveWithoutFlash) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    ASSERT_GT(s.spawn_unit("Player", "HTNK", 1, 20, 20, 0, {}, false, 0, 68), 0u);
    ASSERT_GT(s.spawn_unit("Player", "HTNK", 1, 24, 20, 0, {}, false, 0, 68), 0u);
    ASSERT_TRUE(s.issue_move_group({0, 1}, 22, 20)); // 两车抢同一点 → 各自落槽位
    const int worst = max_render_step(s);
    EXPECT_LE(worst, 15) << "每帧位移不得跳变（闪现）";
    for (const auto& u : s.units) {
        EXPECT_LE(std::abs(u.col - 22) + std::abs(u.row - 20), 3) << "应停在目标附近";
    }
    // 不重叠：载具各占一格
    EXPECT_FALSE(s.units[0].col == s.units[1].col && s.units[0].row == s.units[1].row);
}

// 列队跟走：同一走廊两车一前一后 → 后车预订"前车当前格"接力，
// 不逐帧重规划（画面连续）且均能到达
TEST(SimReserve, ColumnFollowsWithoutFlash) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    ASSERT_GT(s.spawn_unit("Player", "HTNK", 1, 20, 20, 0, {}, false, 0, 68), 0u);
    ASSERT_GT(s.spawn_unit("Player", "HTNK", 1, 23, 20, 0, {}, false, 0, 68), 0u);
    ASSERT_TRUE(s.issue_move_group({0, 1}, 33, 20));
    const int worst = max_render_step(s);
    EXPECT_LE(worst, 15) << "每帧位移不得跳变（闪现）";
    for (const auto& u : s.units) {
        EXPECT_LE(std::abs(u.col - 33) + std::abs(u.row - 20), 3) << "应到达目标附近";
    }
}

// 预订随单位消失而释放（派生自单位表，无残留状态）
TEST(SimReserve, ReservationReleasedWhenUnitRemoved) {
    sim::SimWorld s;
    s.w = 32;
    s.h = 32;
    s.blocked.assign(32 * 32, 0);
    ASSERT_GT(s.spawn_unit("Player", "HTNK", 1, 10, 10, 0, {}, false, 0, 0), 0u);
    ASSERT_TRUE(s.issue_move(0, 30, 10));
    for (int i = 0; i < 200 && s.units[0].frac == 0; ++i) s.tick();
    const int nc = s.units[0].next_col, nr = s.units[0].next_row;
    ASSERT_LT(s.free_subcell(nc, nr, 1), 0);
    ASSERT_TRUE(s.remove_unit(0));
    EXPECT_EQ(s.free_subcell(nc, nr, 1), 0) << "单位消失后预订必须释放";
}

// 让路（OpenRA Nudge）：绕不过去的死路里，空闲友军会横挪一格放行；
// 敌人不让路（不惊动停机单位；"目的地格之争"见
// SimMove.BlockedUnitReplansImmediatelyAroundDynamicObstacle）。
TEST(SimReserve, IdleFriendlyMakesWayWhenNoDetour) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    // 把第 20 行封成 1 宽走廊（相邻四行整图宽全堵，两端也无出口），
    // 只在 (15,22) 留一个侧口袋：被挡者无法绕行，只能请趴窝友军让位；
    // 友军唯一可去的就是那个口袋。
    for (int c = 0; c < 64; ++c)
        for (int r : {18, 19, 21, 22}) s.blocked[r * 64 + c] = 1;
    s.blocked[22 * 64 + 15] = 0; // 口袋（(15,20) 的正南邻格）
    ASSERT_GT(s.spawn_unit("Player", "HTNK", 1, 15, 20, 0, {}, false, 0, 68), 0u); // 趴窝友军
    ASSERT_GT(s.spawn_unit("Player", "HTNK", 1, 10, 20, 0, {}, false, 0, 68), 0u); // 被挡者
    ASSERT_TRUE(s.issue_move(1, 25, 20));
    int t = 0;
    while (t < 1200 && any_moving(s)) {
        s.tick();
        ++t;
    }
    ASSERT_FALSE(any_moving(s));
    EXPECT_EQ(s.units[1].col, 25) << "绕不过去的死路里，友军应让路后通过";
    EXPECT_EQ(s.units[1].row, 20);
    EXPECT_EQ(s.units[0].col, 15) << "让路者应挪进侧边口袋";
    EXPECT_EQ(s.units[0].row, 22);
}

// 敌人不让路：敌方单位趴窝时，被挡单位只能等/绕，绝不与其重叠
TEST(SimReserve, EnemyBlockerNeverYields) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    for (int c = 0; c < 64; ++c)
        for (int r : {18, 19, 21, 22}) s.blocked[r * 64 + c] = 1;
    s.blocked[22 * 64 + 15] = 0;
    ASSERT_GT(s.spawn_unit("Enemy", "HTNK", 1, 15, 20, 0, {}, false, 0, 68), 0u);
    ASSERT_GT(s.spawn_unit("Player", "HTNK", 1, 10, 20, 0, {}, false, 0, 68), 0u);
    ASSERT_TRUE(s.issue_move(1, 25, 20));
    for (int t = 0; t < 400; ++t) s.tick();
    EXPECT_EQ(s.units[0].col, 15) << "敌方单位不得让路";
    EXPECT_EQ(s.units[0].row, 20);
    EXPECT_TRUE(s.units[1].col != 15 || s.units[1].row != 20) << "不得与敌人重叠";
}

// 拥堵吞吐回归（OpenRA 式多单位协调的验收）：8 辆车挤过 1 格宽缺口。
// 未做"同格之争先预订者优先 + 让路"前，这种场景会退化成每 30 帧一跳（实测
// 同规模编队耗时 4 倍以上）；现在要求限时内全员精确落位。
TEST(SimReserve, ChokepointCrossingDoesNotStall) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    for (int r = 0; r < 64; ++r) s.blocked[r * 64 + 30] = 1; // 整列墙
    s.blocked[20 * 64 + 30] = 0;                             // 唯一缺口
    std::vector<size_t> idx;
    const int start[8][2] = {{18, 16}, {18, 20}, {18, 24}, {20, 18},
                             {20, 22}, {22, 18}, {22, 22}, {20, 26}};
    for (const auto& p : start) {
        if (s.spawn_unit("Player", "HTNK", 1, p[0], p[1], 0, {}, false, 0, 68))
            idx.push_back(s.units.size() - 1);
    }
    ASSERT_EQ(idx.size(), 8u);
    ASSERT_EQ(s.issue_move_group(idx, 36, 20), 8u);
    int t = 0;
    while (t < 500 && any_moving(s)) {
        s.tick();
        ++t;
    }
    EXPECT_FALSE(any_moving(s)) << "拥堵不得退化成长时间等待（限 500 帧）";
    for (const size_t i : idx) {
        const auto& u = s.units[i];
        EXPECT_LE(std::abs(u.col - u.dest_col) + std::abs(u.row - u.dest_row), 1)
            << "编队成员应各自精确落位";
    }
}

// ── M5.1 战斗内核：护甲/Verses 伤害 + 主副武器选择 ───────────────────────────

// 护甲下标顺序（原版 11 类；自证：rulesmd [AP] 注释"让 plate 几乎免疫"）
TEST(SimCombat, ArmorIndexOrderMatchesOriginal) {
    using ra2r::assets::armor_index;
    EXPECT_EQ(armor_index("none"), sim::kArmorNone);
    EXPECT_EQ(armor_index("flak"), sim::kArmorFlak);
    EXPECT_EQ(armor_index("plate"), sim::kArmorPlate); // 第 3 项（AP 注释自证）
    EXPECT_EQ(armor_index("light"), sim::kArmorLight);
    EXPECT_EQ(armor_index("heavy"), sim::kArmorHeavy);
    EXPECT_EQ(armor_index("concrete"), sim::kArmorConcrete);
    EXPECT_EQ(armor_index("STEEL"), sim::kArmorSteel);   // 大小写不敏感
    EXPECT_EQ(armor_index("bogus"), sim::kArmorNone);    // 未知回退 none
}

// Verses 伤害：Damage × Verses[armor]%（四舍五入）；0% = 免疫
TEST(SimCombat, DamageUsesWarheadVerses) {
    sim::SimWorld s;
    sim::SimWeapon w;
    w.damage = 100;
    w.warhead.verses[sim::kArmorNone] = 100;
    w.warhead.verses[sim::kArmorFlak] = 50;
    w.warhead.verses[sim::kArmorPlate] = 0;
    w.warhead.verses[sim::kArmorHeavy] = 25;
    EXPECT_EQ(s.damage_against(w, sim::kArmorNone), 100);
    EXPECT_EQ(s.damage_against(w, sim::kArmorFlak), 50);
    EXPECT_EQ(s.damage_against(w, sim::kArmorPlate), 0);
    EXPECT_EQ(s.damage_against(w, sim::kArmorHeavy), 25);
    w.damage = 15; // 15×50% = 7.5 → 8（四舍五入）
    EXPECT_EQ(s.damage_against(w, sim::kArmorFlak), 8);
}

// 主武器对目标护甲 0% → 改用副武器；两者都无效 → 不开火（返回 nullptr）
TEST(SimCombat, SecondaryWeaponSelectedWhenPrimaryIneffective) {
    sim::SimWorld s;
    sim::SimUnit u;
    u.weapon.damage = 50;
    u.weapon.range = 4;
    u.weapon.warhead.verses[sim::kArmorHeavy] = 0; // 主武器打不动重甲
    u.weapon2.damage = 30;
    u.weapon2.range = 5;
    u.weapon2.warhead.verses[sim::kArmorHeavy] = 100; // 副武器专打重甲
    u.has_secondary = true;
    const sim::SimWeapon* w = s.effective_weapon(u, sim::kArmorHeavy);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->damage, 30) << "主武器无效时应改用副武器";
    EXPECT_EQ(s.effective_weapon(u, sim::kArmorNone), &u.weapon) << "主武器有效时用主武器";
    // 都无效 → 不开火
    u.weapon.warhead.verses[sim::kArmorNone] = 0;
    u.weapon2.warhead.verses[sim::kArmorNone] = 0;
    EXPECT_EQ(s.effective_weapon(u, sim::kArmorNone), nullptr);
}

// 原版数据抽查（需游戏目录）：rulesmd [AP] 实测值（注释"让 plate 几乎免疫"自证顺序）
TEST(SimCombat, OriginalRulesVersesSpotCheck) {
    RA2R_REQUIRE_ASSETS();
    const auto* db = test::rules_db();
    ASSERT_NE(db, nullptr);
    const auto* ap = db->warhead("AP");
    ASSERT_NE(ap, nullptr);
    EXPECT_EQ(ap->verses[sim::kArmorPlate], 15) << "第 3 项 = plate（rulesmd 注释自证）";
    EXPECT_EQ(ap->verses[sim::kArmorHeavy], 100);
    EXPECT_EQ(ap->verses[sim::kArmorSteel], 45);
    EXPECT_EQ(ap->cell_spread_x100, 30); // CellSpread=.3
    EXPECT_EQ(ap->percent_at_max, 50);   // PercentAtMax=.5
    EXPECT_EQ(ap->prone_damage, 50);     // ProneDamage=50%
    EXPECT_EQ(ap->inf_death, 3);
    // E1：Armor=none + Secondary=Para + 主武器带弹头
    const auto* e1 = db->unit("E1");
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(ra2r::assets::armor_index(e1->armor), sim::kArmorNone);
    EXPECT_FALSE(e1->secondary.empty());
    const auto* m60 = db->weapon(e1->primary);
    ASSERT_NE(m60, nullptr);
    EXPECT_FALSE(m60->warhead.empty());
}

// ── M5.2 全实体抛射体：飞行时间/撞墙/追踪 ───────────────────────────────────

// 有弹道：首帧不结算伤害，飞行数帧后命中；无弹道（speed=0）保持瞬时命中
TEST(SimProjectile, BallisticFlightThenHit) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "HTNK", 1, 20, 16, 0, {}, false, 0, 68), 0u);
    sim::SimWeapon w = test_weapon(100, 20, 8);
    w.proj.speed = 32; // 32 frac/帧 ≈ 0.125 格/帧
    s.units[0].weapon = w;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    const int hp0 = s.units[1].hp;
    s.tick(); // 首帧开火 → 生成抛射体（不结算伤害）
    EXPECT_EQ(s.units[1].hp, hp0) << "有弹道时不应瞬时命中";
    EXPECT_FALSE(s.projectiles.empty()) << "应生成抛射体";
    int t = 0;
    for (; t < 200 && s.units[1].hp == hp0; ++t) s.tick();
    EXPECT_LT(s.units[1].hp, hp0) << "飞行后应命中";
}

// 无弹道（speed=0）＝瞬时命中（旧行为；既有测试依赖）
TEST(SimProjectile, InstantWeaponHasNoProjectile) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "HTNK", 1, 20, 16, 0, {}, false, 0, 68), 0u);
    s.units[0].weapon = test_weapon(100, 20, 8);
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    const int hp0 = s.units[1].hp;
    s.tick();
    EXPECT_LT(s.units[1].hp, hp0) << "无弹道应瞬时命中";
    EXPECT_TRUE(s.projectiles.empty());
}

// SubjectToWalls=yes：路径上有阻挡格 → 撞墙引爆，目标不受伤
TEST(SimProjectile, WallBlocksProjectile) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "HTNK", 1, 24, 16, 0, {}, false, 0, 68), 0u);
    s.blocked[16 * 32 + 20] = 1; // 路径中段一堵墙
    sim::SimWeapon w = test_weapon(100, 20, 12);
    w.proj.speed = 32;
    w.proj.subject_walls = true;
    s.units[0].weapon = w;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    const int hp0 = s.units[1].hp;
    for (int t = 0; t < 200; ++t) s.tick();
    EXPECT_EQ(s.units[1].hp, hp0) << "被墙挡住不应命中";
}

// ROT>0 追踪弹：目标边打边跑也能命中
TEST(SimProjectile, HomingTracksMovingTarget) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "HTNK", 1, 22, 16, 0, {}, false, 0, 68), 0u);
    sim::SimWeapon w = test_weapon(100, 20, 14);
    w.proj.speed = 24;
    w.proj.rot = 32;
    s.units[0].weapon = w;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    ASSERT_TRUE(s.issue_move(1, 22, 26)); // 目标同时移动
    const int hp0 = s.units[1].hp;
    int t = 0;
    for (; t < 400 && s.units[1].hp == hp0; ++t) s.tick();
    EXPECT_LT(s.units[1].hp, hp0) << "追踪弹应命中移动目标";
}

// ── M5.3 范围伤害（CellSpread/PercentAtMax）与连发（Burst）────────────────────

// 范围伤害：命中点附近多单位同时受伤、按距离线性衰减、发射者豁免
TEST(SimArea, SpreadDamagesNearbyAndFallsOffWithDistance) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "HTNK", 1, 18, 16, 0, {}, false, 0, 68), 0u); // 目标
    ASSERT_GT(s.spawn_unit("Enemy", "HTNK", 1, 19, 16, 0, {}, false, 0, 68), 0u); // 邻格
    sim::SimWeapon w = test_weapon(100, 20, 8);
    w.proj.speed = 64;
    w.warhead.cell_spread_x100 = 200; // CellSpread=2 格
    w.warhead.percent_at_max = 0;     // 边缘 0%（线性衰减）
    s.units[0].weapon = w;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    const int hp_a = s.units[1].hp, hp_b = s.units[2].hp, hp_self = s.units[0].hp;
    for (int t = 0; t < 200 && s.units[1].hp == hp_a; ++t) s.tick();
    EXPECT_LT(s.units[1].hp, hp_a) << "目标应受伤";
    EXPECT_LT(s.units[2].hp, hp_b) << "相邻格单位应受范围伤害";
    EXPECT_LT(s.units[1].hp, s.units[2].hp) << "圆心伤害应高于 1 格处（线性衰减）";
    EXPECT_EQ(s.units[0].hp, hp_self) << "发射者应豁免自伤";
}

// 连发：Burst=3 在进入 ROF 前连打 3 发（间隔 3 帧）
TEST(SimArea, BurstFiresMultipleShotsBeforeRof) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "HTNK", 1, 20, 16, 0, {}, false, 0, 68), 0u);
    sim::SimWeapon w = test_weapon(10, 90, 8); // 长 ROF：3 发必须在 90 帧前打完
    w.burst = 3;
    s.units[0].weapon = w;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    const int hp0 = s.units[1].hp;
    for (int t = 0; t < 12; ++t) s.tick();
    EXPECT_LE(s.units[1].hp, hp0 - 30) << "Burst=3 应连打 3 发（3×10 伤害）";
    EXPECT_EQ(s.units[0].burst_left, 0) << "三发打完应复位";
}

// ── M5.4 老兵/精英：XP 阈值、能力乘数、精英换装、自愈 ────────────────────────

// XP 阈值 = 自身价值 × VeteranRatio × 等级；晋升按能力重算上限血/速度
TEST(SimVeteran, PromotionThresholdAndAbilityMultipliers) {
    sim::SimWorld s;
    s.w = 32;
    s.h = 32;
    s.blocked.assign(32 * 32, 0);
    s.veteran.ratio_x100 = 300; // 3.0
    s.veteran.armor_x100 = 150;
    s.veteran.speed_x100 = 120;
    ASSERT_GT(s.spawn_unit("Player", "HTNK", 1, 10, 10, 0, test_weapon(50, 20, 4), false, 0, 60),
              0u);
    sim::SimUnit& u = s.units[0];
    u.value = 100;
    u.vet_flags = sim::kVetStronger | sim::kVetFaster;
    u.hp_base = u.hp_max = u.hp = 200;
    u.speed = u.speed_base = 60; // 显式基准（spawn 默认运动参数会覆盖 speed）
    EXPECT_EQ(u.veterancy, 0);
    u.xp = 299; // 差 1 点不升
    s.promote(u);
    EXPECT_EQ(u.veterancy, 0);
    u.xp = 300; // 阈值 = 100 × 3.0
    s.promote(u);
    EXPECT_EQ(u.veterancy, 1) << "达到阈值应升到老兵";
    EXPECT_EQ(u.hp_max, 300) << "STRONGER：上限血 ×1.5";
    EXPECT_EQ(u.hp, 300) << "升星应补足新增上限";
    EXPECT_EQ(u.speed, 72) << "FASTER：速度 ×1.2（60 → 72）";
    u.xp = 600; // 二级阈值 = 价值 × 3.0 × 2
    s.promote(u);
    EXPECT_EQ(u.veterancy, 2) << "应升到精英";
}

// 精英换装：2 级时用 ElitePrimary（对目标护甲有效时）
TEST(SimVeteran, EliteWeaponReplacesPrimaryAtLevelTwo) {
    sim::SimWorld s;
    sim::SimUnit u;
    u.weapon = test_weapon(10, 20, 4);
    u.elite_weapon = test_weapon(30, 20, 5);
    u.has_elite = true;
    u.veterancy = 1;
    EXPECT_EQ(s.effective_weapon(u, sim::kArmorNone), &u.weapon) << "1 级仍用主武器";
    u.veterancy = 2;
    EXPECT_EQ(s.effective_weapon(u, sim::kArmorNone), &u.elite_weapon) << "2 级换精英武器";
}

// 击杀记经验：目标价值给击杀者（达到阈值即晋升）
TEST(SimVeteran, KillGrantsXpToShooter) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "E1", 2, 20, 16, 0, {}, false, 0, 51), 0u);
    s.units[1].hp = 5;    // 一枪打死
    s.units[1].value = 100;
    s.units[0].weapon = test_weapon(50, 20, 8);
    s.units[0].value = 100;
    s.veteran.ratio_x100 = 300;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    for (int t = 0; t < 60 && s.units.size() > 1; ++t) s.tick();
    ASSERT_LE(s.units.size(), 1u) << "目标应被击杀";
    EXPECT_EQ(s.units[0].xp, 100) << "击杀应获得目标价值经验";
}

// SELF_HEAL：精英步兵缓慢回血（1 hp / 15 帧）
TEST(SimVeteran, SelfHealRegeneratesSlowly) {
    sim::SimWorld s = make_world(32, 32, 51, 2); // 步兵
    sim::SimUnit& u = s.units[0];
    u.hp_base = u.hp_max = 100;
    u.hp = 50;
    u.vet_flags = sim::kVetSelfHeal;
    u.veterancy = 1;
    for (int t = 0; t < 15; ++t) s.tick();
    EXPECT_EQ(u.hp, 51) << "15 帧回 1 点";
    for (int t = 0; t < 45; ++t) s.tick();
    EXPECT_EQ(u.hp, 54) << "持续回血";
}

// ── M5.5 IFV（Gunner 载具：乘客决定武器）与装载/卸载 ────────────────────────

// 装载/卸载：相邻上车（保存原武器 → 换 Gunner 武器）、乘客移出地图、下车恢复
TEST(SimIFV, LoadSwapsWeaponAndUnloadRestores) {
    sim::SimWorld s = make_world(32, 32);
    s.units[0].passenger_cap = 1;
    s.units[0].weapon = test_weapon(10, 20, 4);
    int gunner_calls = 0;
    s.gunner_weapon = [&](const std::string&, const std::string&, bool, sim::SimWeapon& out) {
        ++gunner_calls;
        out = test_weapon(99, 30, 7); // 模拟 IFV 模式武器
    };
    ASSERT_GT(s.spawn_unit("Player", "E1", 2, 17, 16, 0, {}, false, 0, 51), 0u); // 相邻乘客
    ASSERT_TRUE(s.issue_load(0, 1));
    EXPECT_EQ(gunner_calls, 1);
    EXPECT_EQ(s.units.size(), 1u) << "乘客应从地图移除";
    EXPECT_EQ(s.units[0].passenger_type, "E1");
    EXPECT_EQ(s.units[0].weapon.damage, 99) << "上车后应使用 Gunner 武器";
    ASSERT_TRUE(s.issue_unload(0));
    EXPECT_EQ(s.units.size(), 2u) << "下车应重新落位";
    EXPECT_EQ(s.units[0].weapon.damage, 10) << "下车应恢复原武器";
    EXPECT_TRUE(s.units[0].passenger_type.empty());
}

// IFV 全组合（原版数据）：[InfantryTypes] 中每个 IFVMode>=0 的乘客，
// 模式+1 必须落在 FV 的武器槽内（IFVMode=N → Weapon(N+1)）
TEST(SimIFV, AllPassengerModesMapToFvWeaponSlots) {
    RA2R_REQUIRE_ASSETS();
    const auto* db = test::rules_db();
    ASSERT_NE(db, nullptr);
    const auto* fv = db->unit("FV");
    ASSERT_NE(fv, nullptr);
    ASSERT_FALSE(fv->weapons.empty());
    int checked = 0;
    for (const auto& [k, pname] : db->rules().section("InfantryTypes")) {
        (void)k;
        const auto* p = db->unit(pname);
        if (!p || p->ifv_mode < 0) continue;
        EXPECT_LT(static_cast<size_t>(p->ifv_mode), fv->weapons.size())
            << pname << " IFVMode=" << p->ifv_mode << " 超出 FV 武器槽";
        ++checked;
    }
    EXPECT_GT(checked, 10) << "原版应有多名可上 IFV 的步兵";
    // 具体抽查：GI（E1）IFVMode=2 → Weapon3=CRM60；工程师槽 = RepairBullet
    const auto* e1 = db->unit("E1");
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->ifv_mode, 2);
    ASSERT_GE(fv->weapons.size(), 3u);
    EXPECT_EQ(fv->weapons[2], "CRM60") << "IFVMode=2 → Weapon3（GI 的 IFV 武器）";
    EXPECT_EQ(fv->weapons[1], "RepairBullet") << "IFVMode=1 → Weapon2（工程师）";
}

// ── M5.6 特殊武器效果（心灵控制/EMP/铁幕/传送/辐射）与工程师占领 ─────────────

// 心灵控制：目标归属改为控制方；控制者死亡后恢复
TEST(SimSpecial, MindControlSwitchesOwnerAndReverts) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "E1", 2, 20, 16, 0, {}, false, 0, 51), 0u);
    sim::SimWeapon w = test_weapon(0, 20, 8); // 纯控制武器（伤害 0）
    w.warhead.mind_control = true;
    s.units[0].weapon = w;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    for (int t = 0; t < 30; ++t) s.tick();
    ASSERT_EQ(s.units.size(), 2u);
    EXPECT_EQ(s.units[1].owner, "Player") << "被心灵控制后应归属控制方";
    EXPECT_EQ(s.units[1].mc_by, s.units[0].id);
    // 控制者死亡（直接置死由 tick 清理）→ 恢复原归属
    s.units[0].hp = 0;
    for (int t = 0; t < 5; ++t) s.tick();
    for (const auto& u : s.units) {
        if (u.type == "E1") {
            EXPECT_EQ(u.owner, "Enemy") << "控制者死亡应恢复原归属";
        }
    }
}

// EMP：瘫痪期间不能移动（指令无效）
TEST(SimSpecial, EmpDisablesUnit) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "E1", 2, 20, 16, 0, {}, false, 0, 51), 0u);
    sim::SimWeapon w = test_weapon(0, 20, 8);
    w.warhead.em_effect = true;
    s.units[0].weapon = w;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    for (int t = 0; t < 30 && s.units[1].emp_ticks == 0; ++t) s.tick();
    ASSERT_GT(s.units[1].emp_ticks, 0) << "应被 EMP 瘫痪";
    const int c0 = s.units[1].col, r0 = s.units[1].row;
    ASSERT_TRUE(s.issue_move(1, 25, 16)); // 瘫痪中下移动指令
    for (int t = 0; t < 30 && s.units[1].emp_ticks > 0; ++t) s.tick();
    EXPECT_EQ(s.units[1].col, c0) << "瘫痪期间不得移动";
    EXPECT_EQ(s.units[1].row, r0);
}

// 铁幕：免伤（无敌帧内 hp 不变）
TEST(SimSpecial, IronCurtainBlocksDamage) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "E1", 2, 20, 16, 0, {}, false, 0, 51), 0u);
    sim::SimWeapon w = test_weapon(0, 20, 8);
    w.warhead.iron_curtain = true;
    s.units[0].weapon = w;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    for (int t = 0; t < 30 && s.units[1].iron_ticks == 0; ++t) s.tick();
    ASSERT_GT(s.units[1].iron_ticks, 0) << "应获得铁幕";
    const int hp0 = s.units[1].hp;
    // 换高伤害武器继续打（无敌期间应免伤）
    s.units[0].weapon = test_weapon(100, 5, 8);
    s.units[0].cooldown = 0;
    s.tick();
    EXPECT_EQ(s.units[1].hp, hp0) << "铁幕期间应免伤";
}

// 传送（超时空）：目标被传走（移出战场）
TEST(SimSpecial, TeleportRemovesTarget) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "E1", 2, 20, 16, 0, {}, false, 0, 51), 0u);
    sim::SimWeapon w = test_weapon(0, 20, 8);
    w.warhead.teleport = true;
    s.units[0].weapon = w;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    for (int t = 0; t < 30 && s.units.size() > 1; ++t) s.tick();
    EXPECT_LE(s.units.size(), 1u) << "被传送的目标应移出战场";
}

// 辐射：命中格辐射场，逐帧伤害且随时间衰减
TEST(SimSpecial, RadiationDamagesOverTime) {
    sim::SimWorld s = make_world(32, 32);
    ASSERT_GT(s.spawn_unit("Enemy", "E1", 2, 20, 16, 0, {}, false, 0, 51), 0u);
    sim::SimWeapon w = test_weapon(0, 20, 8);
    w.warhead.rad_level = 100;
    s.units[0].weapon = w;
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    for (int t = 0; t < 40 && s.units[1].hp == s.units[1].hp_max; ++t) s.tick();
    // 命中后应开始受辐射伤害
    int hp_before = s.units[1].hp;
    for (int t = 0; t < 30; ++t) s.tick();
    EXPECT_LT(s.units[1].hp, hp_before) << "辐射场应持续造成伤害";
}

// 工程师占领：相邻即时，建筑换归属、工程师消耗
TEST(SimSpecial, EngineerCaptureTakesBuilding) {
    sim::SimWorld s = make_world(32, 32);
    // 敌方建筑（手动放置；用 spawn_building 简化）
    const uint32_t bid = s.spawn_building("Enemy", "GAPOWR", 20, 16, 2, 2, 800, 200, false, 1,
                                          750);
    ASSERT_GT(bid, 0u);
    ASSERT_GT(s.spawn_unit("Player", "ENGINEER", 2, 21, 16, 0, {}, false, 0, 51), 0u);
    const size_t eng = s.units.size() - 1;
    ASSERT_TRUE(s.issue_capture(eng, 0));
    EXPECT_EQ(s.buildings[0].owner, "Player") << "占领后建筑应换归属";
    EXPECT_EQ(s.units.size(), eng) << "工程师应被消耗";
}

// ── M5.7 飞行单位：直线飞行、忽略地形/占用、AA/AG 目标种类 ──────────────────

// 空中直飞：墙不挡飞机（地面单位会被挡）
TEST(SimAir, FliesOverWallsAndIgnoresOccupancy) {
    sim::SimWorld s = make_world(32, 32);
    for (int r = 0; r < 32; ++r) s.blocked[r * 32 + 20] = 1; // 整列墙
    s.units[0].air = true;
    ASSERT_TRUE(s.issue_move(0, 26, 16));
    int t = 0;
    while (t < 600 && (s.units[0].col != 26 || s.units[0].row != 16)) {
        s.tick();
        ++t;
    }
    EXPECT_EQ(s.units[0].col, 26) << "飞机应飞越墙体";
    EXPECT_EQ(s.units[0].row, 16);
    EXPECT_TRUE(s.projectiles.empty());
}

// 对空/对地：can_ag=no 的武器打不了地面；can_aa=no 的打不了空中
TEST(SimAir, AaAgTargetKindGating) {
    sim::SimWorld s;
    sim::SimUnit u;
    u.weapon = test_weapon(50, 20, 6);
    u.weapon.can_aa = true;
    u.weapon.can_ag = false; // 纯对空
    EXPECT_EQ(s.effective_weapon(u, sim::kArmorNone, false), nullptr) << "对空武器不得打地面";
    EXPECT_NE(s.effective_weapon(u, sim::kArmorNone, true), nullptr) << "应能打空中";
    u.weapon.can_aa = false;
    u.weapon.can_ag = true; // 纯对地
    EXPECT_NE(s.effective_weapon(u, sim::kArmorNone, false), nullptr);
    EXPECT_EQ(s.effective_weapon(u, sim::kArmorNone, true), nullptr) << "对地武器不得打空中";
}

// ── M5.8 海军：舰船走水面航路（naval_nav）────────────────────────────────────

TEST(SimNavy, UsesNavalNavInsteadOfGroundNav) {
    sim::SimWorld s = make_world(32, 32);
    // 水面航路：只留第 16 行一条水道（其余全不可航行）
    s.naval_nav.assign(32 * 32, 1);
    for (int c = 5; c <= 30; ++c) s.naval_nav[16 * 32 + c] = 0;
    s.units[0].naval = true;
    ASSERT_TRUE(s.issue_move(0, 26, 16));
    int t = 0;
    while (t < 900 && (s.units[0].col != 26 || s.units[0].row != 16)) {
        s.tick();
        ++t;
    }
    EXPECT_EQ(s.units[0].col, 26) << "舰船应沿水面水道航行";
    EXPECT_EQ(s.units[0].row, 16);
    // 水面外（如 (26,20)）不可达 → 不下达/原地
    const int c0 = s.units[0].col, r0 = s.units[0].row;
    s.issue_move(0, 26, 20);
    for (int i = 0; i < 200; ++i) s.tick();
    EXPECT_EQ(s.units[0].row, r0) << "陆上目标不可达，舰船应原地";
    EXPECT_EQ(s.units[0].col, c0);
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
    ASSERT_TRUE(s.configure_building(bid, 0, test_weapon(40, 20, 5)));
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
    ASSERT_TRUE(s.configure_building(bid, 0, test_weapon(40, 20, 5)));
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
    ASSERT_TRUE(s.configure_building(b1, 0, test_weapon(40, 20, 5)));
    ASSERT_TRUE(s.configure_building(b2, 0, test_weapon(40, 20, 5)));
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
    ASSERT_TRUE(s.configure_building(bid, 0, test_weapon(40, 20, 5)));
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
    s.spawn_unit("A", "HTNK", 1, 10, 10, 0, test_weapon(90, 5, 3), false, 0, 68);
    s.spawn_unit("B", "HTNK", 1, 12, 10, 0, test_weapon(1, 100, 1), false, 0, 68);
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
        s.spawn_unit("Player", "HTNK", 1, 10, 10, 0, test_weapon(90, 5, 3), false, 0, 68);
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

// 游戏速度（原版游戏内滑条：0 最慢、6 最快；rulesmd 的标志值与之相反）。
// 本项目把 15Hz 基准定为普通档 3。
TEST(GameSpeed, FpsAscendsFromSlowestToFastest) {
    EXPECT_EQ(sim::game_speed_fps(sim::kGameSpeedMin), 8); // 0 = 最慢 = 8fps
    for (int i = sim::kGameSpeedMin; i < sim::kGameSpeedMax; ++i)
        EXPECT_LT(sim::game_speed_fps(i), sim::game_speed_fps(i + 1))
            << "档 " << i << " 应慢于档 " << (i + 1);
    EXPECT_EQ(sim::game_speed_fps(sim::kGameSpeedMax), 60); // 6 = 最快 = 60fps
}

TEST(GameSpeed, DefaultKeeps15HzBaseline) {
    // 默认档 = 既有 15Hz 基准（引擎默认手感不变）
    EXPECT_EQ(sim::game_speed_fps(sim::kGameSpeedDefault), 15);
    EXPECT_EQ(sim::game_speed_interval_ms(sim::kGameSpeedDefault), 66);
    // 越快间隔越短（最快档间隔 < 最慢档间隔）
    EXPECT_LT(sim::game_speed_interval_ms(6), sim::game_speed_interval_ms(0));
}

TEST(GameSpeed, ClampAndNames) {
    EXPECT_EQ(sim::clamp_game_speed(-5), sim::kGameSpeedMin);
    EXPECT_EQ(sim::clamp_game_speed(99), sim::kGameSpeedMax);
    EXPECT_EQ(sim::game_speed_fps(-5), sim::game_speed_fps(sim::kGameSpeedMin));
    EXPECT_EQ(sim::game_speed_fps(99), sim::game_speed_fps(sim::kGameSpeedMax));
    EXPECT_STRNE(sim::game_speed_name(3), "");
}
