// RA2R — SHP 实现（规格见 docs/formats/shp.md，已与真实文件逐字节核对）
#include "ra2r/assets/shp_file.h"

#include <algorithm>

#include "ra2r/core/endian.h"

namespace ra2r::assets {

bool ShpFile::open(const uint8_t* data, size_t size, std::string* error) {
    width_ = 0;
    height_ = 0;
    data_ = data;
    size_ = size;
    frames_.clear();
    if (size < 8) {
        if (error) *error = "shp too small";
        return false;
    }
    if (core::read_u16_le(data) != 0) {
        if (error) *error = "not a TS/RA2 shp (first u16 != 0)";
        return false;
    }
    width_ = core::read_u16_le(data + 2);
    height_ = core::read_u16_le(data + 4);
    const uint16_t count = core::read_u16_le(data + 6);
    if (8 + 24ull * count > size) {
        if (error) *error = "frame table out of range";
        return false;
    }
    frames_.resize(count);
    for (int i = 0; i < count; ++i) {
        const uint8_t* p = data + 8 + 24 * i;
        ShpFrameHeader& f = frames_[i];
        f.x = core::read_u16_le(p);
        f.y = core::read_u16_le(p + 2);
        f.cx = core::read_u16_le(p + 4);
        f.cy = core::read_u16_le(p + 6);
        f.flags = p[8];
        f.offset = core::read_u32_le(p + 20);
        if (f.x + f.cx > width_ || f.y + f.cy > height_) {
            if (error) *error = "frame rect out of canvas";
            return false;
        }
    }
    return true;
}

bool ShpFile::decode_frame(int i, std::vector<uint8_t>& out, std::string* error) const {
    out.clear();
    if (i < 0 || i >= static_cast<int>(frames_.size())) {
        if (error) *error = "frame index out of range";
        return false;
    }
    const ShpFrameHeader& f = frames_[i];
    if (f.offset == 0 || f.cx == 0 || f.cy == 0) {
        out.assign(static_cast<size_t>(f.cx) * f.cy, 0);
        return true;
    }
    const bool compressed = (f.flags & 0x02) != 0;
    if (f.offset >= size_) {
        if (error) *error = "frame offset out of range";
        return false;
    }
    // 未压缩帧的原始像素必须完整落在文件内；压缩帧长度按 RLE 流实际读取，
    // 不能按 cx·cy 预判（文件末帧的压缩载荷短于原始尺寸是常态）
    if (!compressed && static_cast<uint64_t>(f.offset) + static_cast<uint64_t>(f.cx) * f.cy >
                           size_) {
        if (error) *error = "frame data out of range";
        return false;
    }
    out.assign(static_cast<size_t>(f.cx) * f.cy, 0);
    const uint8_t* src = data_ + f.offset;

    if (!compressed) {
        // 未压缩：行优先原始像素
        std::copy(src, src + static_cast<ptrdiff_t>(f.cx) * f.cy, out.begin());
        return true;
    }

    // 逐扫描线 RLE：每行 u16 行长（含自身 2 字节）；载荷中 0x00+计数 = 透明游程
    size_t pos = 0;
    const size_t avail = size_ - f.offset;
    for (uint16_t y = 0; y < f.cy; ++y) {
        if (pos + 2 > avail) break; // 剩余行保持透明
        const uint16_t line_len = core::read_u16_le(src + pos);
        pos += 2;
        const size_t line_end = pos + (line_len >= 2 ? line_len - 2 : 0);
        if (line_end > avail) {
            if (error) *error = "rle line out of range";
            return false;
        }
        uint16_t x = 0;
        size_t p = pos;
        while (p < line_end && x < f.cx) {
            const uint8_t v = src[p++];
            if (v != 0) {
                out[static_cast<size_t>(y) * f.cx + x] = v;
                ++x;
            } else {
                if (p >= line_end) break;
                const uint8_t n = src[p++];
                const uint16_t take = static_cast<uint16_t>(
                    std::min<size_t>(n, static_cast<size_t>(f.cx) - x));
                // 透明像素保持 0，仅前进 x
                x = static_cast<uint16_t>(x + take);
            }
        }
        pos = line_end;
    }
    return true;
}

} // namespace ra2r::assets
