// RA2R — VXL 实现
#include "ra2r/assets/vxl_file.h"

#include <cstring>

#include "ra2r/core/endian.h"
#include "vxl_normals.inc"

namespace ra2r::assets {

namespace {
constexpr char kMagic[16] = "Voxel Animation";
constexpr size_t kHeaderSize = 802;
constexpr size_t kSectionHeaderSize = 28;
constexpr size_t kTailerSize = 92;

float read_f32_le(const uint8_t* p) {
    uint32_t v = core::read_u32_le(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
} // namespace

const float* VxlFile::normal(uint8_t normal_type, int idx) {
    if (normal_type == 2) {
        if (idx < 0) idx = 0;
        if (idx >= kNormalCountTS) idx = kNormalCountTS - 1;
        return kVxlNormalsTS[idx];
    }
    if (idx < 0) idx = 0;
    if (idx >= kNormalCountRA2) idx = kNormalCountRA2 - 1;
    return kVxlNormalsRA2[idx];
}

bool VxlFile::open(const uint8_t* data, size_t size, std::string* error) {
    sections_.clear();
    if (size < kHeaderSize || std::memcmp(data, kMagic, 16) != 0) {
        if (error) *error = "not a VXL file (magic mismatch)";
        return false;
    }
    const uint32_t n_limbs = core::read_u32_le(data + 0x14);
    const uint32_t body_size = core::read_u32_le(data + 0x1C);
    std::memcpy(palette_.data(), data + 0x22, 768);

    const size_t body_start = kHeaderSize + kSectionHeaderSize * n_limbs;
    const size_t tailer_start = body_start + body_size;
    if (n_limbs == 0 || n_limbs > 64 || tailer_start + kTailerSize * n_limbs > size) {
        if (error) *error = "VXL size fields out of range";
        return false;
    }

    sections_.resize(n_limbs);
    for (uint32_t i = 0; i < n_limbs; ++i) {
        const uint8_t* sh = data + kHeaderSize + kSectionHeaderSize * i;
        const uint8_t* tl = data + tailer_start + kTailerSize * i;
        VxlSection& s = sections_[i];
        s.name.assign(reinterpret_cast<const char*>(sh), 16);
        s.name = s.name.c_str(); // 截到 NUL
        const int32_t span_start_off = static_cast<int32_t>(core::read_u32_le(tl));
        const int32_t span_end_off = static_cast<int32_t>(core::read_u32_le(tl + 4));
        const int32_t span_data_off = static_cast<int32_t>(core::read_u32_le(tl + 8));
        s.det = read_f32_le(tl + 0x0C);
        for (int k = 0; k < 3; ++k) {
            s.min[k] = read_f32_le(tl + 0x40 + k * 4);
            s.max[k] = read_f32_le(tl + 0x4C + k * 4);
        }
        s.sx = tl[0x58];
        s.sy = tl[0x59];
        s.sz = tl[0x5A];
        s.normal_type = tl[0x5B];
        if (s.sx == 0 || s.sy == 0 || s.sz == 0) {
            if (error) *error = "VXL section with zero dimension";
            return false;
        }
        const size_t col_count = static_cast<size_t>(s.sx) * s.sy;
        const size_t span_lists_size = col_count * 4;
        if (span_start_off < 0 || span_end_off < 0 || span_data_off < 0 ||
            static_cast<size_t>(span_start_off) + span_lists_size > body_size ||
            static_cast<size_t>(span_end_off) + span_lists_size > body_size ||
            static_cast<size_t>(span_data_off) > body_size) {
            if (error) *error = "VXL span offsets out of range";
            return false;
        }
        const uint8_t* body = data + body_start;
        const uint8_t* starts = body + span_start_off;
        const uint8_t* ends = body + span_end_off;
        const uint8_t* sdata = body + span_data_off;

        s.voxels.assign(static_cast<size_t>(s.sx) * s.sy * s.sz, VxlVoxel{});
        for (uint16_t y = 0; y < s.sy; ++y) {
            for (uint16_t x = 0; x < s.sx; ++x) {
                const size_t j = static_cast<size_t>(x) + static_cast<size_t>(s.sx) * y;
                const int32_t st = static_cast<int32_t>(core::read_u32_le(starts + j * 4));
                const int32_t en = static_cast<int32_t>(core::read_u32_le(ends + j * 4));
                if (st < 0 || en < st) continue;
                if (static_cast<uint32_t>(en) >= body_size - span_data_off) {
                    if (error) *error = "VXL span data out of range";
                    return false;
                }
                const uint8_t* p = sdata + st;
                const uint8_t* pend = sdata + en; // 末字节含在内
                int z = 0;
                while (p <= pend && z < s.sz) {
                    z += p[0]; // SKIP
                    if (p + 1 > pend) break;
                    const int count = p[1];
                    p += 2;
                    if (p + count * 2 > pend + 1) {
                        if (error) *error = "VXL run overruns column";
                        return false;
                    }
                    for (int k = 0; k < count && z < s.sz; ++k, ++z) {
                        const size_t vi = static_cast<size_t>(x) +
                                          static_cast<size_t>(s.sx) * (y + s.sy * z);
                        s.voxels[vi].color = p[0];
                        s.voxels[vi].normal = p[1];
                        p += 2;
                    }
                    if (p <= pend) ++p; // 末尾重复 COUNT 字节
                }
            }
        }
    }
    return true;
}

} // namespace ra2r::assets
