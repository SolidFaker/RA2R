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

// NewTheater 文件名代号（建筑 SHP 名第 2 字母）：
//   TEMPERATE=T、SNOW=A、URBAN=U、DESERT=D、LUNAR=L、NEWURBAN=N，通用回退 G。
// 实测依据（同一建筑不同变体逐帧比对）：
//   GAPOWR(A) 底座覆雪 vs GGPOWR(G) 橄榄绿；GASAND(A) 雪白沙袋 vs GTSAND(T) 橄榄绿；
//   CAOILD(A) 塔身积雪 vs CTOILD(T) 无雪；GAWALL(A) 白色墙基 vs GTWALL(T) 绿墙基。
// 全库 SHP 名第 2 字母只有 A/D/G/L/N/T/U 高频（其余字母个位数）。
// 注意：类型名（GAPOWR/YAGGUN/NATSLA…）第 2 字母恰为 A（雪），
// 因此**不能按原名直接加载**，必须先换成剧场代号。
char theater_code(const std::string& theater);

} // namespace ra2r::assets
