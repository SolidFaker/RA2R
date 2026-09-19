// RA2R 渲染层功能测试（需游戏目录）：步兵走序列朝向块/idle/Buildup 进度/体素光栅。
// 帧识别法：把 render_objects 的单对象渲染结果与参考 SHP 帧逐像素比对，得出
// 实际绘制的帧号（与 tools/probe/probe_infanim.cpp 同法）。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ra2r/assets/shp_file.h"
#include "ra2r/render/object_layer.h"
#include "ra2r/render/palette_lut.h"
#include "ra2r/render/voxel_raster.h"
#include "test_util.h"

using namespace ra2r;

namespace {
struct ShpRef {
    assets::ShpFile shp;
    std::vector<std::vector<uint8_t>> frames; // 每帧：整画布 RGBA（同 LUT）
    int w = 0, h = 0;
    bool load(const std::string& name, const std::vector<uint8_t>& pal) {
        const auto raw = test::read_asset(name);
        if (raw.empty()) return false;
        std::string err;
        if (!shp.open(raw.data(), raw.size(), &err)) return false;
        render::PaletteLut lut;
        lut.build(pal.data());
        w = static_cast<int>(shp.width());
        h = static_cast<int>(shp.height());
        for (int f = 0; f < static_cast<int>(shp.frame_count()); ++f) {
            std::vector<uint8_t> idx;
            if (!shp.decode_frame(f, idx, nullptr)) {
                frames.emplace_back();
                continue;
            }
            const auto& fr = shp.frame(f);
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
            for (uint32_t y = 0; y < fr.cy; ++y)
                for (uint32_t x = 0; x < fr.cx; ++x) {
                    const uint8_t c = idx[static_cast<size_t>(y) * fr.cx + x];
                    if (!c) continue;
                    uint8_t r, g, b, a;
                    lut.rgba(c, 0, r, g, b, a);
                    const int px = static_cast<int>(fr.x) + static_cast<int>(x);
                    const int py = static_cast<int>(fr.y) + static_cast<int>(y);
                    uint8_t* d = rgba.data() + (static_cast<size_t>(py) * w + px) * 4;
                    d[0] = r;
                    d[1] = g;
                    d[2] = b;
                    d[3] = 255;
                }
            frames.push_back(std::move(rgba));
        }
        return true;
    }
    // 在画布上匹配帧号（扫描参考帧可见包围盒的所有对齐位置；-1 = 无匹配）
    // 先用少量"探针像素"粗筛，再做全量比对（否则大画布下太慢）
    int identify(const std::vector<uint8_t>& canvas, int bw, int bh, int max_frame = 0x7fffffff) const {
        for (int f = 0; f < static_cast<int>(frames.size()) && f <= max_frame; ++f) {
            const auto& ref = frames[static_cast<size_t>(f)];
            if (ref.empty()) continue;
            int x0 = w, y0 = h, x1 = -1, y1 = -1;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (ref[(static_cast<size_t>(y) * w + x) * 4 + 3]) {
                        x0 = std::min(x0, x);
                        y0 = std::min(y0, y);
                        x1 = std::max(x1, x);
                        y1 = std::max(y1, y);
                    }
            if (x1 < 0) continue; // 空帧
            // 探针（可见包围盒内均匀取最多 8 个非透明像素）
            std::vector<std::pair<int, int>> probes;
            const int pw = x1 - x0 + 1, ph = y1 - y0 + 1;
            for (int k = 0; k < 8 && static_cast<int>(probes.size()) < 8; ++k) {
                const int px = x0 + (pw * k) / 8;
                for (int y = y0; y <= y1; ++y) {
                    const int py = y0 + (ph * ((k * 3 + y) % 8)) / 8;
                    if (ref[(static_cast<size_t>(py) * w + px) * 4 + 3]) {
                        probes.emplace_back(px, py);
                        break;
                    }
                }
            }
            const auto px_of = [&](int x, int y) {
                return canvas.data() + (static_cast<size_t>(y) * bw + x) * 4;
            };
            const auto rp_of = [&](int x, int y) {
                return ref.data() + (static_cast<size_t>(y) * w + x) * 4;
            };
            for (int oy = -y0; oy + y1 < bh; ++oy)
                for (int ox = -x0; ox + x1 < bw; ++ox) {
                    if (ox < -x0 || oy < -y0) continue;
                    bool ok = true;
                    for (const auto& pr : probes) {
                        const int cx = ox + pr.first, cy = oy + pr.second;
                        if (cx < 0 || cy < 0 || cx >= bw || cy >= bh) {
                            ok = false;
                            break;
                        }
                        const uint8_t* c = px_of(cx, cy);
                        const uint8_t* r = rp_of(pr.first, pr.second);
                        if (c[3] == 0 || c[0] != r[0] || c[1] != r[1] || c[2] != r[2]) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok) continue;
                    for (int y = y0; y <= y1 && ok; ++y)
                        for (int x = x0; x <= x1; ++x) {
                            if (!rp_of(x, y)[3]) continue;
                            const int cx = ox + x, cy = oy + y;
                            if (cx < 0 || cy < 0 || cx >= bw || cy >= bh) {
                                ok = false;
                                break;
                            }
                            const uint8_t* c = px_of(cx, cy);
                            const uint8_t* r = rp_of(x, y);
                            if (c[3] == 0 || c[0] != r[0] || c[1] != r[1] ||
                                c[2] != r[2]) {
                                ok = false;
                                break;
                            }
                        }
                    if (ok) return f;
                }
        }
        return -1;
    }
};

// 单对象渲染 → 帧号（走 stage 同款缓存路径：预热一次后同一缓存继续用）
struct RenderFixture {
    render::IsometricGrid grid;
    int bw = 0, bh = 0, ox = 0, oy = 0;
    render::ObjectRenderCache cache;
    std::vector<uint8_t> pal;

    bool init(const std::string& palette = "UNITURB.PAL", int cells = 4) {
        pal = test::read_asset(palette);
        if (pal.size() < 768) return false;
        grid.map_bounds(cells, cells, 15, bw, bh, ox, oy); // 与 stage 同参数（max_height=15）
        std::vector<render::PlacedObject> none;
        std::vector<uint8_t> warm(static_cast<size_t>(bw) * bh * 4, 0);
        render::render_objects(none, render::UnitPaletteCfg{palette.c_str(), {}}, grid, loader, bw,
                               bh, ox, oy, warm, 0.5f, &cache);
        return cache.art_ready && cache.art_file != nullptr;
    }
    static const std::vector<uint8_t>* loader(const std::string& n);
};

// FileLoader：静态资产缓存（render_objects 要求 const 指针稳定）
const std::vector<uint8_t>* RenderFixture::loader(const std::string& n) {
    static std::map<std::string, std::vector<uint8_t>> cache;
    const auto it = cache.find(n);
    if (it != cache.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<uint8_t> v = test::read_asset(n);
    const auto [ins, ok] = cache.emplace(n, std::move(v));
    (void)ok;
    return ins->second.empty() ? nullptr : &ins->second;
}
} // namespace

// ── 步兵：朝向块序（逆时针）＋ 走序列速率 ───────────────────────────────────

TEST(RenderInfantry, FacingBlocksAreCounterClockwise) {
    RA2R_REQUIRE_ASSETS();
    RenderFixture fx;
    ASSERT_TRUE(fx.init());
    ShpRef ref;
    ASSERT_TRUE(ref.load("GI.SHP", fx.pal));
    // 走序列 Walk=8,6,6：块 f 的走路帧 = 8 + f*6 + phase
    for (int d = 0; d < 8; ++d) {
        render::PlacedObject po{};
        po.kind = 2;
        po.id = "E1"; // rulesmd Image=GI
        po.cx = 1;
        po.cy = 1;
        po.dir = static_cast<uint8_t>(d * 32);
        po.moving = 1;
        po.anim_clock = 0;
        // 用探针同款画布：直接调 render_objects
        std::vector<render::PlacedObject> objs{po};
        std::vector<uint8_t> canvas(static_cast<size_t>(fx.bw) * fx.bh * 4, 0);
        for (size_t i = 0; i < canvas.size() / 4; ++i) {
            canvas[i * 4] = 9;
            canvas[i * 4 + 1] = 9;
            canvas[i * 4 + 2] = 9;
            canvas[i * 4 + 3] = 255;
        }
        render::render_objects(objs, render::UnitPaletteCfg{"UNITURB.PAL", {}}, fx.grid,
                               RenderFixture::loader, fx.bw, fx.bh, fx.ox, fx.oy, canvas, 0.5f,
                               &fx.cache);
        const int frame = ref.identify(canvas, fx.bw, fx.bh);
        const int expect = 8 + ((7 - d) & 7) * 6; // 逆时针块序
        EXPECT_EQ(frame, expect) << "dir=" << d;
    }
}

TEST(RenderInfantry, WalkRateIsThreeTicksPerFrame) {
    RA2R_REQUIRE_ASSETS();
    RenderFixture fx;
    ASSERT_TRUE(fx.init());
    ShpRef ref;
    ASSERT_TRUE(ref.load("GI.SHP", fx.pal));
    // dir=7 → 块 0 → 帧 8..13，每 3 逻辑帧推进 1 帧
    const int expect[12] = {8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11};
    for (int t = 0; t < 12; ++t) {
        render::PlacedObject po{};
        po.kind = 2;
        po.id = "E1";
        po.cx = 1;
        po.cy = 1;
        po.dir = 7 * 32;
        po.moving = 1;
        po.anim_clock = static_cast<uint32_t>(t);
        std::vector<render::PlacedObject> objs{po};
        std::vector<uint8_t> canvas(static_cast<size_t>(fx.bw) * fx.bh * 4, 0);
        for (size_t i = 0; i < canvas.size() / 4; ++i) {
            canvas[i * 4] = 9;
            canvas[i * 4 + 1] = 9;
            canvas[i * 4 + 2] = 9;
            canvas[i * 4 + 3] = 255;
        }
        render::render_objects(objs, render::UnitPaletteCfg{"UNITURB.PAL", {}}, fx.grid,
                               RenderFixture::loader, fx.bw, fx.bh, fx.ox, fx.oy, canvas, 0.5f,
                               &fx.cache);
        EXPECT_EQ(ref.identify(canvas, fx.bw, fx.bh), expect[t]) << "t=" << t;
    }
}

TEST(RenderInfantry, IdlePlaysSequenceThenReturnsToGuard) {
    RA2R_REQUIRE_ASSETS();
    RenderFixture fx;
    ASSERT_TRUE(fx.init());
    ShpRef ref;
    ASSERT_TRUE(ref.load("GI.SHP", fx.pal));
    auto frame_at = [&](uint32_t clock, uint8_t idle_kind) {
        render::PlacedObject po{};
        po.kind = 2;
        po.id = "E1";
        po.cx = 1;
        po.cy = 1;
        po.dir = 0;
        po.idle_kind = idle_kind;
        po.idle_start = 0;
        po.anim_clock = clock;
        std::vector<render::PlacedObject> objs{po};
        std::vector<uint8_t> canvas(static_cast<size_t>(fx.bw) * fx.bh * 4, 0);
        for (size_t i = 0; i < canvas.size() / 4; ++i) {
            canvas[i * 4] = 9;
            canvas[i * 4 + 1] = 9;
            canvas[i * 4 + 2] = 9;
            canvas[i * 4 + 3] = 255;
        }
        render::render_objects(objs, render::UnitPaletteCfg{"UNITURB.PAL", {}}, fx.grid,
                               RenderFixture::loader, fx.bw, fx.bh, fx.ox, fx.oy, canvas, 0.5f,
                               &fx.cache);
        return ref.identify(canvas, fx.bw, fx.bh);
    };
    // Idle1=56,15,0,S → t=0 起每 3 帧一档；t=45 起回 Guard（dir=0 → 块 7 → 帧 7）
    EXPECT_EQ(frame_at(0, 1), 56);
    EXPECT_EQ(frame_at(2, 1), 56);
    EXPECT_EQ(frame_at(3, 1), 57);
    EXPECT_EQ(frame_at(44, 1), 70);
    EXPECT_EQ(frame_at(45, 1), 7);
    // Idle2=71,15,0,E
    EXPECT_EQ(frame_at(0, 2), 71);
    EXPECT_EQ(frame_at(3, 2), 72);
}

// ── 建筑 Buildup 进度（帧 = ticks·建造段/build_total）──────────────────────

TEST(RenderBuilding, BuildupProgressAndInstantFallback) {
    RA2R_REQUIRE_ASSETS();
    RenderFixture fx;
    ASSERT_TRUE(fx.init("UNITTEM.PAL", 14)); // 建筑精灵画布大（GAPOWRMK 266x224），格数要够
    ShpRef mk;
    ASSERT_TRUE(mk.load("GAPOWRMK.SHP", fx.pal));
    auto buildup_frame = [&](int ticks, int total) {
        render::PlacedObject po{};
        po.kind = 0;
        po.id = "GAPOWR";
        po.cx = 1;
        po.cy = 1;
        po.hp = 1;
        po.build_p = static_cast<float>(ticks) / static_cast<float>(std::max(1, total));
        po.build_ticks = ticks;
        po.build_total = total;
        std::vector<render::PlacedObject> objs{po};
        std::vector<uint8_t> canvas(static_cast<size_t>(fx.bw) * fx.bh * 4, 0);
        for (size_t i = 0; i < canvas.size() / 4; ++i) {
            canvas[i * 4] = 9;
            canvas[i * 4 + 1] = 9;
            canvas[i * 4 + 2] = 9;
            canvas[i * 4 + 3] = 255;
        }
        render::render_objects(objs, render::UnitPaletteCfg{"UNITTEM.PAL", {}}, fx.grid,
                               RenderFixture::loader, fx.bw, fx.bh, fx.ox, fx.oy, canvas, 0.5f,
                               &fx.cache);
        return mk.identify(canvas, fx.bw, fx.bh, 24); // 只看建造段（0..24），避免匹配到阴影段
    };
    // GAPOWRMK：25 建造帧 + 25 阴影帧（shadow_start=25）；54 帧工期
    EXPECT_EQ(buildup_frame(0, 54), 0);
    EXPECT_EQ(buildup_frame(10, 54), 4);   // 10·25/54 = 4
    EXPECT_EQ(buildup_frame(20, 54), 9);   // 20·25/54 = 9
    EXPECT_EQ(buildup_frame(53, 54), 24);  // 收尾帧
}

// ── 体素光栅：确定性 + 朝向改变输出 ─────────────────────────────────────────

TEST(RenderVoxel, RasterIsDeterministicAndYawSensitive) {
    RA2R_REQUIRE_ASSETS();
    const auto raw = test::read_asset("HTNK.VXL");
    ASSERT_FALSE(raw.empty());
    assets::VxlFile vxl;
    std::string err;
    ASSERT_TRUE(vxl.open(raw.data(), raw.size(), &err)) << err;
    render::VoxelView v;
    v.scale = 0.35f;
    v.pitch = 0.0f;
    v.yaw = 0.0f;
    const auto a = render::rasterize_voxel_model(vxl, nullptr, 0, v);
    const auto b = render::rasterize_voxel_model(vxl, nullptr, 0, v);
    ASSERT_GT(a.w, 0);
    ASSERT_GT(a.h, 0);
    EXPECT_EQ(a.w, b.w);
    EXPECT_EQ(a.h, b.h);
    EXPECT_EQ(a.rgba, b.rgba); // 同参数逐字节一致
    v.yaw = 1.5708f;
    const auto c = render::rasterize_voxel_model(vxl, nullptr, 0, v);
    EXPECT_GT(c.w, 0);
    EXPECT_NE(a.rgba, c.rgba); // 朝向改变 → 输出改变
}
