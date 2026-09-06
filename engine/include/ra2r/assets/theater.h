#pragma once
// RA2R — 剧场配置表（M2 从 tools/mapview 提入引擎）
//
// 剧场 → 瓦片后缀 / 地形 INI / 地形调色盘 / 单位调色盘
// （实测反查结果，见 docs/formats/palettes.md；NEWURBAN 地形盘复用城市盘）。
#include <string>

namespace ra2r::assets {

struct TheaterConfig {
    const char* ext;      // 瓦片文件后缀（不含点）
    const char* ini;      // 地形 INI（TileSet 定义）
    const char* pal;      // 地形调色盘
    const char* unit_pal; // 单位/建筑/步兵调色盘
};

// 大小写不敏感；未知剧场回退温和（TEMPERATE）
TheaterConfig theater_config(const std::string& theater);

} // namespace ra2r::assets
