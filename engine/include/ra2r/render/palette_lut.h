#pragma once
// RA2R — 调色板查找表（M2 渲染模块）
//
// 原版调色板管线：768 字节 6-6-6 调色盘 → 显示时 ×4 展开为 8-8-8；
// 光照等级（Lighting Level）0..31 在原版由亮度 ramp 表实现，
// M2 起步版用线性衰减近似（0 = 全亮，31 = 最暗），后续对齐原版 ramp 时
// 只改 build/at 内部，接口不变。
#include <array>
#include <cstdint>

namespace ra2r::render {

class PaletteLut {
public:
    static constexpr int kColors = 256;
    static constexpr int kLevels = 32;

    // pal_768: 256 × RGB（每通道 6 位）；内部展开为 256×32 RGBA 查找表。
    // remap16（可选）：48 字节 = 16 × RGB，替换调色盘索引 16..31（阵营色重映射，
    // 原版 Remap 段；见 assets::HouseRamp）。
    void build(const uint8_t* pal_768, const uint8_t* remap16 = nullptr);

    // 取色：idx 为调色板索引（0 输出全透明），level 为光照等级 0..31
    void rgba(uint8_t idx, int level, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;

    // 直接访问预乘查找表（256 色 × 32 级，行主序：idx + 256*level）
    const std::array<uint32_t, kColors * kLevels>& table() const { return lut_; }

private:
    std::array<uint32_t, kColors * kLevels> lut_{};
};

} // namespace ra2r::render
