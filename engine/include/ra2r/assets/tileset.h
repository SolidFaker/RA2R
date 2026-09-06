#pragma once
// RA2R — 剧场瓦片集（tile_id → 瓦片文件名 / TileSet 归属 / 雷达色）
//
// 命名规律（OpenRA LegacyTilesetImporter 语义，实测 RA2 一致）：
//   地形 INI（URBANMD.INI 等）的 [TileSet####] 节依次贡献 TilesInSet 个瓦片，
//   文件名 = FileName + 两位序号 + "." + 剧场后缀（urb/tem/sno/des/lun/ubn）；
//   全局瓦片号（tile_id）按节顺序跨全部 TileSet 连续累加，
//   文件缺失也占号（名字留空保持索引对齐）。
#include <cstdint>
#include <string>
#include <vector>

namespace ra2r::assets {

// 单个 TileSet 的元数据（地形 INI 节）
struct TileSetInfo {
    std::string set_name;  // SetName=（分类名，如 "Paved Road"）
    uint8_t low_r = 0, low_g = 0, low_b = 0;    // LowRadarColor（最低高度）
    uint8_t high_r = 0, high_g = 0, high_b = 0; // HighRadarColor（最高高度）
};

class TerrainTileset {
public:
    // ini 为地形 INI 文本；ext 为剧场后缀（不含点，如 "urb"）
    bool build(const uint8_t* ini, size_t size, const std::string& ext, std::string* error);

    // tile_id（0 基）→ 文件名；越界或占位缺失返回空串
    const std::string& name_for(uint16_t tile_id) const;
    // tile_id → 所属 TileSet 序号（越界返回 -1）
    int set_index_for(uint16_t tile_id) const;
    // TileSet 元数据（越界返回 nullptr）
    const TileSetInfo* set_info(int set_index) const;
    size_t size() const { return names_.size(); }
    int set_count() const { return static_cast<int>(sets_.size()); }

private:
    std::vector<std::string> names_;
    std::vector<uint16_t> set_of_tile_; // tile → set 序号
    std::vector<TileSetInfo> sets_;
};

} // namespace ra2r::assets
