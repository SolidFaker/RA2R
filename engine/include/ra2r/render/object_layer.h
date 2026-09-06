#pragma once
// RA2R — 对象层渲染（建筑 SHP / 载具 VXL / 步兵 SHP 序列）+ 全单位陈列
//
// 通用放置管线（mapview 与 stage 共用）：给定 PlacedObject 列表，
// 按深度排序绘制到画布。放置约定与序列语义见 object_layer.cpp 注释；
// 格式参考 docs/formats/*.md 与 third_party/reference/LegacySequenceImporter.cs。
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ra2r/render/isometric.h"

namespace ra2r::render {

// 文件读取器：名字（大小写不敏感）→ 条目字节；未找到返回 nullptr
using FileLoader = std::function<const std::vector<uint8_t>*(const std::string&)>;

// 剧场单位调色盘名（建筑/步兵共用；载具用 VXL 内嵌盘）
struct UnitPaletteCfg {
    const char* unit_pal; // 如 "UNITSNO.PAL"
};

struct ObjectRenderStats {
    int buildings = 0;
    int units = 0;
    int infantry = 0;
    int skipped = 0;
};

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
    //   idle=帧0(阴影帧3)、damaged-idle=帧1(阴影帧4)、make=帧2(阴影帧5，
    //   按 build_p 0..1 缩放表现建造动画)；hp<128 → 受损帧；build_p<0 = 建成
    int hp = 256;
    float build_p = -1.0f;
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
        int ax = 0, ay = 0; // 锚点（底盘中心相对图像左上）
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
    bool art_ready = false;                     // artmd/rulesmd Image 表已解析
    std::map<std::string, std::string> art_images;
    void clear() {
        voxels.clear();
        buildings.clear();
        shp_frames.clear();
        art_ready = false;
        art_images.clear();
    }
};

} // namespace ra2r::render
