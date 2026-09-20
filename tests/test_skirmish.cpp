// RA2R 遭遇战测试：基地车展开 / 阵营色 ramp / 开局兵力（需资产部分自动 SKIP）
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ra2r/sim/skirmish.h"
#include "test_util.h"

using namespace ra2r;

// ── 基地车展开（纯逻辑）──────────────────────────────────────────────────────

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
