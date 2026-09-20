// RA2R 遭遇战测试：基地车展开 / 阵营色 ramp / 开局兵力（需资产部分自动 SKIP）
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ra2r/sim/skirmish.h"
#include "test_util.h"

using namespace ra2r;

// ── 基地车展开（纯逻辑）──────────────────────────────────────────────────────

// 展开时的固定 UnitFactory（无武器/非矿车；kind 按类型名）
static sim::UnitFactory test_factory() {
    sim::UnitFactory f;
    f.kind_of = [](const std::string&) { return 1; };
    f.weapon = [](const std::string&, sim::SimWeapon&) {};
    f.miner = [](const std::string&, bool& m, int& c) { m = false, c = 20; };
    return f;
}

TEST(SkirmishDeploy, McvDeploysIntoConyardAndRemovesUnit) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    s.spawn_unit("Player", "AMCV", 1, 32, 32, 64, {}, false, 0, 68);
    ASSERT_EQ(s.units.size(), 1u);
    const uint32_t bid = sim::deploy_mcv(s, 0, "GACNST", 4, 4, 2000, 0, 54, 1000);
    ASSERT_NE(bid, 0u);
    EXPECT_TRUE(s.units.empty()) << "展开消耗车体";
    ASSERT_EQ(s.buildings.size(), 1u);
    const auto& b = s.buildings[0];
    // 4×4 地基以基地车格为（地图空间）中心：锚 = 中心 −1,−1 换算回引擎格。
    // min_d = min_s = 0 时车格 (32,32) 对应地图 (48,−16)，锚 → (32,30)。
    EXPECT_EQ(b.col, 32);
    EXPECT_EQ(b.row, 30);
    EXPECT_EQ(b.max_hp, 1000);
    EXPECT_TRUE(b.under_construction) << "展开动画期间在建";
    // 地基格全阻挡，且基地车原格在覆盖范围内
    std::vector<std::pair<int, int>> cells;
    s.foundation_cells(b.col, b.row, 4, 4, cells);
    ASSERT_EQ(cells.size(), 16u);
    bool unit_cell_covered = false;
    for (const auto& c : cells) {
        EXPECT_EQ(s.blocked[static_cast<size_t>(c.second) * 64 + c.first], 1);
        if (c.first == 32 && c.second == 32) unit_cell_covered = true;
    }
    EXPECT_TRUE(unit_cell_covered) << "展开后车格应落在地基内";
    for (int t = 0; t < 54; ++t) s.tick();
    EXPECT_FALSE(s.buildings[0].under_construction);
    EXPECT_EQ(s.buildings[0].hp, 1000);
}

TEST(SkirmishDeploy, ConyardPacksBackIntoMcvAndFreesFootprint) {
    // UndeploysInto= 语义：收起建造厂 → 基地车；地基阻挡立即解除、建筑按出售
    // 路径无爆炸移除，重新展开可放回同一位置（落点 = 地基中心）。
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    s.no_build.assign(64 * 64, 0);
    s.ore.assign(64 * 64, 0);
    s.spawn_unit("Player", "AMCV", 1, 32, 32, 64, {}, false, 0, 68);
    const uint32_t bid = sim::deploy_mcv(s, 0, "GACNST", 4, 4, 2000, 0, 54, 1000);
    ASSERT_NE(bid, 0u);
    for (int t = 0; t < 54; ++t) s.tick(); // 完工
    ASSERT_EQ(s.buildings.size(), 1u);
    const std::vector<std::pair<int, int>> cells = s.buildings[0].footprint_cells;
    const int anchor_col = s.buildings[0].col, anchor_row = s.buildings[0].row;

    const uint32_t uid = sim::pack_building(s, 0, "AMCV", test_factory());
    ASSERT_NE(uid, 0u);
    ASSERT_EQ(s.units.size(), 1u);
    EXPECT_EQ(s.units[0].type, "AMCV");
    EXPECT_EQ(s.units[0].owner, "Player");
    // 地基格立即解除阻挡（不等 tick 清扫）
    for (const auto& c : cells)
        EXPECT_EQ(s.blocked[static_cast<size_t>(c.second) * 64 + c.first], 0);
    // 被移除建筑标记 dead 且是出售路径（无爆炸）
    EXPECT_FALSE(s.buildings[0].alive);
    EXPECT_TRUE(s.buildings[0].sold);
    EXPECT_TRUE(s.explosions.empty()) << "收起不应有爆炸";
    // 重新展开：可放回同一锚点（中心格空、车体忽略自身）
    const size_t mcv_idx = 0;
    const uint32_t bid2 = sim::deploy_mcv(s, mcv_idx, "GACNST", 4, 4, 2000, 0, 54, 1000);
    ASSERT_NE(bid2, 0u);
    s.tick(); // 清扫已移除的旧建筑
    ASSERT_EQ(s.buildings.size(), 1u);
    EXPECT_EQ(s.buildings[0].col, anchor_col);
    EXPECT_EQ(s.buildings[0].row, anchor_row);
}

TEST(SkirmishDeploy, OccupiedCenterIsRetriedAtNeighbors) {
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    s.spawn_unit("Player", "AMCV", 1, 32, 32, 0, {}, false, 0, 68);
    // 中心落点被占 → 固定邻序试近旁
    s.blocked[31 * 64 + 31] = 1;
    const uint32_t bid = sim::deploy_mcv(s, 0, "GACNST", 4, 4, 2000, 0, 1, 1000);
    ASSERT_NE(bid, 0u);
    EXPECT_TRUE(s.units.empty());
    EXPECT_EQ(s.buildings.size(), 1u);
}

TEST(SkirmishDeploy, FacingPreservedByConfigure) {
    sim::SimWorld s;
    s.w = 32;
    s.h = 32;
    s.blocked.assign(32 * 32, 0);
    s.spawn_unit("Player", "AMCV", 1, 16, 16, 128, {}, false, 0, 68);
    const uint32_t bid = sim::deploy_mcv(s, 0, "GACNST", 4, 4, 2000, 0, 1, 1000);
    ASSERT_NE(bid, 0u);
    ASSERT_TRUE(s.configure_building(bid, 128, {}));
    EXPECT_EQ(s.buildings[0].dir, 128);
    EXPECT_EQ(s.buildings[0].turret_dir, 128); // 炮塔初值 = 建筑朝向
}

// ── 阵营色 ramp（纯逻辑）────────────────────────────────────────────────────

TEST(SkirmishColor, RampHas16DistinctStepsAndBrightnessAtTop) {
    assets::ColorDef c;
    c.name = "TestBlue";
    c.h = 160;
    c.s = 255;
    c.v = 255;
    const auto r = sim::house_color_ramp(c);
    // 16 色；起点最暗、终点最亮（V = 最大亮度，ModEnc 语义）
    auto lum = [&](int i) {
        return 3 * r.rgb[i][0] + 6 * r.rgb[i][1] + r.rgb[i][2];
    };
    EXPECT_LT(lum(0), lum(15));
    int unique = 0;
    for (int i = 1; i < 16; ++i)
        if (lum(i) != lum(i - 1)) ++unique;
    EXPECT_GE(unique, 10) << "色带应有明显分层";
    // 与另一个 H 的色带不同（H 生效）
    assets::ColorDef c2 = c;
    c2.h = 20;
    const auto r2 = sim::house_color_ramp(c2);
    EXPECT_NE(r.rgb[8][0], r2.rgb[8][0]);
}

// ── 科技树 / 开局兵力（需 rulesmd）──────────────────────────────────────────

TEST(SkirmishRules, TechTreeAndStartPlan) {
    RA2R_REQUIRE_RULES();
    const assets::RulesDB& rules = *test::rules_db();
    // 建造厂（ConstructionYard）与基础链条存在
    const auto* conyard = rules.unit("GACNST");
    ASSERT_NE(conyard, nullptr);
    EXPECT_TRUE(conyard->construction_yard);
    const auto* power = rules.unit("GAPOWR");
    ASSERT_NE(power, nullptr);
    EXPECT_GT(power->cost, 0);
    EXPECT_GT(power->power, 0);
    // 建造前置：无建造厂不可造；有建造厂 + 资金可造
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    s.credits["Player"] = 10000;
    if (power->owner.empty() || !std::count(power->owner.begin(), power->owner.end(),
                                            std::string("Americans"))) {
        GTEST_SKIP() << "该 rules 版本 GAPOWR 不属于 Americans";
    }
    EXPECT_FALSE(sim::check_buildable(s, rules, "Player", "Americans", 10, *power).ok);
    s.spawn_building("Player", "GACNST", 20, 20, 4, 4, 2000, 0, false, 1, 1000);
    EXPECT_TRUE(sim::check_buildable(s, rules, "Player", "Americans", 10, *power).ok);
    // 可造列表非空且包含电厂
    const auto list = sim::buildable_buildings(s, rules, "Player", "Americans", 10);
    bool has_power = false;
    for (const auto* t : list)
        if (t->name == "GAPOWR") has_power = true;
    EXPECT_TRUE(has_power);
    // 开局兵力方案（盟军中档）
    const auto plan = sim::start_unit_plan(rules, "Americans", 2);
    EXPECT_FALSE(plan.base_actor.empty());
}

TEST(SkirmishRules, SpawnStartPlaceBaseAndSupport) {
    RA2R_REQUIRE_RULES();
    const assets::RulesDB& rules = *test::rules_db();
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    sim::UnitFactory f;
    f.kind_of = [](const std::string&) { return 1; };
    f.weapon = [](const std::string&, sim::SimWeapon&) {};
    f.miner = [](const std::string&, bool&, int&) {};
    const int n = sim::spawn_start(s, rules, "Player", "Americans", 2, 32, 32, f, 1234);
    EXPECT_GT(n, 0);
    ASSERT_FALSE(s.units.empty());
    // 基地车在场
    bool has_mcv = false;
    for (const auto& u : s.units)
        if (u.kind == 1) has_mcv = true;
    EXPECT_TRUE(has_mcv);
}

// 出生点落在水面/树丛等"展不开"的地形上时，基地车落到最近的**可展开**
// 格（原版遭遇战由地图保证出生点，本引擎按 waypoint 出生需要兜底）：
// 此前 dttd.yrm 的对手 waypoint 在水里 → AI 永远展不开、没有基地。
TEST(SkirmishRules, SpawnStartFindsDeployableSpotWhenWaypointBlocked) {
    RA2R_REQUIRE_RULES();
    const assets::RulesDB& rules = *test::rules_db();
    sim::SimWorld s;
    s.w = 64;
    s.h = 64;
    s.blocked.assign(64 * 64, 0);
    // 出生点周围 7×7 全不可通行（模拟 waypoint 落在水面）
    for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -3; dx <= 3; ++dx) s.blocked[(32 + dy) * 64 + (32 + dx)] = 1;
    sim::UnitFactory f;
    f.kind_of = [](const std::string&) { return 1; };
    f.weapon = [](const std::string&, sim::SimWeapon&) {};
    f.miner = [](const std::string&, bool&, int&) {};
    const int n = sim::spawn_start(s, rules, "Player", "Americans", 2, 32, 32, f, 1234, 4, 4);
    ASSERT_GT(n, 0);
    ASSERT_FALSE(s.units.empty());
    const auto& mcv = s.units[0];
    EXPECT_EQ(s.blocked[static_cast<size_t>(mcv.row) * 64 + mcv.col], 0) << "基地车不能在水里";
    // 以基地车格为中心能放下完整 4×4 地基（deploy_mcv 的锚点换算）
    int mrx = 0, mry = 0;
    s.cell_to_map(mcv.col, mcv.row, mrx, mry);
    int ac = 0, ar = 0;
    s.map_to_cell(mrx - 1, mry - 1, ac, ar);
    EXPECT_TRUE(s.can_place(ac, ar, 4, 4, mcv.id));
    // 完整链路：基地车展开成建造厂成功（AI 开局的第一步）
    const uint32_t bid = sim::deploy_mcv(s, 0, "GACNST", 4, 4, 2000, 0, 54, 1000);
    EXPECT_GT(bid, 0u);
}
