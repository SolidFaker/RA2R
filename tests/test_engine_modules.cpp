// RA2R 引擎纯逻辑模块测试：CacheManager / ShroudMap(迷雾) / TerrainTile(地形帧)
// 无需游戏素材（地形帧测试用合成 TMP 头）。
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "ra2r/cache/cache_manager.h"
#include "ra2r/render/fog.h"

using namespace ra2r;

// ── CacheManager：Blob 缓存（命中/未命中/失效/原子写）──────────────────────

namespace {
std::filesystem::path temp_dir(const char* tag) {
    const auto p = std::filesystem::temp_directory_path() / (std::string("ra2r_test_") + tag);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}
} // namespace

TEST(CacheManager, OpenAndRoundTrip) {
    const auto root = temp_dir("cache");
    cache::CacheManager cm;
    std::string err;
    ASSERT_TRUE(cm.open(root, &err)) << err;
    const std::vector<uint8_t> src = {'S', 'H', 'P', '1'};
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    std::vector<uint8_t> out;
    // 首次未命中
    EXPECT_FALSE(cm.get(src.data(), src.size(), 3, out));
    EXPECT_EQ(cm.misses(), 1u);
    EXPECT_TRUE(cm.put(src.data(), src.size(), 3, payload));
    EXPECT_EQ(cm.writes(), 1u);
    // 命中且内容一致
    ASSERT_TRUE(cm.get(src.data(), src.size(), 3, out));
    EXPECT_EQ(out, payload);
    EXPECT_EQ(cm.hits(), 1u);
}

TEST(CacheManager, SchemaVersionInvalidates) {
    const auto root = temp_dir("cache_schema");
    cache::CacheManager cm;
    ASSERT_TRUE(cm.open(root));
    const std::vector<uint8_t> src = {9, 9};
    const std::vector<uint8_t> v1 = {1};
    const std::vector<uint8_t> v2 = {2};
    ASSERT_TRUE(cm.put(src.data(), src.size(), 1, v1));
    ASSERT_TRUE(cm.put(src.data(), src.size(), 2, v2));
    std::vector<uint8_t> out;
    ASSERT_TRUE(cm.get(src.data(), src.size(), 1, out));
    EXPECT_EQ(out, v1); // schema 1 与 2 是不同条目
    ASSERT_TRUE(cm.get(src.data(), src.size(), 2, out));
    EXPECT_EQ(out, v2);
}

TEST(CacheManager, DifferentSourcesDifferentEntries) {
    const auto root = temp_dir("cache_src");
    cache::CacheManager cm;
    ASSERT_TRUE(cm.open(root));
    const std::vector<uint8_t> a = {1, 2, 3};
    const std::vector<uint8_t> b = {1, 2, 4};
    ASSERT_TRUE(cm.put(a.data(), a.size(), 1, {10}));
    std::vector<uint8_t> out;
    EXPECT_FALSE(cm.get(b.data(), b.size(), 1, out)); // 源不同 → 未命中
    ASSERT_TRUE(cm.put(b.data(), b.size(), 1, {20}));
    ASSERT_TRUE(cm.get(a.data(), a.size(), 1, out));
    EXPECT_EQ(out, std::vector<uint8_t>{10});
    ASSERT_TRUE(cm.get(b.data(), b.size(), 1, out));
    EXPECT_EQ(out, std::vector<uint8_t>{20});
}

TEST(CacheManager, ReopenPersistsAcrossSessions) {
    const auto root = temp_dir("cache_persist");
    const std::vector<uint8_t> src = {7, 7, 7};
    {
        cache::CacheManager cm;
        ASSERT_TRUE(cm.open(root));
        ASSERT_TRUE(cm.put(src.data(), src.size(), 5, {42}));
    }
    cache::CacheManager cm2;
    ASSERT_TRUE(cm2.open(root));
    std::vector<uint8_t> out;
    ASSERT_TRUE(cm2.get(src.data(), src.size(), 5, out));
    EXPECT_EQ(out, std::vector<uint8_t>{42});
}

TEST(CacheManager, OpenInvalidPathFails) {
    cache::CacheManager cm;
    std::string err;
    // 把文件当目录用 → 失败
    const auto f = std::filesystem::temp_directory_path() / "ra2r_test_file.txt";
    std::FILE* fp = std::fopen(f.string().c_str(), "wb");
    if (fp) {
        std::fputc('x', fp);
        std::fclose(fp);
    }
    EXPECT_FALSE(cm.open(f, &err));
    EXPECT_FALSE(err.empty());
    std::error_code ec;
    std::filesystem::remove(f, ec);
}

// ── ShroudMap：状态语义与光照映射 ───────────────────────────────────────────

TEST(ShroudMap, DemoLayeringVisibleFogBlack) {
    const auto sm = render::ShroudMap::demo(64, 64, 32, 32, 5);
    EXPECT_EQ(sm.state(32, 32), render::ShroudMap::kVisible);      // 圆心可见
    EXPECT_EQ(sm.state(32, 34), render::ShroudMap::kVisible);      // 半径内
    EXPECT_GT(sm.state(32, 40), render::ShroudMap::kVisible);      // 环外：迷雾
    EXPECT_EQ(sm.state(0, 0), render::ShroudMap::kBlack);          // 远处：黑幕
    // 越界 = 黑幕（防御性）
    EXPECT_EQ(sm.state(-1, 0), render::ShroudMap::kBlack);
    EXPECT_EQ(sm.state(64, 0), render::ShroudMap::kBlack);
}

TEST(ShroudMap, LightMappingMonotonicAndBounds) {
    // 可见 = 当前光照（0）；黑幕 = 最暗（31）
    EXPECT_EQ(render::ShroudMap::to_light(render::ShroudMap::kVisible), 0);
    EXPECT_EQ(render::ShroudMap::to_light(render::ShroudMap::kBlack), 31);
    uint8_t prev = 0;
    for (uint8_t s = render::ShroudMap::kVisible; s <= render::ShroudMap::kBlack; ++s) {
        const uint8_t l = render::ShroudMap::to_light(s);
        EXPECT_GE(l, prev) << "state " << static_cast<int>(s);
        EXPECT_LE(l, 31);
        prev = l;
    }
    // 未知状态（0 / >5）不应崩溃
    EXPECT_LE(render::ShroudMap::to_light(0), 31);
    EXPECT_LE(render::ShroudMap::to_light(200), 31);
}

TEST(ShroudMap, FromMapDefaultsAllVisible) {
    // 无 [Shroud] 节的地图 → 全可见（1）
    assets::MapFile mf;
    std::string err;
    const char* candidates[] = {"tests/fixtures", "../tests/fixtures", "../../tests/fixtures"};
    std::filesystem::path dir;
    for (const char* c : candidates)
        if (std::filesystem::is_directory(c)) dir = c;
    ASSERT_FALSE(dir.empty());
    ASSERT_TRUE(mf.open(dir / "sample.map", &err)) << err;
    const auto sm = render::ShroudMap::from_map(mf);
    EXPECT_EQ(sm.state(0, 0), render::ShroudMap::kVisible);
    EXPECT_EQ(sm.state(mf.cell_w() - 1, mf.cell_h() - 1), render::ShroudMap::kVisible);
}
