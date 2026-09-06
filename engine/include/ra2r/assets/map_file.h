#pragma once
// RA2R — RA2/YR 地图（.map/.yrm/.yro/.mmx 的 INI 外壳 + IsoMapPack5）
// 规格见 docs/formats/map.md；每瓦片 11 字节（X,Y,tileID,extra,subtile,height,extra2）
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "ra2r/core/ini_file.h"

namespace ra2r::assets {

struct MapCell {
    bool present = false;
    uint16_t x = 0, y = 0;       // 单元格坐标（[Map] Size 范围内）
    uint16_t tile_id = 0;        // 瓦片集内瓦片号（0xFFFF = 空）
    uint16_t extra = 0;
    uint8_t subtile = 0;
    uint8_t height = 0;          // 高度 0..15
    uint8_t extra2 = 0;
};

// 地图对象（[Units]/[Structures] 节，渲染与后续 M3 共用）
struct MapUnit {
    std::string id;   // 如 "MGTK"
    std::string owner;
    int cx = 0, cy = 0; // 格坐标（cell = cy·W + cx）
    uint8_t dir = 0;    // 朝向 0..255（32 = 45°）
    int health = 256;
};

struct MapBuilding {
    std::string id;
    std::string owner;
    int cx = 0, cy = 0;
    int rx = 0, ry = 0;  // 地图空间存储格（地基顶格，[Structures] rx,ry 原值）
    uint8_t dir = 0;
    int health = 256;
};

// 步兵（[Infantry] 节）：字段序 owner,id,health,cell,dir,subcell（实测）
struct MapInfantry {
    std::string id;
    std::string owner;
    int cx = 0, cy = 0;
    uint8_t dir = 0;     // 朝向 0..255
    uint8_t subcell = 0; // 格内位置 0..4
    int health = 256;
};

// 地图地形对象（[Terrain] 节：树木/岩石，key = rx + ry·1000）
struct MapTerrain {
    std::string name; // 如 "TREE24"（艺术 = name + "." + 剧场后缀 的 TEM 瓦片）
    int cx = 0, cy = 0;
};

class MapFile {
public:
    bool open(const std::filesystem::path& path, std::string* error);
    bool open(const uint8_t* data, size_t size, std::string* error);

    const core::IniFile& ini() const { return ini_; }
    int left() const { return left_; }
    int top() const { return top_; }
    int right() const { return right_; }
    int bottom() const { return bottom_; }
    // 可玩区域网格尺寸（归一化后 left/top 恒为 0，right/bottom 为排他上界）
    int cell_w() const { return right_; }
    int cell_h() const { return bottom_; }
    const std::string& theater() const { return theater_; }
    int present_count() const { return present_; }
    size_t cells_total() const { return cells_.size(); }
    // 地图空间 → 引擎格线性映射基准（col=(rx−ry−min_d)/2, row=rx+ry−min_s；
    // 由 IsoMapPack5 点阵实测推导，M3 模拟层/建筑地基格换算共用）
    int min_d() const { return min_d_; }
    int min_s() const { return min_s_; }

    // ── 覆盖物（[OverlayPack]/[OverlayDataPack]，LCW 解压后的格栅数组）──
    // 每格 1 字节：overlay type（0xFF=无）；索引 = rx + 512*ry（rx/ry 为地图原始坐标）
    const std::vector<uint8_t>& overlays() const { return overlay_; }
    const std::vector<uint8_t>& overlay_data() const { return overlay_data_; }
    int overlay_present() const { return overlay_present_; }
    uint8_t overlay_type(int rx, int ry) const {
        const size_t idx = static_cast<size_t>(rx) + 512 * static_cast<size_t>(ry);
        return idx < overlay_.size() ? overlay_[idx] : 0xFF;
    }
    uint8_t overlay_data_at(int rx, int ry) const {
        const size_t idx = static_cast<size_t>(rx) + 512 * static_cast<size_t>(ry);
        return idx < overlay_data_.size() ? overlay_data_[idx] : 0;
    }
    // 覆盖物有效渲染帧（输入为格坐标 col/row）：
    //   桥类：高架桥面 25/26 帧 = OverlayData 件号（0..17）；低桥多格件
    //     （77-104/125-128/209-236）只在件原点（data=0）绘制（其余 0xFF 跳过）；
    //     桥基 237-240 每格帧 1；
    //   围墙类：OverlayData 原值（编辑器已预计算邻接件号）；
    //   其余：OverlayData 原值（矿石生长档等）。
    uint8_t overlay_render_frame(int cx, int cy) const;
    // 多格覆盖物（桥件）的精灵锚定偏移：桥面帧偏移已按件原点设计，恒为 0
    void overlay_anchor_offset(uint8_t type, int& dx, int& dy) const;
    // 覆盖物实际绘制格：低桥多格件（74-101/122-125/205-236）在件原点（data=0）
    // 处返回件中格（data=1 的同类型邻格）引擎坐标，其余返回本格
    void overlay_draw_cell(int cx, int cy, int& dx, int& dy) const;

    // ── 地图对象（[Units]/[Structures]，解析于 open）──
    const std::vector<MapUnit>& units() const { return units_; }
    const std::vector<MapBuilding>& buildings() const { return buildings_; }
    const std::vector<MapInfantry>& infantry() const { return infantry_; }
    // [Terrain] 地形对象（树木/岩石）
    const std::vector<MapTerrain>& terrain_objects() const { return terrain_; }

    // ── 黑幕/迷雾（[Shroud]，可选节）──
    // 状态语义（RA2）：1=可见，2..4=迷雾逐级加深，5=黑幕；无节视为全可见(1)。
    // 索引与 cells_ 一致（cx + cy·cell_w）
    const std::vector<uint8_t>& shroud() const { return shroud_; }

    // 相对地图坐标访问（x ∈ [0, cell_w)，y ∈ [0, cell_h)）
    const MapCell& cell(int x, int y) const {
        return cells_[static_cast<size_t>(y) * cell_w() + x];
    }

private:
    bool parse(std::string* error);

    core::IniFile ini_;
    int left_ = 0, top_ = 0, right_ = 0, bottom_ = 0;
    std::string theater_;
    int present_ = 0;
    int min_d_ = 0, min_s_ = 0; // 反对角线映射基准（对象/迷雾的格栅序号换算用）
    std::vector<MapCell> cells_;
    std::vector<uint8_t> overlay_;       // [OverlayPack] LCW 解压后
    std::vector<uint8_t> overlay_data_;  // [OverlayDataPack] LCW 解压后
    int overlay_present_ = 0;
    std::vector<MapUnit> units_;
    std::vector<MapBuilding> buildings_;
    std::vector<MapInfantry> infantry_;
    std::vector<MapTerrain> terrain_;
    std::vector<uint8_t> shroud_;
};

} // namespace ra2r::assets
