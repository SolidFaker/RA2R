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

namespace {
// 已解码瓦片模板缓存：地形绘制与遮挡补画共用（键 = 文件名，含剧场后缀）。
// 单线程渲染路径 + 确定性：仅作缓存，不影响输出。
std::map<std::string, TerrainTile>& terrain_tile_cache() {
    static std::map<std::string, TerrainTile> cache;
    return cache;
}

// 已转换 RGBA 的瓦片帧缓存（键 = 文件名 + 帧号）：遮挡补画每帧都要重复绘制同一片
// 高地形，不能每次都重新反序列化帧 + 逐像素转换（实测这是卡顿主因：10 对象/帧就
// 要 10ms）。LRU 上限 2048 帧（≈15MB），够放"视野内高地形 + 静态层"工作集。
struct RgbaEntry {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    uint64_t used = 0;
};
std::map<std::string, RgbaEntry>& rgba_frame_cache() {
    static std::map<std::string, RgbaEntry> cache;
    return cache;
}
uint64_t g_rgba_clock = 0;

const std::vector<uint8_t>* cached_rgba(const std::string& name, int frame_i,
                                        const std::vector<uint8_t>& raw, const TerrainTile& t,
                                        const PaletteLut& lut, int level, int& w, int& h) {
    std::string key = name;
    key.push_back('#');
    key += std::to_string(frame_i);
    key.push_back('#');
    key += std::to_string(level);
    RgbaEntry& e = rgba_frame_cache()[key];
    if (!e.rgba.empty()) {
        e.used = ++g_rgba_clock;
        w = e.w;
        h = e.h;
        return &e.rgba;
    }
    const TerrainTileFrame& fr = t.frame(frame_i);
    if (fr.bounds_w <= 0 || fr.bounds_h <= 0) return nullptr;
    e.rgba.assign(static_cast<size_t>(fr.bounds_w) * fr.bounds_h * 4, 0);
    for (size_t p = 0; p < fr.pixels.size(); ++p) {
        const uint8_t idx = fr.pixels[p];
        if (idx == 0) continue;
        uint8_t r, g, b, a;
        lut.rgba(idx, level, r, g, b, a);
        uint8_t* d = e.rgba.data() + p * 4;
        d[0] = r;
        d[1] = g;
        d[2] = b;
        d[3] = 255;
    }
    e.w = fr.bounds_w;
    e.h = fr.bounds_h;
    e.used = ++g_rgba_clock;
    if (rgba_frame_cache().size() > 2048) { // LRU 淘汰：丢最久未用的
        auto victim = rgba_frame_cache().begin();
        for (auto it = rgba_frame_cache().begin(); it != rgba_frame_cache().end(); ++it)
            if (it->second.used < victim->second.used) victim = it;
        rgba_frame_cache().erase(victim);
    }
    w = e.w;
    h = e.h;
    (void)raw;
    return &e.rgba;
}

// 该格是否在高度断崖边（有邻居高度不同）——悬崖面只出现在这种格上。
// 只看高度字节，不解码瓦片，代价 O(8)。
bool cell_on_height_edge(const std::vector<SceneCell>& cells, int cell_w, int cell_h, int c,
                         int r) {
    const size_t i = static_cast<size_t>(r) * cell_w + c;
    if (i >= cells.size()) return false;
    const uint8_t h = cells[i].height;
    static const int kD[8][2] = {{1, 0},  {-1, 0}, {0, 1},  {0, -1},
                                 {1, 1},  {-1, 1}, {1, -1}, {-1, -1}};
    for (const auto& d : kD) {
        const int nc = c + d[0], nr = r + d[1];
        if (nc < 0 || nr < 0 || nc >= cell_w || nr >= cell_h) continue;
        const size_t ni = static_cast<size_t>(nr) * cell_w + nc;
        if (ni < cells.size() && cells[ni].height != h) return true;
    }
    return false;
}

// 该格是否可能遮挡前方对象：
//   ① 有高度抬升（高台/斜坡面本身就会压住后方对象）；
//   ② 有瓦片扩展区（悬崖面）**且位于高度断崖边**——海边/桥面等平面瓦片也带
//      扩展区，只按 has_extra 判定会把普通平面地形画到单位身上（"被平面地形
//      遮挡"的 bug）。平面瓦片的邻居高度相同 → 判为不遮挡。
bool terrain_cell_occludes(const SceneCell& cell, const std::vector<SceneCell>& cells,
                           int cell_w, int cell_h, int c, int r, int obj_height,
                           const assets::TerrainTileset& tileset, const FileLoader& load) {
    // 规则（用户要求）：**只有比对象地面更高的地形**（或与对象同高、但带向上悬崖面
    // 的断崖边格）才允许补画遮挡。同高度及更低的地形绝不遮挡——高台顶面的平台瓦片、
    // 平地、下方坡面都不可能挡住站在其上的单位/建筑。
    if (cell.height < obj_height) return false;
    if (cell.height > obj_height) return true;
    // 同高度：只有"断崖边 + 瓦片带扩展面"才可能是向上抬起的悬崖面
    if (!cell_on_height_edge(cells, cell_w, cell_h, c, r)) return false;
    const std::string& name = tileset.name_for(cell.tile_id);
    if (name.empty()) return false;
    const std::vector<uint8_t>* raw = load(name);
    if (!raw) return false;
    TerrainTile& t = terrain_tile_cache()[name];
    if (!t.is_open()) {
        std::string terr;
        if (!t.open(raw->data(), raw->size(), &terr)) return false;
    }
    const int frame_i = cell.subtile % t.frame_count();
    return t.frame(frame_i).has_extra;
}
} // namespace

bool terrain_tile_is_occluder(const SceneCell& cell, const std::vector<SceneCell>& cells,
                              int cell_w, int cell_h, int c, int r,
                              const assets::TerrainTileset& tileset, const FileLoader& load) {
    // 注意：**格高 > 0 但不是悬崖面**（高台平顶，无扩展面）不算遮挡物——
    // 它们与站在台上的对象同高，参与排序反而会盖住对象（用户的"等高不遮挡"）。
    if (!cell_on_height_edge(cells, cell_w, cell_h, c, r)) return false;
    const std::string& name = tileset.name_for(cell.tile_id);
    if (name.empty()) return false;
    const std::vector<uint8_t>* raw = load(name);
    if (!raw) return false;
    TerrainTile& t = terrain_tile_cache()[name];
    if (!t.is_open()) {
        std::string terr;
        if (!t.open(raw->data(), raw->size(), &terr)) return false;
    }
    const TerrainTileFrame& fr = t.frame(cell.subtile % t.frame_count());
    return fr.has_extra && fr.ramp_kind == 0; // 悬崖面；坡面（ramp!=0）不算遮挡物
}

bool draw_terrain_cell(const SceneCell& cell, int cx, int cy,
                       const assets::TerrainTileset& tileset, const PaletteLut& terrain_lut,
                       const FileLoader& load, cache::CacheManager* cache,
                       const IsometricGrid& grid, int bw, int bh, int ox, int oy, int light_level,
                       std::vector<uint8_t>& canvas) {
    const std::string& name = tileset.name_for(cell.tile_id);
    if (name.empty()) return false;
    const std::vector<uint8_t>* raw = load(name);
    if (!raw) return false;
    TerrainTile& t = terrain_tile_cache()[name];
    if (!t.is_open()) {
        std::string terr;
        if (!t.open(raw->data(), raw->size(), &terr)) return false;
    }
    const int frame_i = cell.subtile % t.frame_count();
    (void)cache; // 帧像素已随模板常驻内存；RGBA 走 rgba_frame_cache（见 cached_rgba）
    int fw = 0, fh = 0;
    const std::vector<uint8_t>* rgba =
        cached_rgba(name, frame_i, *raw, t, terrain_lut, light_level, fw, fh);
    if (!rgba) return false;
    const TerrainTileFrame& fr = t.frame(frame_i);
    // 放置：帧原点 = 格包围盒左上 + bounds 偏移（砖墙几何：菱形中心严格落在格中心）；
    // 高度每级抬 kHeightLevelPx=15px（半瓦高）
    int px, py;
    grid.cell_to_pixel(cx, cy, px, py);
    const int top_x = ox + px + fr.bounds_x;
    const int top_y = oy + py + fr.bounds_y - cell.height * kHeightLevelPx;
    for (int fy = 0; fy < fh; ++fy) {
        const uint8_t* s = rgba->data() + static_cast<size_t>(fy) * fw * 4;
        for (int fx = 0; fx < fw; ++fx) {
            if (!s[fx * 4 + 3]) continue;
            const int x = top_x + fx, y = top_y + fy;
            if (x < 0 || y < 0 || x >= bw || y >= bh) continue;
            uint8_t* d = canvas.data() + (static_cast<size_t>(y) * bw + x) * 4;
            d[0] = s[fx * 4 + 0];
            d[1] = s[fx * 4 + 1];
            d[2] = s[fx * 4 + 2];
            d[3] = 255;
        }
    }
    return true;
}

void redraw_terrain_front(const std::vector<SceneCell>& cells, int cell_w, int cell_h,
                          const assets::TerrainTileset& tileset, const PaletteLut& terrain_lut,
                          const FileLoader& load, cache::CacheManager* cache,
                          const IsometricGrid& grid, int bw, int bh, int ox, int oy,
                          int obj_cx, int obj_cy, int fw, int fh, int obj_height,
                          std::vector<uint8_t>& canvas) {
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    const int front = obj_cy + fh - 1; // 对象前沿行（其后再画的地形压在对象上）
    for (int row = front + 1; row <= front + 6; ++row) {
        if (row < 0 || row >= cell_h) break;
        for (int col = obj_cx - 2; col <= obj_cx + fw + 1; ++col) {
            if (col < 0 || col >= cell_w) continue;
            const size_t i = static_cast<size_t>(row) * cell_w + col;
            if (i >= cells.size() || !cells[i].present) continue;
            const SceneCell& cell = cells[i];
            if (!terrain_cell_occludes(cell, cells, cell_w, cell_h, col, row, obj_height, tileset,
                                       load))
                continue; // 同高度/更低地形不补画（否则会遮住单位/建筑）
            draw_terrain_cell(cell, col, row, tileset, terrain_lut, load, cache, grid, bw, bh, ox,
                              oy, 0, canvas);
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
    // 绘制顺序：砖墙行序（后行盖前行）的画家算法
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
    int drawn = 0;
    for (const auto& [cx, cy] : order) {
        const size_t i = static_cast<size_t>(cy) * cell_w + cx;
        const SceneCell& cell = cells[i];
        const int level = light_fn ? std::clamp((*light_fn)(cx, cy), 0, 24) : 0;
        if (draw_terrain_cell(cell, cx, cy, tileset, terrain_lut, load, cache, grid, bw, bh, ox,
                              oy, level, canvas))
            ++drawn;
    }
    // 装饰层：地形之上、对象之下
    if (!decor.empty() && !no_decor)
        render_map_decor(decor, terrain_lut, resource_lut, unit_lut, grid, load, bw, bh, ox, oy,
                         canvas);
    if (drawn_out) *drawn_out = drawn;
}

} // namespace ra2r::render
