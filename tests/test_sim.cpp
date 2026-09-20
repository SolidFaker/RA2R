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
        EXPECT_EQ(s.units[0].dir, c.expect) << "目标 (" << tc << "," << tr << ")";
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
