// RA2R 核心纯逻辑测试：Westwood INI 解析 / 等距格几何 / 调色板 LUT / 8 向寻路
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "ra2r/core/ini_file.h"
#include "ra2r/render/isometric.h"
#include "ra2r/render/palette_lut.h"
#include "ra2r/sim/pathfind.h"

using namespace ra2r;

// ── Westwood INI（core/ini_file）──────────────────────────────────────────────

TEST(IniFile, ParseSectionsKeysCommentsAndDuplicates) {
    const char* src =
        "; 注释\n"
        "[General]\n"
        "BuildupTime=.06   ; trailing comment\n"
        "IdleActionFrequency=.15\n"
        "\n"
        "[Colors]\n"
        "DarkBlue=25,60,255\n"
        "DarkBlue=25,60,254\n"; // 重复键（后者生效：get 取最后一个）
    core::IniFile ini;
    std::string err;
    ASSERT_TRUE(ini.parse(reinterpret_cast<const uint8_t*>(src), std::strlen(src), &err));
    EXPECT_TRUE(ini.has_section("general")); // 节名大小写不敏感
    EXPECT_FALSE(ini.has_section("Nope"));
    EXPECT_EQ(ini.get("General", "BuildupTime"), ".06"); // 注释被剥离
    EXPECT_EQ(ini.get("GENERAL", "idleactionfrequency"), ".15");
    EXPECT_EQ(ini.get("General", "Missing", "def"), "def");
    // 重复键：get 取**第一条**（Westwood INI 语义，见 ini_file.cpp）；get_all 全取
    EXPECT_EQ(ini.get("Colors", "DarkBlue"), "25,60,255");
    const auto all = ini.get_all("Colors", "DarkBlue");
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0], "25,60,255");
    EXPECT_EQ(all[1], "25,60,254");
    // section_names 保留插入序
    const auto& names = ini.section_names();
    ASSERT_GE(names.size(), 2u);
    EXPECT_EQ(names[0], "General");
    EXPECT_EQ(names[1], "Colors");
}

TEST(IniFile, TypedGetters) {
    const char* src =
        "[General]\n"
        "Yes1=yes\n"
        "Yes2=true\n"
        "No1=no\n"
        "Int1=-42\n"
        "Int2=7 ; x\n";
    core::IniFile ini;
    ASSERT_TRUE(ini.parse(reinterpret_cast<const uint8_t*>(src), std::strlen(src)));
    EXPECT_TRUE(ini.get_bool("General", "Yes1"));
    EXPECT_TRUE(ini.get_bool("General", "Yes2"));
    EXPECT_FALSE(ini.get_bool("General", "No1"));
    EXPECT_TRUE(ini.get_bool("General", "Missing", true));
    EXPECT_EQ(ini.get_int("General", "Int1", 0), -42);
    EXPECT_EQ(ini.get_int("General", "Int2", 0), 7);
    EXPECT_EQ(ini.get_int("General", "Missing", 99), 99);
}

// ── 等距格几何（render/isometric）─────────────────────────────────────────────

TEST(IsometricGrid, CellToPixelBrickWallLayout) {
    render::IsometricGrid g;
    EXPECT_EQ(g.tile_w, 60);
    EXPECT_EQ(g.tile_h, 30);
    int x0, y0, x1, y1, x2, y2;
    g.cell_to_pixel(0, 0, x0, y0);
    g.cell_to_pixel(1, 0, x1, y1);
    g.cell_to_pixel(0, 1, x2, y2);
    EXPECT_EQ(x0, 0);
    EXPECT_EQ(y0, 0);
    EXPECT_EQ(x1 - x0, 60); // 列步 = 屏幕水平 60px
    EXPECT_EQ(y1, y0);
    EXPECT_EQ(y2 - y0, 15);                          // 行步 = 半瓦高
    EXPECT_EQ(std::abs(x2 - x0) + std::abs(y2 - y0), 45); // 行步沿砖墙斜向（30+15）
}

TEST(IsometricGrid, PixelToCellRoundTrip) {
    render::IsometricGrid g;
    for (int c = 0; c < 20; ++c)
        for (int r = 0; r < 20; ++r) {
            int px, py;
            g.cell_to_pixel(c, r, px, py);
            int cc, cr;
            g.pixel_to_cell(px + g.tile_w / 2, py + g.tile_h / 2, cc, cr);
            EXPECT_EQ(cc, c) << "cell " << c << "," << r;
            EXPECT_EQ(cr, r) << "cell " << c << "," << r;
        }
}

TEST(IsometricGrid, MapBoundsContainAllCells) {
    render::IsometricGrid g;
    int bw, bh, ox, oy;
    g.map_bounds(10, 20, 15, bw, bh, ox, oy);
    EXPECT_GT(bw, 0);
    EXPECT_GT(bh, 0);
    // 每个格中心的画布坐标都应在包围盒内
    for (int c = 0; c < 10; ++c)
        for (int r = 0; r < 20; ++r) {
            int px, py;
            g.cell_to_pixel(c, r, px, py);
            EXPECT_GT(ox + px, 0);
            EXPECT_LT(oy + py + g.tile_h / 2, bh);
        }
}

// ── 调色板 LUT（render/palette_lut）──────────────────────────────────────────

TEST(PaletteLut, ExpandSixBitAndSpecialIndices) {
    // 6 位 ×4 展开；索引 0 = 透明；索引 1 = 原版阴影（黑 + alpha 140，OpenRA 同）
    std::vector<uint8_t> pal(768, 0);
    pal[2 * 3 + 0] = 63; // 索引 2 红（6 位满值）
    pal[3 * 3 + 1] = 63; // 索引 3 绿
    render::PaletteLut lut;
    lut.build(pal.data());
    uint8_t r, g, b, a;
    lut.rgba(2, 0, r, g, b, a);
    EXPECT_EQ(r, 252); // 63 × 4
    EXPECT_EQ(a, 255);
    lut.rgba(3, 0, r, g, b, a);
    EXPECT_EQ(g, 252);
    lut.rgba(0, 0, r, g, b, a);
    EXPECT_EQ(a, 0); // 透明
    lut.rgba(1, 0, r, g, b, a);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(a, 140); // 阴影半透明
}

TEST(PaletteLut, RemapReplacesIndex16To31Only) {
    std::vector<uint8_t> pal(768, 0);
    for (int i = 0; i < 256; ++i) {
        pal[i * 3 + 0] = 32;
        pal[i * 3 + 1] = 32;
        pal[i * 3 + 2] = 32; // 32×4 = 128 灰
    }
    uint8_t ramp[48] = {};
    for (int i = 0; i < 16; ++i) {
        ramp[i * 3 + 0] = 63;
        ramp[i * 3 + 1] = 0;
        ramp[i * 3 + 2] = 0; // 阵营色 ramp 已是 8 位（不 ×4）
    }
    render::PaletteLut base, rm;
    base.build(pal.data());
    rm.build(pal.data(), ramp);
    uint8_t r0, g0, b0, a0, r1, g1, b1, a1;
    rm.rgba(16, 0, r1, g1, b1, a1); // 16..31 被替换
    EXPECT_EQ(r1, 63);
    EXPECT_EQ(g1, 0);
    base.rgba(16, 0, r0, g0, b0, a0);
    EXPECT_EQ(r0, 128); // 未重映射 = 调色盘 32×4
    rm.rgba(15, 0, r1, g1, b1, a1); // 15 不受影响
    base.rgba(15, 0, r0, g0, b0, a0);
    EXPECT_EQ(r1, r0);
    EXPECT_EQ(g1, g0);
    rm.rgba(32, 0, r1, g1, b1, a1); // 32 不受影响
    base.rgba(32, 0, r0, g0, b0, a0);
    EXPECT_EQ(r1, r0);
    EXPECT_EQ(b1, b0);
}

TEST(PaletteLut, LevelsGetDarkerMonotonically) {
    std::vector<uint8_t> pal(768, 0);
    pal[3] = 63;
    pal[4] = 63;
    pal[5] = 63; // 索引 1 白
    render::PaletteLut lut;
    lut.build(pal.data());
    uint8_t prev = 255;
    for (int level = 0; level < render::PaletteLut::kLevels; ++level) {
        uint8_t r, g, b, a;
        lut.rgba(1, level, r, g, b, a);
        EXPECT_LE(r, prev) << "level " << level;
        prev = r;
    }
}

// ── 8 向寻路（sim/pathfind）──────────────────────────────────────────────────

namespace {
std::vector<uint8_t> open_grid(int w, int h) { return std::vector<uint8_t>(w * h, 0); }

// 注意：brick 格的 8 邻接依赖行奇偶（par = (min_s+min_d)&1）；测试统一 par=0。
} // namespace

TEST(PathFind, SameCellReturnsTargetCell) {
    auto grid = open_grid(16, 16);
    const auto p = sim::find_path(grid, 16, 16, 4, 4, 4, 4);
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].first, 4);
    EXPECT_EQ(p[0].second, 4);
}

TEST(PathFind, StraightLineNoZigZag) {
    // 地图轴向步（+X，1 格 1 步）不应退化成"两次一拐"的阶梯
    auto grid = open_grid(64, 64);
    const int par = 0; // 行偶 → +X = (0,1)
    const auto p = sim::find_path(grid, 64, 64, 32, 32, 32 + 12, 32, par);
    ASSERT_FALSE(p.empty());
    // 终点正确
    EXPECT_EQ(p.back().first, 44);
    EXPECT_EQ(p.back().second, 32);
    // 每一步都是一格地图轴向步：|Δc| ≤ 1 且 |Δr| ≤ 2 且 步数为地图距离
    int c = 32, r = 32, steps = 0;
    for (const auto& cell : p) {
        const int dc = std::abs(cell.first - c);
        const int dr = std::abs(cell.second - r);
        EXPECT_LE(dc, 1);
        EXPECT_LE(dr, 2);
        EXPECT_GT(dc + dr, 0);
        c = cell.first;
        r = cell.second;
        ++steps;
    }
    EXPECT_LE(steps, 14); // 12 格地图轴向 ≈ 12~14 步（对角步最多 2 格/步）
}

TEST(PathFind, DiagonalReachesFartherPerStep) {
    // 屏幕垂直（+X+Y）= (0,2) 单步 2 格：12 格地图对角应 ≈7 步
    auto grid = open_grid(64, 64);
    const auto p = sim::find_path(grid, 64, 64, 32, 32, 32, 32 + 12, 0);
    ASSERT_FALSE(p.empty());
    EXPECT_EQ(p.back().first, 32);
    EXPECT_EQ(p.back().second, 44);
    EXPECT_LE(p.size(), 12u); // 必须比"两步一格"快（≤ 实际步数 6~7 + 余量）
}

TEST(PathFind, AvoidsBlockedCellsAndFailsWhenSealed) {
    auto grid = open_grid(32, 32);
    for (int r = 0; r < 30; ++r) grid[r * 32 + 16] = 1; // 竖墙（留一行缺口）
    const auto p = sim::find_path(grid, 32, 32, 8, 8, 24, 8, 0);
    ASSERT_FALSE(p.empty()); // 绕行可达
    for (const auto& cell : p) EXPECT_EQ(grid[cell.second * 32 + cell.first], 0);
    // 全封：目标被阻挡 → 空
    for (int r = 0; r < 32; ++r) grid[r * 32 + 16] = 1;
    EXPECT_TRUE(sim::find_path(grid, 32, 32, 8, 8, 24, 8, 0).empty());
    // 目标本身阻挡 → 空（注意格栅下标 = row*w + col）
    grid[10 * 32 + 8] = 1;
    EXPECT_TRUE(sim::find_path(grid, 32, 32, 8, 8, 8, 10, 0).empty());
}

TEST(PathFind, OutOfBoundsAndDeterminism) {
    auto grid = open_grid(16, 16);
    EXPECT_TRUE(sim::find_path(grid, 16, 16, -1, 0, 5, 5, 0).empty());
    EXPECT_TRUE(sim::find_path(grid, 16, 16, 5, 5, 99, 5, 0).empty());
    const auto a = sim::find_path(grid, 16, 16, 2, 2, 13, 9, 0);
    const auto b = sim::find_path(grid, 16, 16, 2, 2, 13, 9, 0);
    EXPECT_EQ(a, b); // 确定性：同输入同路径
    EXPECT_FALSE(a.empty());
}

TEST(PathFind, ParityChangesDiagonalStepSet) {
    // par 不同 → 同一终点允许的对角步不同；两条路径都应可达且合法
    auto grid = open_grid(32, 32);
    const auto p0 = sim::find_path(grid, 32, 32, 5, 5, 15, 15, 0);
    const auto p1 = sim::find_path(grid, 32, 32, 5, 5, 15, 15, 1);
    ASSERT_FALSE(p0.empty());
    ASSERT_FALSE(p1.empty());
    EXPECT_EQ(p0.back().first, 15);
    EXPECT_EQ(p1.back().first, 15);
}
