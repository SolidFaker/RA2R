// RA2R 夹具测试：用 tools/gen_fixtures 合成的示例文件（tests/fixtures/）驱动
// 各格式解析器——**不依赖游戏素材**，CI 可直接跑。
//
// 覆盖：PAL（768B/6 位/重映射）、SHP（未压缩/RLE/空帧）、VXL（span 编码）、
// HVA（列主序 + 1/16 平移）、MIX（扩展 v2 + CRC32 名索引 + 嵌套读）、
// 地图（[Map] + IsoMapPack5 未压缩块 + 对象/航点）、rulesmd/artmd（类型/序列）。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ra2r/assets/hva_file.h"
#include "ra2r/assets/map_file.h"
#include "ra2r/assets/mix_file.h"
#include "ra2r/assets/pal_file.h"
#include "ra2r/assets/rules_db.h"
#include "ra2r/assets/shp_file.h"
#include "ra2r/assets/shp_layout.h"
#include "ra2r/assets/vxl_file.h"
#include "ra2r/core/ini_file.h"
#include "ra2r/render/palette_lut.h"

using namespace ra2r;

namespace {
std::filesystem::path fixture_dir() {
    // 优先相对当前工作目录（ctest 从 build 目录跑时也能找到）
    const char* candidates[] = {"tests/fixtures", "../tests/fixtures", "../../tests/fixtures"};
    for (const char* c : candidates)
        if (std::filesystem::is_directory(c)) return c;
    return "tests/fixtures";
}

std::vector<uint8_t> read_fixture(const std::string& name) {
    std::ifstream f(fixture_dir() / name, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}
} // namespace

// ── PAL ─────────────────────────────────────────────────────────────────────

TEST(Fixtures, PalLoadsAndExpands) {
    const auto raw = read_fixture("sample.pal");
    ASSERT_EQ(raw.size(), 768u);
    assets::Palette pal;
    std::string perr;
    ASSERT_TRUE(pal.load(raw.data(), raw.size(), &perr)) << perr;
    // 标记色：索引 2 纯红（6 位 63 → 8 位 252）
    render::PaletteLut lut;
    lut.build(raw.data());
    uint8_t r, g, b, a;
    lut.rgba(2, 0, r, g, b, a);
    EXPECT_EQ(r, 252);
    EXPECT_EQ(g, 0);
    // Remap 段 16..31 可被替换
    uint8_t ramp[48] = {};
    for (int i = 0; i < 16; ++i) ramp[i * 3] = 7;
    render::PaletteLut rm;
    rm.build(raw.data(), ramp);
    rm.rgba(16, 0, r, g, b, a);
    EXPECT_EQ(r, 7);
}

// ── SHP ─────────────────────────────────────────────────────────────────────

TEST(Fixtures, ShpFramesDecode) {
    const auto raw = read_fixture("sample.shp");
    ASSERT_FALSE(raw.empty());
    assets::ShpFile shp;
    std::string err;
    ASSERT_TRUE(shp.open(raw.data(), raw.size(), &err)) << err;
    EXPECT_EQ(shp.frame_count(), 4u);
    EXPECT_EQ(shp.width(), 8u);
    EXPECT_EQ(shp.height(), 6u);
    // 帧 0：未压缩，(x+y)%6+1
    std::vector<uint8_t> px;
    ASSERT_TRUE(shp.decode_frame(0, px, &err)) << err;
    ASSERT_EQ(px.size(), 48u);
    EXPECT_EQ(px[0], 1);
    EXPECT_EQ(px[1], 2);
    EXPECT_EQ(px[8], 2);
    // 帧 1：RLE（前 2 像素透明，其余字面）
    ASSERT_TRUE(shp.decode_frame(1, px, &err)) << err;
    ASSERT_EQ(px.size(), 48u);
    EXPECT_EQ(px[0], 0);
    EXPECT_EQ(px[1], 0);
    EXPECT_EQ(px[2], 3);
    EXPECT_EQ(px[9], 0);
    EXPECT_EQ(px[10], 3);
    // 帧 2：空帧（offset 0）→ 全透明
    ASSERT_TRUE(shp.decode_frame(2, px, &err)) << err;
    ASSERT_EQ(px.size(), 48u);
    for (uint8_t v : px) EXPECT_EQ(v, 0);
    // 帧 3：RLE 全透明
    ASSERT_TRUE(shp.decode_frame(3, px, &err)) << err;
    for (uint8_t v : px) EXPECT_EQ(v, 0);
    // 阴影段判定：全空帧在后半 → shadow_start = 2
    EXPECT_EQ(assets::shp_shadow_start(shp), 2);
}

// ── VXL ─────────────────────────────────────────────────────────────────────

TEST(Fixtures, VxlSpanDecodes) {
    const auto raw = read_fixture("sample.vxl");
    ASSERT_FALSE(raw.empty());
    assets::VxlFile vxl;
    std::string err;
    ASSERT_TRUE(vxl.open(raw.data(), raw.size(), &err)) << err;
    ASSERT_EQ(vxl.sections().size(), 1u);
    const auto& sec = vxl.sections()[0];
    EXPECT_EQ(sec.sx, 2);
    EXPECT_EQ(sec.sy, 2);
    EXPECT_EQ(sec.sz, 2);
    EXPECT_EQ(sec.name, "SEC_BODY");
    // 每个体素颜色 = 2 + 列号（列号 j = x + 2*y）
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 2; ++x)
            for (uint32_t z = 0; z < 2; ++z) {
                const auto& v = sec.at(static_cast<int>(x), static_cast<int>(y),
                                       static_cast<int>(z));
                EXPECT_EQ(v.color, 2 + x + 2 * y) << "x=" << x << " y=" << y << " z=" << z;
            }
}

// ── HVA ─────────────────────────────────────────────────────────────────────

TEST(Fixtures, HvaColumnMajorTranslation) {
    const auto raw = read_fixture("sample.hva");
    ASSERT_FALSE(raw.empty());
    assets::HvaFile hva;
    ASSERT_TRUE(hva.open(raw.data(), raw.size()));
    EXPECT_EQ(hva.frame_count(), 1u);
    ASSERT_EQ(hva.section_count(), 1u);
    ASSERT_EQ(hva.section_names().size(), 1u);
    EXPECT_EQ(hva.section_names()[0], "SEC_BODY");
    // 平移 (16,32,48)/16 = (1,2,3) 体素；矩阵为列主序（转置后是单位阵）
    const float* m = hva.matrix(0, 0);
    ASSERT_NE(m, nullptr);
    EXPECT_FLOAT_EQ(m[3], 16.0f);
    EXPECT_FLOAT_EQ(m[7], 32.0f);
    EXPECT_FLOAT_EQ(m[11], 48.0f);
    EXPECT_FLOAT_EQ(m[0], 1.0f);
    EXPECT_FLOAT_EQ(m[5], 1.0f);
    EXPECT_FLOAT_EQ(m[10], 1.0f);
}

// ── MIX ─────────────────────────────────────────────────────────────────────

TEST(Fixtures, MixExtendedV2ReadsEntries) {
    const auto raw = read_fixture("sample.mix");
    ASSERT_FALSE(raw.empty());
    assets::MixFile mix;
    std::string err;
    ASSERT_TRUE(mix.open(raw.data(), raw.size(), &err)) << err;
    EXPECT_TRUE(mix.structure_valid());
    EXPECT_FALSE(mix.is_encrypted());
    EXPECT_FALSE(mix.has_checksum());
    EXPECT_EQ(mix.file_count(), 2u);
    // CRC32 名索引（大小写不敏感：名字比较前统一大写）
    const auto* ini_e = mix.find_by_name("sample.ini");
    ASSERT_NE(ini_e, nullptr);
    const auto* pal_e = mix.find_by_name("SAMPLE.PAL");
    ASSERT_NE(pal_e, nullptr);
    // 读出条目并与夹具文件逐字节比对
    std::vector<uint8_t> out;
    ASSERT_TRUE(mix.read_entry(*pal_e, out));
    EXPECT_EQ(out, read_fixture("sample.pal"));
    ASSERT_TRUE(mix.read_entry(*ini_e, out));
    // 条目内容是 rulesmd 片段（含 [General]）
    const std::string text(out.begin(), out.end());
    EXPECT_NE(text.find("[General]"), std::string::npos);
    EXPECT_NE(text.find("BuildupTime=.06"), std::string::npos);
    // 未知名字 → nullptr
    EXPECT_EQ(mix.find_by_name("no_such_entry.xyz"), nullptr);
}

// ── 地图 ────────────────────────────────────────────────────────────────────

TEST(Fixtures, MapParsesPackAndObjects) {
    const auto raw = read_fixture("sample.map");
    ASSERT_FALSE(raw.empty());
    assets::MapFile mf;
    std::string err;
    ASSERT_TRUE(mf.open(raw.data(), raw.size(), &err)) << err;
    EXPECT_EQ(mf.theater(), "TEMPERAT");
    // Size=0,0,16,16 → 宽 16；IsoMapPack5 16×16 反对角线 → 行 31
    EXPECT_EQ(mf.cell_w(), 16);
    EXPECT_GT(mf.cell_h(), 16);
    // 格点阵：全部 present（256 条记录）
    int present = 0;
    for (int y = 0; y < mf.cell_h(); ++y)
        for (int x = 0; x < mf.cell_w(); ++x)
            if (mf.cell(x, y).present) ++present;
    EXPECT_EQ(present, 256);
    // 高度 (rx+ry)%3 至少出现 0/1/2
    bool h0 = false, h1 = false, h2 = false;
    for (int y = 0; y < mf.cell_h(); ++y)
        for (int x = 0; x < mf.cell_w(); ++x) {
            if (!mf.cell(x, y).present) continue;
            const int h = mf.cell(x, y).height;
            if (h == 0) h0 = true;
            if (h == 1) h1 = true;
            if (h == 2) h2 = true;
        }
    EXPECT_TRUE(h0 && h1 && h2);
    // 对象
    ASSERT_EQ(mf.buildings().size(), 1u);
    EXPECT_EQ(mf.buildings()[0].id, "GAPOWR");
    EXPECT_EQ(mf.buildings()[0].owner, "Americans");
    ASSERT_EQ(mf.units().size(), 1u);
    EXPECT_EQ(mf.units()[0].id, "MTNK");
    EXPECT_EQ(mf.units()[0].dir, 64);
    ASSERT_EQ(mf.infantry().size(), 1u);
    EXPECT_EQ(mf.infantry()[0].id, "E1");
    EXPECT_EQ(mf.infantry()[0].dir, 128);
    ASSERT_EQ(mf.terrain_objects().size(), 1u);
    EXPECT_EQ(mf.terrain_objects()[0].name, "TREE01");
    // [Terrain] key = rx + ry·1000 = 8 + 6·1000 = 6008
    // 航点（INI 节，OpenRA 语义 key=rx+ry*1000）
    EXPECT_EQ(mf.ini().get("Waypoints", "0"), "1020");
    EXPECT_EQ(mf.ini().get("Waypoints", "1"), "3050");
    // 建筑 (rx=3,ry=3) 应落在网格内（映射 (col,row) 由引擎换算）
    EXPECT_GE(mf.buildings()[0].cx, 0);
    EXPECT_LT(mf.buildings()[0].cx, mf.cell_w());
    EXPECT_GE(mf.buildings()[0].cy, 0);
    EXPECT_LT(mf.buildings()[0].cy, mf.cell_h());
}

// ── INI / rulesdb ───────────────────────────────────────────────────────────

TEST(Fixtures, RulesDbLoadsSyntheticIni) {
    const auto rules = read_fixture("sample_rules.ini");
    const auto art = read_fixture("sample.ini");
    ASSERT_FALSE(rules.empty());
    ASSERT_FALSE(art.empty());
    assets::RulesDB db;
    std::string err;
    ASSERT_TRUE(db.load(rules.data(), rules.size(), art.data(), art.size(), &err)) << err;
    // 建筑类型
    const auto* powr = db.unit("GAPOWR");
    ASSERT_NE(powr, nullptr);
    EXPECT_EQ(powr->kind, 0);
    EXPECT_EQ(powr->cost, 800);
    EXPECT_EQ(powr->power, 200);
    EXPECT_EQ(powr->strength, 750);
    EXPECT_EQ(powr->fw, 2);
    EXPECT_EQ(powr->fh, 2);
    EXPECT_EQ(powr->image, "GGPOWR");
    EXPECT_EQ(powr->buildup, "GAPOWRMK"); // 来自 artmd 片段
    EXPECT_FALSE(powr->anim.empty());     // ActiveAnim=GAPOWR_A
    EXPECT_EQ(powr->anim_dmg, "GAPOWR_AD");
    EXPECT_EQ(powr->anim_two, "GAPOWR_B");
    EXPECT_EQ(powr->anim_two_dmg, "GAPOWR_BD"); // ActiveAnimTwoDamaged=
    EXPECT_EQ(powr->idle_anim, "GAPOWR_I");     // IdleAnim=（空闲配件动画）
    EXPECT_EQ(powr->idle_anim_dmg, "GAPOWR_ID");
    EXPECT_EQ(powr->idle_two, "GAPOWR_J");      // IdleAnimTwo=（第二常驻装置）
    EXPECT_EQ(powr->prod_anim, "GAPOWR_P");     // ProductionAnim=
    EXPECT_EQ(powr->prod_anim_dmg, "GAPOWR_PD");
    // 建造厂
    const auto* cnst = db.unit("GACNST");
    ASSERT_NE(cnst, nullptr);
    EXPECT_TRUE(cnst->construction_yard);
    EXPECT_EQ(cnst->tech_level, -1);
    // 防御建筑（武器）
    const auto* pill = db.unit("GAPILL");
    ASSERT_NE(pill, nullptr);
    EXPECT_EQ(pill->primary, "PillboxWeapon");
    EXPECT_TRUE(pill->turret);
    EXPECT_EQ(pill->turret_anim, "GGPILLTUR");
    // 步兵（Image 覆盖 + 序列）
    const auto* e1 = db.unit("E1");
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->kind, 2);
    EXPECT_EQ(e1->image, "GI");
    // 武器
    const auto* m60 = db.weapon("M60");
    ASSERT_NE(m60, nullptr);
    EXPECT_EQ(m60->damage, 15);
    EXPECT_EQ(m60->rof, 20);
    EXPECT_EQ(m60->range, 4);
    // 国家 / 颜色 / 阵营 / 前置组
    const auto* usa = db.country("Americans");
    ASSERT_NE(usa, nullptr);
    EXPECT_EQ(usa->side, "GDI");
    EXPECT_EQ(usa->color, "DarkBlue");
    EXPECT_TRUE(usa->multiplay);
    const auto* color = db.color("DarkBlue");
    ASSERT_NE(color, nullptr);
    EXPECT_EQ(color->h, 25);
    EXPECT_EQ(color->s, 60);
    EXPECT_EQ(color->v, 255);
    EXPECT_EQ(db.side_countries("GDI").size(), 1u);
    EXPECT_EQ(db.prereq_group("POWER").size(), 2u);
    // [General] 原始键可查
    EXPECT_EQ(db.rules().get("General", "BuildupTime"), ".06");
    // artmd 序列节
    EXPECT_EQ(db.art().get("GISequence", "Walk"), "8,6,6");
    EXPECT_EQ(db.art().get("GISequence", "Idle1"), "56,15,0,S");
}
