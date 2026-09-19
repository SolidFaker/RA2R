// RA2R — 调色板查找表实现（接口见 palette_lut.h）
#include "ra2r/render/palette_lut.h"

#include <algorithm>

namespace ra2r::render {

namespace {
// 光照等级 → 亮度系数：0 全亮，31 最暗（约 30% 亮度）
inline float level_brightness(int level) {
    const int l = std::clamp(level, 0, PaletteLut::kLevels - 1);
    return 1.0f - 0.7f * static_cast<float>(l) / static_cast<float>(PaletteLut::kLevels - 1);
}
} // namespace

void PaletteLut::build(const uint8_t* pal_768, const uint8_t* remap16) {
    for (int idx = 0; idx < kColors; ++idx) {
        int r8, g8, b8;
        if (remap16 && idx >= 16 && idx <= 31) {
            // 阵营色重映射段：remap16 已是 8 位 RGB（[Colors] H,S,V 生成），不再 ×4
            r8 = remap16[(idx - 16) * 3];
            g8 = remap16[(idx - 16) * 3 + 1];
            b8 = remap16[(idx - 16) * 3 + 2];
        } else {
            r8 = pal_768[idx * 3] * 4;       // 6 位 → 8 位
            g8 = pal_768[idx * 3 + 1] * 4;
            b8 = pal_768[idx * 3 + 2] * 4;
        }
        // 索引 1 = 原版阴影索引（SHP 阴影帧只用索引 1）：映射为半透明黑
        // ARGB(140,0,0,0) —— 与 OpenRA PaletteFromFile ShadowIndex=1 的
        // 重映射（ImmutablePalette: colors[i] = 140u << 24）一致。
        // 原调色盘该索引是深蓝（UNITURB.PAL idx1=(0,0,49)），直接用会画成蓝影。
        const bool shadow = (idx == 1);
        for (int level = 0; level < kLevels; ++level) {
            const float f = level_brightness(level);
            const uint8_t r = (idx == 0 || shadow)
                                  ? 0
                                  : static_cast<uint8_t>(static_cast<float>(r8) * f);
            const uint8_t g = (idx == 0 || shadow)
                                  ? 0
                                  : static_cast<uint8_t>(static_cast<float>(g8) * f);
            const uint8_t b = (idx == 0 || shadow)
                                  ? 0
                                  : static_cast<uint8_t>(static_cast<float>(b8) * f);
            const uint8_t a = idx == 0 ? 0 : (shadow ? 140 : 255);
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
