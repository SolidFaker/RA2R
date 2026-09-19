#pragma once
// RA2R stage — 舞台应用状态与资源/渲染流程（UI 事件循环留在 main.cpp）
//
// 职责：应用状态聚合 + 资源重建、类型表解析、地图生成/加载、全图渲染、
// 地图扫描与 BMP 写出。所有算法复用引擎模块，本文件只做舞台粘合。
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "ra2r/assets/file_index.h"
#include "ra2r/assets/rules_db.h"
#include "ra2r/assets/tileset.h"
#include "ra2r/cache/cache_manager.h"
#include "ra2r/render/object_layer.h"
#include "ra2r/render/palette_lut.h"
#include "ra2r/render/terrain_tile.h"
#include "ra2r/sim/sim_world.h"
#include "ra2r/sim/skirmish.h"
#include "ra2r/ui/backend.h"
#include "stage/stage_map.h"

namespace stage {

// 遭遇战状态（battle1.yrm 等多人图：玩家 + AI 各一基地）
struct SkirmishState {
    bool active = false;
    ra2r::sim::SkirmishCfg cfg;      // 玩家/对手阵营与阵营色
    std::vector<ra2r::assets::HouseRamp> ramps; // [0]=玩家 [1]=对手
    std::map<std::string, uint8_t> house_remap; // House 名 → ramps 下标+1（0 = 不重映射）
    int player_wp = 0, opponent_wp = 1;         // 出生点 waypoint 下标
    int ai_deploy_at = 240;                     // AI 展开基地车的逻辑帧时刻
    int ai_build_at = 420;                      // AI 起造首个建筑的时刻
    bool ai_done = false;                       // AI 脚本已跑完
    std::vector<std::string> player_countries;  // 可选国家（Multiplay=yes）
    std::vector<std::string> color_names;       // 可选阵营色（[Colors]）
    int country_sel = 0, color_sel = 0, class_sel = 2; // UI 选择（对应上面两张表）
    int tech_level = 10;
    int64_t credits = 10000;
};

// ── 应用状态 ──
struct StageApp {
    // 资源
    ra2r::assets::FileIndex index;
    ra2r::assets::TerrainTileset tileset;
    ra2r::render::PaletteLut terrain_lut, unit_lut;
    ra2r::render::PaletteLut resource_lut; // TEMPERAT.PAL（矿石/宝石资源盘）
    ra2r::cache::CacheManager cache;
    std::map<std::string, std::vector<uint8_t>> file_cache;
    std::map<int, std::string> overlay_names; // RULESMD [OverlayTypes] 类型号 → 名
    // 舞台
    StageMap map;
    std::vector<ra2r::render::PlacedObject> objects;
    // M3 模拟层（加载地图模式：单位/步兵来自 sim；算法/平坦模式无 sim）
    ra2r::sim::SimWorld sim;
    bool sim_active = false;
    std::vector<uint32_t> selection; // sim.units 的 id（单位死亡后下标会变，用 id）
    int last_click_unit = -1;        // 上次点中的单位下标（双击同型全选判定）
    uint32_t sel_building_id = 0;    // 选中的建筑 id（修理/出售目标；0 = 无）
    // 规则数据库：rulesmd/artmd 全量载入 + 类型化查询（建筑/载具/步兵/武器统一抽象）
    ra2r::assets::RulesDB rules;
    // M3 建造 UI 状态
    bool build_mode = false; // 建造模式：左键在模拟图上起造
    int build_sel = -1;      // 建造面板选中的建筑类型（obj_lists[0] 下标）
    std::map<int, std::vector<uint32_t>> groups; // 编队：1..9 → 单位 id 表
    // 框选拖拽状态（模拟模式左键空地按下 → 松开按框选）
    bool box_active = false;
    float box_x0 = 0, box_y0 = 0;
    int sim_steps = -1; // --simsteps N：无头自检推进 N 逻辑帧后转储（-1=关闭）
    bool sim_attack = false; // --simattack：自检用攻击脚本（单位0 攻击首个敌对单位）
    bool sim_build = false;  // --simbuild：自检用建造脚本（单位0 阵营起造 GAPOWR）
    bool sim_demo = false;   // --simdemo：M3 验收演示（1v1 建设到 T2 + 交战）
    bool perf_log = false;   // --perf：渲染各阶段耗时打印（性能诊断）
    // M4 遭遇战（battle1.yrm 等多人图）
    SkirmishState sk;
    bool sk_autostart = false; // --skirmish：载入地图后自动开局
    bool sk_ai = true;         // 对手 AI 自动展开/建造
    int queue_sel = -1;        // 侧边栏选中的建筑（obj_lists[0] 下标）
    bool placing = false;      // 待放置（建造完成 → 地图上落点）
    int hover_cx = -1, hover_cy = -1; // 放置预览格
    int sel_building = -1;     // 选中的建筑（sim.buildings 下标；UI 用）
    // UI 状态
    int theater_sel = 0;
    int mode = 0;
    int map_w = 64, map_h = 64;
    int seed = 12345;
    int map_sel = -1;
    std::vector<std::string> map_files;
    int obj_cat = 0;
    int obj_sel[3] = {-1, -1, -1};
    std::vector<std::string> obj_lists[3];
    int obj_dir = 0;
    float obj_scale = 0.3f; // 载具体素比例（0.1..2；0.3 ≈ 原版一格 tile 观感）
    bool no_decor = false; // --nodecor：跳过装饰层（与带装饰渲染对照）
    // 视图
    float pan_x = 0, pan_y = 0, zoom = 1.0f;
    bool recenter = true; // 地图切换后视口居中并缩放适配
    // 渲染产物
    ra2r::ui::BackendHost* host = nullptr; // 呈现后端（main 注入；render_all 上传画布用）
    int bw = 0, bh = 0, ox = 0, oy = 0;
    ra2r::render::ObjectRenderStats obj_stats; // 上次渲染统计（建筑/载具/步兵/跳过）
    bool dirty = true;
    // 性能：静态层缓存（地形+装饰跨帧复用）+ 对象渲染缓存 + 重渲染节流
    std::vector<uint8_t> static_canvas;
    bool static_valid = false;
    ra2r::render::ObjectRenderCache obj_cache;
    uint64_t last_vhash = 0;     // 上次渲染时的模拟视觉状态哈希
    uint64_t last_render_ms = 0; // 上次重渲染时刻（≤30Hz 节流）
};

// 剧场 / 地图生成模式 / 对象类别 名称表（UI 下拉与生成流程共用）
extern const char* const kTheaters[6];
extern const char* const kModeNames[3];
extern const char* const kCatNames[3];

// 名字 → 条目字节（大小写不敏感，读缓存；未找到返回 nullptr）
const std::vector<uint8_t>* load_file(StageApp& a, const std::string& name);

// 按剧场重建瓦片集与调色盘
bool rebuild_resources(StageApp& a, const std::string& theater, std::string* error);

// 解析 RULESMD.INI 的类型节（[BuildingTypes]/[InfantryTypes]/
// [VehicleTypes]+[AircraftTypes]，载具合并飞行器）
void load_type_lists(StageApp& a);

// 生成/加载地图（模式 0=算法 1=平坦 2=加载游戏目录地图）；
// 成功后置 recenter/dirty，视口下一帧居中适配。
void generate_map(StageApp& a, std::string* error);

// 高度衔接：按 TMP 帧头 ramp 元数据为低于邻居的格选斜坡瓦片
// （方向→ramp 码与坡长档映射实测自官方 23 图，见 docs/formats/tileset.md）。
void assign_slopes(StageApp& a);

// 全图重渲染（地形画家算法 + 对象层）并上传纹理；
// 需要 a.tex 已按 map_bounds 创建（由调用方保证）。
void render_all(StageApp& a, std::string* error);

// 懒解析 rulesmd/artmd 全量规则库 → a.rules（单位类型/武器/配件统一查询）
void ensure_rules(StageApp& a);

// sim 单位/步兵 → 渲染对象（含段内像素插值偏移），附加到 objects_out 尾部
void append_sim_objects(StageApp& a, std::vector<ra2r::render::PlacedObject>& objects_out);

// 选中单位描画标记（绿色格框，直接写画布）
void draw_selection_markers(StageApp& a, const ra2r::render::IsometricGrid& grid, int bw,
                            int bh, int ox, int oy, std::vector<uint8_t>& canvas);

// 爆炸动画（EXPLOMED 帧序列）与单位血条，直接写画布
void draw_sim_fx(StageApp& a, const ra2r::render::IsometricGrid& grid, int bw, int bh, int ox,
                 int oy, std::vector<uint8_t>& canvas);

// 建筑配件动画（ActiveAnim 帧序列）：behind=true 画在建筑身后（YSort 类），
// false 画在建筑之上（门/火焰等）；帧 = 建筑 anim_clock % 帧数（15fps）
void draw_building_anims(StageApp& a, const ra2r::render::IsometricGrid& grid, int bw, int bh,
                         int ox, int oy, std::vector<uint8_t>& canvas, bool behind);

// 扫描游戏目录顶层地图文件（.map/.yrm/.yro/.mmx）
void scan_map_files(StageApp& a, const std::filesystem::path& dir);

// ── M4 遭遇战流程 ──
// 地图 waypoint（[Waypoints] 节）→ 引擎格：col=(rx−ry+W−1)/2, row=rx+ry−W−1
// （OpenRA ReadWaypoints 同式；W = [Map] Size 宽）
std::vector<std::pair<int, int>> map_waypoints(const StageApp& a);
// 开始遭遇战：玩家/对手按 waypoint 出生，生成基地车 + 开局兵力（OpenRA
// MPStartUnits 机制，数值取 rulesmd）；需要地图已加载。
bool start_skirmish(StageApp& a, std::string* error);
// 展开选中的基地车（DeploysInto=；播放 Buildup 动画）
bool deploy_selected_mcv(StageApp& a);
// 现场建造/展开逻辑帧时长 = [General] BuildupTime（YR .06 分钟 → 54 帧 @15Hz）；
// 无 Buildup 美术返回 0（原版不进入建造状态，即放即完成）
int onsite_ticks(StageApp& a, const std::string& type);
// 侧边栏可造建筑（Owner/Prerequisite/TechLevel/建造厂过滤）
std::vector<const ra2r::assets::UnitTypeDef*> buildable_for(StageApp& a, const std::string& owner);
// 按角色取本阵营建筑（owner 的科技树校验必须通过）：
// role = "conyard" / "power" / "refinery" / "barracks" / "weapon"；无匹配返回 nullptr
const ra2r::assets::UnitTypeDef* faction_building(StageApp& a, const std::string& owner,
                                                  const std::string& country, const char* role);
// AI 脚本推进（展开基地车 → 起造 → 自动落点），按逻辑帧时刻触发
void skirmish_ai_tick(StageApp& a);
// 玩家建造：排队（扣款）→ 推进 → 就绪后在 (col,row) 放置
bool queue_player_build(StageApp& a, const std::string& type);
bool place_player_build(StageApp& a, int col, int row);

// BMP 写出（--shot/--test 自检；RGBA 输入，自底向上 24bpp）
bool write_bmp(const std::filesystem::path& path, int w, int h,
               const std::vector<uint8_t>& rgba);

} // namespace stage
