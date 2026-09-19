#pragma once
// RA2R — 对象层渲染（建筑 SHP / 载具 VXL / 步兵 SHP 序列）+ 全单位陈列
//
// 通用放置管线（mapview 与 stage 共用）：给定 PlacedObject 列表，
// 按深度排序绘制到画布。放置约定与序列语义见 object_layer.cpp 注释；
// 格式参考 docs/formats/*.md 与 third_party/reference/LegacySequenceImporter.cs。
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ra2r/assets/rules_db.h"
#include "ra2r/assets/shp_layout.h"
#include "ra2r/render/isometric.h"

namespace ra2r::render {

// 文件读取器：名字（大小写不敏感）→ 条目字节；未找到返回 nullptr
using FileLoader = std::function<const std::vector<uint8_t>*(const std::string&)>;

// 剧场单位调色盘名（建筑/步兵共用；载具用 VXL 内嵌盘）
struct UnitPaletteCfg {
    const char* unit_pal; // 如 "UNITSNO.PAL"
    std::string theater;  // 剧场名（NewTheater 美术名回退用；空 = TEMPERATE）
    // 阵营色重映射表（[Colors] H,S,V → 16 色 ramp）。对象用 remap = 下标+1 引用；
    // 空表 = 全部按原调色盘（Remap 段仍是红渐变）
    const std::vector<ra2r::assets::HouseRamp>* house_ramps = nullptr;
};

struct ObjectRenderStats {
    int buildings = 0;
    int units = 0;
    int infantry = 0;
    int skipped = 0;
};

// 建造/展开动画时长（[General] BuildupTime，YR rulesmd = 0.06 分钟 = 3.6s =
// 54 逻辑帧 @15Hz；ModEnc/Buildup 页）。仅作 build_total 缺失时的兜底；
// 正常路径由 stage 把 BuildupTime 折算成放置时的 build_total（object_layer.cpp）
constexpr int kBuildupTicks = 54;

struct ObjectRenderCache; // 定义见下（render_objects 的可选缓存参数）

// 待渲染对象（kind: 0=建筑 1=载具 2=步兵）
struct PlacedObject {
    int kind = 0;
    std::string id;
    int cx = 0, cy = 0;
    uint8_t dir = 0;
    uint8_t subcell = 0;
    int height = 0;  // 所在格高度级（每级 kHeightLevelPx=15px，向上抬升）
    int off_x = 0, off_y = 0; // 格内像素偏移（M3 平滑移动插值用；建筑恒 0）
    uint8_t alpha = 255;      // 不透明度（其余恒 255）
    // 建筑帧语义（OpenRA ra2 ^Structure 约定）：
    //   idle=帧0(阴影帧3)、damaged-idle=帧1(阴影帧4)、make=帧2(阴影帧5)；
    //   hp<128 → 受损帧；build_p<0 = 建成
    int hp = 256;
    float build_p = -1.0f;
    // 阵营色：0 = 不重映射；N = cfg.house_ramps[N-1]（Remap 段 16..31 整段替换）
    uint8_t remap = 0;
    // ── 动画状态（渲染用；默认静态）──
    // 建造：已用/总逻辑帧。有 Buildup 动画时按 build_total 播完一遍（= 放置时
    // 传入的 [General] BuildupTime 帧数），随后显示 make 帧直到完工（object_layer.cpp）
    int build_ticks = 0;
    int build_total = 0;
    // 步兵：moving=行进中（播 Walk 序列，每 3 逻辑帧 1 帧）；
    // anim_clock = 逻辑帧时钟（走/idle 序列相位）；idle_kind = sim 触发的 idle
    // 动作（1=Idle1 2=Idle2，0=无；播放从 idle_start 起的 artmd Idle1/Idle2 段）
    uint8_t moving = 0;
    uint32_t anim_clock = 0;
    uint8_t idle_kind = 0;
    uint32_t idle_start = 0;
};

// 渲染对象到画布（画在地形之后，按 (cx+cy, cx) 深度排序）。
// 画布为全图 RGBA（bw×bh），坐标经 (ox, oy) 偏移。
// obj_scale：载具体素比例（默认 0.3 ≈ 原版一格 tile 观感，对照 OpenRA ra2
// RenderVoxels Scale=11.7；范围 0.1..4）。
ObjectRenderStats render_objects(const std::vector<PlacedObject>& objs,
                                 const UnitPaletteCfg& cfg, const IsometricGrid& grid,
                                 const FileLoader& load, int bw, int bh, int ox, int oy,
                                 std::vector<uint8_t>& canvas, float obj_scale = 0.3f,
                                 ObjectRenderCache* cache = nullptr);

// 建筑美术名解析（NewTheater 回退链，artmd NewTheater=yes 的建筑）：
//   第 2 字母换剧场代号（见 assets::theater_code）→ 换 'G'（通用）→ 原名
// 实测：类型名第 2 字母是 'A'（雪地代号），按原名加载会拿到雪地美术
// （GAPOWR 底座覆雪）；GTGCAN 的底座则只有 GAGCAN(雪)/GGGCAN(通用) 变体。
std::string resolve_art_name(const std::string& image, const std::string& theater,
                             const FileLoader& load);

// 全单位陈列场景：把名库中全部 .VXL 单位渲染成网格（验收用）。
// vxl_names 为大写 .VXL 名列表（由调用方从名称索引收集）。
struct ShowcaseResult {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    int count = 0;
};
ShowcaseResult run_showcase(const std::vector<std::string>& vxl_names, const FileLoader& load,
                            int scale);

// 对象渲染缓存（跨帧复用解码结果；由调用方持有并在资产变化时 clear）：
//   载体素按 (美术名, 朝向, 比例16) 缓存光栅结果；
//   建筑按 (美术名, 透明度) 缓存首帧 RGBA（建筑静态，避免每帧解码）。
struct ObjectRenderCache {
    struct VoxelEntry {
        int w = 0, h = 0;
        int ax = 0, ay = 0; // 锚点（模型原点相对图像左上）
        int bx = 0, by = 0; // 主体节基座中心锚点（建筑炮塔落点/车体顶心用）
        std::vector<uint8_t> rgba;
    };
    struct BldEntry {
        int cx = 0, cy = 0;            // 首帧像素尺寸
        int fx = 0, fy = 0;            // 帧在画布内偏移
        int canvas_w = 0, canvas_h = 0; // SHP 画布尺寸（放置锚点用）
        std::vector<uint8_t> rgba;     // 首帧 RGBA
    };
    std::map<std::string, VoxelEntry> voxels;
    std::map<std::string, BldEntry> buildings;  // 键：美术名#透明度
    std::map<std::string, BldEntry> shp_frames; // 通用 SHP 帧缓存：美术名|帧号
                                               //（步兵朝向帧 + 建筑配件动画帧共用）
    // 分段布局缓存（按美术名；帧内容判定较贵，跨帧复用）
    std::map<std::string, ra2r::assets::ShpLayout> anim_layouts; // 配件动画 SHP
    std::map<std::string, int> body_shadow;                      // 建筑本体阴影段起点
    bool art_ready = false;                     // artmd/rulesmd Image 表已解析
    // artmd 解析结果本体（跨帧复用）。必须与 art_images 一起缓存：只缓存
    // Image 表会让后续帧的 art 为空，Buildup=/Sequence= 等键全部查不到。
    std::shared_ptr<ra2r::core::IniFile> art_file;
    std::map<std::string, std::string> art_images;
    void clear() {
        voxels.clear();
        buildings.clear();
        shp_frames.clear();
        anim_layouts.clear();
        body_shadow.clear();
        art_ready = false;
        art_file.reset();
        art_images.clear();
    }
};

} // namespace ra2r::render
