// RA2R stage — 舞台地图模型实现（接口见 stage_map.h）
#include "stage/stage_map.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "ra2r/assets/theater.h"
#include "ra2r/render/map_scene.h"

namespace stage {

void StageMap::set_size(int nw, int nh) {
    w = nw;
    h = nh;
    cells.assign(static_cast<size_t>(w) * h, StageCell{});
    decor.clear();
}

void StageMap::generate_flat(int nw, int nh) {
    set_size(nw, nh);
    // 全部 = 默认瓦片 0（set 0 的第 0 瓦片），高度 0
}

void StageMap::generate_algorithm(int nw, int nh, const ra2r::assets::TerrainTileset& ts,
                                  uint32_t seed) {
    set_size(nw, nh);
    std::mt19937 rng(seed);
    // 找水域瓦片：优先精确匹配 "Water"（避免误中排在前面的 "Water Cliffs"）；
    // 回退：SetName 含 Water 但排除 Cliff/Cave
    uint16_t water_tile = 0xFFFF;
    for (uint16_t t = 0; t < ts.size() && water_tile == 0xFFFF; ++t) {
        const auto* info = ts.set_info(ts.set_index_for(t));
        if (info && info->set_name == "Water") water_tile = t;
    }
    if (water_tile == 0xFFFF) {
        for (uint16_t t = 0; t < ts.size(); ++t) {
            const auto* info = ts.set_info(ts.set_index_for(t));
            if (info && info->set_name.find("Water") != std::string::npos &&
                info->set_name.find("Cliff") == std::string::npos &&
                info->set_name.find("Cave") == std::string::npos) {
                water_tile = t;
                break;
            }
        }
    }
    water_tile_ = water_tile;
    // 高度场：随机团块（高斯衰减峰）
    std::vector<int> hfield(static_cast<size_t>(w) * h, 0);
    const int hills = std::max(4, w * h / 60);
    std::uniform_int_distribution<int> cx_d(0, w - 1), cy_d(0, h - 1);
    std::uniform_int_distribution<int> r_d(3, 9), peak_d(1, 3);
    for (int i = 0; i < hills; ++i) {
        const int hx = cx_d(rng), hy = cy_d(rng), hr = r_d(rng), hp = peak_d(rng);
        for (int dy = -hr; dy <= hr; ++dy) {
            for (int dx = -hr; dx <= hr; ++dx) {
                const int x = hx + dx, y = hy + dy;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (d > static_cast<float>(hr)) continue;
                const float hrf = static_cast<float>(hr);
                const int v = static_cast<int>(static_cast<float>(hp) * (1.0f - d / (hrf + 1.0f)) +
                                               0.5f);
                if (v > 0) {
                    hfield[static_cast<size_t>(y) * w + x] =
                        std::max(hfield[static_cast<size_t>(y) * w + x], v);
                }
            }
        }
    }
    // 湖泊
    const int lakes = std::max(1, w * h / 300);
    std::uniform_int_distribution<int> lr_d(2, 7);
    for (int i = 0; i < lakes; ++i) {
        const int lx = cx_d(rng), ly = cy_d(rng), lr = lr_d(rng);
        for (int dy = -lr; dy <= lr; ++dy) {
            for (int dx = -lr; dx <= lr; ++dx) {
                const int x = lx + dx, y = ly + dy;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                if (std::sqrt(dx * dx + dy * dy) > lr) continue;
                StageCell& c = cells[static_cast<size_t>(y) * w + x];
                if (water_tile != 0xFFFF) c.tile_id = water_tile;
                c.height = 0;
            }
        }
    }
    // 高度写入（保留水域的 0 高度）
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            StageCell& c = cells[static_cast<size_t>(y) * w + x];
            if (c.tile_id == water_tile) continue;
            c.height = static_cast<uint8_t>(hfield[static_cast<size_t>(y) * w + x]);
        }
    }
    // 高度平滑：相邻格高度差收敛到 ≤2（含岸线）。剧场斜坡美术只有 1/2 级档
    // （3 级以上无对角帧，官方图 Δh≥3 用悬崖板手铺）；水面恒 0，只抬低侧。
    for (int pass = 0; pass < 16; ++pass) {
        bool changed = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                StageCell& c = cells[static_cast<size_t>(y) * w + x];
                if (c.tile_id == water_tile) continue;
                static const int kN4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& d : kN4) {
                    const int nx = x + d[0], ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const int nbh = static_cast<int>(cells[static_cast<size_t>(ny) * w + nx].height);
                    if (nbh - static_cast<int>(c.height) > 2) {
                        c.height = static_cast<uint8_t>(nbh - 2);
                        changed = true;
                    }
                }
            }
        }
        if (!changed) break;
    }
    // 高度衔接（斜坡瓦片选择）由 assign_slopes() 在 stage_app 中按 TMP 帧头
    // ramp 元数据完成——方向/坡长映射见 docs/formats/tileset.md。
}

bool StageMap::load(const ra2r::assets::MapFile& map, const ra2r::render::FileLoader& load) {
    w = map.cell_w();
    h = map.cell_h();
    theater = map.theater();
    cells.assign(static_cast<size_t>(w) * h, StageCell{});
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto& c = map.cell(x, y);
            StageCell& s = cells[static_cast<size_t>(y) * w + x];
            if (!c.present || c.tile_id == 0xFFFF) continue;
            s.tile_id = c.tile_id;
            s.height = c.height;
            s.subtile = c.subtile;
        }
    }
    // 装饰列表（树木/岩石 + 覆盖物）：与 mapview 共用共享管线
    decor.clear();
    const ra2r::assets::TheaterConfig cfg = ra2r::assets::theater_config(theater);
    ra2r::render::build_scene_decor(map, cfg, load, false, decor);
    // 出生点（[Waypoints]）：值 = ry·1000 + rx（原版地图空间）→ 引擎格。
    // 公式与 OpenRA ImportRA2MapCommand.ReadWaypoints 一致（W = [Map] Size 宽）。
    waypoints.clear();
    for (const auto& [k, v] : map.ini().section("Waypoints")) {
        (void)k;
        const int pos = std::atoi(v.c_str());
        const int ry = pos / 1000, rx = pos - ry * 1000;
        waypoints.emplace_back((rx - ry + w - 1) / 2, rx + ry - w - 1);
    }
    return true;
}

} // namespace stage
