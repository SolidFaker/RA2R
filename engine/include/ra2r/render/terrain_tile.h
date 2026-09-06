#pragma once
// RA2R — RA2/YR 地形瓦片（TMP）解码与渲染（M2 地形模块）
//
// 格式（参考 OpenRA TmpTSLoader.cs，third_party/reference/）：
//   头:   [u32 模板宽][u32 模板高][i32 瓦片宽][i32 瓦片高]（RA2 恒 60×30）
//         [模板宽×模板高 个 u32 帧偏移]（绝对文件偏移）
//   帧:   52 字节头（20B 杂项 + extraX/Y/W/H + flags + 12B 保留；flags&1 表示
//         存在悬崖扩展区），随后菱形像素行（4,8,...,60,...,4，共 900 字节）+
//         同布局的深度行；flags&1 时再跟扩展区像素与深度
//   像素: 调色板索引（0=透明）；深度值 <32 有效（z 排序用）
#include <cstdint>
#include <string>
#include <vector>

#include "ra2r/render/raster.h"

namespace ra2r::render {

// 单帧（含悬崖扩展后的像素区域与深度）
struct TerrainTileFrame {
    int bounds_x = 0, bounds_y = 0; // 区域原点（帧坐标；含扩展时为负）
    int bounds_w = 0, bounds_h = 0;
    std::vector<uint8_t> pixels; // 调色板索引（bounds_w × bounds_h，0=透明）
    std::vector<uint8_t> depth;  // 深度（<32 有效）
    bool has_extra = false;      // 是否含悬崖扩展区
    // 帧头元数据（偏移 44..46）：原版引擎用于高度衔接与斜坡语义
    int height_off = 0;          // 瓦片自带高度偏移（i8；悬崖/斜坡帧非 0）
    uint8_t terrain_kind = 0;    // 地形类型（0=普通地面）
    uint8_t ramp_kind = 0;       // 斜坡方向（0=非斜坡；1..15 见 docs/formats/tileset.md）
};

class TerrainTile {
public:
    // 解析 TMP（RA2 变体：数据未压缩；TD 的 LCW 变体不在此支持）
    bool open(const uint8_t* data, size_t size, std::string* error = nullptr);

    bool is_open() const { return !frames_.empty(); }
    int template_w() const { return template_w_; }
    int template_h() const { return template_h_; }
    int tile_w() const { return tile_w_; }
    int tile_h() const { return tile_h_; }
    int frame_count() const { return static_cast<int>(frames_.size()); }
    const TerrainTileFrame& frame(int i) const { return frames_[i]; }

    // 渲染帧为 RGBA（pal 为 768 字节 6-6-6 调色盘；索引 0 输出透明）
    RasterImage render_frame(int i, const uint8_t* pal_768) const;

    // ── 帧序列化（规范缓存用，schema 版本见 kFrameSchema）──
    // 布局: [u32 'TLRF'][u16 ver][i32 bx][i32 by][i32 bw][i32 bh][u32 px_n]
    //       [pixels][depth][i8 height][u8 terrain][u8 ramp]
    // schema 2: 增补帧头元数据三字节（height_off/terrain_kind/ramp_kind）
    // schema 3: 修正序列化头长 30→26（此前像素区后移致缓存帧行内容右移 4/7px）
    static constexpr uint16_t kFrameSchema = 3;
    static bool serialize_frame(const TerrainTileFrame& f, std::vector<uint8_t>& out);
    static bool deserialize_frame(const uint8_t* data, size_t size, TerrainTileFrame& out,
                                  std::string* error);

private:
    int template_w_ = 0, template_h_ = 0;
    int tile_w_ = 0, tile_h_ = 0;
    std::vector<TerrainTileFrame> frames_;
};

} // namespace ra2r::render
