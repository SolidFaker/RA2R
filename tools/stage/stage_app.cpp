// RA2R stage — 舞台应用状态与资源/渲染流程实现（接口见 stage_app.h）
#include "stage/stage_app.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "ra2r/assets/anim_frames.h"
#include "ra2r/assets/map_file.h"
#include "ra2r/assets/shp_file.h"
#include "ra2r/assets/shp_layout.h"
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
    if (upal && upal->size() >= 768) {
        a.unit_lut.build(upal->data());
        a.unit_pal_bytes.assign(upal->begin(), upal->end()); // 阵营色 LUT 原料
        a.remap_luts.clear();
    }
    a.static_valid = false; // 调色板/瓦片集变化 → 静态层重建
    return true;
}

// 阵营色重映射 LUT：remap = house_remap 的 1-based 下标（0/越界 = 原盘）。
// 与 object_layer 建筑本体的处理一致（PaletteLut::build 替换 16..31 段）。
const ra2r::render::PaletteLut& remap_lut(StageApp& a, uint8_t remap) {
    if (remap == 0 || a.unit_pal_bytes.size() < 768 ||
        remap > static_cast<int>(a.sk.ramps.size()))
        return a.unit_lut;
    const auto it = a.remap_luts.find(remap);
    if (it != a.remap_luts.end()) return it->second;
    ra2r::render::PaletteLut& l = a.remap_luts[remap];
    l.build(a.unit_pal_bytes.data(), a.sk.ramps[static_cast<size_t>(remap) - 1].rgb[0]);
    return l;
}

// House → 阵营色 ramp（16×3 字节；nullptr = 不重映射。体素炮塔用）
const uint8_t* remap_ramp(StageApp& a, uint8_t remap) {
    if (remap == 0 || remap > static_cast<int>(a.sk.ramps.size())) return nullptr;
    return a.sk.ramps[static_cast<size_t>(remap) - 1].rgb[0];
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

// 地图寻路障碍（引擎格坐标逐格判定）：水面/悬崖/冰 + [Terrain] 树木/岩石 +
// 墙/围栏/沙袋覆盖物（桥面、矿石/宝石、弹坑可通行）。围墙类覆盖物用 art 判定
// （overlay_type_art_name → is_unit_palette_art，含 240-247 段的原版硬编码表），
// BARB/WOOD/FENC/CYCL 等名字兜底。load_map 的 terrain_block 回调直接用它。
std::function<bool(int, int)> make_terrain_block(const StageApp& a,
                                                 const ra2r::assets::MapFile& mf) {
    const char theater_letter = ra2r::render::wall_theater_letter(a.map.theater);
    const std::string ext = ra2r::assets::theater_config(a.map.theater).ext;
    // [Terrain] 对象（树木/岩石）：格 → 阻挡（原版树木/岩石阻挡地面通行）
    std::vector<uint8_t> decor_block(static_cast<size_t>(mf.cell_w()) * mf.cell_h(), 0);
    for (const auto& t : mf.terrain_objects())
        if (t.cx >= 0 && t.cy >= 0 && t.cx < mf.cell_w() && t.cy < mf.cell_h())
            decor_block[static_cast<size_t>(t.cy) * mf.cell_w() + t.cx] = 1;
    return [&a, &mf, decor_block, theater_letter, ext](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= mf.cell_w() || y >= mf.cell_h()) return false;
        if (decor_block[static_cast<size_t>(y) * mf.cell_w() + x]) return true;
        const auto& c = mf.cell(x, y);
        if (!c.present) return false;
        // 覆盖物：墙/围栏/沙袋 → 阻挡通行（桥面/矿石/宝石/弹坑不阻挡）
        const uint8_t ot = mf.overlay_type(c.x, c.y);
        if (ot != 0xFF) {
            const auto it = a.overlay_names.find(ot);
            const std::string name = it != a.overlay_names.end() ? it->second : std::string();
            const std::string art =
                ra2r::render::overlay_type_art_name(ot, name, ext, theater_letter);
            if (ra2r::render::is_unit_palette_art(art)) return true;
            if (name == "BARB" || name == "WOOD" || name == "FENC" || name == "CYCL")
                return true; // 铁丝网/木栅栏等（原版硬编码表外的普通墙）
            if (name.rfind("TROCK", 0) == 0 || name.rfind("ROCK", 0) == 0)
                return true; // 岩石类覆盖物（TROCK01 等：不可跨越）
        }
        const auto* info = a.tileset.set_info(a.tileset.set_index_for(c.tile_id));
        if (!info) return false;
        const std::string& n = info->set_name;
        if (n.find("Bridge") != std::string::npos || n.find("bridge") != std::string::npos)
            return false;
        return n.find("Water") != std::string::npos ||
               n.find("Cliff") != std::string::npos || n.find("Ice") != std::string::npos;
    };
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
            // 地形通过性：水面/悬崖/冰 + 树木/岩石 + 墙/围栏（见 make_terrain_block）
            make_terrain_block(a, mf));
        // 路网统计（自检诊断）：阻挡 = 水面/悬崖/冰 + 树木/岩石 + 墙/围栏 + 建筑地基
        if (a.sim_active) {
            int nav_blocked = 0;
            for (const uint8_t v : a.sim.blocked) nav_blocked += v ? 1 : 0;
            std::printf("[stage] 路网：不可通行格 %d（树木/岩石/墙/围栏/水面/建筑地基）\n",
                        nav_blocked);
        }
        // 配件动画标记（油井摇臂/工厂门/常驻配件/体素炮塔旋转的时钟由 sim
        // 推进；SHP 炮塔静态朝向不推进时钟，跟踪转向归 M5 战斗）
        for (auto& b : a.sim.buildings) {
            if (const auto* u = a.rules.unit(b.type)) {
                if (!u->anim.empty() || !u->anim_two.empty() || !u->anim_three.empty() ||
                    !u->idle_anim.empty() || !u->idle_two.empty() || !u->prod_anim.empty() ||
                    !u->special.empty() ||
                    (u->turret && u->turret_voxel))
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

// 前向声明（定义在下方匿名命名空间：建筑血条，画在本体上方）
namespace {
void draw_building_hp_bars(StageApp& a, const ra2r::render::IsometricGrid& grid, int bw, int bh,
                           int ox, int oy, std::vector<uint8_t>& canvas);
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
    // 选中建筑的**地基菱形描边画在对象之前**：被建筑本体遮挡，只露出底座轮廓
    //（此前画在对象之后，绿色菱形盖住了建筑）
    if (a.sim_active) draw_building_selection_underlay(a, grid, bw, bh, ox, oy, canvas);
    std::vector<ra2r::render::PlacedObject> objs;
    if (a.sim_active) {
        append_sim_objects(a, objs);
    } else {
        objs = a.objects;
    }
    const ra2r::render::UnitPaletteCfg upal{cfg.unit_pal, a.map.theater,
                                            a.sk.ramps.empty() ? nullptr : &a.sk.ramps};
    // 建筑配件动画/炮塔按**该建筑所在深度**紧随本体绘制（render_objects 的
    // post_draw 回调）：靠下的单位/建筑仍能正确遮挡靠上的建筑配件，且动画不再
    // 越过前方单位（此前是全局后画一遍，等于把所有配件动画提到最上层）。
    const ra2r::render::PostObjectDraw post_draw =
        a.sim_active
            ? ra2r::render::PostObjectDraw([&](const ra2r::render::PlacedObject& o,
                                               std::vector<uint8_t>& cv) {
                  if (o.kind != 0) return;
                  for (const auto& b : a.sim.buildings) {
                      if (!b.alive || b.type != o.id || b.col != o.cx || b.row != o.cy) continue;
                      draw_building_anims_for(a, b, grid, bw, bh, ox, oy, cv);
                      break;
                  }
              })
            : ra2r::render::PostObjectDraw{};
    a.obj_stats = ra2r::render::render_objects(objs, upal, grid,
                                               [&](const std::string& n) {
                                                   return load_file(a, n);
                                               },
                                               bw, bh, ox, oy, canvas, a.obj_scale,
                                               &a.obj_cache, post_draw);
    if (a.sim_active) {
        if (!a.selection.empty() || a.sel_building_id != 0)
            draw_selection_markers(a, grid, bw, bh, ox, oy, canvas);
        draw_sim_fx(a, grid, bw, bh, ox, oy, canvas);
        draw_building_hp_bars(a, grid, bw, bh, ox, oy, canvas);
    }
    // 建造落点预览：**逐格**标记——可建格绿色、被占（建筑/覆盖物/矿石/单位/
    // 地形）格红色；任一格红即拒绝放置（place_player_build 再校验一次）
    if (a.placing && a.hover_cx >= 0 && a.hover_cy >= 0) {
        const auto* bt = a.rules.unit(a.sim.build_queue.count("Player")
                                          ? a.sim.build_queue["Player"].type
                                          : std::string());
        const int fw = bt ? bt->fw : 1, fh = bt ? bt->fh : 1;
        std::vector<std::pair<int, int>> cells;
        a.sim.foundation_cells(a.hover_cx, a.hover_cy, fw, fh, cells);
        for (const auto& c : cells) {
            int px, py;
            grid.cell_to_pixel(c.first, c.second, px, py);
            const int cxp = ox + px + grid.tile_w / 2;
            const int cyp = oy + py + grid.tile_h / 2;
            const bool cell_ok = a.sim.cell_buildable(c.first, c.second);
            const uint8_t r = cell_ok ? 60 : 220, g = cell_ok ? 255 : 40, b = 60;
            const auto dot = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= bw || y >= bh) return;
                uint8_t* d = canvas.data() + (static_cast<size_t>(y) * bw + x) * 4;
                d[0] = r;
                d[1] = g;
                d[2] = b;
                d[3] = 255;
            };
            // 菱形描边（每 2px 一点，虚线观感）
            for (int t = -30; t <= 30; t += 2) {
                dot(cxp + t, cyp - 15 + std::abs(t) / 2);
                dot(cxp + t, cyp + 15 - std::abs(t) / 2);
            }
        }
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

std::string country_side_of(StageApp& a, const std::string& country) {
    const auto it = a.sk.country_side.find(country);
    return it != a.sk.country_side.end() ? it->second : std::string();
}

void skirmish_auto_opponent(StageApp& a) {
    if (a.sk.player_countries.empty()) return;
    const std::string ps = country_side_of(a, a.sk.cfg.player.country);
    if (ps.empty()) return;
    // 玩家→对手的默认对应：盟军→苏军、其余→盟军（不同阵营才是“遭遇战”）。
    // 优先经典国家（Russians/Americans），否则该阵营第一个可选国家
    const std::string want_side = ps == "盟军" ? "苏军" : "盟军";
    const std::string classic = want_side == "苏军" ? "Russians" : "Americans";
    if (a.sk.cfg.opponent.country.empty() ||
        country_side_of(a, a.sk.cfg.opponent.country) == ps) {
        a.sk.cfg.opponent.country.clear();
        for (const auto& n : a.sk.player_countries)
            if (n == classic && country_side_of(a, n) == want_side) {
                a.sk.cfg.opponent.country = n;
                break;
            }
        if (a.sk.cfg.opponent.country.empty())
            for (const auto& n : a.sk.player_countries)
                if (country_side_of(a, n) == want_side) {
                    a.sk.cfg.opponent.country = n;
                    break;
                }
    }
    // 对手颜色未显式选择且与玩家解析后相同 → 换成区分色（红↔蓝）
    if (a.sk.cfg.opponent.color.empty()) {
        const auto resolved = [&](const ra2r::sim::SkirmishPlayerCfg& p) -> std::string {
            if (!p.color.empty()) return p.color;
            const auto* c = a.rules.country(p.country);
            return c ? c->color : std::string();
        };
        const std::string pc = resolved(a.sk.cfg.player);
        if (!pc.empty() && resolved(a.sk.cfg.opponent) == pc)
            a.sk.cfg.opponent.color = pc == "DarkRed" ? "DarkBlue" : "DarkRed";
    }
}

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
    // 遭遇战 UI 列表：可选国家（Multiplay=yes）与阵营色（[Colors] 全部）
    a.sk.player_countries.clear();
    a.sk.country_side.clear();
    for (const auto& c : a.rules.countries()) {
        if (!c.multiplay) continue;
        a.sk.player_countries.push_back(c.name);
        std::string side = c.side;
        for (char& ch : side) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        a.sk.country_side[c.name] = side == "GDI"    ? "盟军"
                                    : side == "NOD"  ? "苏军"
                                    : side == "THIRDSIDE" ? "尤里"
                                                          : c.side;
    }
    a.sk.color_names.clear();
    for (const auto& c : a.rules.colors()) a.sk.color_names.push_back(c.name);
    // 默认阵营：玩家 = 列表首项；对手未选时按**不同阵营**配对（此前硬编码
    // Russians，玩家选苏军时两边同阵营同色）。显式同阵营的对手（CLI/UI 指定，
    // 如镜像局）不覆盖——同阵营自动切换只发生在玩家改选国家时
    if (a.sk.cfg.player.country.empty() && !a.sk.player_countries.empty())
        a.sk.cfg.player.country = a.sk.player_countries[0];
    if (a.sk.cfg.opponent.country.empty()) skirmish_auto_opponent(a);
    // 阵营色：未选时用国家节 Color=（国家未定时留空，等开局/配对后再解析）
    const auto init_color = [&](ra2r::sim::SkirmishPlayerCfg& p) {
        if (!p.color.empty() || p.country.empty()) return;
        if (!a.sk.color_names.empty()) {
            const auto* c = a.rules.country(p.country);
            p.color = c ? c->color : a.sk.color_names[0];
        }
    };
    init_color(a.sk.cfg.player);
    init_color(a.sk.cfg.opponent);
}

void append_sim_objects(StageApp& a, std::vector<ra2r::render::PlacedObject>& objects_out) {
    const IsometricGrid grid;
    // 阵营色下标（House → ramps 下标+1；未开局/中立 = 0）
    const auto remap_of = [&](const std::string& owner) -> uint8_t {
        const auto it = a.sk.house_remap.find(owner);
        return it != a.sk.house_remap.end() ? it->second : 0;
    };
    // 建筑：BB 底座平台（矿场卸货台/重工出口台）先于建筑本体入列——
    // 同格按入列序绘制（object_layer 确定性决胜），底座压在建筑之下
    for (const auto& b : a.sim.buildings) {
        if (!b.alive) continue;
        if (b.col < 0 || b.row < 0 || b.col >= a.map.w || b.row >= a.map.h) continue;
        const auto* u = a.rules.unit(b.type);
        const uint8_t rm = remap_of(b.owner);
        if (u && u->bib) {
            const std::string bib_name = u->image + "BB";
            if (load_file(a, bib_name + ".SHP")) {
                ra2r::render::PlacedObject bib{0, bib_name, b.col, b.row, 0, 0,
                                               static_cast<int>(a.map.cell(b.col, b.row).height),
                                               0, 0, static_cast<uint8_t>(255), 256, -1.0f, rm};
                bib.fw = u->fw; // 与本体同深度（同格按入列序：底座先画）
                bib.fh = u->fh;
                objects_out.push_back(bib);
            }
        }
        // 帧语义：idle/damaged-idle/make 由 hp/建造状态驱动（object_layer）；
        // 建造中若有 artmd Buildup= 则播放其独立建造动画（固定 3s 播一遍，
        // 之后停在 make 帧直到工期结束——见 object_layer.cpp）
        ra2r::render::PlacedObject po{};
        po.kind = 0;
        po.id = b.type;
        po.cx = b.col;
        po.cy = b.row;
        po.height = static_cast<int>(a.map.cell(b.col, b.row).height);
        po.alpha = 255;
        po.hp = b.hp;
        po.build_p = b.under_construction
                         ? static_cast<float>(b.build_ticks) /
                               static_cast<float>(std::max(1, b.build_total))
                         : -1.0f;
        po.remap = rm;
        po.build_ticks = b.under_construction ? b.build_ticks : 0;
        po.build_total = b.build_total;
        po.fw = u ? u->fw : 1; // 深度排序取靠下边行（多格建筑遮挡前方/被后方遮挡）
        po.fh = u ? u->fh : 1;
        objects_out.push_back(po);
    }
    for (size_t i = 0; i < a.sim.units.size(); ++i) {
        const ra2r::sim::SimUnit& u = a.sim.units[i];
        if (!u.alive) continue;
        int cx = u.col, cy = u.row;
        if (cx < 0 || cy < 0 || cx >= a.map.w || cy >= a.map.h) continue;
        // 单位屏幕偏移（格内插值 + 转角平滑；与选中标记/目的地连线共用）
        int off_x = 0, off_y = 0;
        ra2r::sim::unit_render_offset(u, off_x, off_y);
        ra2r::render::PlacedObject po{};
        po.kind = u.kind;
        po.id = u.type;
        po.cx = cx;
        po.cy = cy;
        po.dir = static_cast<uint8_t>(u.dir * 32);
        po.height = static_cast<int>(a.map.cell(cx, cy).height);
        po.off_x = off_x;
        po.off_y = off_y;
        po.alpha = 255;
        po.hp = 256;
        po.build_p = -1.0f;
        po.remap = remap_of(u.owner);
        // 步兵行走/idle 动画：moving + 逻辑帧时钟（object_layer 播 Walk/Idle 序列）
        po.moving = (u.kind == 2 && a.sim.unit_moving(i)) ? 1 : 0;
        po.anim_clock = static_cast<uint32_t>(a.sim.logic_ticks);
        po.idle_kind = u.idle_kind;
        po.idle_start = u.idle_start;
        // 子格（步兵 0..2 等腰三角分布）：移动中也保持偏移
        //（否则同格 3 人贴在一起看上去只有 1 个）
        po.subcell = u.subcell;
        objects_out.push_back(po);
    }
}

// 选中建筑的地基菱形描边（**画在对象之前**：被建筑本体遮挡，只露出底座轮廓；
// 旧画序在对象之后，绿色菱形会盖住建筑）
void draw_building_selection_underlay(StageApp& a, const IsometricGrid& grid, int bw, int bh,
                                      int ox, int oy, std::vector<uint8_t>& canvas) {
    if (a.sel_building_id == 0) return;
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
    for (const auto& b : a.sim.buildings) {
        if (b.id != a.sel_building_id || !b.alive) continue;
        const int hgt = static_cast<int>(a.map.cell(b.col, b.row).height);
        for (const auto& c : b.footprint_cells) {
            int px, py;
            grid.cell_to_pixel(c.first, c.second, px, py);
            const int cx0 = ox + px + grid.tile_w / 2;
            const int cy0 = oy + py + grid.tile_h / 2 - hgt * 15;
            line(cx0, cy0 - 15, cx0 + 30, cy0);
            line(cx0 + 30, cy0, cx0, cy0 + 15);
            line(cx0, cy0 + 15, cx0 - 30, cy0);
            line(cx0 - 30, cy0, cx0, cy0 - 15);
        }
        break;
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
        // 单位屏幕位置（格心 + 段内插值/转角平滑偏移）——连线起点用它，
        // 而不是格心（移动中的连线要从单位本体出发）
        int off_x = 0, off_y = 0;
        ra2r::sim::unit_render_offset(u, off_x, off_y);
        // 单位所在格菱形描边（画在格心，保持格栅观感）
        const int cx0 = ox + px + grid.tile_w / 2;
        const int cy0 = oy + py + grid.tile_h / 2 -
                        static_cast<int>(a.map.cell(u.col, u.row).height) * 15;
        line(cx0, cy0 - 15, cx0 + 30, cy0);
        line(cx0 + 30, cy0, cx0, cy0 + 15);
        line(cx0, cy0 + 15, cx0 - 30, cy0);
        line(cx0 - 30, cy0, cx0, cy0 - 15);
        const int ux = cx0 + off_x, uy = cy0 + off_y; // 单位实际屏幕位置
        // 目的地指示：当前位置 → 本次命令分配的目的地格（编队时各人一条）
        if (u.dest_col >= 0 && (u.dest_col != u.col || u.dest_row != u.row) &&
            u.dest_col < a.map.w && u.dest_row < a.map.h) {
            int dx, dy;
            grid.cell_to_pixel(u.dest_col, u.dest_row, dx, dy);
            const int tx = ox + dx + grid.tile_w / 2;
            const int ty = oy + dy + grid.tile_h / 2 -
                           static_cast<int>(a.map.cell(u.dest_col, u.dest_row).height) * 15;
            const auto dot = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                if (x < 0 || y < 0 || x >= bw || y >= bh) return;
                uint8_t* d = canvas.data() + (static_cast<size_t>(y) * bw + x) * 4;
                d[0] = r;
                d[1] = g;
                d[2] = b;
                d[3] = 255;
            };
            const int steps = std::max(std::abs(tx - ux), std::abs(ty - uy)) / 3 + 1;
            for (int s = 1; s < steps; ++s) { // 虚线示意（不遮住路径观感）
                const int x = ux + (tx - ux) * s / steps;
                const int y = uy + (ty - uy) * s / steps;
                dot(x, y, 255, 255, 255);
            }
            dot(tx, ty, 255, 255, 255); // 目的地格心标记
            dot(tx + 1, ty, 255, 255, 255);
            dot(tx, ty + 1, 255, 255, 255);
            dot(tx + 1, ty + 1, 255, 255, 255);
        }
    }
    // 选中建筑的地基描边在 draw_building_selection_underlay（画在对象之前）
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
// NewTheater 美术名回退：第 2 字母换剧场代号 → 'G' 通用 → 原名
// （类型名第 2 字母是雪 'A'，按原名加载会拿到雪地美术）
std::string resolve_art(StageApp& a, const std::string& name, const char* ext) {
    if (name.size() < 2) return name;
    const auto has = [&](const std::string& n) { return load_file(a, n + ext) != nullptr; };
    const char codes[2] = {ra2r::assets::theater_code(a.map.theater), 'G'};
    for (char c : codes) {
        std::string v = name;
        v[1] = c;
        if (v != name && has(v)) return v;
    }
    return name;
}

// 动画 SHP 分段布局（引擎按帧内容自判；按美术名缓存，判定需解码若干帧）
const ra2r::assets::ShpLayout& anim_layout(StageApp& a, const std::string& art,
                                           const ra2r::assets::ShpFile& shp) {
    const auto it = a.obj_cache.anim_layouts.find(art);
    if (it != a.obj_cache.anim_layouts.end()) return it->second;
    return a.obj_cache.anim_layouts.emplace(art, ra2r::assets::shp_layout(shp)).first->second;
}

// 建筑血条：受损建筑常显、选中建筑必显；画在本体精灵上方（顶格锚点 − 本体帧高）
void draw_building_hp_bars(StageApp& a, const ra2r::render::IsometricGrid& grid, int bw, int bh,
                           int ox, int oy, std::vector<uint8_t>& canvas) {
    // grid 用于包围盒高度（Height= × tile_h）
    const auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= bw || y >= bh) return;
        uint8_t* d = canvas.data() + (static_cast<size_t>(y) * bw + x) * 4;
        d[0] = r;
        d[1] = g;
        d[2] = b;
        d[3] = 255;
    };
    // 立体血条（参考图）：沿 (Δx,Δy)=(+2,−1) **向右上**的长条长方体——
    //   顶面 2px（亮）、正面 th px（本色）、底面 2px（暗）、两端封口，
    //   近黑描边一圈，正面上每 10px 一道刻度线；血量从右端起为亮绿/亮色。
    const auto iso_bar = [&](int x0, int y0, int len, int th, int fill) {
        const uint8_t orr = 12, org = 12, orb = 16;  // 描边（近黑）
        const uint8_t efr = 145, efg = 30, efb = 34; // 空槽正面（暗红）
        const uint8_t etr = 195, etg = 70, etb = 74; // 空槽顶面
        const uint8_t ebr = 80, ebg = 14, ebb = 18;  // 空槽底面
        const uint8_t hfr = 70, hfg = 225, hfb = 90; // 血量正面（亮绿）
        const uint8_t htr = 130, htg = 255, htb = 150;// 血量顶面
        const uint8_t hbr = 30, hbg = 120, hbb = 45; // 血量底面
        const uint8_t tkr = 245, tkg = 255, tkb = 245;// 刻度线（近白）
        for (int t = -3; t <= len + 3; ++t) {
            const int cx = x0 + t, cy = y0 - t / 2;
            for (int k = -3; k <= th + 2; ++k) {
                const bool core = t >= 0 && t < len && k >= -2 && k <= th + 1;
                if (core) continue; // 内芯随后按面填色
                put(cx, cy + k, orr, org, orb);
            }
        }
        for (int t = 0; t < len; ++t) {
            const int cx = x0 + t, cy = y0 - t / 2;
            const bool on = t < fill; // 血量自左端起算（与旧表现一致）
            const bool tick = (t % 10) == 0;
            for (int k = -2; k < 0; ++k) // 顶面
                put(cx, cy + k, on ? htr : etr, on ? htg : etg, on ? htb : etb);
            for (int k = 0; k < th; ++k) { // 正面（带刻度）
                if (tick) {
                    put(cx, cy + k, tkr, tkg, tkb);
                } else {
                    put(cx, cy + k, on ? hfr : efr, on ? hfg : efg, on ? hfb : efb);
                }
            }
            put(cx, cy + th, on ? hbr : ebr, on ? hbg : ebg, on ? hbb : ebb);     // 底面
            put(cx, cy + th + 1, on ? hbr : ebr, on ? hbg : ebg, on ? hbb : ebb); // 底面
        }
    };
    // 立方体边棱（仅选中建筑）：围绕**包围盒立方体的顶部三个角**
    // （北/东/西 顶面角）各画三条棱：沿顶面的两条边（方向指向
    // 相邻顶角 = 向立方体**内部**延伸）+ 垂直向下的 z 棱（半格高 15px）。
    // 顶面 = 地基菱形上移建筑高度（锚 → 精灵可见顶边）。
    const auto corner_gizmo = [&](int px, int py, int e1x, int e1y, int e2x, int e2y) {
        const uint8_t lr = 245, lg = 245, lb = 255;
        const auto line = [&](int x1, int y1, int x2, int y2) {
            const int dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
            const int dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
            int err = dx + dy, x = x1, y = y1;
            for (;;) {
                put(x, y, lr, lg, lb);
                if (x == x2 && y == y2) break;
                const int e2 = err * 2;
                if (e2 >= dy) {
                    err += dy;
                    x += sx;
                }
                if (e2 <= dx) {
                    err += dx;
                    y += sy;
                }
            }
        };
        line(px, py, px + e1x, py + e1y); // 顶面边 1（指向相邻顶角）
        line(px, py, px + e2x, py + e2y); // 顶面边 2
        line(px, py, px, py + 15);        // z：向下（进盒子内部）
    };
    for (const auto& b : a.sim.buildings) {
        if (!b.alive || b.under_construction) continue;
        if (b.col < 0 || b.row < 0 || b.col >= a.map.w || b.row >= a.map.h) continue;
        const bool sel = b.id == a.sel_building_id;
        const int ax = ox + b.col * 60 + (b.row & 1) * 30 + 30;
        const int ay = oy + b.row * 15 - static_cast<int>(a.map.cell(b.col, b.row).height) * 15;
        // 本体精灵可见顶边（与 object_layer 建筑 blit 同款几何：锚在精灵底边）
        const auto* ut = a.rules.unit(b.type);
        int top = ay - 70; // 兜底：本体精灵高 ≈60~120px
        if (ut) {
            const std::string art = resolve_art(a, ut->image, ".SHP");
            if (const auto* raw = load_file(a, art + ".SHP")) {
                ra2r::assets::ShpFile shp;
                std::string err;
                if (shp.open(raw->data(), raw->size(), &err) && shp.frame_count() > 0) {
                    const auto& f0 = shp.frame(0);
                    top = ay + f0.y - static_cast<int>(shp.height()) / 2;
                }
            }
        }
        // 包围盒高度：artmd `Height=`（单位：**整格高**），
        // 正交投影下屏幕高度取一半 → 每格 **15px**
        //（kHeightLevelPx，与地形高度级同一压缩比）；
        // 无 Height= 时回退精灵可见高。
        const int box_h = (ut && ut->height_cells > 0)
                              ? ut->height_cells * ra2r::render::kHeightLevelPx
                              : std::max(0, ay - top);
        // 地基四角（北 = 锚格顶顶点；西 = 最左格左顶点；
        // 东 = 最右格右顶点）：三棱线与血条**共用同一套坐标**
        std::vector<std::pair<int, int>> cells;
        a.sim.foundation_cells(b.col, b.row, b.fw, b.fh, cells);
        int west_x = INT_MAX, west_y = 0;
        int east_x = INT_MIN, east_y = 0;
        for (const auto& c : cells) {
            int px, py;
            grid.cell_to_pixel(c.first, c.second, px, py);
            const int x = ox + px, y = oy + py; // 格包围盒左上（60×30 菱形）
            if (x < west_x) {
                west_x = x;
                west_y = y + 15;
            }
            if (x + 60 > east_x) {
                east_x = x + 60;
                east_y = y + 15;
            }
        }
        const int anchor_y = oy + b.row * 15; // 北角基础高（与三棱线一致）
        const int ny = anchor_y - box_h;      // 顶面北角
        const int wy = west_y - box_h;        // 顶面西角
        const int ey = east_y - box_h;        // 顶面东角
        if (sel) {
            corner_gizmo(ax, ny, 30, 15, -30, 15);   // 北顶角 → 东/西顶角
            corner_gizmo(east_x, ey, -30, -15, -30, 15); // 东顶角 → 北/南顶角
            corner_gizmo(west_x, wy, 30, -15, 30, 15);   // 西顶角 → 北/南顶角
        }
        if (b.hp >= b.max_hp && !sel) continue; // 满血且未选中 → 不画血条
        // 血条：长度 = **建筑边长**——血条压在顶面西北棱上，
        // 该棱长 = fh 格 × 一格棱 30px（屏幕 x 跨度）；左端对齐顶面
        // **西顶角**，沿棱线向右上、抬起 8px（2px 避开三棱线）
        const int len = b.fh * 30;
        const int th = 5;
        const int x0 = west_x;
        const int y0 = wy - 8;
        const int fill = std::max(1, b.hp * len / std::max(1, b.max_hp));
        iso_bar(x0, y0, len, th, fill);
    }
}

// 体素节包围盒经 HVA 帧变换（旋转 3×3 + 平移×det）后的 AABB 角点。
// 供建筑炮塔的"包围盒中心"对齐基准用（见 draw_bld_turret_voxel）。
void section_bbox_hva(const ra2r::assets::VxlSection& sec, const ra2r::assets::HvaFile* hva,
                      int frame, float mn[3], float mx[3]) {
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float* m = identity;
    float t[3] = {0, 0, 0};
    if (hva && hva->is_open()) {
        for (uint32_t i = 0; i < hva->section_count(); ++i)
            if (hva->section_names()[i] == sec.name) {
                const float* hm = hva->matrix(static_cast<uint32_t>(frame), i);
                m = hm;
                const float det = sec.det > 0 ? sec.det : 1.0f / 16.0f;
                t[0] = hm[3] * det;
                t[1] = hm[7] * det;
                t[2] = hm[11] * det;
                break;
            }
    }
    mn[0] = mn[1] = mn[2] = 1e9f;
    mx[0] = mx[1] = mx[2] = -1e9f;
    for (int c = 0; c < 8; ++c) {
        const float p[3] = {(c & 1) ? sec.max[0] : sec.min[0], (c & 2) ? sec.max[1] : sec.min[1],
                            (c & 4) ? sec.max[2] : sec.min[2]};
        for (int a = 0; a < 3; ++a) {
            const float v = m[a * 4 + 0] * p[0] + m[a * 4 + 1] * p[1] + m[a * 4 + 2] * p[2] + t[a];
            mn[a] = std::min(mn[a], v);
            mx[a] = std::max(mx[a], v);
        }
    }
}

// 单帧 blit（缓存 + 建筑锚点定位）；ax/ay = 建筑顶格顶顶点像素。
// 阴影帧像素为索引 1（PaletteLut 映射为 ARGB(140,0,0,0)），此处按 alpha 混合。
// remap = 建筑所属 House 的阵营色（0 = 不重映射）。
void blit_bld_shp_frame(StageApp& a, const std::string& art_in, int f, int ax, int ay, int bw,
                        int bh, std::vector<uint8_t>& canvas, uint8_t remap = 0) {
    const std::string art = resolve_art(a, art_in, ".SHP");
    const auto* raw = load_file(a, art + ".SHP");
    if (!raw) return;
    ra2r::assets::ShpFile shp;
    std::string err;
    if (!shp.open(raw->data(), raw->size(), &err) || shp.frame_count() == 0) return;
    if (f < 0 || f >= static_cast<int>(shp.frame_count())) return;
    const std::string key = art + '|' + std::to_string(f) + '|' + std::to_string(remap);
    ra2r::render::ObjectRenderCache::BldEntry ent;
    if (a.obj_cache.shp_frames.count(key)) {
        ent = a.obj_cache.shp_frames[key];
    } else {
        const auto& frame = shp.frame(f);
        std::vector<uint8_t> idx;
        if (!shp.decode_frame(f, idx, &err) || frame.cx == 0 || frame.cy == 0) return;
        const auto& lut = remap_lut(a, remap);
        ent.rgba.assign(static_cast<size_t>(frame.cx) * frame.cy * 4, 0);
        for (size_t p = 0; p < idx.size(); ++p) {
            const uint8_t c = idx[p];
            if (c == 0) continue;
            uint8_t r, g, b, al;
            lut.rgba(c, 0, r, g, b, al);
            uint8_t* d = ent.rgba.data() + p * 4;
            d[0] = r;
            d[1] = g;
            d[2] = b;
            d[3] = al; // 索引 1 → 140（阴影），其余 255
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
            const uint8_t sa = s[x * 4 + 3];
            if (!sa) continue;
            const int px2 = bx + x, py2 = by + y;
            if (px2 < 0 || py2 < 0 || px2 >= bw || py2 >= bh) continue;
            uint8_t* d = canvas.data() + (static_cast<size_t>(py2) * bw + px2) * 4;
            if (sa >= 255) {
                d[0] = s[x * 4 + 0];
                d[1] = s[x * 4 + 1];
                d[2] = s[x * 4 + 2];
                d[3] = 255;
                continue;
            }
            const uint8_t ia = static_cast<uint8_t>(255 - sa);
            d[0] = static_cast<uint8_t>((s[x * 4 + 0] * sa + d[0] * ia) / 255);
            d[1] = static_cast<uint8_t>((s[x * 4 + 1] * sa + d[1] * ia) / 255);
            d[2] = static_cast<uint8_t>((s[x * 4 + 2] * sa + d[2] * ia) / 255);
            d[3] = 255;
        }
    }
}

// 单个配件动画帧绘制：锚点与建筑同语义（画布中心对顶格顶顶点）；
// 配件 SHP 画布与建筑同尺寸（实测 GAWEAP 266x224 = GAWEAP_A = GAWEAPBB，
// GAPOWR 142x110 = GAPOWR_A…），美术已自定位，无需偏移换算。
// **帧序列以 artmd 段元数据为准**（原版动画节的 Start/LoopStart/LoopEnd）：
//   - 循环区间 = [LoopStart, LoopEnd)（**LoopEnd 为开区间上界**，实测：
//     NAYARD_A LoopEnd=15 → 15 帧、GAPOWR_A LoopEnd=8 → 8 帧、GACNST_A
//     LoopEnd=3 → 3 帧；三段受害/受益都与此一致）；
//   - 受损变体是**独立的 artmd 段**（ActiveAnimDamaged=GAPOWR_AD 等，
//     自带 Image=（同文件）与 Start/LoopStart/LoopEnd，如 GAPOWR_A 帧 8..15）；
//     无显式受损段时回退"同文件第 2 段"（四段布局）。
//   - 阴影：文件后半为阴影段（阴影帧号 = 动画帧号 + n/2）；仅当 artmd
//     Shadow=yes（两段文件，如 NACNST_C 6 帧=臂 3+影 3）或 n%4==0（四段，
//     如 GAPOWR_A 32 帧）时成立；NACNST_A（22 帧，无 Shadow=yes）后半是
//     受损灯（与空闲灯一样逐帧解码，非阴影）——按 Shadow 键区分。
// 段名与 SHP 文件名可能不同（[NACNST_CD] Image=NACNST_C；[GAPOWR_AD]→GAPOWR_A）。
void draw_bld_anim_frame(StageApp& a, const std::string& section, int frame_i, bool damaged,
                         int col, int row, int height,
                         const ra2r::render::IsometricGrid& grid, int bw, int bh, int ox,
                         int oy, std::vector<uint8_t>& canvas, uint8_t remap) {
    (void)grid; // 帧位由调用方算好（保留参数以统一绘制函数签名）
    const auto& art_ini = a.rules.art();
    std::string img = art_ini.get(section, "Image", "");
    if (img.empty()) img = section;
    const std::string art = resolve_art(a, img, ".SHP");
    const auto* raw = load_file(a, art + ".SHP");
    if (!raw) return;
    ra2r::assets::ShpFile shp;
    std::string err;
    if (!shp.open(raw->data(), raw->size(), &err) || shp.frame_count() == 0) return;
    const auto& lay = anim_layout(a, art, shp);
    const int n = static_cast<int>(shp.frame_count());
    // 帧序列计划：artmd Start/LoopStart/LoopEnd（LoopEnd 开区间）+ Rate= 帧率
    // + Shadow= 阴影 + 元数据缺失时的分段回退，全部由引擎侧 plan_anim_frames
    // 统一计算（见 engine/src/assets/anim_frames.cpp）
    const auto plan = ra2r::assets::plan_anim_frames(art_ini, section, n, lay, damaged);
    if (plan.count <= 0) return;
    const int fi = plan.first + ((frame_i / plan.step) % plan.count + plan.count) % plan.count;
    const int si = plan.shadow && n / 2 > 0 && fi + n / 2 < n ? fi + n / 2 : -1;
    // 建筑锚点（object_layer 建筑段同款）：顶格顶顶点
    const int ax = ox + col * 60 + (row & 1) * 30 + 30;
    const int ay = oy + row * 15 - height * ra2r::render::kHeightLevelPx;
    if (si >= 0) blit_bld_shp_frame(a, art, si, ax, ay, bw, bh, canvas, remap); // 阴影垫底
    blit_bld_shp_frame(a, art, fi, ax, ay, bw, bh, canvas, remap);
}

// SHP 炮塔（rulesmd Turret=yes + TurretAnim=，如 GAPILLTUR）：面向帧 =
// 建筑朝向 dir · 帧数 / 256；画在建筑之上（锚点同建筑，+TurretAnimX/Y 像素；
// 炮塔 SHP 帧 = 面向帧，无动画/阴影分段）
void draw_bld_turret_shp(StageApp& a, const ra2r::assets::UnitTypeDef& u,
                         const ra2r::sim::SimBuilding& b, int hgt,
                         const ra2r::render::IsometricGrid& grid, int bw, int bh, int ox, int oy,
                         std::vector<uint8_t>& canvas, uint8_t remap) {
    (void)grid; // 锚点直接按格坐标换算（保留参数以统一绘制函数签名）
    const auto* raw = load_file(a, resolve_art(a, u.turret_anim, ".SHP") + ".SHP");
    if (!raw) return;
    ra2r::assets::ShpFile shp;
    std::string err;
    if (!shp.open(raw->data(), raw->size(), &err) || shp.frame_count() == 0) return;
    const int fi = static_cast<int>((b.turret_dir * shp.frame_count()) / 256);
    const int ax = ox + b.col * 60 + (b.row & 1) * 30 + 30;
    const int ay = oy + b.row * 15 - hgt * ra2r::render::kHeightLevelPx;
    blit_bld_shp_frame(a, u.turret_anim, fi, ax, ay, bw, bh, canvas, remap);
}

// 体素炮塔（rulesmd TurretAnimIsVoxel=true，如 YAGGUN.VXL/SAM.VXL）：
// 整模合成（全部 section，跨节深度），yaw = 建筑朝向（与载具同约定），
// HVA 帧按 anim_clock 循环（盖特枪管旋转动画）。
// 放置规则（实测标定，见 docs/DEBUGGING.md §3.17）：
//   炮塔模型原点 = 地基几何中心 + TurretAnimX/Y（勒普顿换算像素）。
//   地基中心相对建筑锚点（顶格顶顶点）= ((fw−fh)·15, (fw+fh)·7.5)——2×2 巨炮
//   (0,30)、1×1 (0,15)，与行奇偶无关；早期用"各格顶顶点均值"在 2×2 时随行
//   奇偶差 30px（偶行 +45、奇行 +15），是巨炮错位主因。
void draw_bld_turret_voxel(StageApp& a, const ra2r::assets::UnitTypeDef& u,
                           const ra2r::sim::SimBuilding& b, int hgt,
                           const ra2r::render::IsometricGrid& grid, int bw, int bh, int ox, int oy,
                           std::vector<uint8_t>& canvas, uint8_t remap) {
    (void)grid; // 锚点直接按格坐标换算（保留参数以统一绘制函数签名）
    // 建筑炮塔体素比例：与原版一致 = ModEnc「RA2 一格 = 42.4264 体素」
    //（1 体素 = 6.03397 勒普顿，256/6.03397 per cell）→ 每体素沿格轴水平步进
    // 60/42.4264 = 1.4142px；本投影 ex=(2s,−s) → s = 30/42.4264 = 0.35355。
    //（勿用 0.5：那会把炮塔放大 41%；MK 建成帧 F1 联合扫描峰值 0.36~0.44 亦印证，
    //  且 GTGCAN 炮塔罩 @0.5=105px 已占满 113px 平台——原版约占 2/3。）
    constexpr float kBldTurretScale = 0.35355f;
    const std::string turret_art = resolve_art(a, u.turret_anim, ".VXL");
    const auto* raw = load_file(a, turret_art + ".VXL");
    if (!raw) return;
    ra2r::assets::VxlFile vxl;
    std::string err;
    if (!vxl.open(raw->data(), raw->size(), &err)) return;
    ra2r::assets::HvaFile hva;
    bool have_hva = false;
    if (const auto* hraw = load_file(a, turret_art + ".HVA"))
        have_hva = hva.open(hraw->data(), hraw->size());
    // 炮管是独立体素模型：原版命名约定 <美术名>BARL.VXL（巨炮 GTGCANBARL、
    // 坦克 GTNKBARL…），与炮塔同坐标系，必须一起做深度合成，否则缺炮管
    // 且与炮塔互相穿插（TurretRecoil=yes 的炮塔都有这一件）。
    const std::string barrel_art = resolve_art(a, u.image + "BARL", ".VXL");
    ra2r::assets::VxlFile bvxl;
    ra2r::assets::HvaFile bhva;
    bool have_barrel = false, have_bhva = false;
    if (const auto* braw = load_file(a, barrel_art + ".VXL")) {
        have_barrel = bvxl.open(braw->data(), braw->size(), &err);
        if (have_barrel)
            if (const auto* bhraw = load_file(a, barrel_art + ".HVA"))
                have_bhva = bhva.open(bhraw->data(), bhraw->size());
    }
    // 对齐基准 = **炮塔 VXL 的包围盒中心 (x,y)**（含 HVA 帧 0 的旋转+平移），
    // 不是模型原点：这些美术的原点常贴在模型一侧（YAGGUN 主体中心在模型
    // x=+12、FLAKTUR +8.5、SAM +1.8），原点即锚会把炮塔整体推右 6~9px；
    // 包围盒中心才是炮塔的旋转中心（旋转时自转不漂移）。z 仍以模型原点
    // （地面）为准——TurretAnimY 的勒普顿/像素差在 y 上体现。
    float bmin[3] = {1e9f, 1e9f, 1e9f}, bmax[3] = {-1e9f, -1e9f, -1e9f};
    for (const auto& sec : vxl.sections()) {
        float mn[3], mx[3];
        section_bbox_hva(sec, have_hva ? &hva : nullptr, 0, mn, mx);
        for (int k = 0; k < 3; ++k) {
            bmin[k] = std::min(bmin[k], mn[k]);
            bmax[k] = std::max(bmax[k], mx[k]);
        }
    }
    const float bcx = (bmin[0] + bmax[0]) * 0.5f;
    const float bcy = (bmin[1] + bmax[1]) * 0.5f;
    // 枪管旋转动画（HVA 多帧，如盖特机炮双管循环）：原版只在**开火时**旋转，
    // 待机恒帧 0（否则炮管持续换位闪动）。开火驱动留 M5 战斗系统接入。
    (void)0;
    const int hva_frame = 0;
    const int bhva_frame = 0;
    const int scale16 = static_cast<int>(kBldTurretScale * 16.0f);
    const std::string vkey = turret_art + '+' + (have_barrel ? barrel_art : std::string("-")) + '|' +
                             std::to_string(b.turret_dir) + '|' + std::to_string(scale16) + '|' +
                             std::to_string(hva_frame) + '|' + std::to_string(bhva_frame) + '|' +
                             std::to_string(remap);
    ra2r::render::ObjectRenderCache::VoxelEntry ent;
    if (!a.obj_cache.voxels.count(vkey)) {
        ra2r::render::VoxelView view;
        view.yaw = b.turret_dir / 256.0f * 6.2831853f - 1.5707963f;
        view.pitch = 0.0f;
        view.scale = kBldTurretScale;
        view.remap = remap_ramp(a, remap); // 阵营色（体素内嵌盘的 Remap 段替换）
        float ax = 0, ay = 0, body_ax = 0, body_ay = 0, orig_ax = 0, orig_ay = 0;
        const ra2r::render::VoxelPart parts[2] = {
            {&vxl, have_hva ? &hva : nullptr, hva_frame},
            {have_barrel ? &bvxl : nullptr, have_bhva ? &bhva : nullptr, bhva_frame}};
        const auto img = ra2r::render::rasterize_voxel_parts(
            parts, have_barrel ? 2u : 1u, view, &ax, &ay, &body_ax, &body_ay, &orig_ax,
            &orig_ay);
        ent.w = img.w;
        ent.h = img.h;
        // 锚点 = 模型原点 (0,0,0)【模型空间】在光栅中的位置（不含任何节 HVA
        // 平移，见 engine voxel_raster.cpp）——rules TurretAnimX/Y 以炮塔枢轴
        // 为参照（OpenRA pxOrigin 同款）。
        ent.ax = static_cast<int>(std::lround(orig_ax));
        ent.ay = static_cast<int>(std::lround(orig_ay));
        (void)body_ax;
        (void)body_ay;
        ent.rgba = img.rgba;
        a.obj_cache.voxels.emplace(vkey, ent);
    } else {
        ent = a.obj_cache.voxels[vkey];
    }
    // 放置（对齐规则 = ModEnc TurretAnimX/Y 语义 + 真值标定，见 DEBUGGING §3.17）：
    //   炮塔 VXL **包围盒中心 (x,y)** 落在「建筑精灵锚点（顶格顶顶点 = 底座
    //   画布中心）+ (TurretAnimX, TurretAnimY) 像素」；z 以模型原点（地面）为准。
    //   偏移单位是**像素**（ModEnc 权威："default ... dead-center (0,0,0)",
    //   positive Y moves downward）；早前按勒普顿+地基中心是误标定。
    //   包围盒中心的屏幕位移（原点→中心）由同一投影换算：
    //   ox = [cx(cy−sy) − cy(sy+cy)]·2s, oy = [cx(cy+sy) + cy(cy−sy)]·s（pitch=0）。
    // ZAdjust 是深度遮挡修正（正=朝观察者），非屏幕位移，本渲染器炮塔后画于
    // 底座之上，无需应用。
    const float yaw = static_cast<float>(b.turret_dir) / 256.0f * 6.2831853f - 1.5707963f;
    const float cyw = std::cos(yaw), syw = std::sin(yaw);
    const float projx = (bcx * (cyw - syw) - bcy * (syw + cyw)) * (2.0f * kBldTurretScale);
    const float projy = (bcx * (cyw + syw) + bcy * (cyw - syw)) * kBldTurretScale;
    const int ax0 = static_cast<int>(std::lround(
        static_cast<float>(ox + b.col * 60 + (b.row & 1) * 30 + 30 + u.turret_x) - projx));
    const int ay0 =
        static_cast<int>(std::lround(static_cast<float>(oy + b.row * 15 + u.turret_y) - projy)) -
        hgt * ra2r::render::kHeightLevelPx;
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

void draw_building_anims_for(StageApp& a, const ra2r::sim::SimBuilding& b,
                             const IsometricGrid& grid, int bw, int bh, int ox, int oy,
                             std::vector<uint8_t>& canvas) {
    {
        if (!b.alive || b.under_construction) return;
        if (b.col < 0 || b.row < 0 || b.col >= a.map.w || b.row >= a.map.h) return;
        const auto* u = a.rules.unit(b.type);
        if (!u) return;
        // 建筑所属阵营色（配件动画/炮塔与本体同一 House ramp；0 = 不重映射）
        const auto rm_it = a.sk.house_remap.find(b.owner);
        const uint8_t remap =
            rm_it != a.sk.house_remap.end() ? rm_it->second : static_cast<uint8_t>(0);
        // 纯炮塔/常驻配件建筑（无 ActiveAnim）不推进时钟，但炮塔/配件仍需绘制
        const bool any_anim = !u->anim.empty() || !u->anim_two.empty() ||
                              !u->anim_three.empty() || !u->special.empty();
        if (!b.has_anim && !u->turret) return;
        const int hgt = static_cast<int>(a.map.cell(b.col, b.row).height);
        const bool dmg = b.hp < 128;
        // 受损/生产变体都是**独立的 artmd 段名**（ActiveAnimDamaged=GAPOWR_AD、
        // IdleAnimDamaged=NACNST_CD、ProductionAnimDamaged=NACNST_BD…）：
        // Image= 指向同一 SHP，Start/LoopStart/LoopEnd 给出该状态的帧区间。
        const auto draw_anim = [&](const std::string& sec, const std::string& sec_dmg, int clock,
                                   bool damaged_state) {
            if (sec.empty() && sec_dmg.empty()) return;
            if (damaged_state && !sec_dmg.empty()) {
                draw_bld_anim_frame(a, sec_dmg, clock, false, b.col, b.row, hgt, grid, bw, bh,
                                    ox, oy, canvas, remap);
                return;
            }
            if (!sec.empty())
                draw_bld_anim_frame(a, sec, clock, damaged_state, b.col, b.row, hgt, grid, bw,
                                    bh, ox, oy, canvas, remap);
        };
        // 生产状态（该 House 队列在建）：生产动画顶替空闲动画——原版建造厂
        // 机械臂 NACNST_B / 战争工厂吊臂只在生产时摆动，空闲时 IdleAnim 是
        // 静止帧（artmd [NACNST_C] LoopStart=0 LoopEnd=1 → 仅帧 0）。
        bool producing = false;
        int prod_ticks = 0;
        if (!u->prod_anim.empty()) {
            const auto qit = a.sim.build_queue.find(b.owner);
            if (qit != a.sim.build_queue.end() && !qit->second.ready && qit->second.total > 0) {
                producing = true;
                prod_ticks = qit->second.ticks;
            }
        }
        // ActiveAnim 系列**画在本体之上**：原版把动画作为建筑的附加层合成，
        // 与本体像素同位覆盖（如建造厂雷达盘 GACNST_A 全部像素落在本体画布内，
        // 画在本体之下会完全不可见——旧实现按 ActiveAnimYSort 非空当作"身后类"
        // 画在建筑之下，导致基地/工厂/电厂的配件动画整体消失）。
        // ActiveAnimYSort 是**相对其它对象的排序偏移**，不是"在本体之后"。
        if (any_anim && !u->anim.empty())
            draw_anim(u->anim, u->anim_dmg, static_cast<int>(b.anim_clock), dmg);
        if (producing)
            draw_anim(u->prod_anim, u->prod_anim_dmg, prod_ticks, dmg);
        else if (!u->idle_anim.empty())
            draw_anim(u->idle_anim, u->idle_anim_dmg, static_cast<int>(b.anim_clock), dmg);
        // 第二常驻装置（YAGRND 双摇臂 YAGRND_A + YAGRND_C）独立于生产状态播放
        if (!u->idle_two.empty())
            draw_anim(u->idle_two, u->idle_two_dmg, static_cast<int>(b.anim_clock), dmg);
        if (!u->anim_two.empty())
            draw_anim(u->anim_two, u->anim_two_dmg, static_cast<int>(b.anim_clock), dmg);
        if (!u->anim_three.empty())
            draw_anim(u->anim_three, u->anim_three_dmg, static_cast<int>(b.anim_clock), dmg);
        // SpecialAnim（光棱塔棱镜充能 GAPRIS_A、磁暴塔放电 NATSLA_B 等）是
        // 动作动画：artmd 的 IsAnimDelayedFire=yes / DelayedFireDelay=28
        // 表明它在开火前播放，待机时不画（M5 战斗按动作播放）。
        if (u->turret) {
            if (u->turret_voxel)
                draw_bld_turret_voxel(a, *u, b, hgt, grid, bw, bh, ox, oy, canvas, remap);
            else
                draw_bld_turret_shp(a, *u, b, hgt, grid, bw, bh, ox, oy, canvas, remap);
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

// ── M4 遭遇战流程 ──

// 落成建筑的收编：注入朝向 + Primary 武器（防御建筑炮塔转向/攻击用），并按
// 与地图装载相同的判据标记动画时钟（ActiveAnim/SpecialAnim/体素炮塔旋转）。
// 新落成的建筑必须调用，否则建造出来的建筑没有任何配件动画（只画静态本体）。
void adopt_building(StageApp& a, uint32_t id, int dir) {
    ensure_rules(a);
    for (auto& b : a.sim.buildings) {
        if (b.id != id) continue;
        const auto* u = a.rules.unit(b.type);
        ra2r::sim::SimWeapon w;
        if (u) {
            if (const auto* wp = a.rules.weapon(u->primary))
                w = {wp->damage, wp->rof, wp->range};
        }
        a.sim.configure_building(id, dir, w, u && u->refinery);
        if (u && (!u->anim.empty() || !u->anim_two.empty() || !u->anim_three.empty() ||
                  !u->idle_anim.empty() || !u->idle_two.empty() || !u->prod_anim.empty() ||
                  !u->special.empty() ||
                  (u->turret && u->turret_voxel)))
            b.has_anim = true;
        return;
    }
}

std::vector<std::pair<int, int>> map_waypoints(const StageApp& a) { return a.map.waypoints; }

// 收起建筑为基地车（rulesmd UndeploysInto=；M4 基地生命周期）：选中建筑 →
// 移除建筑（无爆炸）→ 原地生成基地车并选中。返回是否成功。
bool pack_selected_building(StageApp& a) {
    if (!a.sim_active) return false;
    ensure_rules(a);
    for (size_t i = 0; i < a.sim.buildings.size(); ++i) {
        const auto& b = a.sim.buildings[i];
        if (b.id != a.sel_building_id || !b.alive) continue;
        if (b.under_construction || b.owner != "Player") {
            std::printf("[stage] %s 无法收起（在建/非玩家建筑）\n", b.type.c_str());
            return false;
        }
        const auto* ut = a.rules.unit(b.type);
        if (!ut || ut->undeploys_into.empty()) {
            std::printf("[stage] %s 没有 UndeploysInto=（不可收起）\n", b.type.c_str());
            return false;
        }
        const auto* mcv = a.rules.unit(ut->undeploys_into);
        if (!mcv) {
            std::printf("[stage] %s 的 UndeploysInto=%s 在 rulesmd 无定义\n", b.type.c_str(),
                        ut->undeploys_into.c_str());
            return false;
        }
        ra2r::sim::UnitFactory uf;
        uf.kind_of = [&](const std::string& type) {
            const auto* u = a.rules.unit(type);
            return u ? u->kind : 1;
        };
        uf.weapon = [&](const std::string& type, ra2r::sim::SimWeapon& w) {
            const auto* u = a.rules.unit(type);
            if (!u) return;
            const auto* wp = a.rules.weapon(u->primary);
            if (wp) w = {wp->damage, wp->rof, wp->range};
        };
        uf.miner = [&](const std::string& type, bool& is_miner, int& cap) {
            const auto* u = a.rules.unit(type);
            if (!u) return;
            is_miner = u->harvester;
            cap = u->capacity;
        };
        const std::string btype = b.type;
        const uint32_t uid =
            ra2r::sim::pack_building(a.sim, i, ut->undeploys_into, uf);
        if (!uid) {
            std::printf("[stage] %s 收起失败（无空位）\n", btype.c_str());
            return false;
        }
        a.sel_building_id = 0;
        a.selection.assign(1, uid);
        a.dirty = true;
        std::printf("[stage] %s 收起 → %s id=%u\n", btype.c_str(), ut->undeploys_into.c_str(),
                    uid);
        return true;
    }
    return false;
}

// 现场建造/展开时长（逻辑帧）：原版 [General] BuildupTime = 建筑建造/展开动画
// 运行的平均分钟数（YR rulesmd = .06 → 3.6s = 54 逻辑帧 @15Hz；ModEnc）。
// 无 Buildup 美术 = 原版不进入建造状态（即放即完成），返回 0（调用方按
// under_construction=false 生成）。
int onsite_ticks(StageApp& a, const std::string& type) {
    ensure_rules(a);
    const auto* u = a.rules.unit(type);
    if (!u || u->buildup.empty()) return 0;
    if (!load_file(a, u->buildup + ".SHP")) return 0;
    const double minutes =
        std::atof(a.rules.rules().get("General", "BuildupTime", ".05").c_str());
    const int ticks = static_cast<int>(minutes * 60.0 * 15.0 + 0.5);
    return std::max(1, ticks);
}

std::vector<const ra2r::assets::UnitTypeDef*> buildable_for(StageApp& a,
                                                            const std::string& owner) {
    ensure_rules(a);
    std::string country;
    if (owner == "Player") country = a.sk.cfg.player.country;
    else if (owner == "Opponent") country = a.sk.cfg.opponent.country;
    return ra2r::sim::buildable_buildings(a.sim, a.rules, owner, country, a.sk.tech_level);
}

const ra2r::assets::UnitTypeDef* faction_building(StageApp& a, const std::string& owner,
                                                  const std::string& country, const char* role) {
    for (const auto& [k, name] : a.rules.rules().section("BuildingTypes")) {
        (void)k;
        const auto* t = a.rules.unit(name);
        if (!t || t->kind != 0 || t->owner.empty()) continue;
        bool mine = false;
        for (const auto& o : t->owner)
            if (o == country) {
                mine = true;
                break;
            }
        if (!mine) continue;
        bool match = false;
        if (std::strcmp(role, "conyard") == 0) match = t->construction_yard;
        else if (std::strcmp(role, "power") == 0)
            match = t->power > 0 && t->build_cat == "Power";
        else if (std::strcmp(role, "refinery") == 0) match = t->refinery;
        else if (std::strcmp(role, "barracks") == 0) match = t->factory == "InfantryType";
        else if (std::strcmp(role, "weapon") == 0) match = t->weapons_factory;
        if (!match) continue;
        if (std::strcmp(role, "conyard") != 0 &&
            !ra2r::sim::check_buildable(a.sim, a.rules, owner, country, a.sk.tech_level, *t).ok)
            continue;
        return t;
    }
    return nullptr;
}

bool start_skirmish(StageApp& a, std::string* error) {
    if (a.mode != 2 || a.map_files.empty()) {
        if (error) *error = "遭遇战需要先加载地图";
        return false;
    }
    ensure_rules(a);
    const auto wps = map_waypoints(a);
    if (wps.size() < 2) {
        if (error) *error = "地图缺少至少 2 个 waypoint（出生点）";
        return false;
    }
    // 玩家/对手阵营与阵营色（遭遇战"区分阵营"）
    ra2r::sim::SkirmishCfg& cfg = a.sk.cfg;
    if (cfg.player.country.empty()) cfg.player.country = "Americans";
    if (cfg.opponent.country.empty()) {
        skirmish_auto_opponent(a); // 对手默认取不同阵营（玩家选苏军 → 盟军）
        if (cfg.opponent.country.empty()) cfg.opponent.country = "Russians";
    }
    cfg.player.house = "Player";
    cfg.opponent.house = "Opponent";
    cfg.player.start_class = a.sk.class_sel;
    cfg.credits = a.sk.credits;
    cfg.tech_level = a.sk.tech_level;
    // 阵营色：玩家未选时用国家节的 Color=；对手颜色未显式指定且与玩家相同 →
    // 自动换区分色（否则两边同色无法分辨敌我）
    const bool ocolor_explicit = !cfg.opponent.color.empty();
    const auto resolve_color = [&](ra2r::sim::SkirmishPlayerCfg& p) {
        if (!p.color.empty()) return;
        const auto* c = a.rules.country(p.country);
        p.color = c ? c->color : "DarkBlue";
    };
    resolve_color(cfg.player);
    resolve_color(cfg.opponent);
    if (!ocolor_explicit && cfg.opponent.color == cfg.player.color)
        cfg.opponent.color = cfg.player.color == "DarkBlue" ? "Gold" : "DarkBlue";
    // 阵营色 ramp（[Colors] H,S,V → 16 色）
    a.sk.ramps.clear();
    a.sk.house_remap.clear();
    const auto add_ramp = [&](const ra2r::sim::SkirmishPlayerCfg& p) {
        const auto* c = a.rules.color(p.color);
        ra2r::assets::HouseRamp r;
        if (c) {
            const auto hr = ra2r::sim::house_color_ramp(*c);
            for (int i = 0; i < 16; ++i)
                for (int k = 0; k < 3; ++k) r.rgb[i][k] = hr.rgb[i][k];
        }
        a.sk.ramps.push_back(r);
        a.sk.house_remap[p.house] = static_cast<uint8_t>(a.sk.ramps.size());
    };
    add_ramp(cfg.player);
    add_ramp(cfg.opponent);

    // 重开一局：清空 sim 与建造队列
    a.sim = {};
    a.sim_active = false;
    a.selection.clear();
    a.queue_sel = -1;
    a.placing = false;
    a.sk.active = false;
    // 步兵 idle 动作平均间隔（[General] IdleActionFrequency 分钟 → 逻辑帧 @15Hz）
    {
        const double freq =
            std::atof(a.rules.rules().get("General", "IdleActionFrequency", ".083").c_str());
        a.sim.idle_freq_ticks = static_cast<int>(freq * 60.0 * 15.0 + 0.5);
    }
    // 地图装载（含地形阻挡/矿石），与加载模式同一条路径
    ra2r::assets::MapFile mf;
    if (!mf.open(a.map_files[a.map_sel], error)) return false;
    const auto weapon_of = [&](const std::string& type, ra2r::sim::SimWeapon& w) {
        if (const auto* u = a.rules.unit(type))
            if (const auto* wp = a.rules.weapon(u->primary))
                w = {wp->damage, wp->rof, wp->range};
    };
    const auto miner_of = [&](const std::string& type, bool& is_miner, int& cap) {
        if (const auto* u = a.rules.unit(type)) {
            is_miner = u->harvester;
            cap = u->capacity;
        }
    };
    a.sim.load_map(
        mf,
        [&](const std::string& type, int& fw, int& fh) {
            if (const auto* u = a.rules.unit(type)) {
                fw = u->fw;
                fh = u->fh;
            }
        },
        weapon_of, miner_of,
        [&](const std::string& type) {
            const auto* u = a.rules.unit(type);
            return u && u->refinery;
        },
        [&](const std::string& type) -> int {
            const auto* u = a.rules.unit(type);
            return u ? u->power : 0;
        },
        [&](int x, int y) -> int16_t {
            if (x < 0 || y < 0 || x >= mf.cell_w() || y >= mf.cell_h()) return 0;
            const auto& c = mf.cell(x, y);
            if (!c.present) return 0;
            const auto it = a.overlay_names.find(mf.overlay_type(c.x, c.y));
            if (it == a.overlay_names.end()) return 0;
            const std::string& n = it->second;
            if ((n.rfind("TIB", 0) == 0 && n.rfind("TIBTRE", 0) != 0) ||
                n.rfind("GEM", 0) == 0)
                return 50;
            return 0;
        },
        // 地形通过性：水面/悬崖/冰 + 树木/岩石 + 墙/围栏（见 make_terrain_block）
        make_terrain_block(a, mf));
    // 清掉地图自带对象（遭遇战从零开始）
    a.sim.buildings.clear();
    a.sim.units.clear();
    a.sim.explosions.clear();
    a.sim.credits.clear();
    a.sim.build_queue.clear();
    a.sim.next_id = 1;
    // 单位工厂（rulesmd → SimUnit 属性）
    ra2r::sim::UnitFactory uf;
    uf.kind_of = [&](const std::string& type) {
        const auto* u = a.rules.unit(type);
        return u ? u->kind : 1;
    };
    uf.weapon = weapon_of;
    uf.miner = miner_of;
    // 开局：玩家 waypoint0、对手 waypoint1；出生格按基地车展开后的地基尺寸选择
    // （地图 waypoint 可能落在水面/树丛，原地展不开时自动找最近的空地）
    const auto base_size = [&](const std::string& country) {
        const auto plan = ra2r::sim::start_unit_plan(a.rules, country, cfg.player.start_class);
        int fw = 4, fh = 4;
        if (const auto* mcv = a.rules.unit(plan.base_actor))
            if (const auto* cy = a.rules.unit(mcv->deploys_into)) {
                fw = cy->fw;
                fh = cy->fh;
            }
        return std::pair<int, int>{fw, fh};
    };
    const auto [pfw, pfh] = base_size(cfg.player.country);
    const auto [ofw, ofh] = base_size(cfg.opponent.country);
    const int n0 = ra2r::sim::spawn_start(a.sim, a.rules, "Player", cfg.player.country,
                                          cfg.player.start_class, wps[0].first, wps[0].second, uf,
                                          static_cast<uint32_t>(cfg.seed), pfw, pfh);
    const int n1 = ra2r::sim::spawn_start(a.sim, a.rules, "Opponent", cfg.opponent.country,
                                          cfg.opponent.start_class, wps[1].first, wps[1].second,
                                          uf, static_cast<uint32_t>(cfg.seed) + 7919u, ofw, ofh);
    a.sim.credits["Player"] = cfg.credits;
    a.sim.credits["Opponent"] = cfg.credits;
    a.sk.active = true;
    a.sk.ai_done = false;
    a.sim_active = true;
    a.obj_cache.clear();
    a.last_vhash = 0;
    a.dirty = true;
    std::printf(
        "[stage] 遭遇战开始：玩家 %s/%s class=%d %d 单位 @(%d,%d)；对手 %s/%s %d 单位 "
        "@(%d,%d)\n",
        cfg.player.country.c_str(), cfg.player.color.c_str(), cfg.player.start_class, n0,
        wps[0].first, wps[0].second, cfg.opponent.country.c_str(), cfg.opponent.color.c_str(), n1,
        wps[1].first, wps[1].second);
    return n0 > 0;
}

bool deploy_selected_mcv(StageApp& a) {
    if (!a.sim_active) return false;
    bool any = false;
    for (size_t i = 0; i < a.sim.units.size(); ++i) {
        // 注意：deploy_mcv 内部会 erase 该单位 → 下标与引用失效，先拷出所需字段
        const std::string utype = a.sim.units[i].type;
        const int ucol = a.sim.units[i].col, urow = a.sim.units[i].row;
        const std::string uowner = a.sim.units[i].owner;
        const uint32_t uid = a.sim.units[i].id;
        if (!a.sim.units[i].alive) continue;
        if (std::find(a.selection.begin(), a.selection.end(), uid) == a.selection.end())
            continue;
        const auto* t = a.rules.unit(utype);
        if (!t || t->deploys_into.empty()) continue;
        const auto* bt = a.rules.unit(t->deploys_into);
        if (!bt) continue;
        const std::string btype = t->deploys_into;
        const int ot = onsite_ticks(a, btype); // BuildupTime 逻辑帧；0 = 无动画即完成
        const int udir = static_cast<int>(a.sim.units[i].dir) * 32; // 展开保留车体朝向
        const uint32_t bid = ra2r::sim::deploy_mcv(a.sim, i, btype, bt->fw, bt->fh, bt->cost,
                                                   bt->power, ot > 0 ? ot : 1, bt->strength);
        if (bid) adopt_building(a, bid, udir);
        std::printf("[stage] %s(%s) 展开 → %s @(%d,%d) 动画 %d 逻辑帧 %s\n", utype.c_str(),
                    uowner.c_str(), btype.c_str(), ucol, urow, ot, bid ? "ok" : "fail");
        if (bid) {
            any = true;
            a.selection.clear();
            a.sel_building_id = bid; // 展开后选中建造厂（标准建筑：可建造/修理/出售/收起）
        }
        break; // 展开后 units 下标失效，一帧只处理一台
    }
    a.dirty = true;
    return any;
}

bool queue_player_build(StageApp& a, const std::string& type) {
    const auto* t = a.rules.unit(type);
    if (!t) return false;
    const auto chk = ra2r::sim::check_buildable(a.sim, a.rules, "Player", a.sk.cfg.player.country,
                                                a.sk.tech_level, *t);
    if (!chk.ok) {
        std::printf("[stage] 无法建造 %s：%s\n", type.c_str(), chk.reason.c_str());
        return false;
    }
    // 工期：原版 BuildSpeed=.7 × Cost（M3 近似：cost/2 逻辑帧）
    const int total = std::max(30, t->cost / 2);
    const bool ok = a.sim.queue_build("Player", type, t->cost, total);
    std::printf("[stage] 排队建造 %s $%d 工期 %d 帧 %s\n", type.c_str(), t->cost, total,
                ok ? "ok" : "fail（队列占用/资金不足）");
    a.dirty = true;
    return ok;
}

bool place_player_build(StageApp& a, int col, int row) {
    std::string type;
    if (!a.sim.take_ready_build("Player", &type)) {
        std::printf("[stage] 没有待放置的建筑\n");
        return false;
    }
    const auto* t = a.rules.unit(type);
    if (!t) return false;
    if (!a.sim.can_place(col, row, t->fw, t->fh)) {
        // 位置非法：退回队列（保持就绪）
        a.sim.build_queue["Player"] = {type, 1, 1, t->cost, true};
        std::printf("[stage] %s 无法放置 @(%d,%d)（地基被占）\n", type.c_str(), col, row);
        return false;
    }
    const int ot = onsite_ticks(a, type); // BuildupTime 逻辑帧；0 = 无动画即完成
    const uint32_t id = a.sim.spawn_building("Player", type, col, row, t->fw, t->fh, t->cost,
                                             t->power, ot > 0, ot > 0 ? ot : 1, t->strength);
    if (id) adopt_building(a, id, 0);
    std::printf("[stage] 放置 %s @(%d,%d) id=%u 建造 %d 帧\n", type.c_str(), col, row, id, ot);
    a.placing = false;
    a.queue_sel = -1;
    a.dirty = true;
    return id != 0;
}

void skirmish_ai_tick(StageApp& a) {
    if (!a.sk.active || !a.sk_ai || a.sk.ai_done) return;
    if (a.sim.units.empty() && a.sim.buildings.empty()) return;
    const std::string& oc = a.sk.cfg.opponent.country;
    // 按角色取本阵营建筑（候选必须通过科技树校验，故不会把盟军 GAPOWR 挑给苏军）
    const auto faction = [&](const char* role) {
        return faction_building(a, "Opponent", oc, role);
    };
    // 1) 展开基地车
    if (a.sk.ai_deploy_at > 0) {
        --a.sk.ai_deploy_at;
        if (a.sk.ai_deploy_at == 0) {
            for (size_t i = 0; i < a.sim.units.size(); ++i) {
                // deploy_mcv 会 erase 该单位 → 先拷贝字段（引用/下标随后失效）
                const std::string utype = a.sim.units[i].type;
                const std::string uowner = a.sim.units[i].owner;
                const int ucol = a.sim.units[i].col, urow = a.sim.units[i].row;
                if (uowner != "Opponent") continue;
                const auto* t = a.rules.unit(utype);
                if (!t || t->deploys_into.empty()) continue;
                const auto* bt = a.rules.unit(t->deploys_into);
                if (!bt) break;
                const std::string btype = t->deploys_into;
                const int ot = onsite_ticks(a, btype); // BuildupTime 逻辑帧
                const int udir = static_cast<int>(a.sim.units[i].dir) * 32;
                const uint32_t id = ra2r::sim::deploy_mcv(a.sim, i, btype, bt->fw, bt->fh,
                                                          bt->cost, bt->power,
                                                          ot > 0 ? ot : 1, bt->strength);
                if (id) adopt_building(a, id, udir);
                std::printf("[stage] AI 展开 %s → %s @(%d,%d) id=%u\n", utype.c_str(),
                            btype.c_str(), ucol, urow, id);
                // 展开失败（地基被单位/地形挤住）→ 1s 后重试；出生点已按"可展开"
                // 选择（spawn_start 传基地地基尺寸），重试是移动阻挡等情况的兜底
                if (!id) a.sk.ai_deploy_at = 60;
                a.dirty = true;
                break;
            }
        }
    }
    // 2) 建造脚本：电厂 → 矿场 → 兵营 → 重工（每 240 帧一项，自动就近落点）
    if (a.sk.ai_build_at > 0) {
        --a.sk.ai_build_at;
        if (a.sk.ai_build_at == 0) {
            a.sk.ai_build_at = 240;
            static const char* kRoles[4] = {"power", "refinery", "barracks", "weapon"};
            for (const char* role : kRoles) {
                const auto* wt = faction(role);
                if (!wt) continue;
                if (a.sim.has_building("Opponent", wt->name, false)) continue; // 已有
                const auto chk = ra2r::sim::check_buildable(a.sim, a.rules, "Opponent", oc,
                                                            a.sk.tech_level, *wt);
                if (!chk.ok) continue;
                const auto* cy = faction("conyard");
                if (!cy) continue;
                int cc = -1, cr = -1;
                for (const auto& b : a.sim.buildings)
                    if (b.owner == "Opponent" && b.type == cy->name) {
                        cc = b.col;
                        cr = b.row;
                        break;
                    }
                if (cc < 0) continue;
                int bx = -1, by = -1;
                for (int rad = 0; rad < 24 && bx < 0; ++rad)
                    for (int dy = -rad; dy <= rad && bx < 0; ++dy)
                        for (int dx = -rad; dx <= rad && bx < 0; ++dx) {
                            if (rad > 0 && std::abs(dx) != rad && std::abs(dy) != rad) continue;
                            if (a.sim.can_place(cc + dx, cr + dy, wt->fw, wt->fh)) {
                                bx = cc + dx;
                                by = cr + dy;
                            }
                        }
                if (bx < 0) continue;
                const int ot = onsite_ticks(a, wt->name); // BuildupTime 逻辑帧；0 = 即完成
                const bool ok = a.sim.spawn_building("Opponent", wt->name, bx, by, wt->fw, wt->fh,
                                                     wt->cost, wt->power, ot > 0,
                                                     ot > 0 ? ot : 1, wt->strength);
                if (ok) adopt_building(a, a.sim.buildings.back().id, 0);
                std::printf("[stage] AI 建造 %s @(%d,%d) %s\n", wt->name.c_str(), bx, by,
                            ok ? "ok" : "fail");
                a.dirty = true;
                break; // 每轮一项
            }
        }
    }
}

// BMP 写出（--shot/--test 自检）
bool write_bmp(const std::filesystem::path& path, int w, int h,
               const std::vector<uint8_t>& rgba) {
    const int stride = (w * 3 + 3) & ~3;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto put8 = [&](uint32_t v) { f.put(static_cast<char>(v & 0xFF)); };
    auto w16 = [&](uint16_t v) {
        put8(v & 0xFF);
        put8(v >> 8);
    };
    auto w32 = [&](uint32_t v) {
        put8(v & 0xFF);
        put8((v >> 8) & 0xFF);
        put8((v >> 16) & 0xFF);
        put8((v >> 24) & 0xFF);
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
