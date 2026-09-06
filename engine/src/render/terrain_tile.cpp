// RA2R — 地形瓦片（TMP）解码实现
//
// 拆分（docs/design/code-organization.md）：
//   unpack_tile_rows   — 菱形像素行解包（像素与深度共用）
//   parse_frame        — 单帧解析（头 + 像素 + 深度 + 可选悬崖扩展）
//   render_frame       — 调色板索引 → RGBA
//   serialize/deserialize_frame — 规范缓存序列化（CacheManager 用）
#include "ra2r/render/terrain_tile.h"

#include <algorithm>
#include <cstring>

#include "ra2r/core/endian.h"

namespace ra2r::render {

namespace {

// 菱形行解包：行宽 4,8,...,W,...,4（W=60 时共 900 字节），
// 每行居中写入 bounds 区域；像素与深度数据共用本函数。
void unpack_tile_rows(const uint8_t* src, uint8_t* dst, int tile_w, int tile_h,
                      const TerrainTileFrame& bounds) {
    int width = 4;
    for (int j = 0; j < tile_h; ++j) {
        const int start_x = (tile_w - width) / 2 - bounds.bounds_x;
        const int start_y = j - bounds.bounds_y;
        const size_t start = static_cast<size_t>(start_y) * bounds.bounds_w + start_x;
        std::copy(src, src + width, dst + start);
        src += width;
        width += (j < tile_h / 2 - 1 ? 1 : -1) * 4;
    }
}

// 解析单帧（data 指向帧头；span = 本帧可用字节数，由相邻帧偏移或文件尾界定）
//
// 帧头 52 字节（XCC t_tmp_image_header，见 third_party/reference 说明）：
//   [i32 x][i32 y]            帧在模板屏幕坐标中的位置（(u-v)·W/2, (u+v)·H/2）
//   [i32 extra_ofs]           扩展像素数据偏移（帧内）
//   [i32 z_ofs]               深度数据偏移（恒 52 + W·H/2）
//   [i32 extra_z_ofs]         扩展深度偏移
//   [i32 x_extra/y_extra/cx_extra/cy_extra]  扩展区（模板左上角相对坐标）
//   [u32 flags]               位域——实测 RA2 文件此处为未初始化垃圾（0xCD...），不可信；
//                             扩展区存在性由 extra_ofs/extra_z_ofs 与尺寸合理性判别
//   [i8 height/terrain/ramp][6×u8 雷达色][3B pad]
bool parse_frame(const uint8_t* data, size_t span, int tile_w, int tile_h, int u, int v,
                 TerrainTileFrame& out, std::string* error) {
    const size_t base_bytes = static_cast<size_t>(tile_w) * tile_h / 2; // 菱形像素区
    if (span < 52 + 2 * base_bytes) {
        if (error) *error = "tmp frame truncated";
        return false;
    }
    const int32_t extra_ofs = core::read_i32_le(data + 8);
    const int32_t z_ofs = core::read_i32_le(data + 12);
    const int32_t extra_z_ofs = core::read_i32_le(data + 16);
    // 扩展区坐标：存储为模板左上角相对，换算到帧相对（OpenRA TmpTSLoader 语义）
    const int32_t x_extra = core::read_i32_le(data + 20) - (u - v) * tile_w / 2;
    const int32_t y_extra = core::read_i32_le(data + 24) - (u + v) * tile_h / 2;
    const int32_t cx_extra = core::read_i32_le(data + 28);
    const int32_t cy_extra = core::read_i32_le(data + 32);
    auto sane_rect = [&](int32_t cw, int32_t ch, int32_t ofs) {
        return cw > 0 && ch > 0 && cw <= 512 && ch <= 512 && ofs >= 0 &&
               static_cast<size_t>(ofs) + static_cast<size_t>(cw) * ch <= span;
    };
    const bool has_extra_px = sane_rect(cx_extra, cy_extra, extra_ofs);
    const bool has_extra_z = sane_rect(cx_extra, cy_extra, extra_z_ofs);
    // 帧头元数据（offset 40..42，紧随 flags）：height=瓦片自带高度偏移（i8），
    // terrain=地形类型，ramp=斜坡方向（原版高度衔接的驱动数据）；43..48 为雷达色
    out.height_off = static_cast<int8_t>(data[40]);
    out.terrain_kind = data[41];
    out.ramp_kind = data[42];
    out.has_extra = has_extra_px;
    // 像素区域 = 瓦片矩形 ∪（可选）悬崖扩展矩形
    out.bounds_x = 0;
    out.bounds_y = 0;
    out.bounds_w = tile_w;
    out.bounds_h = tile_h;
    if (out.has_extra) {
        const int x0 = std::min(0, x_extra);
        const int y0 = std::min(0, y_extra);
        const int x1 = std::max(tile_w, x_extra + cx_extra);
        const int y1 = std::max(tile_h, y_extra + cy_extra);
        out.bounds_x = x0;
        out.bounds_y = y0;
        out.bounds_w = x1 - x0;
        out.bounds_h = y1 - y0;
    }
    out.pixels.assign(static_cast<size_t>(out.bounds_w) * out.bounds_h, 0);
    out.depth = out.pixels;
    // 基础像素区（帧头后）+ 深度区（z_ofs）
    unpack_tile_rows(data + 52, out.pixels.data(), tile_w, tile_h, out);
    const int32_t z_base = z_ofs > 0 ? z_ofs : static_cast<int32_t>(52 + base_bytes);
    if (static_cast<size_t>(z_base) + base_bytes > span) {
        if (error) *error = "tmp depth data out of range";
        return false;
    }
    unpack_tile_rows(data + z_base, out.depth.data(), tile_w, tile_h, out);
    if (!out.has_extra) return true;
    // 悬崖扩展：像素（非 0 覆盖）+ 深度（<32 才保留）
    const uint8_t* ep = data + extra_ofs;
    for (int j = 0; j < cy_extra; ++j) {
        const size_t start =
            static_cast<size_t>(j + y_extra - out.bounds_y) * out.bounds_w + x_extra -
            out.bounds_x;
        for (int i = 0; i < cx_extra; ++i) {
            const uint8_t e = *ep++;
            if (e != 0) out.pixels[start + i] = e;
        }
    }
    if (has_extra_z) {
        const uint8_t* ez = data + extra_z_ofs;
        for (int j = 0; j < cy_extra; ++j) {
            const size_t start =
                static_cast<size_t>(j + y_extra - out.bounds_y) * out.bounds_w + x_extra -
                out.bounds_x;
            for (int i = 0; i < cx_extra; ++i) {
                const uint8_t e = *ez++;
                if (e < 32) out.depth[start + i] = e;
            }
        }
    }
    return true;
}

} // namespace

bool TerrainTile::open(const uint8_t* data, size_t size, std::string* error) {
    frames_.clear();
    if (size < 16) {
        if (error) *error = "tmp too small";
        return false;
    }
    template_w_ = static_cast<int>(core::read_u32_le(data));
    template_h_ = static_cast<int>(core::read_u32_le(data + 4));
    tile_w_ = core::read_i32_le(data + 8);
    tile_h_ = core::read_i32_le(data + 12);
    if (template_w_ < 1 || template_h_ < 1 || template_w_ > 64 || template_h_ > 64 ||
        tile_w_ < 4 || tile_h_ < 4 || tile_w_ > 512 || tile_h_ > 512) {
        if (error) *error = "tmp header out of range";
        return false;
    }
    const size_t n_frames = static_cast<size_t>(template_w_) * template_h_;
    if (16 + n_frames * 4 > size) {
        if (error) *error = "tmp offset table truncated";
        return false;
    }
    frames_.resize(n_frames);
    for (size_t k = 0; k < n_frames; ++k) {
        const uint32_t off = core::read_u32_le(data + 16 + k * 4);
        if (off == 0) continue; // 空帧哨兵（模板格无内容，如悬崖板左下角）
        if (off >= size) {
            if (error) *error = "tmp frame offset out of range";
            return false;
        }
        // 帧跨度 = 下一个非零偏移 - 当前偏移（无则到文件尾）
        size_t span = size - off;
        for (size_t j = k + 1; j < n_frames; ++j) {
            const uint32_t next = core::read_u32_le(data + 16 + j * 4);
            if (next > off) {
                span = next - off;
                break;
            }
        }
        const int u = static_cast<int>(k % template_w_);
        const int v = static_cast<int>(k / template_w_);
        if (!parse_frame(data + off, span, tile_w_, tile_h_, u, v, frames_[k], error)) {
            return false;
        }
    }
    return true;
}

RasterImage TerrainTile::render_frame(int i, const uint8_t* pal_768) const {
    RasterImage out;
    if (i < 0 || i >= static_cast<int>(frames_.size())) return out;
    const TerrainTileFrame& f = frames_[i];
    out.w = f.bounds_w;
    out.h = f.bounds_h;
    out.rgba.assign(static_cast<size_t>(f.bounds_w) * f.bounds_h * 4, 0);
    for (size_t p = 0; p < f.pixels.size(); ++p) {
        const uint8_t idx = f.pixels[p];
        if (idx == 0) continue; // 透明
        uint8_t* d = out.rgba.data() + p * 4;
        d[0] = pal_768[idx * 3] * 4;       // 6-6-6 → 8-8-8（×4）
        d[1] = pal_768[idx * 3 + 1] * 4;
        d[2] = pal_768[idx * 3 + 2] * 4;
        d[3] = 255;
    }
    return out;
}

bool TerrainTile::serialize_frame(const TerrainTileFrame& f, std::vector<uint8_t>& out) {
    // 布局: [TLRF 4][ver 2][bx/by/bw/bh/px_n 5×4] = 26 字节头，随后 pixels/depth/元数据。
    // 头长必须与 deserialize 严格一致——曾因算式多加常量导致像素区后移 7 字节，
    // 缓存命中帧逐行右移 7px（3×3 平图实测定位，DEBUGGING 3.13）。
    const size_t head = 4 + 2 + 5 * 4;
    out.assign(head + f.pixels.size() * 2 + 3, 0);
    uint8_t* p = out.data();
    p[0] = 'T';
    p[1] = 'L';
    p[2] = 'R';
    p[3] = 'F';
    core::write_u16_le(p + 4, kFrameSchema);
    core::write_i32_le(p + 6, f.bounds_x);
    core::write_i32_le(p + 10, f.bounds_y);
    core::write_i32_le(p + 14, f.bounds_w);
    core::write_i32_le(p + 18, f.bounds_h);
    core::write_u32_le(p + 22, static_cast<uint32_t>(f.pixels.size()));
    std::memcpy(p + head, f.pixels.data(), f.pixels.size());
    std::memcpy(p + head + f.pixels.size(), f.depth.data(), f.depth.size());
    p[head + 2 * f.pixels.size()] = static_cast<uint8_t>(f.height_off);
    p[head + 2 * f.pixels.size() + 1] = f.terrain_kind;
    p[head + 2 * f.pixels.size() + 2] = f.ramp_kind;
    return true;
}

bool TerrainTile::deserialize_frame(const uint8_t* data, size_t size, TerrainTileFrame& out,
                                    std::string* error) {
    const size_t head = 26;
    if (size < head || std::memcmp(data, "TLRF", 4) != 0) {
        if (error) *error = "bad tile cache entry";
        return false;
    }
    const uint16_t ver = core::read_u16_le(data + 4);
    if (ver != kFrameSchema) {
        if (error) *error = "tile cache schema mismatch";
        return false;
    }
    out.bounds_x = core::read_i32_le(data + 6);
    out.bounds_y = core::read_i32_le(data + 10);
    out.bounds_w = core::read_i32_le(data + 14);
    out.bounds_h = core::read_i32_le(data + 18);
    const uint32_t px_n = core::read_u32_le(data + 22);
    if (px_n > 1u << 20 || size < head + 2ull * px_n + 3) {
        if (error) *error = "tile cache entry truncated";
        return false;
    }
    out.pixels.assign(data + head, data + head + px_n);
    out.depth.assign(data + head + px_n, data + head + 2 * px_n);
    out.height_off = static_cast<int8_t>(data[head + 2 * px_n]);
    out.terrain_kind = data[head + 2 * px_n + 1];
    out.ramp_kind = data[head + 2 * px_n + 2];
    out.has_extra = out.bounds_w != 60 || out.bounds_h != 30;
    return true;
}

} // namespace ra2r::render
