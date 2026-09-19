#pragma once
// RA2R 工具公共：BMP 写出（各 viewer 原各自复制一份，统一到此处；
// 也顺带把 -Wconversion 的 int→char 写入收敛到一处）。
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ra2r::tools {

// rgba 为 RGBA8（w×h），写出 24 位 BMP（BGR、自底向上、行 4 字节对齐）
inline bool write_bmp_rgba(const std::filesystem::path& path, int w, int h,
                           const std::vector<uint8_t>& rgba) {
    const int stride = (w * 3 + 3) & ~3;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const auto put8 = [&](uint32_t v) { f.put(static_cast<char>(v & 0xFF)); };
    const auto w16 = [&](uint16_t v) {
        put8(v & 0xFF);
        put8(v >> 8);
    };
    const auto w32 = [&](uint32_t v) {
        put8(v & 0xFF);
        put8((v >> 8) & 0xFF);
        put8((v >> 16) & 0xFF);
        put8((v >> 24) & 0xFF);
    };
    w16(0x4D42);
    w32(static_cast<uint32_t>(54 + stride * h));
    w32(0);
    w32(54);
    w32(40);
    w32(static_cast<uint32_t>(w));
    w32(static_cast<uint32_t>(h));
    w16(1);
    w16(24);
    w32(0);
    w32(static_cast<uint32_t>(stride * h));
    w32(2835);
    w32(2835);
    w32(0);
    w32(0);
    std::vector<uint8_t> row(static_cast<size_t>(stride), 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
            row[static_cast<size_t>(x) * 3 + 0] = p[2];
            row[static_cast<size_t>(x) * 3 + 1] = p[1];
            row[static_cast<size_t>(x) * 3 + 2] = p[0];
        }
        f.write(reinterpret_cast<const char*>(row.data()), stride);
    }
    return static_cast<bool>(f);
}

} // namespace ra2r::tools
