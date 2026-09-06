// RA2R — 调色板查找表实现（接口见 palette_lut.h）
#include "ra2r/render/palette_lut.h"

#include <algorithm>

namespace ra2r::render {

namespace {
// 光照等级 → 亮度系数：0 全亮，31 最暗（约 30% 亮度）
inline float level_brightness(int level) {
    const int l = std::clamp(level, 0, PaletteLut::kLevels - 1);
    return 1.0f - 0.7f * l / (PaletteLut::kLevels - 1);
}
} // namespace

void PaletteLut::build(const uint8_t* pal_768) {
    for (int idx = 0; idx < kColors; ++idx) {
        const int r8 = pal_768[idx * 3] * 4;       // 6 位 → 8 位
        const int g8 = pal_768[idx * 3 + 1] * 4;
        const int b8 = pal_768[idx * 3 + 2] * 4;
        for (int level = 0; level < kLevels; ++level) {
            const float f = level_brightness(level);
            const uint8_t r = idx == 0 ? 0 : static_cast<uint8_t>(r8 * f);
            const uint8_t g = idx == 0 ? 0 : static_cast<uint8_t>(g8 * f);
            const uint8_t b = idx == 0 ? 0 : static_cast<uint8_t>(b8 * f);
            const uint8_t a = idx == 0 ? 0 : 255; // 索引 0 = 透明
            lut_[idx + kColors * level] = static_cast<uint32_t>(a) << 24 |
                                          static_cast<uint32_t>(r) << 16 |
                                          static_cast<uint32_t>(g) << 8 | b;
        }
    }
}

void PaletteLut::rgba(uint8_t idx, int level, uint8_t& r, uint8_t& g, uint8_t& b,
                      uint8_t& a) const {
    const uint32_t v = lut_[idx + kColors * std::clamp(level, 0, kLevels - 1)];
    a = static_cast<uint8_t>(v >> 24);
    r = static_cast<uint8_t>(v >> 16);
    g = static_cast<uint8_t>(v >> 8);
    b = static_cast<uint8_t>(v);
}

} // namespace ra2r::render
