#pragma once
// RA2R — 小地图渲染（M2 渲染模块）
//
// 原版小地图：每格按其所属 TileSet 的雷达色着色（LowRadarColor/HighRadarColor
// 随高度 0..15 线性过渡）；无地形格为黑。
#include "ra2r/assets/map_file.h"
#include "ra2r/assets/tileset.h"
#include "ra2r/render/raster.h"

namespace ra2r::render {

// 渲染地图小地图（1 像素/格，与 map.cell_w()×cell_h() 同尺寸）
RasterImage render_minimap(const assets::MapFile& map, const assets::TerrainTileset& ts);

} // namespace ra2r::render
