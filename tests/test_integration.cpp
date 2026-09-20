// RA2R 集成测试：夹具地图 → SimWorld 装载 → 移动/采矿（无游戏素材）。
// 覆盖 "解析 → 模拟" 的跨模块链路，CI 全平台可跑。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ra2r/assets/map_file.h"
#include "ra2r/sim/sim_world.h"

using namespace ra2r;

namespace {
std::filesystem::path fixture_dir() {
    const char* candidates[] = {"tests/fixtures", "../tests/fixtures", "../../tests/fixtures"};
    for (const char* c : candidates)
        if (std::filesystem::is_directory(c)) return c;
    return "tests/fixtures";
}

// 夹具地图 → SimWorld（footprint/weapon 等回调按类型名给固定值）
bool load_fixture_world(sim::SimWorld& s) {
    assets::MapFile mf;
    std::string err;
    if (!mf.open(fixture_dir() / "sample.map", &err)) return false;
    s.w = mf.cell_w();
    s.h = mf.cell_h();
    s.blocked.assign(static_cast<size_t>(s.w) * s.h, 0);
    s.ore.assign(static_cast<size_t>(s.w) * s.h, 0);
    return s.load_map(
        mf, [](const std::string& t, int& fw, int& fh) {
            if (t == "GAPOWR") fw = 2, fh = 2;
        },
        [](const std::string& t, sim::SimWeapon& w) {
            if (t == "MTNK") w = {90, 65, 6};
            if (t == "E1") w = {15, 20, 4};
        },
        [](const std::string&, bool& m, int& c) { m = false, c = 20; },
        [](const std::string&) { return false; },
        [](const std::string& t) { return t == "GAPOWR" ? 200 : 0; },
        [](int, int) { return static_cast<int16_t>(0); },
        [](int, int) { return false; });
}
} // namespace

TEST(FixtureIntegration, LoadsMapIntoSimWorld) {
    sim::SimWorld s;
    ASSERT_TRUE(load_fixture_world(s));
    // 建筑/单位/步兵 各 1（来自夹具 [Structures]/[Units]/[Infantry]）
    ASSERT_EQ(s.buildings.size(), 1u);
    EXPECT_EQ(s.buildings[0].type, "GAPOWR");
    EXPECT_EQ(s.buildings[0].owner, "Americans");
    EXPECT_EQ(s.buildings[0].fw, 2);
    EXPECT_EQ(s.buildings[0].fh, 2);
    EXPECT_EQ(s.buildings[0].power, 200);
    ASSERT_EQ(s.units.size(), 2u);
    EXPECT_EQ(s.units[0].type, "MTNK");
    EXPECT_EQ(s.units[0].kind, 1);
    EXPECT_EQ(s.units[0].dir, 64 / 32);
    EXPECT_EQ(s.units[1].type, "E1");
    EXPECT_EQ(s.units[1].kind, 2);
    EXPECT_EQ(s.units[1].dir, 128 / 32);
    // 建筑地基 4 格全部阻挡（footprint_cells 由地图空间 rx/ry 换算，非简单矩形）
    ASSERT_EQ(s.buildings[0].footprint_cells.size(), 4u);
    for (const auto& c : s.buildings[0].footprint_cells)
        EXPECT_EQ(s.blocked[static_cast<size_t>(c.second) * s.w + c.first], 1);
    // 现场放置（同锚点/同地基）必须与地图装载产生**完全相同**的引擎格地基——
    // 否则摆放预览/阻挡与地图建筑错位（曾用引擎矩形，同行错位呈对角线）
    {
        sim::SimWorld s2;
        s2.w = s.w;
        s2.h = s.h;
        s2.min_d = s.min_d;
        s2.min_s = s.min_s;
        s2.blocked.assign(static_cast<size_t>(s.w) * s.h, 0);
        const uint32_t id = s2.spawn_building("Americans", "GAPOWR", s.buildings[0].col,
                                              s.buildings[0].row, 2, 2, 300, 200, false, 1,
                                              750);
        ASSERT_NE(id, 0u);
        EXPECT_EQ(s2.buildings[0].footprint_cells, s.buildings[0].footprint_cells);
        EXPECT_EQ(s2.buildings[0].rx, s.buildings[0].rx);
        EXPECT_EQ(s2.buildings[0].ry, s.buildings[0].ry);
    }
    // 电力净值（建筑落成即计入；夹具建筑非在建）
    s.tick();
    EXPECT_EQ(s.power_net["Americans"], 200);
}

TEST(FixtureIntegration, UnitMovesAndArrives) {
    sim::SimWorld s;
    ASSERT_TRUE(load_fixture_world(s));
    const int c0 = s.units[0].col, r0 = s.units[0].row;
    // 找一个 6 格外的空地（避免踩到建筑地基）
    int tc = c0 + 6, tr = r0;
    if (tc >= s.w) tc = c0 - 6;
    ASSERT_TRUE(s.issue_move(0, tc, tr));
    for (int t = 0; t < 200 && s.units[0].order != sim::kOrderNone; ++t) s.tick();
    EXPECT_EQ(s.units[0].col, tc);
    EXPECT_EQ(s.units[0].row, tr);
    EXPECT_EQ(s.units[0].frac, 0);
    EXPECT_FALSE(s.unit_moving(0));
}

TEST(FixtureIntegration, UnitAttacksAndDamages) {
    sim::SimWorld s;
    ASSERT_TRUE(load_fixture_world(s));
    // MTNK（weapon 90/65/6）攻击 E1：应扣血直至移除
    ASSERT_TRUE(s.issue_attack_unit(0, 1));
    const int hp0 = s.units[1].hp;
    bool damaged = false;
    for (int t = 0; t < 400; ++t) {
        s.tick();
        if (s.units.size() < 2) break;
        if (s.units[1].hp < hp0) {
            damaged = true;
            break;
        }
    }
    EXPECT_TRUE(damaged) << "攻击未生效";
}

TEST(FixtureIntegration, DeterministicAcrossRuns) {
    auto run = []() {
        sim::SimWorld s;
        if (!load_fixture_world(s)) return std::string("load-fail");
        s.idle_freq_ticks = 40;
        s.issue_move(0, s.w - 2, s.h - 2);
        s.issue_attack_unit(0, 1);
        for (int t = 0; t < 300; ++t) s.tick();
        return std::to_string(s.visual_hash());
    };
    EXPECT_EQ(run(), run());
}
