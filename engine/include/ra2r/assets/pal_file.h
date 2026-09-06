#pragma once
// RA2R — PAL 调色板（256×3 字节 RGB 二进制 或 JASC 文本）
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ra2r::assets {

struct Palette {
    static constexpr int kColors = 256;
    std::array<uint8_t, kColors * 3> rgb{}; // 6-6-6 → 8-8-8（乘 4 后）
    bool loaded = false;

    bool load(const std::filesystem::path& path, std::string* error = nullptr);
    bool load(const uint8_t* data, size_t size, std::string* error = nullptr);

    // 索引 → RGBA（索引 0 强制透明）
    void to_rgba(uint8_t idx, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;
};

} // namespace ra2r::assets
