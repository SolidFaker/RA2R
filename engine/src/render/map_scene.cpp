// RA2R — 共享地图场景渲染实现（接口见 map_scene.h）
#include "ra2r/render/map_scene.h"

#include <algorithm>
#include <cstdio>

#include "ra2r/core/ini_file.h"
#include "ra2r/render/terrain_tile.h"

namespace ra2r::render {

std::vector<SceneCell> scene_cells(const assets::MapFile& map, bool transpose) {
    // 场景网格：transpose 时宽高互换（诊断对照用），行主序 [sy*w + sx]
    const int w = transpose ? map.cell_h() : map.cell_w();
    const int h = transpose ? map.cell_w() : map.cell_h();
    std::vector<SceneCell> out(static_cast<size_t>(w) * h);
    for (int cy = 0; cy < map.cell_h(); ++cy) {
        for (int cx = 0; cx < map.cell_w(); ++cx) {
            const auto& c = map.cell(cx, cy);
            const int sx = transpose ? cy : cx;
            const int sy = transpose ? cx : cy;
            SceneCell& s = out[static_cast<size_t>(sy) * w + sx];
            s.tile_id = c.tile_id;
            s.subtile = c.subtile;
            s.height = c.height;
            s.present = c.present;
        }
    }
    return out;
}

std::map<int, std::string> scene_overlay_names(const FileLoader& load) {
    std::map<int, std::string> ov_names;
    const auto* raw = load("RULESMD.INI");
    if (!raw) return ov_names;
    ra2r::core::IniFile rini;
    std::string err;
    if (!rini.parse(raw->data(), raw->size(), &err)) return ov_names;
    for (const auto& [k, v] : rini.section("OverlayTypes")) {
        const int t = std::atoi(k.c_str());
        if (t > 0 && !v.empty()) ov_names[t] = v;
    }
    // 类型 0 = 沙袋围墙（rulesmd 表从 1 开始，缺此项）
    ov_names[0] = "GASAND";
    // Image= 覆盖（BRIDGE1→BRIDGE、BRIDGEB1→BRIDGB 等）
    for (auto& [t, name] : ov_names) {
        const std::string img = rini.get(name, "Image", "");
        if (!img.empty()) name = img;
    }
    return ov_names;
}

void build_scene_decor(const assets::MapFile& map, const assets::TheaterConfig& cfg,
                       const FileLoader& load, bool transpose,
                       std::vector<MapDecorObject>& decor) {
    // 树木/岩石（[Terrain] → 名 + 剧场后缀的文件）
    const auto dec_h = [&](int cx, int cy) {
        if (cx < 0 || cy < 0 || cx >= map.cell_w() || cy >= map.cell_h()) return 0;
        return static_cast<int>(map.cell(cx, cy).height);
    };
    for (const auto& t : map.terrain_objects()) {
        decor.push_back({MapDecorObject::kTmpTile, t.name + "." + cfg.ext,
                         transpose ? t.cy : t.cx, transpose ? t.cx : t.cy, dec_h(t.cx, t.cy),
                         0});
    }
    // 覆盖物（OverlayPack/OverlayDataPack → 类型硬编码艺术表 + 帧规则）
    const std::map<int, std::string> ov_names = scene_overlay_names(load);
    for (int y = 0; y < map.cell_h(); ++y) {
        for (int x = 0; x < map.cell_w(); ++x) {
            const auto& c = map.cell(x, y);
            if (!c.present) continue;
            const uint8_t ot = map.overlay_type(c.x, c.y);
            if (ot == 0xFF) continue; // 0 = 沙袋围墙（有效覆盖物）
            const auto it = ov_names.find(ot);
            if (it == ov_names.end()) continue;
            const uint8_t data = map.overlay_render_frame(x, y);
            if (data == 0xFF) continue; // 多格桥件的延续格不单独绘制
            int adx = 0, ady = 0;
            map.overlay_anchor_offset(ot, adx, ady);
            // 低桥多格件：绘制格 = 件中格（data=1）——画布中心对中格时桥面
            // 顶角恰好落在件顶角（与建筑顶格锚点同语义，像素掩码实测）
            int ddx = x, ddy = y;
            map.overlay_draw_cell(x, y, ddx, ddy);
            decor.push_back(
                {MapDecorObject::kTmpTile,
                 overlay_type_art_name(ot, it->second, cfg.ext,
                                       wall_theater_letter(map.theater())),
                 transpose ? ddy + ady : ddx + adx, transpose ? ddx + adx : ddy + ady,
                 static_cast<int>(c.height), data});
        }
    }
}

void render_scene(const std::vector<SceneCell>& cells, int cell_w, int cell_h,
                  const assets::TerrainTileset& tileset, const PaletteLut& terrain_lut,
                  const PaletteLut& resource_lut, const PaletteLut& unit_lut,
                  const std::vector<MapDecorObject>& decor, bool no_decor,
                  const FileLoader& load, cache::CacheManager* cache,
                  const std::function<int(int, int)>* light_fn, const IsometricGrid& grid,
                  int& bw, int& bh, int& ox, int& oy, std::vector<uint8_t>& canvas,
                  int* drawn_out) {
    auto put = [&](int x, int y, const uint8_t* src) {
        if (x < 0 || y < 0 || x >= bw || y >= bh || src[3] == 0) return;
        uint8_t* d = canvas.data() + (static_cast<size_t>(y) * bw + x) * 4;
        d[0] = src[0];
        d[1] = src[1];
        d[2] = src[2];
        d[3] = 255;
    };
    // 绘制顺序：按 (cx+cy, height, cx) 排序画家算法（悬崖/高度重叠正确）
    // cells = 场景网格（行主序 [cy*cell_w + cx]；transpose 已在 scene_cells 提取时完成）
    std::vector<std::pair<int, int>> order;
    order.reserve(static_cast<size_t>(cell_w) * cell_h);
    for (int cy = 0; cy < cell_h; ++cy)
        for (int cx = 0; cx < cell_w; ++cx) {
            const size_t i = static_cast<size_t>(cy) * cell_w + cx;
            if (i < cells.size() && cells[i].present) order.emplace_back(cx, cy);
        }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second < b.second; // 砖墙行序（后行盖前行）
        return a.first < b.first;
    });
    std::map<std::string, TerrainTile> tile_cache;
    int drawn = 0;
    for (const auto& [cx, cy] : order) {
        const size_t i = static_cast<size_t>(cy) * cell_w + cx;
        const SceneCell& cell = cells[i];
        const std::string& name = tileset.name_for(cell.tile_id);
        if (name.empty()) continue;
        const std::vector<uint8_t>* raw = load(name);
        if (!raw) continue;
        TerrainTile& t = tile_cache[name];
        if (!t.is_open()) {
            std::string terr;
            if (!t.open(raw->data(), raw->size(), &terr)) continue;
        }
        const int frame_i = cell.subtile % t.frame_count();
        std::vector<uint8_t> key = *raw;
        key.push_back(static_cast<uint8_t>(frame_i));
        key.push_back(static_cast<uint8_t>(frame_i >> 8));
        std::vector<uint8_t> blob;
        TerrainTileFrame fr_local;
        const TerrainTileFrame* fr = nullptr;
        if (cache && cache->get(key.data(), key.size(), TerrainTile::kFrameSchema, blob) &&
            TerrainTile::deserialize_frame(blob.data(), blob.size(), fr_local, nullptr)) {
            fr = &fr_local; // 缓存命中
        } else {
            fr = &t.frame(frame_i); // 未命中：解码后回写
            TerrainTile::serialize_frame(*fr, blob);
            if (cache) cache->put(key.data(), key.size(), TerrainTile::kFrameSchema, blob);
        }
        if (fr->bounds_w <= 0 || fr->bounds_h <= 0) continue;
        const int level = light_fn ? std::clamp((*light_fn)(cx, cy), 0, 24) : 0;
        // 帧像素 → RGBA（PaletteLut 光照等级）
        std::vector<uint8_t> rgba(static_cast<size_t>(fr->bounds_w) * fr->bounds_h * 4, 0);
        for (size_t p = 0; p < fr->pixels.size(); ++p) {
            const uint8_t idx = fr->pixels[p];
            if (idx == 0) continue;
            uint8_t r, g, b, a;
            terrain_lut.rgba(idx, level, r, g, b, a);
            uint8_t* d = rgba.data() + p * 4;
            d[0] = r;
            d[1] = g;
            d[2] = b;
            d[3] = 255;
        }
        // 放置：帧原点 = 格包围盒左上 + bounds 偏移（砖墙几何：菱形中心严格落在格中心）；
        // 高度每级抬 kHeightLevelPx=15px（半瓦高）
        int px, py;
        grid.cell_to_pixel(cx, cy, px, py);
        const int top_x = ox + px + fr->bounds_x;
        const int top_y = oy + py + fr->bounds_y - cell.height * kHeightLevelPx;
        for (int fy = 0; fy < fr->bounds_h; ++fy) {
            const uint8_t* s = rgba.data() + static_cast<size_t>(fy) * fr->bounds_w * 4;
            for (int fx = 0; fx < fr->bounds_w; ++fx) {
                if (s[fx * 4 + 3]) put(top_x + fx, top_y + fy, s + fx * 4);
            }
        }
        ++drawn;
    }
    // 装饰层：地形之上、对象之下
    if (!decor.empty() && !no_decor)
        render_map_decor(decor, terrain_lut, resource_lut, unit_lut, grid, load, bw, bh, ox, oy,
                         canvas);
    if (drawn_out) *drawn_out = drawn;
}

} // namespace ra2r::render
