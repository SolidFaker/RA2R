// RA2R mapview — M2 地图查看器（等距地形渲染）
//
// 用法: mapview <map> [--gamedir <dir>] [--dump <out.bmp>] [--minimap <out.bmp>]
//                    [--light] [--scale N]
//   游戏目录 = --gamedir > 自动发现（注册表/exe 相对路径，见 core/game_dir）>
//             地图同目录 ../Yuri/；双击运行（无参数）自动打开游戏目录第一张地图。
//   --dump 无头渲染全图到 BMP。
//   交互: 左键拖拽平移, 滚轮缩放(0.25..4), ESC 退出。
//   GUI 约束：DPI 感知 + 统一 DPI 缩放（ra2r::ui 引导）。
//
// 渲染管线: MapFile(地形格) → TerrainTileset(tile_id→文件名) → 全 MIX 名称索引
//           → TerrainTile 模板帧(subtile 选帧) → PaletteLut 着色 → 等距放置
// 绘制顺序: 按 (cx+cy, height, cx) 排序画家算法（悬崖/高度重叠正确，
//           此前行主序在悬崖处有瑕疵）。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "ra2r/assets/file_index.h"
#include "ra2r/assets/map_file.h"
#include "ra2r/assets/mix_file.h"
#include "ra2r/assets/name_db.h"
#include "ra2r/assets/shp_file.h"
#include "ra2r/assets/theater.h"
#include "ra2r/assets/tileset.h"
#include "ra2r/assets/vxl_file.h"
#include "ra2r/cache/cache_manager.h"
#include "ra2r/core/game_dir.h"
#include "ra2r/core/ini_file.h"
#include "ra2r/core/win_unicode.h"
#include "ra2r/render/overlay_layer.h"
#include "ra2r/ui/ui.h"

#include "../common/bmp_write.h"
#include "ra2r/render/isometric.h"
#include "ra2r/render/fog.h"
#include "ra2r/render/map_scene.h"
#include "ra2r/render/minimap.h"
#include "ra2r/render/palette_lut.h"
#include "ra2r/render/terrain_tile.h"
#include "ra2r/render/voxel_raster.h"
#include "ra2r/render/object_layer.h"

namespace fs = std::filesystem;
using ra2r::assets::MapCell;
using ra2r::assets::MapFile;
using ra2r::assets::TerrainTileset;
using ra2r::render::IsometricGrid;
using ra2r::render::PaletteLut;
using ra2r::render::TerrainTile;
using ra2r::render::TerrainTileFrame;

namespace {

// ── BMP 输出（-shot 自检用；实现见 tools/common/bmp_write.h）──
bool write_bmp(const std::filesystem::path& path, int w, int h,
               const std::vector<uint8_t>& rgba) {
    return ra2r::tools::write_bmp_rgba(path, w, h, rgba);
}


} // namespace

static int run(int argc, char** argv) {
    // GUI 约束底座：DPI 感知（窗口创建前）+ 控制台 UTF-8
    ra2r::ui::enable_dpi_awareness();
    ra2r::ui::console_utf8();
    const char* map_path = nullptr;
    const char* gamedir = nullptr;
    const char* dump_path = nullptr;
    const char* cells_path = nullptr;
    [[maybe_unused]] bool no_height = false; // --noheight：高度全按 0 渲染（几何诊断）
    bool transpose = false; // --transpose：格坐标转置渲染（A/B 方向对照）
    const char* minimap_path = nullptr;
    const char* showcase_path = nullptr;
    const char* cache_root = "cache";
    bool light = false;
    bool fog = false;
    bool decor_list = false; // --decorlist：逐条打印装饰对象（格坐标/艺术/帧）
    bool no_decor = false; // --nodecor：跳过装饰层（与带装饰渲染对照）
    bool selftest = false; // --selftest：加载+渲染后立即退出（不弹窗，AI 调用用）
    int fog_cx = -1, fog_cy = -1, fog_r = 10;
    int scale = 2;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gamedir") == 0 && i + 1 < argc) gamedir = argv[++i];
        else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) dump_path = argv[++i];
        else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            if (!map_path) map_path = argv[++i];
        }
        else if (std::strcmp(argv[i], "--cells") == 0 && i + 1 < argc) cells_path = argv[++i];
        else if (std::strcmp(argv[i], "--decorlist") == 0) decor_list = true;
        else if (std::strcmp(argv[i], "--nodecor") == 0) no_decor = true;
        else if (std::strcmp(argv[i], "--selftest") == 0) selftest = true;
        else if (std::strcmp(argv[i], "--noheight") == 0) no_height = true;
        else if (std::strcmp(argv[i], "--transpose") == 0) transpose = true;
        else if (std::strcmp(argv[i], "--minimap") == 0 && i + 1 < argc)
            minimap_path = argv[++i];
        else if (std::strcmp(argv[i], "--showcase") == 0 && i + 1 < argc)
            showcase_path = argv[++i];
        else if (std::strcmp(argv[i], "--cache") == 0 && i + 1 < argc) cache_root = argv[++i];
        else if (std::strcmp(argv[i], "--light") == 0) light = true;
        else if (std::strcmp(argv[i], "--fog") == 0) fog = true;
        else if (std::strcmp(argv[i], "--fog-demo") == 0 && i + 3 < argc) {
            fog = true;
            fog_cx = std::atoi(argv[++i]);
            fog_cy = std::atoi(argv[++i]);
            fog_r = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            scale = std::atoi(argv[++i]);
        else if (!map_path) map_path = argv[i];
    }
    // ── 游戏目录：参数 > 自动发现 > 地图同目录 ../Yuri（保持旧行为）──
    const std::string found_dir = ra2r::core::find_game_dir();
    const fs::path game_dir =
        gamedir ? fs::path(gamedir)
                : (!found_dir.empty()
                       ? fs::path(found_dir)
                       : (map_path ? fs::path(map_path).parent_path() / ".." / "Yuri"
                                   : fs::path()));

    // ── 双击运行（无地图参数）：自动打开游戏目录第一张地图 ──
    std::string first_map;
    if (!map_path && !showcase_path) {
        std::error_code ec;
        std::vector<std::string> found;
        if (!game_dir.empty() && std::filesystem::is_directory(game_dir, ec)) {
            for (auto& de : std::filesystem::directory_iterator(game_dir, ec)) {
                if (!de.is_regular_file()) continue;
                std::string ext = de.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::toupper(c));
                               });
                if (ext == ".MAP" || ext == ".YRM" || ext == ".YRO" || ext == ".MMX" ||
                    ext == ".MPR")
                    found.push_back(de.path().string());
            }
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) first_map = found.front();
        if (first_map.empty()) {
            std::printf("usage: mapview <map> [--gamedir <dir>] [--dump <out.bmp>] "
                        "[--minimap <out.bmp>] [--showcase <out.bmp>] [--cache <root>] "
                        "[--light] [--scale N]\n"
                        "  双击运行时自动打开游戏目录第一张地图。\n");
            return 1;
        }
        map_path = first_map.c_str();
        std::printf("双击运行: 自动打开 %s\n", map_path);
    }

    ra2r::assets::FileIndex index;
    std::string err;
    if (!index.build(game_dir, &err)) {
        std::fprintf(stderr, "file index build failed: %s\n", err.c_str());
        return 1;
    }
    // 条目读取缓存（瓦片/对象文件复用）
    std::map<std::string, std::vector<uint8_t>> file_cache;
    auto load_file = [&](const std::string& name) -> const std::vector<uint8_t>* {
        const auto it = file_cache.find(name);
        if (it != file_cache.end()) return &it->second;
        std::vector<uint8_t> d;
        if (!index.read(name, d)) return nullptr;
        auto [ins, ok] = file_cache.emplace(name, std::move(d));
        return &ins->second;
    };

    // ── 全单位陈列场景（验收：--showcase，独立于地图）──
    if (showcase_path) {
        std::vector<std::string> vxls;
        for (const auto& [name, loc] : index.by_name) {
            (void)loc;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".VXL") == 0)
                vxls.push_back(name);
        }
        std::sort(vxls.begin(), vxls.end());
        const auto res = ra2r::render::run_showcase(vxls, load_file, 2);
        if (!write_bmp(showcase_path, res.w, res.h, res.rgba)) {
            std::fprintf(stderr, "showcase write failed\n");
            return 1;
        }
        std::printf("showcase: %d/%zu units -> %s (%dx%d)\n", res.count, vxls.size(),
                    showcase_path, res.w, res.h);
        return 0;
    }

    // ── 加载地图 ──
    MapFile map;
    if (!map.open(map_path, &err)) {
        std::fprintf(stderr, "map open failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("map: %dx%d theater=%s present=%d\n", map.cell_w(), map.cell_h(),
                map.theater().c_str(), map.present_count());

    const ra2r::assets::TheaterConfig cfg = ra2r::assets::theater_config(map.theater());
    std::printf("theater: ext=%s ini=%s pal=%s gamedir=%s\n", cfg.ext, cfg.ini, cfg.pal,
                game_dir.string().c_str());

    // ── 剧场资源：瓦片集 + 调色盘 LUT ──
    TerrainTileset tileset;
    {
        const auto* ini = load_file(cfg.ini);
        if (!ini || !tileset.build(ini->data(), ini->size(), cfg.ext, &err)) {
            std::fprintf(stderr, "tileset build failed (%s): %s\n", cfg.ini, err.c_str());
            return 1;
        }
    }
    PaletteLut lut;
    {
        const auto* pal = load_file(cfg.pal);
        if (!pal || pal->size() < 768) {
            std::fprintf(stderr, "palette load failed: %s\n", cfg.pal);
            return 1;
        }
        lut.build(pal->data());
    }
    // 资源盘（矿石/宝石）：原版固定用 TEMPERAT.PAL，不随剧场变化
    PaletteLut resource_lut = lut;
    {
        const auto* pal = load_file("TEMPERAT.PAL");
        if (pal && pal->size() >= 768) resource_lut.build(pal->data());
    }
    // 单位盘（墙/围栏 SHP 覆盖物用）
    PaletteLut unit_lut = lut;
    {
        const auto* pal = load_file(cfg.unit_pal);
        if (pal && pal->size() >= 768) unit_lut.build(pal->data());
    }
    std::printf("tileset: %zu 瓦片\n", tileset.size());

    // ── 渲染全图 ──
    const IsometricGrid grid;
    int bw, bh, ox, oy;
    grid.map_bounds(transpose ? map.cell_h() : map.cell_w(),
                    transpose ? map.cell_w() : map.cell_h(), 15, bw, bh, ox, oy);
    std::vector<uint8_t> canvas(static_cast<size_t>(bw) * bh * 4, 0);
    // 光照等级（--light）：离地图中心越远越暗（演示 PaletteLut 32 级管线）
    const float light_r = light
                              ? static_cast<float>(std::max(map.cell_w(), map.cell_h())) * 0.5f
                              : 0.0f;
    // 战争迷雾/黑幕（--fog：地图 [Shroud] 或合成演示）
    ra2r::render::ShroudMap shroud_map(ra2r::render::ShroudMap::from_map(map));
    if (fog) {
        if (fog_cx >= 0) {
            shroud_map = ra2r::render::ShroudMap::demo(map.cell_w(), map.cell_h(), fog_cx,
                                                       fog_cy, fog_r);
        }
        // 地图无 [Shroud] 且未指定演示圆心 → 中心演示
        else if (map.shroud().size() == static_cast<size_t>(map.cell_w()) * map.cell_h() &&
                 std::all_of(map.shroud().begin(), map.shroud().end(),
                             [](uint8_t v) { return v == 1; })) {
            shroud_map = ra2r::render::ShroudMap::demo(map.cell_w(), map.cell_h(),
                                                       map.cell_w() / 2, map.cell_h() / 2, 12);
        }
    }
    // 规范缓存（读穿式：命中直接取解码帧；未命中解码后回写）
    ra2r::cache::CacheManager tile_cache2;
    std::string cache_err;
    tile_cache2.open(cache_root, &cache_err);
    // ── 共享管线：场景格 + 装饰列表 + 一次渲染（与 stage 同一份代码）──
    const auto scells = ra2r::render::scene_cells(map, transpose);
    std::vector<ra2r::render::MapDecorObject> decor;
    ra2r::render::build_scene_decor(map, cfg, load_file, transpose, decor);
    const size_t terrain_count = map.terrain_objects().size();
    const std::function<int(int, int)> light_fn = [&](int cx, int cy) -> int {
        if (fog) return ra2r::render::ShroudMap::to_light(shroud_map.state(cx, cy));
        if (!light) return 0;
        const float dx = static_cast<float>(cx) - static_cast<float>(map.cell_w()) * 0.5f;
        const float dy = static_cast<float>(cy) - static_cast<float>(map.cell_h()) * 0.5f;
        const float dist = std::sqrt(dx * dx + dy * dy);
        return std::clamp(static_cast<int>(dist / light_r * 24.0f), 0, 24);
    };
    int drawn = 0;
    ra2r::render::render_scene(scells, transpose ? map.cell_h() : map.cell_w(),
                               transpose ? map.cell_w() : map.cell_h(), tileset, lut,
                               resource_lut, unit_lut, decor, no_decor, load_file, &tile_cache2,
                               (fog || light) ? &light_fn : nullptr, grid, bw, bh, ox, oy,
                               canvas, &drawn);
    std::printf("decor: %zu objects (%zu terrain + %zu overlay)\n", decor.size(), terrain_count,
                decor.size() - terrain_count);
        if (decor_list) {
            // 墙艺术排查：索引中所有含 WALL/FNCP/FNCB/KRM/SAND/CYCL/BARB/FENC/WOOD 的条目
            for (const auto& [n, loc] : index.by_name) {
                (void)loc;
                bool hit = false;
                for (const char* k : {"WALL", "FNCP", "FNCB", "KRM", "SAND", "CYCL", "BARB",
                                      "FENC", "WOOD", "YAWALL"}) {
                    if (n.find(k) != std::string::npos) {
                        hit = true;
                        break;
                    }
                }
                if (!hit) continue;
                const std::vector<uint8_t>* d = load_file(n);
                std::printf("wallfile: %s (%zu bytes)\n", n.c_str(), d ? d->size() : 0);
            }
            for (const auto& d : decor)
                std::printf("decor cell=(%d,%d) art=%s frame=%d load=%s\n", d.cx, d.cy,
                            d.art.c_str(), d.frame_i, load_file(d.art) ? "ok" : "FAIL");
            // 覆盖物原始数组诊断：线性索引 → 格栅坐标 → 是否存在 present 格
            std::map<uint64_t, std::pair<int, int>> lat;
            for (int y = 0; y < map.cell_h(); ++y)
                for (int x = 0; x < map.cell_w(); ++x) {
                    const MapCell& c = map.cell(x, y);
                    if (c.present)
                        lat[(static_cast<uint64_t>(c.x) << 32) | c.y] = {x, y};
                }
            const auto& ov = map.overlays();
            for (size_t i = 0; i < ov.size(); ++i) {
                if (ov[i] == 0xFF || ov[i] == 0) continue;
                const int rx = static_cast<int>(i % 512), ry = static_cast<int>(i / 512);
                const auto it = lat.find((static_cast<uint64_t>(rx) << 32) | ry);
                if (it != lat.end())
                    std::printf("ovldiag idx=%zu rx=%d ry=%d type=%d data=%d cell=(%d,%d)\n",
                                i, rx, ry, ov[i], map.overlay_data_at(rx, ry),
                                it->second.first, it->second.second);
                else
                    std::printf("ovldiag idx=%zu rx=%d ry=%d type=%d cell=NONE\n", i, rx, ry,
                                ov[i]);
            }
        }
    if (cells_path) {
        FILE* f = std::fopen(cells_path, "w");
        if (f) {
            std::fprintf(f, "cx,cy,tile_id,subtile,height,name\n");
            for (int y = 0; y < map.cell_h(); ++y)
                for (int x = 0; x < map.cell_w(); ++x) {
                    const auto& c = map.cell(x, y);
                    if (!c.present) continue;
                    std::fprintf(f, "%d,%d,%u,%u,%u,%s\n", x, y, c.tile_id, c.subtile,
                                 c.height, tileset.name_for(c.tile_id).c_str());
                }
            std::fclose(f);
            std::printf("cells csv -> %s\n", cells_path);
        }
    }
    std::printf("rendered %d cells -> %dx%d (cache hits=%llu misses=%llu)\n", drawn, bw, bh,
                static_cast<unsigned long long>(tile_cache2.hits()),
                static_cast<unsigned long long>(tile_cache2.misses()));

    // ── 对象层：建筑 + 载具 + 步兵（画在地形之后，按深度排序）──
    std::vector<ra2r::render::PlacedObject> placed;
    const auto cell_h = [&](int cx, int cy) {
        if (cx < 0 || cy < 0 || cx >= map.cell_w() || cy >= map.cell_h()) return 0;
        return static_cast<int>(map.cell(cx, cy).height);
    };
    for (const auto& b : map.buildings())
        placed.push_back({0, b.id, transpose ? b.cy : b.cx, transpose ? b.cx : b.cy, b.dir, 0,
                          cell_h(b.cx, b.cy)});
    for (const auto& u : map.units())
        placed.push_back({1, u.id, transpose ? u.cy : u.cx, transpose ? u.cx : u.cy, u.dir, 0,
                          cell_h(u.cx, u.cy)});
    for (const auto& n : map.infantry())
        placed.push_back({2, n.id, transpose ? n.cy : n.cx, transpose ? n.cx : n.cy, n.dir,
                          n.subcell, cell_h(n.cx, n.cy)});
    const ra2r::render::ObjectRenderStats ostats = ra2r::render::render_objects(
        placed, ra2r::render::UnitPaletteCfg{cfg.unit_pal, map.theater()}, grid, load_file, bw,
        bh, ox, oy, canvas);
    std::printf("objects: buildings=%d units=%d infantry=%d skipped=%d\n", ostats.buildings,
                ostats.units, ostats.infantry, ostats.skipped);

    // ── 小地图（--minimap：雷达色，1 像素/格）──
    if (minimap_path) {
        const ra2r::render::RasterImage mm = ra2r::render::render_minimap(map, tileset);
        if (!write_bmp(minimap_path, mm.w, mm.h, mm.rgba)) {
            std::fprintf(stderr, "minimap write failed\n");
            return 1;
        }
        std::printf("minimap -> %s (%dx%d)\n", minimap_path, mm.w, mm.h);
    }

    // ── BMP 输出（--dump：无头模式，写完即退出）──
    if (dump_path) {
        if (!write_bmp(dump_path, bw, bh, canvas)) {
            std::fprintf(stderr, "write bmp failed\n");
            return 1;
        }
        std::printf("dump -> %s\n", dump_path);
        return 0;
    }
    // ── 自测模式（--selftest：渲染完成即退出，不弹窗口）──
    if (selftest) {
        std::printf("selftest ok: %dx%d canvas=%dx%d\n", map.cell_w(), map.cell_h(), bw, bh);
        return 0;
    }

    // ── SDL 窗口（交互查看）──
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window =
        SDL_CreateWindow("RA2R mapview 地图查看器", 1360, 800, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!window || !renderer) {
        std::fprintf(stderr, "window failed: %s\n", SDL_GetError());
        return 1;
    }
    // 统一 DPI 缩放：窗口物理尺寸 = 逻辑尺寸 × 显示缩放
    ra2r::ui::scale_window_to_dpi(window, 1360, 800);
    SDL_Texture* tex =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, bw, bh);
    SDL_UpdateTexture(tex, nullptr, canvas.data(), bw * 4);
    float zoom = static_cast<float>(scale);
    float pan_x = 0, pan_y = 0;
    bool dragging = false;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) running = false;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                dragging = true;
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
                dragging = false;
            }
            if (ev.type == SDL_EVENT_MOUSE_MOTION && dragging) {
                pan_x += ev.motion.xrel / zoom;
                pan_y += ev.motion.yrel / zoom;
            }
            if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                zoom = std::clamp(zoom + ev.wheel.y * 0.25f, 0.25f, 4.0f);
            }
        }
        // 平移边界：地图不得完全滚出视口（留 160px 边）
        pan_x = std::clamp(pan_x, 160.0f - static_cast<float>(bw) * zoom, 1360.0f - 160.0f);
        pan_y = std::clamp(pan_y, 100.0f - static_cast<float>(bh) * zoom, 800.0f - 100.0f);
        SDL_SetRenderDrawColor(renderer, 16, 16, 20, 255);
        SDL_RenderClear(renderer);
        const SDL_FRect dst = {pan_x, pan_y, static_cast<float>(bw) * zoom,
                               static_cast<float>(bh) * zoom};
        SDL_RenderTexture(renderer, tex, nullptr, &dst);
        SDL_RenderPresent(renderer);
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) { return ra2r::core::run_wide(run, argc, wargv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
