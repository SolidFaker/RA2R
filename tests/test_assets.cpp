// RA2R 资产解析测试（需游戏目录；缺失时自动 GTEST_SKIP）
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ra2r/assets/hva_file.h"
#include "ra2r/assets/map_file.h"
#include "ra2r/assets/shp_file.h"
#include "ra2r/assets/shp_layout.h"
#include "ra2r/assets/vxl_file.h"
#include "test_util.h"

using namespace ra2r;

TEST(Assets, FileIndexBuildsAndReadsKnownFiles) {
    RA2R_REQUIRE_ASSETS();
    EXPECT_FALSE(test::read_asset("GI.SHP").empty());
    EXPECT_FALSE(test::read_asset("UNITURB.PAL").empty());
    EXPECT_FALSE(test::read_asset("GAPOWR.SHP").empty());
    EXPECT_FALSE(test::read_asset("GAPOWRMK.SHP").empty());
    EXPECT_FALSE(test::read_asset("GACNSTMK.SHP").empty());
    EXPECT_TRUE(test::read_asset("NO_SUCH_FILE_XYZ.SHP").empty());
    // 大小写不敏感
    EXPECT_FALSE(test::read_asset("gi.shp").empty());
}

TEST(Assets, ShpFramesAndShadowLayout) {
    RA2R_REQUIRE_ASSETS();
    const auto raw = test::read_asset("GI.SHP");
    ASSERT_FALSE(raw.empty());
    assets::ShpFile shp;
    std::string err;
    ASSERT_TRUE(shp.open(raw.data(), raw.size(), &err)) << err;
    // GI：8 朝向 guard（0..7）+ 行走（8..13×8）+ idle1 56..70 + idle2 71..85
    EXPECT_GE(shp.frame_count(), 86u);
    // 解码非空 + 帧尺寸合理
    std::vector<uint8_t> px;
    ASSERT_TRUE(shp.decode_frame(8, px, &err)) << err;
    EXPECT_GT(shp.frame(8).cx, 0u);
    EXPECT_GT(shp.frame(8).cy, 0u);
    // 建筑本体阴影段：GAPOWR 6 帧 → 3；GAPOWRMK 50 帧 → 25（docs/formats/shp.md）
    const auto body = test::read_asset("GAPOWR.SHP");
    assets::ShpFile bshp;
    ASSERT_TRUE(bshp.open(body.data(), body.size(), &err)) << err;
    EXPECT_EQ(assets::shp_shadow_start(bshp), 3);
    const auto mk = test::read_asset("GAPOWRMK.SHP");
    assets::ShpFile mshp;
    ASSERT_TRUE(mshp.open(mk.data(), mk.size(), &err)) << err;
    EXPECT_EQ(assets::shp_shadow_start(mshp), 25);
}

TEST(Assets, MapFileGridAndObjects) {
    RA2R_REQUIRE_ASSETS();
    assets::MapFile mf;
    std::string err;
    ASSERT_TRUE(mf.open(std::string(test::game_dir()) + "/dttd.yrm", &err)) << err;
    EXPECT_GT(mf.cell_w(), 0);
    EXPECT_GT(mf.cell_h(), 0);
    // 砖墙格 = 全矩形（present ≈ 100%，边角少数缺失）
    int present = 0;
    for (int y = 0; y < mf.cell_h(); ++y)
        for (int x = 0; x < mf.cell_w(); ++x)
            if (mf.cell(x, y).present) ++present;
    const int total = mf.cell_w() * mf.cell_h();
    EXPECT_GT(present, total * 95 / 100);
    // 地图对象/触发数据可达
    EXPECT_FALSE(mf.buildings().empty());
    EXPECT_FALSE(mf.ini().section_names().empty());
}

TEST(Assets, RulesDbTypedLookups) {
    RA2R_REQUIRE_RULES();
    const assets::RulesDB& rules = *test::rules_db();
    EXPECT_GT(rules.unit_count(), 100u);
    EXPECT_GT(rules.weapon_count(), 50u);
    const auto* gapowr = rules.unit("GAPOWR");
    ASSERT_NE(gapowr, nullptr);
    EXPECT_EQ(gapowr->kind, 0);
    EXPECT_GT(gapowr->cost, 0);
    EXPECT_GT(gapowr->power, 0);
    EXPECT_GT(gapowr->strength, 0);
    EXPECT_FALSE(gapowr->buildup.empty()); // artmd Buildup=GAPOWRMK
    const auto* e1 = rules.unit("E1");
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->image, "GI"); // rulesmd Image= 覆盖
    EXPECT_EQ(e1->kind, 2);
    const auto* mcv = rules.unit("AMCV");
    ASSERT_NE(mcv, nullptr);
    EXPECT_EQ(mcv->deploys_into, "GACNST");
    // 武器：E1 Primary= 有伤害/射程
    if (!e1->primary.empty()) {
        const auto* w = rules.weapon(e1->primary);
        if (w) {
            EXPECT_GT(w->damage, 0);
            EXPECT_GT(w->range, 0);
        }
    }
    // 国家 / 颜色 / 前置组
    const auto* usa = rules.country("Americans");
    ASSERT_NE(usa, nullptr);
    EXPECT_FALSE(usa->side.empty());
    EXPECT_FALSE(usa->color.empty());
    EXPECT_NE(rules.color(usa->color), nullptr);
    EXPECT_FALSE(rules.prereq_group("POWER").empty());
    // 大小写不敏感
    EXPECT_NE(rules.unit("gapowr"), nullptr);
}

TEST(Assets, VxlAndHvaLoad) {
    RA2R_REQUIRE_ASSETS();
    const auto raw = test::read_asset("HTNK.VXL");
    ASSERT_FALSE(raw.empty());
    assets::VxlFile vxl;
    std::string err;
    ASSERT_TRUE(vxl.open(raw.data(), raw.size(), &err)) << err;
    EXPECT_FALSE(vxl.sections().empty());
    const auto hraw = test::read_asset("HTNK.HVA");
    if (!hraw.empty()) {
        assets::HvaFile hva;
        ASSERT_TRUE(hva.open(hraw.data(), hraw.size()));
        EXPECT_GT(hva.frame_count(), 0);
    }
}
