#pragma once
// RA2R stage — 舞台地图模型与生成（测试/调试用）
//
// 三种生成模式：
//   平坦填充   全图同瓦片（set 0 第 0 瓦片）高度 0
//   算法       随机水域湖泊 + 高斯团块高度（种子可复现）
//   加载地图   从 MapFile 提取格数据（含对象/装饰列表）
#include <cstdint>
#include <string>
#include <vector>

#include "ra2r/assets/map_file.h"
#include "ra2r/assets/tileset.h"
#include "ra2r/render/overlay_layer.h"

namespace stage {

struct StageCell {
    uint16_t tile_id = 0;
    uint8_t height = 0;
    uint8_t subtile = 0;
};

class StageMap {
public:
    int w = 64, h = 64;
    std::string theater = "TEMPERATE";
    std::vector<StageCell> cells;
    // 装饰元素（树木/岩石/覆盖物）—— 与 mapview 共享 build_scene_decor 产出，
    // art = 最终文件名（含剧场后缀/墙前缀），渲染时直接用
    std::vector<ra2r::render::MapDecorObject> decor;
    uint16_t water_tile() const { return water_tile_; } // 算法图水域瓦片号（0xFFFF=无水）

    void set_size(int w, int h);
    // 平坦填充
    void generate_flat(int w, int h);
    // 算法生成（ts 用于找水域瓦片；seed 可复现）；高度衔接斜坡由 assign_slopes 补
    void generate_algorithm(int w, int h, const ra2r::assets::TerrainTileset& ts,
                            uint32_t seed);
    // 从 MapFile 提取（装饰列表经共享管线 build_scene_decor 构建）
    bool load(const ra2r::assets::MapFile& map,
              const ra2r::render::FileLoader& load);

    const StageCell& cell(int cx, int cy) const {
        return cells[static_cast<size_t>(cy) * w + cx];
    }
    StageCell& cell(int cx, int cy) { return cells[static_cast<size_t>(cy) * w + cx]; }

private:
    uint16_t water_tile_ = 0xFFFF;
};

} // namespace stage
