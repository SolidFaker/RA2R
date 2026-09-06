// RA2R — 小地图渲染实现（接口见 minimap.h）
#include "ra2r/render/minimap.h"

namespace ra2r::render {

namespace {
// 高度 t∈[0,1] 的雷达色插值
inline uint8_t lerp_u8(uint8_t a, uint8_t b, int height) {
    return static_cast<uint8_t>(a + (b - a) * height / 15);
}
} // namespace

RasterImage render_minimap(const assets::MapFile& map, const assets::TerrainTileset& ts) {
    RasterImage out;
    out.w = map.cell_w();
    out.h = map.cell_h();
    out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            const auto& cell = map.cell(x, y);
            uint8_t* d = out.rgba.data() + (static_cast<size_t>(y) * out.w + x) * 4;
            if (!cell.present || cell.tile_id == 0xFFFF) {
                d[3] = 255; // 黑
                continue;
            }
            const int set = ts.set_index_for(cell.tile_id);
            const auto* info = ts.set_info(set);
            if (!info) {
                d[3] = 255;
                continue;
            }
            d[0] = lerp_u8(info->low_r, info->high_r, cell.height);
            d[1] = lerp_u8(info->low_g, info->high_g, cell.height);
            d[2] = lerp_u8(info->low_b, info->high_b, cell.height);
            d[3] = 255;
        }
    }
    return out;
}

} // namespace ra2r::render
