// RA2R stage — 舞台展示 GUI（测试/调试用）
//
// 功能：
//   1. 地形类型选择（6 剧场）
//   2. 地图生成模式：算法（湖泊+高度团块）/ 完全平坦 / 加载游戏目录地图
//   3. 放置对象：建筑 / 步兵 / 载具（左键放置、右键删除）
// 交互：视口拖拽平移（鼠标中键；macOS 触控板两指滑动同效）、滚轮缩放
// （macOS 触控板两指捏合同效；地图切换后自动居中适配）。
//
// GUI 约束（与项目统一）：双击可运行（自动发现游戏目录，找不到弹出目录选择）、
// 中文界面（系统 CJK 字体）、统一 DPI 缩放（ra2r::ui 引导，布局固定尺寸 × S）。
//
// 无头自检：--test 自动放置样本对象并转储（含加载模式第二张转储）、
// --noobj 只转储纯地形（用于与 --test 像素对照）、--shot 全窗口截图、
// --dpi <f> 强制缩放系数（高 DPI 布局验证）。
//
// 渲染管线复用引擎模块：FileIndex + TerrainTileset + TerrainTile +
// PaletteLut + IsometricGrid + CacheManager + object_layer。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "ra2r/core/game_dir.h"
#include "ra2r/core/win_unicode.h"
#include "ra2r/render/isometric.h"
#include "ra2r/render/object_layer.h"
#include "ra2r/ui/backend.h"
#include "ra2r/ui/ui.h"
#include "stage/stage_app.h"

namespace fs = std::filesystem;
using ra2r::render::IsometricGrid;
using stage::StageApp;
using stage::buildable_for;
using stage::deploy_selected_mcv;
using stage::ensure_rules;
using stage::faction_building;
using stage::generate_map;
using stage::kCatNames;
using stage::kModeNames;
using stage::kTheaters;
using stage::load_type_lists;
using stage::pack_selected_building;
using stage::place_player_build;
using stage::queue_player_build;
using stage::rebuild_resources;
using stage::render_all;
using stage::scan_map_files;
using stage::skirmish_ai_tick;
using stage::start_skirmish;
using stage::write_bmp;

namespace {

// GUI 类别 → 渲染 kind（kind: 0=建筑 1=载具 2=步兵；
// kCatNames 顺序为 建筑/步兵/载具，与 kind 编号不同，须转换）
constexpr int kCatKind[3] = {0, 2, 1};

// ── 目录选择对话框（SDL 回调在主线程事件泵期间触发）──
std::string g_picked_folder;
bool g_pick_done = false;
void SDLCALL on_folder_picked(void* userdata, const char* const* filelist, int filter) {
    (void)userdata;
    (void)filter;
    g_pick_done = true;
    if (filelist && filelist[0]) g_picked_folder = filelist[0];
}

#ifdef __APPLE__
// macOS 触控板：SDL 滚轮单位 = 0.1 点，×10 还原为视口像素（1:1 跟手）
constexpr float kMacPanPixels = 10.0f;
// 手势结束后动量滚动继续按平移处理的冷却窗口（1s）
constexpr uint64_t kMacMomentumNs = 1000000000ull;
#endif

} // namespace

// ── 主程序 ──
static int run(int argc, char** argv) {
    const char* gamedir_arg = nullptr;
    std::string cache_root; // 默认 exe 目录/cache（双击运行时 CWD 不可靠）
    const char* shot_path = nullptr;
    bool test_mode = false;
    bool no_obj = false;
    bool a_no_decor = false; // --nodecor：装饰层对照渲染
    bool bridge_test = false; // --bridgetest：纯平图上摆放桥件（桥渲染规则验证）
    std::string map_arg; // --map <文件名>：加载模式直接选图（测试/复现用）
    int flat_w = 0, flat_h = 0; // --flat WxH：纯平小地图（最小实验/几何诊断）
    std::vector<std::array<int, 3>> bumps; // --bump cx,cy,h：纯平图上指定格抬升（可重复）
    float dpi_override = 0.0f; // --dpi：强制缩放系数（高 DPI 布局验证）
    int sim_steps_arg = -1;    // --simsteps N：无头自检推进 N 逻辑帧后转储
    int bench_arg = 0;         // --bench N：渲染性能基准（帧数；每帧 3 逻辑帧 + 全量渲染）
    bool sim_attack_arg = false; // --simattack：自检用攻击脚本
    bool sim_build_arg = false;  // --simbuild：自检用建造脚本
    bool sim_demo_arg = false;   // --simdemo：M3 验收演示
    bool perf_arg = false;       // --perf：渲染耗时打印
    int backend_arg = 0;         // --backend：0=OpenGL(默认) 1=SDLRenderer
    // M4 遭遇战
    bool sk_arg = false;         // --skirmish：加载地图后自动开局
    bool sk_noai = false;        // --noai：对手不自动展开/建造
    std::string sk_country, sk_color, sk_ocountry, sk_ocolor;
    int sk_class = 2, sk_tech = 10;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gamedir") == 0 && i + 1 < argc)
            gamedir_arg = argv[++i];
        else if (std::strcmp(argv[i], "--cache") == 0 && i + 1 < argc)
            cache_root = argv[++i];
        else if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
            shot_path = argv[++i];
        else if (std::strcmp(argv[i], "--test") == 0) test_mode = true;
        else if (std::strcmp(argv[i], "--noobj") == 0) no_obj = true;
        else if (std::strcmp(argv[i], "--nodecor") == 0) a_no_decor = true;
        else if (std::strcmp(argv[i], "--bridgetest") == 0) bridge_test = true;
        else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc)
            map_arg = argv[++i];
        else if (std::strcmp(argv[i], "--flat") == 0 && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &flat_w, &flat_h) != 2 || flat_w < 1 ||
                flat_h < 1 || flat_w > 128 || flat_h > 128) {
                std::fprintf(stderr, "--flat expects <w>x<h> in 1..128\n");
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--bump") == 0 && i + 1 < argc) {
            int bx = -1, by = -1, bh = -1;
            if (std::sscanf(argv[++i], "%d,%d,%d", &bx, &by, &bh) != 3 || bh < 1 ||
                bh > 14) {
                std::fprintf(stderr, "--bump expects <cx>,<cy>,<h> with h in 1..14\n");
                return 1;
            }
            bumps.push_back({bx, by, bh});
        }
        else if (std::strcmp(argv[i], "--dpi") == 0 && i + 1 < argc)
            dpi_override = static_cast<float>(std::atof(argv[++i]));
        else if (std::strcmp(argv[i], "--simsteps") == 0 && i + 1 < argc)
            sim_steps_arg = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--bench") == 0 && i + 1 < argc)
            bench_arg = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--simattack") == 0) sim_attack_arg = true;
        else if (std::strcmp(argv[i], "--simbuild") == 0) sim_build_arg = true;
        else if (std::strcmp(argv[i], "--simdemo") == 0) sim_demo_arg = true;
        else if (std::strcmp(argv[i], "--perf") == 0) perf_arg = true;
        else if (std::strcmp(argv[i], "--skirmish") == 0) sk_arg = true;
        else if (std::strcmp(argv[i], "--noai") == 0) sk_noai = true;
        else if (std::strcmp(argv[i], "--country") == 0 && i + 1 < argc) sk_country = argv[++i];
        else if (std::strcmp(argv[i], "--color") == 0 && i + 1 < argc) sk_color = argv[++i];
        else if (std::strcmp(argv[i], "--ocountry") == 0 && i + 1 < argc)
            sk_ocountry = argv[++i];
        else if (std::strcmp(argv[i], "--ocolor") == 0 && i + 1 < argc) sk_ocolor = argv[++i];
        else if (std::strcmp(argv[i], "--skclass") == 0 && i + 1 < argc)
            sk_class = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--sktech") == 0 && i + 1 < argc)
            sk_tech = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            const std::string b = argv[++i];
            if (b == "sdl" || b == "sdgpu" || b == "sdlrenderer") backend_arg = 1;
            else if (b == "gl" || b == "opengl") backend_arg = 0;
            else {
                std::fprintf(stderr, "--backend expects gl|sdl\n");
                return 1;
            }
        }
        else {
            std::printf(
                "usage: stage [--gamedir <dir>] [--cache <root>] [--shot <bmp>] [--test] "
                "[--noobj] [--dpi <f>] [--backend gl|sdl] [--perf] [--bench <frames>]\n"
                "       stage --map battle1.yrm --skirmish [--country <c>] [--color <c>] "
                "[--ocountry <c>] [--ocolor <c>] [--skclass 0..3] [--sktech 1..10] [--noai]\n"
                "  双击运行（无参数）时自动发现游戏目录；找不到则弹出目录选择窗口。\n");
            return 1;
        }
    }
    // --test 无头转储模式依赖 --shot 作为输出路径前缀（csv/bmp 系列均基于它）；
    // 缺失时直接报错，避免运行到中途 std::string(nullptr) 抛异常崩溃。
    if (test_mode && !shot_path) {
        std::fprintf(stderr, "--test requires --shot <bmp> (dump path prefix)\n");
        return 1;
    }
    ra2r::ui::enable_dpi_awareness();
    ra2r::ui::console_utf8();
#ifdef __APPLE__
    // macOS 触控板：SDL 默认把触控板伪装成鼠标（不产生触摸事件）。开启后
    // 触控板作为独立触摸设备上报 FINGER 事件，用于识别两指手势/区分触控板
    // 与物理滚轮；鼠标移动/点击行为不变（该 hint 仅改变触摸事件的设备归属）。
    SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY, "1");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    // 多后端渲染宿主：OpenGL（默认，失败回退 SDLRenderer）/ SDLRenderer
    ra2r::ui::BackendHost host;
    {
        const ra2r::ui::BackendKind kind =
            backend_arg == 0 ? ra2r::ui::BackendKind::kOpenGL : ra2r::ui::BackendKind::kSDLRenderer;
        std::string berr;
        if (!host.init_window("RA2R stage 舞台展示", 1440, 900, kind, true, &berr)) {
            std::fprintf(stderr, "backend init failed: %s\n", berr.c_str());
            return 1;
        }
    }
    // 统一 DPI 缩放：窗口物理尺寸 = 逻辑尺寸 × 显示缩放；布局固定尺寸乘 S
    float S = ra2r::ui::scale_window_to_dpi(host.window(), 1440, 900);
    if (dpi_override > 0.0f) {
        S = dpi_override;
        SDL_SetWindowSize(host.window(), static_cast<int>(1440 * S + 0.5f),
                          static_cast<int>(900 * S + 0.5f));
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!host.init_imgui()) {
        std::fprintf(stderr, "imgui backend init failed\n");
        return 1;
    }
    // 中文界面：系统 CJK 字体（黑体/雅黑…）为默认字体，随 DPI 缩放
    const bool cjk = ra2r::ui::setup_cjk_font(18.0f * S);
    ImGui::GetStyle().ScaleAllSizes(S);

    if (cache_root.empty()) {
        // SDL_GetStringRule：返回进程生命周期常量串，无需释放
        const char* base = SDL_GetBasePath();
        cache_root = base ? std::string(base) + "cache" : std::string("cache");
    }

    // ── 游戏目录：参数 > 自动发现 > GUI 目录选择 ──
    std::string gamedir;
    if (gamedir_arg) gamedir = gamedir_arg;
    else gamedir = ra2r::core::find_game_dir();

    StageApp a;
    a.no_decor = a_no_decor;
    a.sim_steps = sim_steps_arg;
    a.sim_attack = sim_attack_arg;
    a.sim_build = sim_build_arg;
    a.sim_demo = sim_demo_arg;
    a.perf_log = perf_arg;
    a.test_whole_canvas = test_mode; // 无头转储：画全图（截图基线覆盖整张画布）
    a.sk_autostart = sk_arg;
    a.sk_ai = !sk_noai;
    a.sk.class_sel = sk_class;
    a.sk.tech_level = sk_tech;
    if (!sk_country.empty()) a.sk.cfg.player.country = sk_country;
    if (!sk_color.empty()) a.sk.cfg.player.color = sk_color;
    if (!sk_ocountry.empty()) a.sk.cfg.opponent.country = sk_ocountry;
    if (!sk_ocolor.empty()) a.sk.cfg.opponent.color = sk_ocolor;
    a.host = &host;
    std::string err, init_err;
    const auto try_init = [&](const std::string& dir) -> bool {
        init_err.clear();
        err.clear();
        a.file_cache.clear(); // 换目录重试时丢弃旧目录的缓存条目
        if (!a.index.build(dir, &err)) {
            init_err = err;
            return false;
        }
        a.cache.open(cache_root, &err);
        if (!rebuild_resources(a, kTheaters[0], &err)) {
            init_err = err;
            return false;
        }
        load_type_lists(a);
        scan_map_files(a, dir);
        generate_map(a, &err);
        gamedir = dir;
        return true;
    };
    bool ready = false;
    if (!gamedir.empty()) ready = try_init(gamedir);
    else
        init_err = "未找到游戏目录：请选择《红色警戒2 / 尤里的复仇》安装目录（含 ra2md.mix）。";
    // 无头测试依赖参数/自动发现的目录；解析不到直接报错退出
    if (!ready && test_mode) {
        std::fprintf(stderr, "gamedir unresolved: %s\n", init_err.c_str());
        return 1;
    }
    // --map：加载模式直接选指定地图（文件名匹配，忽略大小写）
    if (ready && !map_arg.empty()) {
        a.map_sel = -1;
        std::string want = map_arg;
        std::transform(want.begin(), want.end(), want.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        for (size_t i = 0; i < a.map_files.size(); ++i) {
            std::string fn = std::filesystem::path(a.map_files[i]).filename().string();
            std::transform(fn.begin(), fn.end(), fn.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (fn == want) {
                a.map_sel = static_cast<int>(i);
                break;
            }
        }
        if (a.map_sel >= 0) {
            a.mode = 2;
            generate_map(a, &err);
        } else {
            std::fprintf(stderr, "--map %s not found in scanned maps\n", map_arg.c_str());
        }
    }
    // --skirmish：地图就绪后直接开局（玩家/对手按 waypoint 出生）
    if (a.sk_autostart && a.map_sel >= 0) {
        std::string skerr;
        if (!start_skirmish(a, &skerr))
            std::fprintf(stderr, "skirmish start failed: %s\n", skerr.c_str());
    }
    char dir_buf[512] = {};
    if (!gamedir.empty()) std::snprintf(dir_buf, sizeof(dir_buf), "%s", gamedir.c_str());
    if (test_mode) {
        const ImFont* f = ImGui::GetIO().FontDefault;
        std::printf("dpi: scale=%.2f cjk_font=%d font=%s gamedir=%s auto=%d\n", S, cjk ? 1 : 0,
                    f ? f->GetDebugName() : "(none)", gamedir.c_str(), gamedir_arg ? 0 : 1);
    }

    // ── 无头测试：算法图 + 放置样本对象 + 转储（--noobj 只渲染纯地形便于对照）──
    if (test_mode && shot_path) {
        if (flat_w > 0) {
            // 最小实验：纯平 W×H 图（tile 0、高度 0、无对象），验证基础几何；
            // --bump 抬升指定格后跑斜坡衔接（高度过渡最小场景）
            a.map.generate_flat(flat_w, flat_h);
            a.map.theater = kTheaters[a.theater_sel];
            for (const auto& b : bumps) {
                if (b[0] >= 0 && b[0] < a.map.w && b[1] >= 0 && b[1] < a.map.h)
                    a.map.cell(b[0], b[1]).height = static_cast<uint8_t>(b[2]);
            }
            if (!bumps.empty()) assign_slopes(a);
            a.objects.clear();
            // ── --bridgetest：在纯平图上摆放桥件，验证桥面铺设规则 ──
            // 复刻 test1.mpr：3 片 1×3（"\"）混凝土桥——227 桥尾 / 215 桥身 /
            // 229 桥头，每片 3 格沿 rx（下右 2:1 直对角线），片与片沿 ry（下左）相接，
            // 桥面画在各片原点（data=0）格：(20,41),(20,42),(19,43)
            // 艺术 = 硬编码表最终文件名（227→LOBRDB23、215→LOBRDB11、229→LOBRDB25）
            if (bridge_test) {
                a.map.decor.clear();
                a.map.decor.push_back(
                    {ra2r::render::MapDecorObject::kTmpTile, "LOBRDB23.tem", 20, 41, 0, 1});
                a.map.decor.push_back(
                    {ra2r::render::MapDecorObject::kTmpTile, "LOBRDB11.tem", 20, 42, 0, 1});
                a.map.decor.push_back(
                    {ra2r::render::MapDecorObject::kTmpTile, "LOBRDB25.tem", 19, 43, 0, 1});
                std::printf("bridgetest: decor %zu 件\n", a.map.decor.size());
            }
            a.dirty = true;
        } else {
        a.map.generate_algorithm(48, 48, a.tileset, 12345);
        a.map.theater = kTheaters[0];
        assign_slopes(a);
        int slopes = 0;
        for (int y = 0; y < a.map.h; ++y)
            for (int x = 0; x < a.map.w; ++x) {
                const std::string& nm = a.tileset.name_for(a.map.cell(x, y).tile_id);
                if (nm.find("RAMP") != std::string::npos) ++slopes;
            }
        std::printf("slope cells: %d\n", slopes);
        a.objects.clear();
        if (!no_obj) {
            const auto hof = [&a](int cx, int cy) {
                return static_cast<int>(a.map.cell(cx, cy).height);
            };
            if (a.obj_lists[0].size() > 5)
                a.objects.push_back({0, a.obj_lists[0][5], 10, 10, 0, 0, hof(10, 10)});
            if (a.obj_lists[1].size() > 3)
                a.objects.push_back({2, a.obj_lists[1][3], 24, 16, 64, 0, hof(24, 16)});
            if (a.obj_lists[2].size() > 2)
                a.objects.push_back({1, a.obj_lists[2][2], 30, 28, 96, 0, hof(30, 28)});
            // 2×2 建筑（GAPOWR）用于锚点像素验证：底边中点应落在前缘中点
            const auto it2 =
                std::find(a.obj_lists[0].begin(), a.obj_lists[0].end(), "GAPOWR");
            if (it2 != a.obj_lists[0].end())
                a.objects.push_back({0, *it2, 20, 20, 0, 0, hof(20, 20)});
        }
        for (const auto& o : a.objects)
            std::printf("place kind=%d id=%s cell=(%d,%d) dir=%d\n", o.kind, o.id.c_str(), o.cx,
                        o.cy, o.dir);
        std::printf("map files scanned: %zu\n", a.map_files.size());
        // 地图数据导出（与 shot 同目录 .csv）：画布坐标 ↔ 格数据对齐诊断用
        {
            const std::string csv = std::string(shot_path) + ".csv";
            FILE* f = std::fopen(csv.c_str(), "w");
            if (f) {
                std::fprintf(f, "cx,cy,tile_id,subtile,height,name\n");
                for (int y = 0; y < a.map.h; ++y)
                    for (int x = 0; x < a.map.w; ++x) {
                        const auto& c = a.map.cell(x, y);
                        std::fprintf(f, "%d,%d,%u,%u,%u,%s\n", x, y, c.tile_id, c.subtile,
                                     c.height, a.tileset.name_for(c.tile_id).c_str());
                    }
                std::fclose(f);
                std::printf("cells csv -> %s\n", csv.c_str());
            }
        }
        } // else: 非纯平模式
        a.dirty = true;
    }

    // 渲染循环
    bool running = true;
    int frame = 0;
    uint64_t sim_clock = SDL_GetTicks(); // M3 模拟层 15Hz 累计时钟（ms）
#ifdef __APPLE__
    // macOS 触控板：两指滑动平移（等价中键拖动）、两指捏合缩放（等价滚轮）。
    // 触控板与物理滚轮在 SDL 层同为 MOUSE_WHEEL 事件，无法直接区分，故用
    // "活跃手指数 / 刚结束的两指手势" 判定来源：触控板→平移，物理滚轮→缩放。
    int mac_fingers = 0;         // 当前触控板上的手指数
    uint64_t mac_two_finger_ns = 0; // 最近一次两指手势时间（含动量冷却）
#endif
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            host.process_event(&ev);
#ifdef __APPLE__
            // 触控板手势：统计手指数（判定滚轮来源）；捏合直接缩放视角
            if (ev.type == SDL_EVENT_FINGER_DOWN) {
                if (++mac_fingers >= 2) mac_two_finger_ns = SDL_GetTicksNS();
            } else if (ev.type == SDL_EVENT_FINGER_MOTION) {
                if (mac_fingers >= 2) mac_two_finger_ns = SDL_GetTicksNS();
            } else if (ev.type == SDL_EVENT_FINGER_UP || ev.type == SDL_EVENT_FINGER_CANCELED) {
                if (mac_fingers >= 2) mac_two_finger_ns = SDL_GetTicksNS(); // 进入动量冷却
                if (mac_fingers > 0) --mac_fingers;
            } else if (ev.type == SDL_EVENT_PINCH_UPDATE) {
                // 两指分离 scale>1 放大，合并 scale<1 缩小（乘性缩放更跟手）
                a.zoom = std::clamp(a.zoom * ev.pinch.scale, 0.1f, 3.0f);
                a.dirty = true;
            }
#endif
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) running = false;
            // 展开基地车（D）/ 取消放置（Esc 之外的右键）
            if (ev.type == SDL_EVENT_KEY_DOWN && (ev.key.key == SDLK_D) && a.sim_active) {
                if (!deploy_selected_mcv(a)) pack_selected_building(a);
            }
            // 停止（S）：多选全体清指令原地驻停
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_S && a.sim_active &&
                !a.selection.empty()) {
                size_t stopped = 0;
                for (const uint32_t sid : a.selection) {
                    for (size_t i = 0; i < a.sim.units.size(); ++i)
                        if (a.sim.units[i].id == sid) {
                            if (a.sim.stop_unit(i)) ++stopped;
                            break;
                        }
                }
                std::fprintf(stderr, "[stage] 停止 %zu 单位\n", stopped);
                a.dirty = true;
            }
            // 编队：Ctrl+1..9 存队，1..9 取队（模拟模式）
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key >= SDLK_1 && ev.key.key <= SDLK_9 &&
                a.sim_active) {
                const int gi = ev.key.key - SDLK_1 + 1;
                const bool ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
                if (ctrl) {
                    a.groups[gi] = a.selection;
                    std::fprintf(stderr, "[stage] 编队 %d 存 %zu 单位\n", gi,
                                 a.selection.size());
                } else {
                    a.selection = a.groups[gi];
                    std::fprintf(stderr, "[stage] 编队 %d 取 %zu 单位\n", gi,
                                 a.selection.size());
                }
                a.dirty = true;
            }
            // 目录选择对话框的回调结果（对话框关闭时在主线程触发）
            if (g_pick_done) {
                g_pick_done = false;
                if (!g_picked_folder.empty()) {
                    std::snprintf(dir_buf, sizeof(dir_buf), "%s", g_picked_folder.c_str());
                    g_picked_folder.clear();
                }
            }
        }
        // 模拟层逻辑帧（15Hz，与渲染帧率解耦；仅视觉状态变化才触发重渲染）
        if (a.sim_active && !test_mode) {
            const uint64_t now = SDL_GetTicks();
            while (sim_clock + 66 <= now) {
                if (a.sk.active) skirmish_ai_tick(a);
                if (a.sim.tick()) a.dirty = true;
                sim_clock += 66;
                if (now - sim_clock > 2000) sim_clock = now; // 窗口阻塞后不追帧
            }
            const uint64_t vh = a.sim.visual_hash();
            if (vh != a.last_vhash) {
                a.last_vhash = vh;
                a.dirty = true;
            }
        }
        host.new_frame();
        host.begin_frame();

        if (!ready) {
            // ── 双击运行找不到游戏目录时的 GUI 兜底：目录选择窗口 ──
            const ImVec2 wsz(560.0f * S, 260.0f * S);
            const ImVec2 vp = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2((vp.x - wsz.x) * 0.5f, (vp.y - wsz.y) * 0.5f));
            ImGui::SetNextWindowSize(wsz);
            ImGui::Begin("选择游戏目录", nullptr,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextWrapped("%s", init_err.c_str());
            ImGui::Spacing();
            ImGui::InputText("游戏目录", dir_buf, sizeof(dir_buf));
            if (ImGui::Button("浏览…")) {
                SDL_ShowOpenFolderDialog(on_folder_picked, nullptr, host.window(),
                                         dir_buf[0] ? dir_buf : nullptr, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("确定") && dir_buf[0] && try_init(dir_buf)) ready = true;
            ImGui::TextDisabled("提示：目录需包含 ra2md.mix（或 ra2.mix）");
            ImGui::End();
        } else {
        // ── 主窗口：占满整个显示器（无装饰），内含左右面板 ──
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("stage_main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings);
        // ── 左面板：控制区 ──
        ImGui::BeginChild("panel", ImVec2(340.0f * S, 0), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("剧场");
        if (ImGui::Combo("##theater", &a.theater_sel, kTheaters, 6)) {
            rebuild_resources(a, kTheaters[a.theater_sel], &err);
            if (a.mode != 2) {
                a.map.theater = kTheaters[a.theater_sel];
                generate_map(a, &err);
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("地图生成模式");
        ImGui::RadioButton(kModeNames[0], &a.mode, 0);
        ImGui::RadioButton(kModeNames[1], &a.mode, 1);
        ImGui::RadioButton(kModeNames[2], &a.mode, 2);
        if (a.mode != 2) {
            ImGui::SliderInt("宽", &a.map_w, 32, 128);
            ImGui::SliderInt("高", &a.map_h, 32, 128);
            if (a.mode == 0) ImGui::SliderInt("种子", &a.seed, 0, 999999);
            if (ImGui::Button("生成")) generate_map(a, &err);
        } else {
            if (ImGui::BeginCombo("地图", a.map_sel >= 0 ? a.map_files[a.map_sel].c_str()
                                                          : "选择地图...")) {
                for (int i = 0; i < static_cast<int>(a.map_files.size()); ++i) {
                    const std::string fn = fs::path(a.map_files[i]).filename().string();
                    if (ImGui::Selectable(fn.c_str(), a.map_sel == i)) a.map_sel = i;
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("加载")) generate_map(a, &err);
            // ── 遭遇战设置（加载地图模式）──
            ImGui::Separator();
            ImGui::TextUnformatted("遭遇战");
            ensure_rules(a);
            const auto combo_str = [&](const char* label, std::string& value,
                                       const std::vector<std::string>& items) {
                if (items.empty()) {
                    ImGui::Text("%s: %s", label, value.c_str());
                    return;
                }
                if (ImGui::BeginCombo(label, value.empty() ? items[0].c_str() : value.c_str())) {
                    for (const auto& s : items)
                        if (ImGui::Selectable(s.c_str(), s == value)) value = s;
                    ImGui::EndCombo();
                }
            };
            // 国家下拉：显示"国家（阵营）"，玩家改选后自动把对手换到不同阵营
            const auto combo_country = [&](const char* label, std::string& value, bool is_player) {
                if (a.sk.player_countries.empty()) {
                    ImGui::Text("%s: %s", label, value.c_str());
                    return;
                }
                const auto disp = [&](const std::string& name) {
                    const std::string side = stage::country_side_of(a, name);
                    return side.empty() ? name : name + "（" + side + "）";
                };
                if (ImGui::BeginCombo(label, disp(value).c_str())) {
                    for (const auto& s : a.sk.player_countries)
                        if (ImGui::Selectable(disp(s).c_str(), s == value)) {
                            value = s;
                            if (is_player) stage::skirmish_auto_opponent(a);
                        }
                    ImGui::EndCombo();
                }
            };
            combo_country("玩家国家", a.sk.cfg.player.country, true);
            combo_str("玩家颜色", a.sk.cfg.player.color, a.sk.color_names);
            combo_country("对手国家", a.sk.cfg.opponent.country, false);
            combo_str("对手颜色", a.sk.cfg.opponent.color, a.sk.color_names);
            static const char* kClassNames[4] = {"仅基地车", "轻装", "中装", "重装"};
            ImGui::Combo("开局兵力", &a.sk.class_sel, kClassNames, 4);
            ImGui::SliderInt("科技等级", &a.sk.tech_level, 1, 10);
            if (ImGui::Button("开始遭遇战")) {
                std::string skerr;
                if (!start_skirmish(a, &skerr))
                    std::fprintf(stderr, "[stage] 遭遇战启动失败: %s\n", skerr.c_str());
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("放置对象（左键放置 / 右键删除）");
        if (ImGui::Combo("类别", &a.obj_cat, kCatNames, 3)) {
        }
        const int sel = a.obj_sel[a.obj_cat];
        if (ImGui::BeginListBox("##objs", ImVec2(300.0f * S, 260.0f * S))) {
            for (int i = 0; i < static_cast<int>(a.obj_lists[a.obj_cat].size()); ++i) {
                const bool is_sel = (sel == i);
                if (ImGui::Selectable(a.obj_lists[a.obj_cat][i].c_str(), is_sel)) {
                    a.obj_sel[a.obj_cat] = i;
                }
            }
            ImGui::EndListBox();
        }
        ImGui::SliderInt("朝向", &a.obj_dir, 0, 255);
        ImGui::SliderFloat("对象比例", &a.obj_scale, 0.25f, 3.0f, "%.2f");
        if (a.obj_sel[a.obj_cat] >= 0) {
            ImGui::Text("当前: %s", a.obj_lists[a.obj_cat][a.obj_sel[a.obj_cat]].c_str());
        }
        if (ImGui::Button("清空对象")) {
            a.objects.clear();
            a.dirty = true;
        }
        ImGui::Text("对象数: %zu", a.objects.size());
        ImGui::Text("渲染: 建筑 %d · 载具 %d · 步兵 %d · 跳过 %d", a.obj_stats.buildings,
                    a.obj_stats.units, a.obj_stats.infantry, a.obj_stats.skipped);
        if (a.sim_active) {
            ImGui::Separator();
            // ── 遭遇战侧边栏：资金/电力 + 建造队列 + 科技树建筑列表 ──
            if (a.sk.active) {
                const auto cit = a.sim.credits.find("Player");
                const auto pit = a.sim.power_net.find("Player");
                const std::string pside = stage::country_side_of(a, a.sk.cfg.player.country);
                const std::string oside = stage::country_side_of(a, a.sk.cfg.opponent.country);
                ImGui::Text("玩家 %s%s / %s   $%lld   电力 %+d", a.sk.cfg.player.country.c_str(),
                            pside.empty() ? "" : ("（" + pside + "）").c_str(),
                            a.sk.cfg.player.color.c_str(),
                            static_cast<long long>(cit != a.sim.credits.end() ? cit->second : 0),
                            pit != a.sim.power_net.end() ? pit->second : 0);
                ImGui::Text("对手 %s%s / %s", a.sk.cfg.opponent.country.c_str(),
                            oside.empty() ? "" : ("（" + oside + "）").c_str(),
                            a.sk.cfg.opponent.color.c_str());
                const auto qit = a.sim.build_queue.find("Player");
                if (qit != a.sim.build_queue.end()) {
                    const auto& item = qit->second;
                    const float p = item.total > 0
                                        ? static_cast<float>(item.ticks) /
                                              static_cast<float>(item.total)
                                        : 0.0f;
                    ImGui::ProgressBar(std::min(1.0f, p), ImVec2(-1, 0), item.type.c_str());
                    if (item.ready) {
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "建造完成：点击放置");
                        if (ImGui::Button("放置##ready")) a.placing = true;
                        ImGui::SameLine();
                        if (ImGui::Button("取消##ready")) {
                            a.sim.cancel_build("Player");
                            a.placing = false;
                        }
                    }
                }
                if (a.placing) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                                  "左键落点（红色=地基被占）");
                ImGui::TextUnformatted("可造建筑（科技树）");
                if (ImGui::BeginListBox("##buildable", ImVec2(300.0f * S, 200.0f * S))) {
                    const auto list = buildable_for(a, "Player");
                    for (const auto* t : list) {
                        char label[160];
                        std::snprintf(label, sizeof label, "%s  $%d  %+d", t->name.c_str(),
                                      t->cost, t->power);
                        if (ImGui::Selectable(label, false)) queue_player_build(a, t->name);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s  地基 %dx%d  科技 %d  前置 %s",
                                              a.rules.rules().get(t->name, "Name", t->name.c_str())
                                                  .c_str(),
                                              t->fw, t->fh, t->tech_level,
                                              t->prereq.empty() ? "无" : t->prereq[0].c_str());
                    }
                    ImGui::EndListBox();
                }
                // 选中基地车 → 展开按钮
                bool mcv_sel = false;
                for (const uint32_t sid : a.selection)
                    for (const auto& u : a.sim.units)
                        if (u.id == sid) {
                            const auto* t = a.rules.unit(u.type);
                            if (t && !t->deploys_into.empty()) mcv_sel = true;
                        }
                ImGui::BeginDisabled(!mcv_sel);
                if (ImGui::Button("展开基地车 (D)")) deploy_selected_mcv(a);
                ImGui::EndDisabled();
                // ── 选中建筑：修理 / 出售（M4）──
                if (a.sel_building_id != 0) {
                    int bidx = -1;
                    for (int i = 0; i < static_cast<int>(a.sim.buildings.size()); ++i)
                        if (a.sim.buildings[i].id == a.sel_building_id) {
                            bidx = i;
                            break;
                        }
                    if (bidx >= 0 && a.sim.buildings[bidx].alive) {
                        const auto& b = a.sim.buildings[bidx];
                        ImGui::Separator();
                        ImGui::Text("建筑: %s (%s)  hp %d/%d", b.type.c_str(), b.owner.c_str(),
                                    b.hp, b.max_hp);
                        // 防御建筑：显示武器并可手动指定/停止攻击（右键敌人也可）
                        if (b.weapon.damage > 0) {
                            ImGui::Text("防御武器: 伤害 %d / ROF %d / 射程 %d%s",
                                        b.weapon.damage, b.weapon.rof, b.weapon.range,
                                        b.target >= 0 ? "（交战中）" : "");
                            ImGui::SameLine();
                            ImGui::BeginDisabled(b.target < 0);
                            if (ImGui::Button("停止攻击")) {
                                a.sim.stop_build_attack(static_cast<size_t>(bidx));
                                a.dirty = true;
                            }
                            ImGui::EndDisabled();
                            if (b.target < 0)
                                ImGui::TextDisabled("右键敌人 → 指定炮塔攻击方向");
                        }
                        if (b.under_construction) {
                            ImGui::TextDisabled("建造中（%.0f%%）",
                                                b.build_total > 0
                                                    ? 100.0 * b.build_ticks / b.build_total
                                                    : 0.0);
                        } else if (b.owner == "Player") {
                            ImGui::BeginDisabled(b.hp >= b.max_hp);
                            if (ImGui::Button(b.repairing ? "停止修理" : "修理")) {
                                a.sim.toggle_repair(static_cast<size_t>(bidx));
                                a.dirty = true;
                            }
                            ImGui::EndDisabled();
                            ImGui::SameLine();
                            if (ImGui::Button("出售")) {
                                const int refund_pct = std::atoi(
                                    a.rules.rules().get("General", "RefundPercent", "50")
                                        .c_str());
                                const int64_t refund =
                                    a.sim.sell_building(static_cast<size_t>(bidx), refund_pct);
                                a.sel_building_id = 0;
                                a.dirty = true;
                                std::fprintf(stderr, "[stage] 出售 %s 退款 $%lld\n",
                                             b.type.c_str(), static_cast<long long>(refund));
                            }
                            // 基地收起（UndeploysInto= 非空，如建造厂 → 基地车）
                            if (const auto* ut = a.rules.unit(b.type);
                                ut && !ut->undeploys_into.empty()) {
                                ImGui::SameLine();
                                if (ImGui::Button("收起基地车 (D)"))
                                    pack_selected_building(a);
                            }
                            if (b.repairing)
                                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                                   "修理中…");
                        } else {
                            ImGui::TextDisabled("敌方建筑（战斗系统 M5）");
                        }
                    } else {
                        a.sel_building_id = 0;
                    }
                }
                ImGui::Separator();
            }
            ImGui::Checkbox("建造模式（左键起造，先选中己方单位）", &a.build_mode);
            if (a.build_mode) {
                if (ImGui::BeginListBox("##blds", ImVec2(300.0f * S, 110.0f * S))) {
                    for (int i = 0; i < static_cast<int>(a.obj_lists[0].size()); ++i) {
                        const std::string& t = a.obj_lists[0][i];
                        const auto* u = a.rules.unit(t);
                        char label[160];
                        std::snprintf(label, sizeof label, "%s  $%d", t.c_str(),
                                      u ? u->cost : 300);
                        if (ImGui::Selectable(label, a.build_sel == i)) a.build_sel = i;
                    }
                    ImGui::EndListBox();
                }
            }
            ImGui::Text("模拟: 单位 %zu · 建筑 %zu · 爆炸 %zu", a.sim.units.size(),
                        a.sim.buildings.size(), a.sim.explosions.size());
            for (const auto& [owner, amt] : a.sim.credits)
                ImGui::Text("资金 %s: $%lld", owner.c_str(), static_cast<long long>(amt));
            for (const auto& [owner, pw] : a.sim.power_net)
                ImGui::Text("电力 %s: %+d", owner.c_str(), pw);
        }
        ImGui::EndChild();

        // ── 右面板：视口 ──
        ImGui::SameLine();
        ImGui::BeginChild("viewport", ImVec2(0, 0), ImGuiChildFlags_Borders);
        // 放置模式：鼠标所在格 = 地基预览（在重渲染前更新，保证预览不滞后）
        if (a.sim_active && a.sk.active && a.placing && a.bw > 0) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const ImVec2 pos0 = ImGui::GetCursorScreenPos();
            const int lx = static_cast<int>((mp.x - pos0.x - a.pan_x) / a.zoom);
            const int ly = static_cast<int>((mp.y - pos0.y - a.pan_y) / a.zoom);
            const IsometricGrid grid;
            int hcx, hcy;
            grid.pixel_to_cell(lx - a.ox, ly - a.oy, hcx, hcy);
            if (hcx != a.hover_cx || hcy != a.hover_cy) {
                a.hover_cx = hcx;
                a.hover_cy = hcy;
                a.dirty = true;
            }
        }
        // 可见画布范围（画家序渲染只画这一块 + 余量；见 render_all）。
        // **必须在下面 render_all 之前更新**：否则首帧范围为空 → 整屏黑。
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        if (a.zoom > 0.0f) {
            a.view_x0 = static_cast<int>((0.0f - a.pan_x) / a.zoom);
            a.view_y0 = static_cast<int>((0.0f - a.pan_y) / a.zoom);
            a.view_x1 = static_cast<int>((avail.x - a.pan_x) / a.zoom);
            a.view_y1 = static_cast<int>((avail.y - a.pan_y) / a.zoom);
        }
        if (a.dirty) {
            // 重渲染节流（~30Hz）：模拟高频变化时按需渲染，UI 保持响应；
            // dirty 保持，节流窗口过后自动补渲染。
            const uint64_t now_ms = SDL_GetTicks();
            if (now_ms - a.last_render_ms >= 33) {
                a.last_render_ms = now_ms;
                const IsometricGrid grid;
                int bw, bh, ox, oy;
                grid.map_bounds(a.map.w, a.map.h, 15, bw, bh, ox, oy);
                render_all(a, &err);
                // 地图切换后：缩放适配 + 居中（放置对象不触发，避免视图跳动）
                if (a.recenter) {
                    const ImVec2 avail2 = ImGui::GetContentRegionAvail();
                    const float fit = std::min(avail2.x / static_cast<float>(a.bw),
                                               avail2.y / static_cast<float>(a.bh));
                    a.zoom = std::clamp(fit, 0.1f, 1.0f);
                    a.pan_x = (avail2.x - static_cast<float>(a.bw) * a.zoom) * 0.5f;
                    a.pan_y = (avail2.y - static_cast<float>(a.bh) * a.zoom) * 0.5f;
                    a.recenter = false;
                }
                a.dirty = false;
            }
        }
        ImGui::InvisibleButton("stage3d", avail,
                               ImGuiButtonFlags_MouseButtonLeft |
                                   ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool view_hovered = ImGui::IsItemHovered();
        // 拖拽地图 = 鼠标**中键**（左键留给点选/框选）；用 IsItemActive 让
        // 指针拖出窗口后仍继续平移
        if (ImGui::IsItemActive() &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            a.pan_x += d.x;
            a.pan_y += d.y;
        }
        if (view_hovered) {
            const ImGuiIO& io = ImGui::GetIO();
#ifdef __APPLE__
            // 触控板两指滑动 = 滚轮事件 → 平移视角（等价中键拖动）；
            // 动量滚动在手指离开后继续平移；物理滚轮（无触控板手势）→ 缩放。
            const bool trackpad_wheel =
                mac_fingers >= 2 ||
                (mac_two_finger_ns != 0 &&
                 (SDL_GetTicksNS() - mac_two_finger_ns) < kMacMomentumNs);
            if (trackpad_wheel) {
                if (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f) {
                    a.pan_x += io.MouseWheelH * kMacPanPixels;
                    a.pan_y += io.MouseWheel * kMacPanPixels;
                    a.dirty = true;
                }
            } else
#endif
            {
                const float w = io.MouseWheel;
                if (w != 0.0f) a.zoom = std::clamp(a.zoom + w * 0.1f, 0.1f, 3.0f);
            }
        }
        if (view_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const int lx = static_cast<int>((mp.x - pos.x - a.pan_x) / a.zoom);
            const int ly = static_cast<int>((mp.y - pos.y - a.pan_y) / a.zoom);
            const IsometricGrid grid;
            int cx, cy;
            grid.pixel_to_cell(lx - a.ox, ly - a.oy, cx, cy);
            if (a.sim_active && a.sk.active && a.placing) {
                // 放置模式：左键落点（地基校验在 place_player_build 内）
                place_player_build(a, cx, cy);
            } else if (a.sim_active && a.build_mode) {
                // 建造模式：以选中单位的 House 起造（立即扣款，工地半透明+进度条）
                if (cx < 0 || cy < 0 || cx >= a.map.w || cy >= a.map.h) {
                } else if (a.build_sel < 0 ||
                           a.build_sel >= static_cast<int>(a.obj_lists[0].size())) {
                    std::fprintf(stderr, "[stage] 未选择建筑类型\n");
                } else {
                    std::string owner;
                    for (const uint32_t sid : a.selection) {
                        for (const auto& u : a.sim.units)
                            if (u.id == sid) {
                                owner = u.owner;
                                break;
                            }
                        if (!owner.empty()) break;
                    }
                    if (owner.empty()) {
                        std::fprintf(stderr, "[stage] 建造：请先选中己方单位\n");
                    } else {
                        const std::string& type = a.obj_lists[0][a.build_sel];
                        const auto* u = a.rules.unit(type);
                        const int fw = u ? u->fw : 1;
                        const int fh = u ? u->fh : 1;
                        const int cost = u ? u->cost : 300;
                        const int power = u ? u->power : 0;
                        // 现场工期 = [General] BuildupTime（YR 54 帧）；无 Buildup 即完成
                        const int total = std::max(1, onsite_ticks(a, type));
                        const bool ok = a.sim.issue_build(owner, type, cx, cy, fw, fh, cost,
                                                          total, power);
                        std::fprintf(stderr, "[stage] 建造 %s @(%d,%d) for %s $%d power%+d %s\n",
                                     type.c_str(), cx, cy, owner.c_str(), cost, power,
                                     ok ? "ok" : "fail");
                        a.dirty = true;
                    }
                }
            } else if (a.sim_active) {
                // 选择：点中单位选中；双击同型全选（同 House）；空地开始框选
                int hit = -1;
                for (int i = 0; i < static_cast<int>(a.sim.units.size()); ++i) {
                    if (a.sim.units[i].col == cx && a.sim.units[i].row == cy) {
                        hit = i;
                        break;
                    }
                }
                if (hit >= 0) {
                    const bool shift = ImGui::GetIO().KeyShift;
                    const bool ctrl = ImGui::GetIO().KeyCtrl;
                    if (a.last_click_unit == hit &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        a.selection.clear();
                        for (int i = 0; i < static_cast<int>(a.sim.units.size()); ++i) {
                            if (a.sim.units[i].type == a.sim.units[hit].type &&
                                a.sim.units[i].owner == a.sim.units[hit].owner)
                                a.selection.push_back(a.sim.units[i].id);
                        }
                    } else if (shift && !ctrl) {
                        // Shift+左键：追加选中（多选）
                        const uint32_t id = a.sim.units[hit].id;
                        if (std::find(a.selection.begin(), a.selection.end(), id) ==
                            a.selection.end())
                            a.selection.push_back(id);
                    } else if (ctrl && !shift) {
                        // Ctrl+左键：从选择中移除
                        const uint32_t id = a.sim.units[hit].id;
                        a.selection.erase(std::remove(a.selection.begin(), a.selection.end(), id),
                                          a.selection.end());
                    } else if (std::find(a.selection.begin(), a.selection.end(),
                                         a.sim.units[hit].id) == a.selection.end() ||
                               a.selection.size() != 1) {
                        a.selection.assign(1, a.sim.units[hit].id);
                    }
                    a.last_click_unit = hit;
                    a.sel_building_id = 0;
                    a.box_active = false;
                } else {
                    // 单位未命中：建筑拾取（按地基格命中——引擎网格矩形与实际
                    // 菱形地基不一致，曾导致部分建筑点不中）
                    int bhit = -1;
                    for (int i = 0; i < static_cast<int>(a.sim.buildings.size()); ++i) {
                        const auto& b = a.sim.buildings[i];
                        if (!b.alive) continue;
                        bool on_cell = false;
                        for (const auto& c : b.footprint_cells)
                            if (c.first == cx && c.second == cy) {
                                on_cell = true;
                                break;
                            }
                        if (on_cell) {
                            bhit = i;
                            break;
                        }
                    }
                    if (bhit >= 0) {
                        a.selection.clear();
                        a.sel_building_id = a.sim.buildings[bhit].id;
                        a.last_click_unit = -1;
                        a.box_active = false;
                    } else {
                        // 空地按下：开始框选（松开时结算；微小位移视为点空清除）
                        a.box_active = true;
                        a.box_x0 = mp.x;
                        a.box_y0 = mp.y;
                    }
                }
                a.dirty = true;
            } else if (cx >= 0 && cy >= 0 && cx < a.map.w && cy < a.map.h &&
                       a.obj_sel[a.obj_cat] >= 0) {
                a.objects.push_back({kCatKind[a.obj_cat],
                                     a.obj_lists[a.obj_cat][a.obj_sel[a.obj_cat]], cx, cy,
                                     static_cast<uint8_t>(a.obj_dir), 0,
                                     static_cast<int>(a.map.cell(cx, cy).height)});
                a.dirty = true;
                std::fprintf(stderr, "[stage] 放置 %s 于 (%d,%d)\n",
                             a.obj_lists[a.obj_cat][a.obj_sel[a.obj_cat]].c_str(), cx, cy);
            }
        }
        if (view_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const int lx = static_cast<int>((mp.x - pos.x - a.pan_x) / a.zoom);
            const int ly = static_cast<int>((mp.y - pos.y - a.pan_y) / a.zoom);
            const IsometricGrid grid;
            int cx, cy;
            grid.pixel_to_cell(lx - a.ox, ly - a.oy, cx, cy);
            if (a.sim_active) {
                if (a.sk.active && a.placing) {
                    // 放置模式：右键取消（原版行为）
                    a.placing = false;
                    a.dirty = true;
                    std::fprintf(stderr, "[stage] 取消放置\n");
                } else if (a.selection.empty() && a.sel_building_id != 0 &&
                           cx >= 0 && cy >= 0 && cx < a.map.w && cy < a.map.h) {
                    // 选中防御建筑（无单位选中）：右键敌方单位 → 命令炮塔攻击该
                    // 方向；右键空地 → 停止攻击（恢复自动索敌）
                    int sbi = -1;
                    for (int i = 0; i < static_cast<int>(a.sim.buildings.size()); ++i)
                        if (a.sim.buildings[i].id == a.sel_building_id) {
                            sbi = i;
                            break;
                        }
                    if (sbi >= 0 && a.sim.buildings[sbi].alive) {
                        int hu = -1;
                        for (int i = 0; i < static_cast<int>(a.sim.units.size()); ++i)
                            if (a.sim.units[i].alive && a.sim.units[i].col == cx &&
                                a.sim.units[i].row == cy) {
                                hu = i;
                                break;
                            }
                        if (hu >= 0 && a.sim.units[hu].owner != a.sim.buildings[sbi].owner) {
                            if (a.sim.issue_build_attack(static_cast<size_t>(sbi),
                                                         static_cast<size_t>(hu)))
                                std::fprintf(stderr, "[stage] %s 攻击 %s\n",
                                             a.sim.buildings[sbi].type.c_str(),
                                             a.sim.units[hu].type.c_str());
                            else
                                std::fprintf(stderr, "[stage] 该建筑无武器（非防御建筑）\n");
                        } else {
                            a.sim.stop_build_attack(static_cast<size_t>(sbi));
                            std::fprintf(stderr, "[stage] %s 停止攻击（自动索敌）\n",
                                         a.sim.buildings[sbi].type.c_str());
                        }
                        a.dirty = true;
                    }
                } else if (a.selection.empty() || cx < 0 || cy < 0 || cx >= a.map.w ||
                           cy >= a.map.h) {
                    // 无选中/出界：忽略
                } else {
                    // id → 单位下标
                    const auto uid_of = [&](uint32_t sid) -> int {
                        for (int i = 0; i < static_cast<int>(a.sim.units.size()); ++i)
                            if (a.sim.units[i].id == sid) return i;
                        return -1;
                    };
                    int hit_unit = -1, hit_bld = -1;
                    for (int i = 0; i < static_cast<int>(a.sim.units.size()); ++i) {
                        if (a.sim.units[i].col == cx && a.sim.units[i].row == cy) {
                            hit_unit = i;
                            break;
                        }
                    }
                    for (int i = 0; i < static_cast<int>(a.sim.buildings.size()); ++i) {
                        if (a.sim.buildings[i].col == cx && a.sim.buildings[i].row == cy) {
                            hit_bld = i;
                            break;
                        }
                    }
                    // 选中方阵营（同 House 多选，取第一个选中单位）
                    std::string own_owner;
                    for (const uint32_t sid : a.selection) {
                        for (const auto& u : a.sim.units) {
                            if (u.id == sid) {
                                own_owner = u.owner;
                                break;
                            }
                        }
                        if (!own_owner.empty()) break;
                    }
                    const bool shift = ImGui::GetIO().KeyShift;
                    if (shift && !a.sim.units.empty()) {
                        // Shift+右键：追加巡逻点（循环巡逻）
                        for (const uint32_t sid : a.selection) {
                            const int ui = uid_of(sid);
                            if (ui >= 0) a.sim.add_waypoint(static_cast<size_t>(ui), cx, cy);
                        }
                    } else if (hit_unit >= 0 &&
                               a.sim.units[hit_unit].owner == own_owner) {
                        // 右键友军 → 护卫
                        for (const uint32_t sid : a.selection) {
                            const int ui = uid_of(sid);
                            if (ui >= 0)
                                a.sim.issue_guard(static_cast<size_t>(ui),
                                                  static_cast<size_t>(hit_unit));
                        }
                    } else if (hit_unit >= 0) {
                        // 右键敌方单位 → 攻击
                        for (const uint32_t sid : a.selection) {
                            const int ui = uid_of(sid);
                            if (ui >= 0)
                                a.sim.issue_attack_unit(static_cast<size_t>(ui),
                                                        static_cast<size_t>(hit_unit));
                        }
                    } else if (hit_bld >= 0) {
                        // 右键建筑 → 攻击
                        for (const uint32_t sid : a.selection) {
                            const int ui = uid_of(sid);
                            if (ui >= 0)
                                a.sim.issue_attack_building(static_cast<size_t>(ui),
                                                            static_cast<size_t>(hit_bld));
                        }
                    } else {
                        // 移动指令：编队流场——点击格 + 周围可走格为槽位，多单位
                        // 一次流场构建后就近落位（不再逐单位散开到可能被挡的格）
                        std::vector<size_t> idx;
                        idx.reserve(a.selection.size());
                        for (const uint32_t sid : a.selection) {
                            const int ui = uid_of(sid);
                            if (ui >= 0) idx.push_back(static_cast<size_t>(ui));
                        }
                        const size_t n = a.sim.issue_move_group(idx, cx, cy);
                        std::fprintf(stderr, "[stage] 编队移动 %zu 单位 → (%d,%d)：%zu 下达\n",
                                     idx.size(), cx, cy, n);
                    }
                    a.dirty = true;
                    std::fprintf(stderr, "[stage] 指令: 单位%d 建筑%d shift=%d\n", hit_unit,
                                 hit_bld, shift ? 1 : 0);
                }
            } else {
                a.objects.erase(
                    std::remove_if(a.objects.begin(), a.objects.end(), [&](const auto& o) {
                        return o.cx == cx && o.cy == cy;
                    }),
                    a.objects.end());
                a.dirty = true;
            }
        }
        // 框选结算：模拟模式左键空地拖拽松开 → 按格范围框选（微小位移=点空清除）
        if (a.sim_active && !a.build_mode && a.box_active &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            a.box_active = false;
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const float dx = mp.x - a.box_x0, dy = mp.y - a.box_y0;
            if (dx * dx + dy * dy > 16.0f) {
                const auto to_cell = [&](float sx, float sy) {
                    const int lx = static_cast<int>((sx - pos.x - a.pan_x) / a.zoom);
                    const int ly = static_cast<int>((sy - pos.y - a.pan_y) / a.zoom);
                    const IsometricGrid grid;
                    int cc, cr;
                    grid.pixel_to_cell(lx - a.ox, ly - a.oy, cc, cr);
                    return std::pair<int, int>{cc, cr};
                };
                const auto [c0x, c0y] = to_cell(a.box_x0, a.box_y0);
                const auto [c1x, c1y] = to_cell(mp.x, mp.y);
                if (c0x < 0 || c1x < 0) {
                    a.selection.clear();
                } else {
                    const int x0 = std::min(c0x, c1x), x1 = std::max(c0x, c1x);
                    const int y0 = std::min(c0y, c1y), y1 = std::max(c0y, c1y);
                    a.selection.clear();
                    for (const auto& u : a.sim.units) {
                        if (!u.alive) continue;
                        // 遭遇战只框选己方（Player）；沙盘（地图装载）模式不过滤
                        if (a.sk.active && u.owner != "Player") continue;
                        if (u.col >= x0 && u.col <= x1 && u.row >= y0 && u.row <= y1)
                            a.selection.push_back(u.id);
                    }
                    std::fprintf(stderr, "[stage] 框选 (%d,%d)-(%d,%d) → %zu 单位\n", x0, y0,
                                 x1, y1, a.selection.size());
                }
            } else {
                a.selection.clear();
                a.last_click_unit = -1;
            }
            a.dirty = true;
        }
        if (a.bw > 0 && a.bh > 0) {
            // 绘制（平移/缩放应用，AddImage 直接给屏幕坐标）
            ImGui::GetWindowDrawList()->AddImage(
                host.texture_id(), ImVec2(pos.x + a.pan_x, pos.y + a.pan_y),
                ImVec2(pos.x + a.pan_x + static_cast<float>(a.bw) * a.zoom,
                       pos.y + a.pan_y + static_cast<float>(a.bh) * a.zoom));
            // 框选：左键拖拽中画绿色矩形边框（松开时按矩形内单位结算）
            if (a.box_active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                ImGui::GetWindowDrawList()->AddRect(
                    ImVec2(a.box_x0, a.box_y0), mp, IM_COL32(60, 255, 90, 255), 0.0f, 0, 1.5f);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(a.box_x0, a.box_y0), mp, IM_COL32(60, 255, 90, 28));
            }
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + 8, pos.y + 4), IM_COL32(255, 255, 160, 255),
                a.sim_active ? "中键拖拽平移 · 滚轮缩放 · 左键框选/点选（Shift 追加、Ctrl 移出）"
                               " · 右键移动/攻击/护卫 · Shift+右键巡逻 · S 停止 · Ctrl+1..9 编队"
                             : "中键拖拽平移 · 滚轮缩放 · 左键放置 · 右键删除");
        }
        ImGui::EndChild();

        ImGui::End(); // stage_main
        } // else: ready → 主界面
        ++frame;
        if (shot_path && !test_mode && frame == 10) {
            // 窗口截图（读当前呈现帧；end_frame 之前）
            int ww = 0, wh = 0;
            SDL_GetWindowSize(host.window(), &ww, &wh);
            std::vector<uint8_t> buf;
            if (host.read_back(ww, wh, buf)) {
                write_bmp(shot_path, ww, wh, buf);
                std::printf("shot saved -> %s\n", shot_path);
            }
            running = false;
        }
        host.end_frame();
        if (test_mode && frame == 8) {
            // 读回画布纹理（含对象）转储（算法图 + 样本对象）
            render_all(a, &err);
            const auto dump_tex = [&](const std::string& path) {
                std::vector<uint8_t> buf;
                if (!host.read_texture(a.bw, a.bh, buf)) return;
                write_bmp(path, a.bw, a.bh, buf);
                std::printf("test dump saved -> %s (%dx%d)\n", path.c_str(), a.bw, a.bh);
                std::printf("objects: buildings=%d units=%d infantry=%d skipped=%d\n",
                            a.obj_stats.buildings, a.obj_stats.units, a.obj_stats.infantry,
                            a.obj_stats.skipped);
            };
            dump_tex(shot_path);
            running = !a.map_files.empty() && flat_w == 0; // 有地图文件则下一帧再验证加载模式（纯平实验跳过）
        }
        if (test_mode && frame == 9) {
            // 加载模式验证：加载游戏目录第一张地图并转储（--map 指定时用指定图）
            a.mode = 2;
            if (map_arg.empty()) a.map_sel = 0;
            generate_map(a, &err);
            // 遭遇战模式：地图重载后重新开局（generate_map 会清空 sim）
            if (a.sk_autostart && a.map_sel >= 0) {
                std::string skerr;
                if (!start_skirmish(a, &skerr))
                    std::fprintf(stderr, "skirmish start failed: %s\n", skerr.c_str());
            }
            if (no_obj) a.objects.clear(); // --noobj：纯地形基线（与对象版逐像素对照）
            render_all(a, &err);
            a.dirty = false;
            std::printf("load map: %s (%dx%d theater=%s)\n",
                        a.map_files[a.map_sel].c_str(), a.map.w, a.map.h, a.map.theater.c_str());
            // 加载图 cells CSV（对象坐标假说裁决用：候选格地形对照）
            {
                const std::string ccsv = std::string(shot_path) + ".loadcells.csv";
                FILE* f = std::fopen(ccsv.c_str(), "w");
                if (f) {
                    std::fprintf(f, "cx,cy,tile_id,subtile,height,name\n");
                    for (int y = 0; y < a.map.h; ++y)
                        for (int x = 0; x < a.map.w; ++x) {
                            const auto& c = a.map.cell(x, y);
                            std::fprintf(f, "%d,%d,%u,%u,%u,%s\n", x, y, c.tile_id, c.subtile,
                                         c.height, a.tileset.name_for(c.tile_id).c_str());
                        }
                    std::fclose(f);
                    std::printf("load cells csv -> %s\n", ccsv.c_str());
                }
            }
            // 对象坐标范围 + 明细导出（验证格栅序号换算是否把对象散布到全图）
            if (!a.objects.empty()) {
                int x0 = a.objects[0].cx, x1 = x0, y0 = a.objects[0].cy, y1 = y0;
                for (const auto& o : a.objects) {
                    x0 = std::min(x0, o.cx);
                    x1 = std::max(x1, o.cx);
                    y0 = std::min(y0, o.cy);
                    y1 = std::max(y1, o.cy);
                }
                std::printf("obj range: cx[%d..%d] cy[%d..%d] of map %dx%d\n", x0, x1, y0, y1,
                            a.map.w, a.map.h);
                const std::string ocsv = std::string(shot_path) + ".objs.csv";
                FILE* f = std::fopen(ocsv.c_str(), "w");
                if (f) {
                    std::fprintf(f, "kind,id,cx,cy,dir\n");
                    for (const auto& o : a.objects)
                        std::fprintf(f, "%d,%s,%d,%d,%d\n", o.kind, o.id.c_str(), o.cx, o.cy,
                                     o.dir);
                    std::fclose(f);
                    std::printf("objs csv -> %s\n", ocsv.c_str());
                }
            }
            {
                std::vector<uint8_t> buf;
                const std::string lp = std::string(shot_path) + ".load.bmp";
                if (host.read_texture(a.bw, a.bh, buf)) {
                    write_bmp(lp, a.bw, a.bh, buf);
                    std::printf("test dump saved -> %s (%dx%d)\n", lp.c_str(), a.bw, a.bh);
                    std::printf("objects: buildings=%d units=%d infantry=%d skipped=%d\n",
                                a.obj_stats.buildings, a.obj_stats.units, a.obj_stats.infantry,
                                a.obj_stats.skipped);
                }
            }
            // ── M3 模拟自检：脚本化指令 → 推进 N 逻辑帧 → 转储对照 ──
            if (a.sim_steps > 0 && a.sim_active && !a.sim.units.empty()) {
                int enemy = -1;
                uint32_t demo_war_a = 0, demo_war_b = 0; // 演示双方战斗单位 id
                if (a.sim_attack) {
                    // 攻击脚本：第一个非采矿单位为攻击方，其后第一个非采矿单位为
                    // 防守方（合成敌对 House AtkA/AtkB，保证同图可达）
                    int atk = -1;
                    for (int i = 0; i < static_cast<int>(a.sim.units.size()); ++i)
                        if (!a.sim.units[i].is_miner) {
                            atk = i;
                            break;
                        }
                    if (atk < 0) atk = 0;
                    int tgt = -1;
                    for (int i = atk + 1; i < static_cast<int>(a.sim.units.size()); ++i)
                        if (!a.sim.units[i].is_miner) {
                            tgt = i;
                            break;
                        }
                    if (tgt < 0) {
                        std::printf("sim: no second combat unit (attack script skipped)\n");
                    } else {
                        a.sim.set_unit_owner(static_cast<size_t>(atk), "AtkA");
                        a.sim.set_unit_owner(static_cast<size_t>(tgt), "AtkB");
                        enemy = tgt;
                        const auto& u0 = a.sim.units[atk];
                        const auto& te = a.sim.units[tgt];
                        const bool ok = a.sim.issue_attack_unit(static_cast<size_t>(atk),
                                                                static_cast<size_t>(tgt));
                        std::printf("sim: %s(AtkA)@(%d,%d) attack %s(AtkB)@(%d,%d) "
                                    "dmg=%d rof=%d range=%d path=%s\n",
                                    u0.type.c_str(), u0.col, u0.row, te.type.c_str(), te.col,
                                    te.row, u0.weapon.damage, u0.weapon.rof, u0.weapon.range,
                                    ok ? "ok" : "fail");
                    }
                } else if (a.sim_build) {
                    // 建造脚本：单位0 阵营起造 GAPOWR（近旁 2×2 空地）
                    const auto& u0 = a.sim.units[0];
                    int bx = -1, by = -1;
                    for (int rad = 1; rad < 40 && bx < 0; ++rad) {
                        for (int dy = -rad; dy <= rad && bx < 0; ++dy) {
                            for (int dx = -rad; dx <= rad && bx < 0; ++dx) {
                                const int x = u0.col + dx, y = u0.row + dy;
                                // 空地基校验走 can_place（含单位/覆盖物/矿石占格）
                                if (a.sim.can_place(x, y, 2, 2)) {
                                    bx = x;
                                    by = y;
                                }
                            }
                        }
                    }
                    if (bx < 0) {
                        std::printf("sim: build spot not found\n");
                    } else {
                        const auto* bu = a.rules.unit("GAPOWR");
                        const int cost = bu ? bu->cost : 300;
                        const int power = bu ? bu->power : 200;
                        const bool ok = a.sim.issue_build(
                            u0.owner, "GAPOWR", bx, by, 2, 2, cost,
                            std::max(1, onsite_ticks(a, "GAPOWR")), power);
                        std::printf("sim: build GAPOWR @(%d,%d) for %s $%d power%+d %s\n", bx,
                                    by, u0.owner.c_str(), cost, power, ok ? "ok" : "fail");
                    }
                } else if (a.sim_demo) {
                    // M3 验收演示：两个合成阵营（各 1 采矿车 + 1 战斗单位）
                    // 从 $10000 起步，一次性下达 电厂→兵营→重工（T2）建设令，
                    // t=1000/1100 互相进攻（脚本在推进循环内触发）
                    if (a.sim.units.size() < 5) {
                        std::printf("sim: demo needs >=5 units, only %zu\n",
                                    a.sim.units.size());
                    } else {
                        a.sim.set_unit_owner(0, "PlayerA");
                        a.sim.set_unit_owner(3, "PlayerA");
                        a.sim.set_unit_owner(1, "PlayerB");
                        a.sim.set_unit_owner(4, "PlayerB");
                        demo_war_a = a.sim.units[3].id;
                        demo_war_b = a.sim.units[4].id;
                        // 基地选址：矿车附近空地（电厂 2 + 兵营 3 + 重工 5 宽，
                        // 按真实地基宽顺序排开）
                        const int kBaseW =
                            (a.rules.unit("GAPOWR") ? a.rules.unit("GAPOWR")->fw : 2) +
                            (a.rules.unit("GAPILE") ? a.rules.unit("GAPILE")->fw : 3) +
                            (a.rules.unit("GAWEAP") ? a.rules.unit("GAWEAP")->fw : 5);
                        const auto free_rect = [&](int near_col, int near_row) {
                            for (int rad = 3; rad < 80; ++rad)
                                for (int dy = -rad; dy <= rad; ++dy)
                                    for (int dx = -rad; dx <= rad; ++dx) {
                                        const int x = near_col + dx, y = near_row + dy;
                                        bool free = true;
                                        for (int j = 0; j < 2 && free; ++j)
                                            for (int i = 0; i < kBaseW && free; ++i) {
                                                if (x + i < 0 || y + j < 0 ||
                                                    x + i >= a.sim.w || y + j >= a.sim.h ||
                                                    a.sim.blocked[static_cast<size_t>(y + j) *
                                                                               a.sim.w +
                                                                           (x + i)])
                                                    free = false;
                                            }
                                        if (free) return std::pair<int, int>{x, y};
                                    }
                            return std::pair<int, int>{-1, -1};
                        };
                        const auto base_a =
                            free_rect(a.sim.units[0].col, a.sim.units[0].row);
                        if (base_a.first < 0) {
                            std::printf("sim: demo base rect not found\n");
                        } else {
                            const auto build_at = [&](const std::string& owner,
                                                      const std::string& type, int x, int y) {
                                const auto* u = a.rules.unit(type);
                                const int fw = u ? u->fw : 1;
                                const int fh = u ? u->fh : 1;
                                const int cost = u ? u->cost : 300;
                                const int power = u ? u->power : 0;
                                const bool ok = a.sim.issue_build(
                                    owner, type, x, y, fw, fh, cost,
                                    std::max(1, onsite_ticks(a, type)), power);
                                std::printf("sim: demo %s build %s @(%d,%d) $%d %s\n",
                                            owner.c_str(), type.c_str(), x, y, cost,
                                            ok ? "ok" : "fail");
                            };
                            int bx2 = base_a.first;
                            build_at("PlayerA", "GAPOWR", bx2, base_a.second);
                            bx2 += a.rules.unit("GAPOWR")->fw;
                            build_at("PlayerA", "GAPILE", bx2, base_a.second);
                            bx2 += a.rules.unit("GAPILE")->fw;
                            build_at("PlayerA", "GAWEAP", bx2, base_a.second);
                            // B 基地在 A 落成之后选址（A 的地基已占位，避免重叠）
                            const auto base_b =
                                free_rect(a.sim.units[1].col, a.sim.units[1].row);
                            if (base_b.first < 0) {
                                std::printf("sim: demo base B rect not found\n");
                            } else {
                                int bx3 = base_b.first;
                                build_at("PlayerB", "GAPOWR", bx3, base_b.second);
                                bx3 += a.rules.unit("GAPOWR")->fw;
                                build_at("PlayerB", "GAPILE", bx3, base_b.second);
                                bx3 += a.rules.unit("GAPILE")->fw;
                                build_at("PlayerB", "GAWEAP", bx3, base_b.second);
                            }
                            std::printf("sim: demo bases A@(%d,%d) B@(%d,%d) 交战 t=1000/1100\n",
                                        base_a.first, base_a.second, base_b.first,
                                        base_b.second);
                        }
                    }
                } else if (a.sk.active) {
                    // ── 遭遇战自检脚本：展开基地车 → 排队建造 → 落点（AI 同步推进）──
                    int mcv_i = -1;
                    for (int i = 0; i < static_cast<int>(a.sim.units.size()); ++i) {
                        const auto& u = a.sim.units[i];
                        if (u.owner != "Player") continue;
                        const auto* t = a.rules.unit(u.type);
                        if (t && !t->deploys_into.empty()) {
                            mcv_i = i;
                            break;
                        }
                    }
                    if (mcv_i >= 0) {
                        a.selection.assign(1, a.sim.units[mcv_i].id);
                        std::printf("sim: 玩家基地车 %s @(%d,%d) 已选中\n",
                                    a.sim.units[mcv_i].type.c_str(), a.sim.units[mcv_i].col,
                                    a.sim.units[mcv_i].row);
                    }
                    // 玩家建造序列按角色取本阵营建筑（盟/苏/尤里通用）
                    const auto pbuild = [&](const char* role) -> std::string {
                        const auto* t = faction_building(a, "Player", a.sk.cfg.player.country, role);
                        return t ? t->name : std::string();
                    };
                    for (int s = 0; s < a.sim_steps; ++s) {
                        if (s == 30) deploy_selected_mcv(a);
                        if (s == 120) queue_player_build(a, pbuild("power"));
                        if (s == 1000) queue_player_build(a, pbuild("refinery"));
                        if (s == 3100) queue_player_build(a, pbuild("barracks"));
                        if (s == 3700) queue_player_build(a, pbuild("weapon"));
                        if (a.sim.build_ready("Player")) {
                            const auto qit = a.sim.build_queue.find("Player");
                            const auto* bt = qit != a.sim.build_queue.end()
                                                 ? a.rules.unit(qit->second.type)
                                                 : nullptr;
                            int cc = -1, cr = -1;
                            for (const auto& b : a.sim.buildings) {
                                const auto* t2 = a.rules.unit(b.type);
                                if (b.owner == "Player" && t2 && t2->construction_yard) {
                                    cc = b.col;
                                    cr = b.row;
                                    break;
                                }
                            }
                            if (bt && cc >= 0) {
                                int bx = -1, by = -1;
                                for (int rad = 0; rad < 24 && bx < 0; ++rad)
                                    for (int dy = -rad; dy <= rad && bx < 0; ++dy)
                                        for (int dx = -rad; dx <= rad && bx < 0; ++dx) {
                                            if (rad > 0 && std::abs(dx) != rad &&
                                                std::abs(dy) != rad)
                                                continue;
                                            if (a.sim.can_place(cc + dx, cr + dy, bt->fw,
                                                                bt->fh)) {
                                                bx = cc + dx;
                                                by = cr + dy;
                                            }
                                        }
                                if (bx >= 0) place_player_build(a, bx, by);
                            }
                        }
                        skirmish_ai_tick(a);
                        a.sim.tick();
                    }
                    // 放置预览自检：有就绪建筑时进入放置模式（截图应出现地基菱形）。
                    // 队列里有**在建项**时不得覆盖（那会抹掉真实生产状态 → 生产
                    // 动画/进度归零）；只在队列空时注入一个假的就绪项。
                    if (!a.sim.build_ready("Player") && !a.sim.build_queue.count("Player")) {
                        // 自检注入：合成一个"就绪"项用于预览渲染（不占资金）
                        const std::string pw = pbuild("power");
                        if (!pw.empty())
                            a.sim.build_queue["Player"] = {pw, 1, 1, 0, true};
                    }
                    if (a.sim.build_ready("Player")) {
                        const auto qit = a.sim.build_queue.find("Player");
                        const auto* bt = qit != a.sim.build_queue.end()
                                             ? a.rules.unit(qit->second.type)
                                             : nullptr;
                        int cc = -1, cr = -1;
                        for (const auto& b : a.sim.buildings)
                            if (b.owner == "Player") {
                                cc = b.col;
                                cr = b.row;
                                break;
                            }
                        if (bt && cc >= 0) {
                            for (int rad = 0; rad < 24 && !a.placing; ++rad)
                                for (int dy = -rad; dy <= rad && !a.placing; ++dy)
                                    for (int dx = -rad; dx <= rad && !a.placing; ++dx) {
                                        if (a.sim.can_place(cc + dx, cr + dy, bt->fw, bt->fh)) {
                                            a.placing = true;
                                            a.hover_cx = cc + dx;
                                            a.hover_cy = cr + dy;
                                        }
                                    }
                        }
                        std::printf("sim: 放置预览 %s（格 %d,%d）\n",
                                    a.placing ? "已开启" : "未找到落点", a.hover_cx, a.hover_cy);
                    }
                    for (const char* p : {"Player", "Opponent"}) {
                        int blds = 0, done = 0, nunit = 0;
                        for (const auto& b : a.sim.buildings)
                            if (b.owner == p) {
                                ++blds;
                                if (!b.under_construction) ++done;
                            }
                        for (const auto& u : a.sim.units)
                            if (u.owner == p) ++nunit;
                        std::printf(
                            "sim: 遭遇战 %s 建筑 %d/%d 完成 单位 %d 资金 $%lld 电力 %+d\n", p,
                            done, blds, nunit,
                            static_cast<long long>(a.sim.credits.count(p) ? a.sim.credits[p] : 0),
                            a.sim.power_net.count(p) ? a.sim.power_net[p] : 0);
                    }
                    for (const auto& b : a.sim.buildings)
                        std::printf("sim: 建筑 %-7s %-9s @(%2d,%2d) %dx%d %s hp=%d/%d\n",
                                    b.type.c_str(), b.owner.c_str(), b.col, b.row, b.fw, b.fh,
                                    b.under_construction ? "建造中" : "完成", b.hp, b.max_hp);
                } else {
                    ra2r::sim::SimUnit& u0 = a.sim.units[0];
                    const int tc = std::min(u0.col + 10, a.map.w - 1);
                    const int tr = std::min(u0.row + 6, a.map.h - 1);
                    const bool ok = a.sim.issue_move(0, tc, tr);
                    std::printf("sim: unit0 %s (%d,%d) -> (%d,%d) path=%s\n",
                                u0.type.c_str(), u0.col, u0.row, tc, tr, ok ? "ok" : "fail");
                }
                a.selection.assign(1, a.sim.units[0].id);
                // 推进循环（演示脚本按时间轴注入事件）
                // 遭遇战分支自带推进循环，此处不得重复推进（否则实际 2N 帧）
                const int post_steps = a.sk.active ? 0 : a.sim_steps;
                for (int s = 0; s < post_steps; ++s) {
                    if (a.sim_demo) {
                        const auto uid_of2 = [&](uint32_t id) -> int {
                            for (int i = 0; i < static_cast<int>(a.sim.units.size()); ++i)
                                if (a.sim.units[i].id == id) return i;
                            return -1;
                        };
                        if (s == 1000) {
                            const int ia = uid_of2(demo_war_a), ib = uid_of2(demo_war_b);
                            if (ia >= 0 && ib >= 0) {
                                a.sim.issue_attack_unit(static_cast<size_t>(ia),
                                                        static_cast<size_t>(ib));
                                std::printf("sim: demo t=1000 PlayerA 进攻\n");
                            }
                        }
                        if (s == 1100) {
                            const int ia = uid_of2(demo_war_a), ib = uid_of2(demo_war_b);
                            if (ia >= 0 && ib >= 0) {
                                a.sim.issue_attack_unit(static_cast<size_t>(ib),
                                                        static_cast<size_t>(ia));
                                std::printf("sim: demo t=1100 PlayerB 反攻\n");
                            }
                        }
                    }
                    a.sim.tick();
                }
                const auto& u1 = a.sim.units[0];
                std::printf("sim: after %d ticks unit0 %s at (%d,%d) frac=%d hp=%d "
                            "moving=%d order=%d miner=%d cargo=%d/%d target=%d explosions=%zu\n",
                            a.sim_steps, u1.type.c_str(), u1.col, u1.row, u1.frac, u1.hp,
                            a.sim.unit_moving(0) ? 1 : 0, u1.order, u1.is_miner ? 1 : 0,
                            u1.cargo, u1.capacity, u1.target, a.sim.explosions.size());
                if (enemy >= 0 && enemy < static_cast<int>(a.sim.units.size())) {
                    const auto& te = a.sim.units[enemy];
                    std::printf("sim: target %s at (%d,%d) hp=%d alive=%d\n", te.type.c_str(),
                                te.col, te.row, te.hp, te.alive ? 1 : 0);
                }
                {
                    int64_t ore_total = 0;
                    for (const auto v : a.sim.ore) ore_total += v;
                    std::printf("sim: ore_total=%lld", static_cast<long long>(ore_total));
                    for (const auto& [owner, amt] : a.sim.credits)
                        std::printf(" %s=$%lld", owner.c_str(), static_cast<long long>(amt));
                    std::printf("\n");
                }
                if (a.sim_build && !a.sim.buildings.empty()) {
                    const auto& nb = a.sim.buildings.back();
                    std::printf("sim: building %s @(%d,%d) under=%d ticks=%d/%d hp=%d\n",
                                nb.type.c_str(), nb.col, nb.row,
                                nb.under_construction ? 1 : 0, nb.build_ticks, nb.build_total,
                                nb.hp);
                    for (const auto& [owner, pw] : a.sim.power_net)
                        std::printf("sim: power %s=%+d\n", owner.c_str(), pw);
                }
                if (a.sim_demo) {
                    const auto uid_of2 = [&](uint32_t id) -> int {
                        for (int i = 0; i < static_cast<int>(a.sim.units.size()); ++i)
                            if (a.sim.units[i].id == id) return i;
                        return -1;
                    };
                    for (const char* p : {"PlayerA", "PlayerB"}) {
                        int blds = 0, done = 0;
                        for (const auto& b : a.sim.buildings)
                            if (b.owner == p) {
                                ++blds;
                                if (!b.under_construction) ++done;
                            }
                        const int wi = uid_of2(p == std::string("PlayerA") ? demo_war_a
                                                                            : demo_war_b);
                        std::printf("sim: demo %s 建筑 %d/%d 完成 资金 $%lld 电力 %+d",
                                    p, done, blds,
                                    static_cast<long long>(a.sim.credits[p]),
                                    a.sim.power_net[p]);
                        if (wi >= 0) {
                            const auto& w = a.sim.units[wi];
                            std::printf(" 战斗单位 %s hp=%d @(%d,%d)", w.type.c_str(), w.hp,
                                        w.col, w.row);
                        } else {
                            std::printf(" 战斗单位 阵亡");
                        }
                        std::printf("\n");
                    }
                }
                render_all(a, &err);
                const std::string sp = std::string(shot_path) + ".sim.bmp";
                std::vector<uint8_t> buf2;
                if (host.read_texture(a.bw, a.bh, buf2)) {
                    write_bmp(sp, a.bw, a.bh, buf2);
                    std::printf("sim dump saved -> %s (%dx%d)\n", sp.c_str(), a.bw, a.bh);
                }
                if (bench_arg > 0) run_bench(a, bench_arg); // --bench：先测再优化
            }
            running = false;
        }
    }

    host.shutdown();
    ImGui::DestroyContext();
    SDL_Quit();
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) { return ra2r::core::run_wide(run, argc, wargv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
