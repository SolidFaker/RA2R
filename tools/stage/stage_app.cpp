// RA2R stage — 舞台应用状态与资源/渲染流程实现（接口见 stage_app.h）
#include "stage/stage_app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "ra2r/assets/map_file.h"
#include "ra2r/assets/shp_file.h"
#include "ra2r/assets/theater.h"
#include "ra2r/assets/hva_file.h"
#include "ra2r/assets/vxl_file.h"
#include "ra2r/core/ini_file.h"
#include "ra2r/render/isometric.h"
#include "ra2r/render/map_scene.h"
#include "ra2r/render/overlay_layer.h"
#include "ra2r/render/voxel_raster.h"

namespace stage {

using ra2r::assets::TheaterConfig;
using ra2r::render::IsometricGrid;
using ra2r::render::TerrainTile;

const char* const kTheaters[6] = {"TEMPERATE", "SNOW", "URBAN", "DESERT", "LUNAR", "NEWURBAN"};
const char* const kModeNames[3] = {"算法生成", "完全平坦", "加载地图"};
const char* const kCatNames[3] = {"建筑", "步兵", "载具"};

// 名字 → 条目字节（大小写不敏感，读缓存）
const std::vector<uint8_t>* load_file(StageApp& a, const std::string& name) {
    const auto it = a.file_cache.find(name);
    if (it != a.file_cache.end()) return &it->second;
    std::vector<uint8_t> d;
    if (!a.index.read(name, d)) return nullptr;
    auto [ins, ok] = a.file_cache.emplace(name, std::move(d));
    return &ins->second;
}

// 按剧场重建瓦片集与调色盘
bool rebuild_resources(StageApp& a, const std::string& theater, std::string* error) {
    const TheaterConfig cfg = ra2r::assets::theater_config(theater);
    const auto* ini = load_file(a, cfg.ini);
    if (!ini || !a.tileset.build(ini->data(), ini->size(), cfg.ext, error)) return false;
    const auto* pal = load_file(a, cfg.pal);
    if (!pal || pal->size() < 768) {
        if (error) *error = std::string("palette missing: ") + cfg.pal;
        return false;
    }
    a.terrain_lut.build(pal->data());
    // 资源盘（矿石/宝石）：原版固定用 TEMPERAT.PAL，不随剧场变化
    a.resource_lut = a.terrain_lut;
    const auto* rpal = load_file(a, "TEMPERAT.PAL");
    if (rpal && rpal->size() >= 768) a.resource_lut.build(rpal->data());
    const auto* upal = load_file(a, cfg.unit_pal);
    if (upal && upal->size() >= 768) a.unit_lut.build(upal->data());
    a.static_valid = false; // 调色板/瓦片集变化 → 静态层重建
    return true;
}

// 解析 RULESMD.INI 的类型节（[BuildingTypes] 等 index=名）
void load_type_lists(StageApp& a) {
    const auto* raw = load_file(a, "RULESMD.INI");
    if (!raw) return;
    ra2r::core::IniFile ini;
    std::string err;
    if (!ini.parse(raw->data(), raw->size(), &err)) return;
    const char* sections[3] = {"BuildingTypes", "InfantryTypes", "VehicleTypes"};
    for (int c = 0; c < 3; ++c) {
        a.obj_lists[c].clear();
        for (const auto& [key, value] : ini.section(sections[c])) {
            (void)key;
            if (!value.empty()) a.obj_lists[c].push_back(value);
        }
        // 载具合并飞行器
        if (c == 2) {
            for (const auto& [key, value] : ini.section("AircraftTypes")) {
                (void)key;
                if (!value.empty()) a.obj_lists[2].push_back(value);
            }
        }
        std::sort(a.obj_lists[c].begin(), a.obj_lists[c].end());
    }
    // 覆盖物类型 → 名（RULESMD [OverlayTypes]；装饰层艺术解析已移至共享管线
    // scene_overlay_names，此处保留类型清单供 UI 后续使用）
    a.overlay_names.clear();
    for (const auto& [key, value] : ini.section("OverlayTypes")) {
        const int t = std::atoi(key.c_str());
        if (t > 0 && !value.empty()) a.overlay_names[t] = value;
    }
}

// 生成/加载地图（模式 0=算法 1=平坦 2=加载）
void generate_map(StageApp& a, std::string* error) {
    if (a.mode == 1) {
        a.map.generate_flat(a.map_w, a.map_h);
        a.map.theater = kTheaters[a.theater_sel];
    } else if (a.mode == 2) {
        if (a.map_sel < 0 || a.map_sel >= static_cast<int>(a.map_files.size())) {
            if (error) *error = "请先选择地图";
            return;
        }
        ra2r::assets::MapFile mf;
        if (!mf.open(a.map_files[a.map_sel], error)) return;
        a.map.load(mf, [&](const std::string& n) { return load_file(a, n); });
        // 同步剧场选择并重建瓦片集/调色盘（地图剧场可能与当前不同）
        const std::string t = a.map.theater;
        for (int i = 0; i < 6; ++i) {
            if (t == kTheaters[i]) a.theater_sel = i;
        }
        rebuild_resources(a, a.map.theater, error);
        // 模拟激活时对象层全部由 sim 驱动（建筑/单位，含建造动画与平滑移动）；
        // 无模拟内容时退回静态对象。
        a.objects.clear();
        const auto cell_h = [&](int cx, int cy) {
            if (cx < 0 || cy < 0 || cx >= a.map.w || cy >= a.map.h) return 0;
            return static_cast<int>(a.map.cell(cx, cy).height);
        };
        ensure_rules(a);
        a.sim = {};
        a.sim_active = a.sim.load_map(
            mf,
            [&](const std::string& type, int& fw, int& fh) {
                if (const auto* u = a.rules.unit(type)) {
                    fw = u->fw;
                    fh = u->fh;
                }
            },
            [&](const std::string& type, ra2r::sim::SimWeapon& w) {
                if (const auto* u = a.rules.unit(type)) {
                    if (const auto* wp = a.rules.weapon(u->primary))
                        w = {wp->damage, wp->rof, wp->range};
                }
            },
            [&](const std::string& type, bool& is_miner, int& cap) {
                if (const auto* u = a.rules.unit(type)) {
                    is_miner = u->harvester;
                    cap = u->capacity;
                }
            },
            [&](const std::string& type) {
                const auto* u = a.rules.unit(type);
                return u && u->refinery;
            },
            [&](const std::string& type) -> int {
                const auto* u = a.rules.unit(type);
                return u ? u->power : 0;
            },
            [&](int x, int y) -> int16_t {
                // 矿石格：OverlayPack 类型 → rulesmd OverlayTypes 名 TIB*/GEM*
                //（TIBTRE* 为泰伯利亚树，不是矿）
                if (x < 0 || y < 0 || x >= mf.cell_w() || y >= mf.cell_h()) return 0;
                const auto& c = mf.cell(x, y);
                if (!c.present) return 0;
                const auto it = a.overlay_names.find(mf.overlay_type(c.x, c.y));
                if (it == a.overlay_names.end()) return 0;
                const std::string& n = it->second;
                if ((n.rfind("TIB", 0) == 0 && n.rfind("TIBTRE", 0) != 0) ||
                    n.rfind("GEM", 0) == 0)
                    return 50; // 原版每格 50 单位（M3 基础档，宝石价值待 M4）
                return 0;
            },
            [&](int x, int y) -> bool {
                // 地形通过性：TileSet 分类名含 Water/Cliff/Ice 即不可通行
                //（桥类除外；M3 基础档，MovementZones 精确语义待 M4）
                if (x < 0 || y < 0 || x >= mf.cell_w() || y >= mf.cell_h()) return false;
                const auto& c = mf.cell(x, y);
                if (!c.present) return false;
                const auto* info = a.tileset.set_info(a.tileset.set_index_for(c.tile_id));
                if (!info) return false;
                const std::string& n = info->set_name;
                if (n.find("Bridge") != std::string::npos ||
                    n.find("bridge") != std::string::npos)
                    return false;
                return n.find("Water") != std::string::npos ||
                       n.find("Cliff") != std::string::npos ||
                       n.find("Ice") != std::string::npos;
            });
        // 配件动画标记（油井摇臂/工厂门/常驻配件/体素炮塔旋转的时钟由 sim
        // 推进；SHP 炮塔静态朝向不推进时钟，跟踪转向归 M5 战斗）
        for (auto& b : a.sim.buildings) {
            if (const auto* u = a.rules.unit(b.type)) {
                if (!u->anim.empty() || !u->anim_two.empty() || !u->anim_three.empty() ||
                    !u->special.empty() || (u->turret && u->turret_voxel))
                    b.has_anim = true;
            }
        }
        a.selection.clear();
        if (!a.sim_active) {
            for (const auto& b : mf.buildings())
                a.objects.push_back({0, b.id, b.cx, b.cy, b.dir, 0, cell_h(b.cx, b.cy)});
            for (const auto& u : mf.units())
                a.objects.push_back({1, u.id, u.cx, u.cy, u.dir, 0, cell_h(u.cx, u.cy)});
            for (const auto& n : mf.infantry())
                a.objects.push_back(
                    {2, n.id, n.cx, n.cy, n.dir, n.subcell, cell_h(n.cx, n.cy)});
        }
    } else {
        a.map.generate_algorithm(a.map_w, a.map_h, a.tileset, static_cast<uint32_t>(a.seed));
        a.map.theater = kTheaters[a.theater_sel];
        assign_slopes(a);
    }
    if (a.mode != 2) {
        // 算法/平坦模式无模拟内容：清掉上次加载地图的 sim（防网格失配）
        a.sim = {};
        a.sim_active = false;
        a.selection.clear();
    }
    a.static_valid = false; // 地图变化 → 静态层重建
    a.obj_cache.clear();
    a.last_vhash = 0;
    a.recenter = true;
    a.dirty = true;
}

// ── 高度衔接：斜坡瓦片按 TMP 帧头元数据选择 ──
//
// 实测锚点（23 张官方图聚合 + 帧元数据 dump，docs/formats/tileset.md §ramp）：
//   ramp 码 → 坡向（升侧）：1=SE 2=SW 3=NW 4=NE 5=S 6=W 7=N 8=E
//     （9..12 为悬崖侧 3 级坡变体；边向（N/S/E/W）无 2 级帧，官方图以对角帧代用）
//   坡长档 = 帧菱形上方扩展像素 / 15px 每级：
//     Δh=1 → 扩展 ≤8px（坡面整体在菱形内）；Δh=2 → 9..17；Δh=3 → 18..46；Δh≥4 → ≥47
//   斜坡格 = 低侧格（cell.height 为坡底），坡面美术自低侧格向上爬升覆盖高度差。
namespace {

struct RampFrame {
    uint16_t tile_id = 0xFFFF;
    uint8_t subtile = 0;
};

// 方向（0=N..7=NW，顺时针）→ 候选 ramp 码（含边向缺帧时的对角回退，官方同款做法）
constexpr int kRampByDir[8][3] = {
    {7, 3, 4}, // N
    {4, 1, 0}, // NE
    {8, 1, 4}, // E
    {1, 4, 0}, // SE
    {5, 2, 1}, // S
    {2, 1, 3}, // SW
    {6, 3, 2}, // W
    {3, 2, 4}, // NW
};

int ramp_span_class(int up_ext_px) {
    if (up_ext_px <= 8) return 1;
    if (up_ext_px <= 17) return 2;
    if (up_ext_px <= 46) return 3;
    return 4;
}

// 扫描剧场的 Slope 集合，按 (ramp 码, 坡长档) 索引可用帧
void build_ramp_index(StageApp& a, RampFrame index[16][5]) {
    for (uint16_t t = 0; t < a.tileset.size(); ++t) {
        const auto* info = a.tileset.set_info(a.tileset.set_index_for(t));
        if (!info) continue;
        const std::string& sn = info->set_name;
        const bool slope_set = sn.find("Slope Set") != std::string::npos ||
                               (sn.find("Slope") != std::string::npos &&
                                sn.find("Road") == std::string::npos);
        if (!slope_set) continue;
        const std::string& name = a.tileset.name_for(t);
        if (name.empty()) continue;
        const auto* raw = load_file(a, name);
        if (!raw) continue;
        TerrainTile tile;
        if (!tile.open(raw->data(), raw->size())) continue;
        for (int i = 0; i < tile.frame_count(); ++i) {
            const auto& f = tile.frame(i);
            if (f.bounds_w <= 0 || f.ramp_kind < 1 || f.ramp_kind > 15) continue;
            const int span = ramp_span_class(-f.bounds_y);
            if (span < 1 || span > 4) continue;
            auto& slot = index[f.ramp_kind][span];
            if (slot.tile_id == 0xFFFF) slot = {t, static_cast<uint8_t>(i)};
        }
    }
}

} // namespace

void assign_slopes(StageApp& a) {
    RampFrame index[16][5] = {};
    build_ramp_index(a, index);
    static const int kOff[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1},
                                   {0, 1},  {-1, 1}, {-1, 0}, {-1, -1}};
    StageMap& m = a.map;
    for (int y = 0; y < m.h; ++y) {
        for (int x = 0; x < m.w; ++x) {
            StageCell& c = m.cell(x, y);
            // 坡放在低侧格：找最高的邻居方向与高度差（水域可作坡底 → 岸坡）
            int bestd = -1, dh = 0;
            for (int d = 0; d < 8; ++d) {
                const int nx = x + kOff[d][0], ny = y + kOff[d][1];
                if (nx < 0 || ny < 0 || nx >= m.w || ny >= m.h) continue;
                const int nh = static_cast<int>(m.cell(nx, ny).height);
                if (nh - static_cast<int>(c.height) > dh) {
                    dh = nh - static_cast<int>(c.height);
                    bestd = d;
                }
            }
            if (bestd < 0) continue;
            const int span = std::clamp(dh, 1, 4);
            // 候选码 → 精确档位优先；档位缺失逐步降档（画不满优于错画）
            const RampFrame* pick = nullptr;
            for (int ci = 0; ci < 3 && !pick; ++ci) {
                const int code = kRampByDir[bestd][ci];
                if (!code) continue;
                for (int s = span; s >= 1 && !pick; --s) {
                    const auto& fr = index[code][s];
                    if (fr.tile_id != 0xFFFF) pick = &fr;
                }
            }
            if (pick) {
                c.tile_id = pick->tile_id;
                c.subtile = pick->subtile;
            }
        }
    }
}

// 全图重渲染（地形 + 对象）。静态层（地形+装饰）只在失效时重建；
// 动态对象层每帧从静态层拷贝后叠加，避免逐帧重解码地形。
void render_all(StageApp& a, std::string* error) {
    const auto t0 = std::chrono::steady_clock::now();
    const IsometricGrid grid;
    int bw, bh, ox, oy;
    grid.map_bounds(a.map.w, a.map.h, 15, bw, bh, ox, oy);
    a.bw = bw;
    a.bh = bh;
    a.ox = ox;
    a.oy = oy;
    const TheaterConfig cfg = ra2r::assets::theater_config(a.map.theater);
    // ── 静态层：地形 + 装饰（地图/剧场/瓦片集变化时重建一次）──
    const size_t canvas_bytes = static_cast<size_t>(bw) * bh * 4;
    if (!a.static_valid || a.static_canvas.size() != canvas_bytes) {
        a.static_canvas.assign(canvas_bytes, 0);
        // 共享管线（与 mapview 同一份代码）：场景格 + 地形/装饰一次渲染
        std::vector<ra2r::render::SceneCell> scells(static_cast<size_t>(a.map.w) * a.map.h);
        for (int cy = 0; cy < a.map.h; ++cy)
            for (int cx = 0; cx < a.map.w; ++cx) {
                const StageCell& c = a.map.cell(cx, cy);
                ra2r::render::SceneCell& s = scells[static_cast<size_t>(cy) * a.map.w + cx];
                s.tile_id = c.tile_id;
                s.subtile = c.subtile;
                s.height = c.height;
                s.present = true; // stage 的格全部有效（空槽默认 Clear01）
            }
        int drawn = 0;
        ra2r::render::render_scene(scells, a.map.w, a.map.h, a.tileset, a.terrain_lut,
                                   a.resource_lut, a.unit_lut, a.map.decor, a.no_decor,
                                   [&](const std::string& n) { return load_file(a, n); },
                                   &a.cache, nullptr, grid, bw, bh, ox, oy, a.static_canvas,
                                   &drawn);
        a.static_valid = true;
    }
    const auto t1 = std::chrono::steady_clock::now();
    // ── 动态层：静态层拷贝 + 对象 + 标记/特效 ──
    std::vector<uint8_t> canvas = a.static_canvas; // 一次整图拷贝（56MB 级 ≈ 数毫秒）
    std::vector<ra2r::render::PlacedObject> objs;
    if (a.sim_active) {
        append_sim_objects(a, objs);
    } else {
        objs = a.objects;
    }
    const ra2r::render::UnitPaletteCfg upal{cfg.unit_pal};
    // 身后类配件动画（YSort）画在全部对象之下
    if (a.sim_active) draw_building_anims(a, grid, bw, bh, ox, oy, canvas, true);
    a.obj_stats = ra2r::render::render_objects(objs, upal, grid,
                                               [&](const std::string& n) {
                                                   return load_file(a, n);
                                               },
                                               bw, bh, ox, oy, canvas, a.obj_scale,
                                               &a.obj_cache);
    // 身前类配件动画（门/火焰）画在对象之上
    if (a.sim_active) draw_building_anims(a, grid, bw, bh, ox, oy, canvas, false);
    if (a.sim_active) {
        if (!a.selection.empty()) draw_selection_markers(a, grid, bw, bh, ox, oy, canvas);
        draw_sim_fx(a, grid, bw, bh, ox, oy, canvas);
    }
    const auto t2 = std::chrono::steady_clock::now();
    // 上传纹理（呈现后端抽象：OpenGL / SDLRenderer）
    if (!a.host) {
        if (error) *error = "no backend host";
        return;
    }
    a.host->update_texture(bw, bh, canvas.data());
    const auto t3 = std::chrono::steady_clock::now();
    if (a.perf_log) {
        std::printf("[perf] 静态重建 %.1fms 拷贝+对象 %.1fms 上传 %.1fms 合计 %.1fms\n",
                    std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    std::chrono::duration<double, std::milli>(t2 - t1).count(),
                    std::chrono::duration<double, std::milli>(t3 - t2).count(),
                    std::chrono::duration<double, std::milli>(t3 - t0).count());
    }
}

// ── M3 模拟层粘合 ──

void ensure_rules(StageApp& a) {
    if (a.rules.unit_count() > 0) return;
    const auto* rraw = load_file(a, "RULESMD.INI");
    const auto* araw = load_file(a, "ARTMD.INI");
    if (!rraw || !araw) return;
    std::string err;
    if (!a.rules.load(rraw->data(), rraw->size(), araw->data(), araw->size(), &err)) {
        std::fprintf(stderr, "rules load: %s\n", err.c_str());
        return;
    }
    std::printf("rules: %zu 单位类型 %zu 武器（rulesmd/artmd 全量载入）\n",
                a.rules.unit_count(), a.rules.weapon_count());
}

void append_sim_objects(StageApp& a, std::vector<ra2r::render::PlacedObject>& objects_out) {
    const IsometricGrid grid;
    // 建筑：BB 底座平台（矿场卸货台/重工出口台）先于建筑本体入列——
    // 同格按入列序绘制（object_layer 确定性决胜），底座压在建筑之下
    for (const auto& b : a.sim.buildings) {
        if (!b.alive) continue;
        if (b.col < 0 || b.row < 0 || b.col >= a.map.w || b.row >= a.map.h) continue;
        const auto* u = a.rules.unit(b.type);
        if (u && u->bib) {
            const std::string bib_name = u->image + "BB";
            if (load_file(a, bib_name + ".SHP"))
                objects_out.push_back({0, bib_name, b.col, b.row, 0, 0,
                                       static_cast<int>(a.map.cell(b.col, b.row).height), 0, 0,
                                       static_cast<uint8_t>(255), 256, -1.0f});
        }
        // 帧语义：idle/damaged-idle/make 由 hp/build_p 驱动（object_layer）
        objects_out.push_back({0, b.type, b.col, b.row, 0, 0,
                               static_cast<int>(a.map.cell(b.col, b.row).height), 0, 0,
                               static_cast<uint8_t>(255), b.hp,
                               b.under_construction
                                   ? static_cast<float>(b.build_ticks) /
                                         std::max(1, b.build_total)
                                   : -1.0f});
    }
    for (size_t i = 0; i < a.sim.units.size(); ++i) {
        const ra2r::sim::SimUnit& u = a.sim.units[i];
        int cx = u.col, cy = u.row;
        if (cx < 0 || cy < 0 || cx >= a.map.w || cy >= a.map.h) continue;
        // 段内插值像素偏移 = (终点格中心 − 起点格中心)·frac/256
        int tx, ty, nx, ny;
        grid.cell_to_pixel(u.col, u.row, tx, ty);
        grid.cell_to_pixel(u.next_col, u.next_row, nx, ny);
        const int off_x = ((nx - tx) * u.frac) / ra2r::sim::kFracMax;
        const int off_y = ((ny - ty) * u.frac) / ra2r::sim::kFracMax;
        objects_out.push_back({u.kind, u.type, cx, cy,
                               static_cast<uint8_t>(u.dir * 32), 0,
                               static_cast<int>(a.map.cell(cx, cy).height), off_x, off_y});
    }
}

void draw_selection_markers(StageApp& a, const IsometricGrid& grid, int bw, int bh, int ox,
                            int oy, std::vector<uint8_t>& canvas) {
    // 简单直线写点（绿色描边）
    const auto line = [&](int x0, int y0, int x1, int y1) {
        const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;
        for (;;) {
            if (x0 >= 0 && y0 >= 0 && x0 < bw && y0 < bh) {
                uint8_t* d = canvas.data() + (static_cast<size_t>(y0) * bw + x0) * 4;
                d[0] = 0;
                d[1] = 255;
                d[2] = 80;
                d[3] = 255;
            }
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    };
    for (const uint32_t sid : a.selection) {
        int si = -1;
        for (int i = 0; i < static_cast<int>(a.sim.units.size()); ++i) {
            if (a.sim.units[i].id == sid) {
                si = i;
                break;
            }
        }
        if (si < 0) continue; // 单位已死亡/移除（id 失效）
        const ra2r::sim::SimUnit& u = a.sim.units[si];
        int px, py;
        grid.cell_to_pixel(u.col, u.row, px, py);
        // 单位所在格菱形描边
        const int cx0 = ox + px + grid.tile_w / 2;
        const int cy0 = oy + py + grid.tile_h / 2 -
                        static_cast<int>(a.map.cell(u.col, u.row).height) * 15;
        line(cx0, cy0 - 15, cx0 + 30, cy0);
        line(cx0 + 30, cy0, cx0, cy0 + 15);
        line(cx0, cy0 + 15, cx0 - 30, cy0);
        line(cx0 - 30, cy0, cx0, cy0 - 15);
    }
}

void draw_sim_fx(StageApp& a, const IsometricGrid& grid, int bw, int bh, int ox, int oy,
                 std::vector<uint8_t>& canvas) {
    const auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= bw || y >= bh) return;
        uint8_t* d = canvas.data() + (static_cast<size_t>(y) * bw + x) * 4;
        d[0] = r;
        d[1] = g;
        d[2] = b;
        d[3] = 255;
    };
    // 爆炸：EXPLOMED.SHP 帧序列，画布中心对格中心
    if (!a.sim.explosions.empty()) {
        const auto* raw = load_file(a, "EXPLOMED.SHP");
        if (raw) {
            ra2r::assets::ShpFile shp;
            std::string err;
            if (shp.open(raw->data(), raw->size(), &err) && shp.frame_count() > 0) {
                std::vector<uint8_t> idx;
                for (const auto& e : a.sim.explosions) {
                    const int fi = e.total > 0 ? e.elapsed * shp.frame_count() / e.total : 0;
                    const auto& fr = shp.frame(std::min<uint32_t>(fi, shp.frame_count() - 1));
                    if (fr.cx == 0 || fr.cy == 0 || !shp.decode_frame(fi, idx, &err)) continue;
                    int px, py;
                    grid.cell_to_pixel(e.col, e.row, px, py);
                    const int bx = ox + px + grid.tile_w / 2 - static_cast<int>(shp.width()) / 2;
                    const int by = oy + py + grid.tile_h / 2 - static_cast<int>(shp.height()) / 2;
                    for (int y = 0; y < static_cast<int>(fr.cy); ++y) {
                        const uint8_t* s = idx.data() + static_cast<size_t>(y) * fr.cx;
                        for (int x = 0; x < static_cast<int>(fr.cx); ++x) {
                            const uint8_t c = s[x];
                            if (c == 0) continue;
                            uint8_t r, g, b, al;
                            a.unit_lut.rgba(c, 0, r, g, b, al);
                            if (al) put(bx + x, by + y, r, g, b);
                        }
                    }
                }
            }
        }
    }
    // 血条：受伤单位头顶（绿底红条；宽 24px 对应 256 血）
    for (const auto& u : a.sim.units) {
        if (!u.alive || u.hp >= 256) continue;
        if (u.col < 0 || u.row < 0 || u.col >= a.map.w || u.row >= a.map.h) continue; // 防御：网格失配
        int px, py;
        grid.cell_to_pixel(u.col, u.row, px, py);
        int nx, ny;
        grid.cell_to_pixel(u.next_col, u.next_row, nx, ny);
        const int cx0 = ox + px + grid.tile_w / 2 + ((nx - px) * u.frac) / ra2r::sim::kFracMax;
        const int cy0 = oy + py + grid.tile_h / 2 + ((ny - py) * u.frac) / ra2r::sim::kFracMax -
                        static_cast<int>(a.map.cell(u.col, u.row).height) * 15 - 22;
        for (int i = 0; i < 24; ++i) put(cx0 - 12 + i, cy0, 220, 40, 40);
        const int fill = std::max(1, (u.hp * 24 + 127) / 256);
        for (int i = 0; i < fill; ++i) put(cx0 - 12 + i, cy0, 60, 220, 60);
    }
    // 建造进度条已由 make 帧缩放动画取代（object_layer 建筑段）
}

namespace {
// 动画 SHP 段长（原版布局，artmd 实证）：帧数 = 段长×4 时为四段
// [正常动画][正常阴影][受损动画][受损阴影]；×2 时 [动画][阴影]；其余全帧。
int anim_seg_len(const ra2r::assets::ShpFile& shp) {
    const int n = static_cast<int>(shp.frame_count());
    if (n % 4 == 0) return n / 4;
    if (n % 2 == 0) return n / 2;
    return n;
}

// 单帧 blit（缓存 + 建筑锚点定位）；ax/ay = 建筑顶格顶顶点像素
void blit_bld_shp_frame(StageApp& a, const std::string& art, int f, int ax, int ay, int bw,
                        int bh, std::vector<uint8_t>& canvas) {
    const auto* raw = load_file(a, art + ".SHP");
    if (!raw) return;
    ra2r::assets::ShpFile shp;
    std::string err;
    if (!shp.open(raw->data(), raw->size(), &err) || shp.frame_count() == 0) return;
    if (f < 0 || f >= static_cast<int>(shp.frame_count())) return;
    const std::string key = art + '|' + std::to_string(f);
    ra2r::render::ObjectRenderCache::BldEntry ent;
    if (a.obj_cache.shp_frames.count(key)) {
        ent = a.obj_cache.shp_frames[key];
    } else {
        const auto& frame = shp.frame(f);
        std::vector<uint8_t> idx;
        if (!shp.decode_frame(f, idx, &err) || frame.cx == 0 || frame.cy == 0) return;
        ent.rgba.assign(static_cast<size_t>(frame.cx) * frame.cy * 4, 0);
        for (size_t p = 0; p < idx.size(); ++p) {
            const uint8_t c = idx[p];
            if (c == 0) continue;
            uint8_t r, g, b, al;
            a.unit_lut.rgba(c, 0, r, g, b, al);
            uint8_t* d = ent.rgba.data() + p * 4;
            d[0] = r;
            d[1] = g;
            d[2] = b;
            d[3] = 255;
        }
        ent.cx = frame.cx;
        ent.cy = frame.cy;
        ent.fx = frame.x;
        ent.fy = frame.y;
        ent.canvas_w = shp.width();
        ent.canvas_h = shp.height();
        a.obj_cache.shp_frames.emplace(key, ent);
    }
    const int bx = ax + ent.fx - ent.canvas_w / 2;
    const int by = ay + ent.fy - ent.canvas_h / 2;
    for (int y = 0; y < ent.cy; ++y) {
        const uint8_t* s = ent.rgba.data() + static_cast<size_t>(y) * ent.cx * 4;
        for (int x = 0; x < ent.cx; ++x) {
            if (s[x * 4 + 3]) {
                const int px2 = bx + x, py2 = by + y;
                if (px2 < 0 || py2 < 0 || px2 >= bw || py2 >= bh) continue;
                uint8_t* d = canvas.data() + (static_cast<size_t>(py2) * bw + px2) * 4;
                d[0] = s[x * 4 + 0];
                d[1] = s[x * 4 + 1];
                d[2] = s[x * 4 + 2];
                d[3] = 255;
            }
        }
    }
}

// 单个配件动画帧绘制：锚点与建筑同语义（画布中心对顶格顶顶点）；
// 配件 SHP 画布与建筑同尺寸（实测 GAWEAP 266x224 = GAWEAP_A = GAWEAPBB，
// GAPOWR 142x110 = GAPOWR_A…），美术已自定位，无需偏移换算。
// 帧布局（原版约定，artmd 实证）：四段 [动画][阴影][受损][受损阴影]。
//   frame_i = 段内帧号（时钟 % 段长）；damaged=true 时切到受损段；
//   阴影帧先画（垫底）、精灵帧置顶。
void draw_bld_anim_frame(StageApp& a, const std::string& art, int frame_i, bool damaged,
                         int col, int row, int height,
                         const ra2r::render::IsometricGrid& grid, int bw, int bh, int ox,
                         int oy, std::vector<uint8_t>& canvas) {
    const auto* raw = load_file(a, art + ".SHP");
    if (!raw) return;
    ra2r::assets::ShpFile shp;
    std::string err;
    if (!shp.open(raw->data(), raw->size(), &err) || shp.frame_count() == 0) return;
    const int n = static_cast<int>(shp.frame_count());
    const int len = anim_seg_len(shp);
    const int base = (damaged && n >= 3 * len) ? 2 * len : 0; // 受损段 = 第 3 段
    const int fi = base + frame_i % len;
    // 建筑锚点（object_layer 建筑段同款）：顶格顶顶点
    const int ax = ox + col * 60 + (row & 1) * 30 + 30;
    const int ay = oy + row * 15 - height * ra2r::render::kHeightLevelPx;
    if (n > fi + len) blit_bld_shp_frame(a, art, fi + len, ax, ay, bw, bh, canvas); // 阴影垫底
    blit_bld_shp_frame(a, art, fi, ax, ay, bw, bh, canvas);
}

// SHP 炮塔（rulesmd Turret=yes + TurretAnim=，如 GAPILLTUR）：面向帧 =
// 建筑朝向 dir · 帧数 / 256；画在建筑之上（锚点同建筑，+TurretAnimX/Y 像素；
// 炮塔 SHP 帧 = 面向帧，无动画/阴影分段）
void draw_bld_turret_shp(StageApp& a, const ra2r::assets::UnitTypeDef& u,
                         const ra2r::sim::SimBuilding& b, int hgt,
                         const ra2r::render::IsometricGrid& grid, int bw, int bh, int ox, int oy,
                         std::vector<uint8_t>& canvas) {
    const auto* raw = load_file(a, u.turret_anim + ".SHP");
    if (!raw) return;
    ra2r::assets::ShpFile shp;
    std::string err;
    if (!shp.open(raw->data(), raw->size(), &err) || shp.frame_count() == 0) return;
    const int fi = static_cast<int>((b.dir * shp.frame_count()) / 256);
    const int ax = ox + b.col * 60 + (b.row & 1) * 30 + 30;
    const int ay = oy + b.row * 15 - hgt * ra2r::render::kHeightLevelPx;
    blit_bld_shp_frame(a, u.turret_anim, fi, ax, ay, bw, bh, canvas);
}

// 体素炮塔（rulesmd TurretAnimIsVoxel=true，如 YAGGUN.VXL/SAM.VXL）：
// 整模合成（全部 section，跨节深度），yaw = 建筑朝向（与载具同约定），
// HVA 帧按 anim_clock 循环（盖特枪管旋转动画），锚点 = 建筑顶格顶顶点 +
// TurretAnimX/Y
void draw_bld_turret_voxel(StageApp& a, const ra2r::assets::UnitTypeDef& u,
                           const ra2r::sim::SimBuilding& b, int hgt,
                           const ra2r::render::IsometricGrid& grid, int bw, int bh, int ox, int oy,
                           std::vector<uint8_t>& canvas) {
    const auto* raw = load_file(a, u.turret_anim + ".VXL");
    if (!raw) return;
    ra2r::assets::VxlFile vxl;
    std::string err;
    if (!vxl.open(raw->data(), raw->size(), &err)) return;
    ra2r::assets::HvaFile hva;
    bool have_hva = false;
    if (const auto* hraw = load_file(a, u.turret_anim + ".HVA"))
        have_hva = hva.open(hraw->data(), hraw->size());
    // 枪管旋转：HVA 帧按逻辑帧时钟循环（每 2 帧一步）
    int hva_frame = 0;
    if (have_hva && hva.frame_count() > 1)
        hva_frame = static_cast<int>((b.anim_clock / 2) % hva.frame_count());
    const int scale16 = static_cast<int>(std::clamp(a.obj_scale, 0.1f, 4.0f) * 16.0f);
    const std::string vkey = u.turret_anim + '|' + std::to_string(b.dir) + '|' +
                             std::to_string(scale16) + '|' + std::to_string(hva_frame);
    ra2r::render::ObjectRenderCache::VoxelEntry ent;
    if (!a.obj_cache.voxels.count(vkey)) {
        ra2r::render::VoxelView view;
        view.yaw = b.dir / 256.0f * 6.2831853f - 1.5707963f;
        view.pitch = 0.0f;
        view.scale = std::clamp(a.obj_scale, 0.1f, 4.0f);
        float ax = 0, ay = 0;
        const auto img = ra2r::render::rasterize_voxel_model(
            vxl, have_hva ? &hva : nullptr, hva_frame, view, &ax, &ay);
        ent.w = img.w;
        ent.h = img.h;
        ent.ax = static_cast<int>(ax);
        ent.ay = static_cast<int>(ay);
        ent.rgba = img.rgba;
        a.obj_cache.voxels.emplace(vkey, ent);
    } else {
        ent = a.obj_cache.voxels[vkey];
    }
    const int ax0 = ox + b.col * 60 + (b.row & 1) * 30 + 30 + u.turret_x;
    const int ay0 = oy + b.row * 15 - hgt * ra2r::render::kHeightLevelPx + u.turret_y;
    for (int y = 0; y < ent.h; ++y) {
        const uint8_t* s = ent.rgba.data() + static_cast<size_t>(y) * ent.w * 4;
        for (int x = 0; x < ent.w; ++x) {
            if (s[x * 4 + 3]) {
                const int px2 = ax0 - ent.ax + x, py2 = ay0 - ent.ay + y;
                if (px2 < 0 || py2 < 0 || px2 >= bw || py2 >= bh) continue;
                uint8_t* d = canvas.data() + (static_cast<size_t>(py2) * bw + px2) * 4;
                d[0] = s[x * 4 + 0];
                d[1] = s[x * 4 + 1];
                d[2] = s[x * 4 + 2];
                d[3] = 255;
            }
        }
    }
}
} // namespace

void draw_building_anims(StageApp& a, const IsometricGrid& grid, int bw, int bh, int ox, int oy,
                         std::vector<uint8_t>& canvas, bool behind) {
    for (const auto& b : a.sim.buildings) {
        if (!b.alive || b.under_construction) continue;
        if (b.col < 0 || b.row < 0 || b.col >= a.map.w || b.row >= a.map.h) continue;
        const auto* u = a.rules.unit(b.type);
        if (!u) continue;
        // 纯炮塔/常驻配件建筑（无 ActiveAnim）不推进时钟，但炮塔/配件仍需绘制
        const bool any_anim = !u->anim.empty() || !u->anim_two.empty() ||
                              !u->anim_three.empty() || !u->special.empty();
        if (!b.has_anim && !u->turret) continue;
        const int hgt = static_cast<int>(a.map.cell(b.col, b.row).height);
        const bool dmg = b.hp < 128;
        if (behind) {
            // 身后类（ActiveAnimYSort，如电厂后侧天线）：画在建筑之下
            if (any_anim && !u->anim.empty() && u->anim_ysort) {
                if (dmg && !u->anim_dmg.empty())
                    draw_bld_anim_frame(a, u->anim_dmg, static_cast<int>(b.anim_clock), false,
                                        b.col, b.row, hgt, grid, bw, bh, ox, oy, canvas);
                else
                    draw_bld_anim_frame(a, u->anim, static_cast<int>(b.anim_clock), dmg, b.col,
                                        b.row, hgt, grid, bw, bh, ox, oy, canvas);
            }
        } else {
            // 身前类（工厂门/油井火焰等）+ 常驻配件（SpecialAnim，如光棱塔棱镜，
            // 待机为静态帧，充能/开火帧归 M5 战斗）+ 炮塔
            if (any_anim && !u->anim.empty() && !u->anim_ysort) {
                if (dmg && !u->anim_dmg.empty())
                    draw_bld_anim_frame(a, u->anim_dmg, static_cast<int>(b.anim_clock), false,
                                        b.col, b.row, hgt, grid, bw, bh, ox, oy, canvas);
                else
                    draw_bld_anim_frame(a, u->anim, static_cast<int>(b.anim_clock), dmg, b.col,
                                        b.row, hgt, grid, bw, bh, ox, oy, canvas);
            }
            if (!u->anim_two.empty())
                draw_bld_anim_frame(a, u->anim_two, static_cast<int>(b.anim_clock), dmg, b.col,
                                    b.row, hgt, grid, bw, bh, ox, oy, canvas);
            if (!u->anim_three.empty())
                draw_bld_anim_frame(a, u->anim_three, static_cast<int>(b.anim_clock), dmg, b.col,
                                    b.row, hgt, grid, bw, bh, ox, oy, canvas);
            if (!u->special.empty()) {
                // 待机静态：帧 0（+阴影）；受损切受损变体；充能动画 M5
                if (dmg && !u->special_dmg.empty())
                    draw_bld_anim_frame(a, u->special_dmg, 0, false, b.col, b.row, hgt, grid,
                                        bw, bh, ox, oy, canvas);
                else
                    draw_bld_anim_frame(a, u->special, 0, dmg, b.col, b.row, hgt, grid, bw, bh,
                                        ox, oy, canvas);
            }
            if (u->turret) {
                if (u->turret_voxel)
                    draw_bld_turret_voxel(a, *u, b, hgt, grid, bw, bh, ox, oy, canvas);
                else
                    draw_bld_turret_shp(a, *u, b, hgt, grid, bw, bh, ox, oy, canvas);
            }
        }
    }
}

// 扫描游戏目录的地图文件（.map/.yrm/.yro/.mmx）
void scan_map_files(StageApp& a, const std::filesystem::path& dir) {
    a.map_files.clear();
    std::error_code ec;
    for (auto& de : std::filesystem::directory_iterator(dir, ec)) {
        if (!de.is_regular_file()) continue;
        std::string ext = de.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (ext == ".MAP" || ext == ".YRM" || ext == ".YRO" || ext == ".MMX" ||
            ext == ".MPR") {
            a.map_files.push_back(de.path().string());
        }
    }
    std::sort(a.map_files.begin(), a.map_files.end());
}

// BMP 写出（--shot/--test 自检）
bool write_bmp(const std::filesystem::path& path, int w, int h,
               const std::vector<uint8_t>& rgba) {
    const int stride = (w * 3 + 3) & ~3;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto w16 = [&](uint16_t v) { f.put(v & 0xFF); f.put(v >> 8); };
    auto w32 = [&](uint32_t v) {
        f.put(v & 0xFF);
        f.put((v >> 8) & 0xFF);
        f.put((v >> 16) & 0xFF);
        f.put((v >> 24) & 0xFF);
    };
    w16(0x4D42);
    w32(54 + stride * h);
    w32(0);
    w32(54);
    w32(40);
    w32(w);
    w32(h);
    w16(1);
    w16(24);
    w32(0);
    w32(stride * h);
    w32(2835);
    w32(2835);
    w32(0);
    w32(0);
    std::vector<uint8_t> row(stride, 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
            row[x * 3 + 0] = p[2];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[0];
        }
        f.write(reinterpret_cast<const char*>(row.data()), stride);
    }
    return true;
}

} // namespace stage
