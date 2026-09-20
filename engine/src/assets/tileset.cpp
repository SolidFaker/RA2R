// RA2R — 剧场瓦片集实现（接口见 tileset.h）
#include "ra2r/assets/tileset.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "ra2r/core/ini_file.h"

namespace ra2r::assets {

namespace {
// 解析 "r,g,b" 字符串 → 三通道（失败返回 0）
void parse_rgb(const std::string& s, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = g = b = 0;
    int v[3] = {};
    if (std::sscanf(s.c_str(), "%d,%d,%d", &v[0], &v[1], &v[2]) != 3) return;
    r = static_cast<uint8_t>(std::max(0, std::min(255, v[0])));
    g = static_cast<uint8_t>(std::max(0, std::min(255, v[1])));
    b = static_cast<uint8_t>(std::max(0, std::min(255, v[2])));
}
} // namespace

bool TerrainTileset::build(const uint8_t* ini, size_t size, const std::string& ext,
                           std::string* error) {
    names_.clear();
    set_of_tile_.clear();
    sets_.clear();
    core::IniFile f;
    if (!f.parse(ini, size, error)) return false;
    // 遍历 [TileSet0000]..[TileSetNNNN]；缺失的节号终止遍历
    for (int set = 0;; ++set) {
        char sec[24]; // 前缀 7 字节 + int 最长 11 位 + NUL：防止 -Wformat-truncation
        std::snprintf(sec, sizeof(sec), "TileSet%04d", set);
        if (!f.has_section(sec)) break;
        TileSetInfo info;
        info.set_name = f.get(sec, "SetName", "");
        parse_rgb(f.get(sec, "LowRadarColor", "0,0,0"), info.low_r, info.low_g, info.low_b);
        parse_rgb(f.get(sec, "HighRadarColor", "0,0,0"), info.high_r, info.high_g, info.high_b);
        sets_.push_back(info);
        const std::string base = f.get(sec, "FileName", "");
        const std::string tiles = f.get(sec, "TilesInSet", "1");
        const int count = std::atoi(tiles.c_str());
        if (count < 0 || count > 256) {
            if (error) *error = std::string(sec) + " TilesInSet out of range";
            return false;
        }
        for (int i = 1; i <= count; ++i) {
            set_of_tile_.push_back(static_cast<uint16_t>(set));
            if (base.empty()) {
                names_.emplace_back(); // 占位（保持全局瓦片号对齐）
                continue;
            }
            char name[64];
            std::snprintf(name, sizeof(name), "%s%02d.%s", base.c_str(), i, ext.c_str());
            names_.emplace_back(name);
        }
    }
    if (names_.empty() && error) *error = "no TileSet sections";
    return !names_.empty();
}

const std::string& TerrainTileset::name_for(uint16_t tile_id) const {
    static const std::string kEmpty;
    return tile_id < names_.size() ? names_[tile_id] : kEmpty;
}

int TerrainTileset::set_index_for(uint16_t tile_id) const {
    return tile_id < set_of_tile_.size() ? set_of_tile_[tile_id] : -1;
}

const TileSetInfo* TerrainTileset::set_info(int set_index) const {
    return set_index >= 0 && set_index < static_cast<int>(sets_.size()) ? &sets_[set_index]
                                                                        : nullptr;
}

} // namespace ra2r::assets
