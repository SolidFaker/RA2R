// RA2R — PAL 调色板实现
#include "ra2r/assets/pal_file.h"

#include <cstdio>
#include <fstream>
#include <vector>

namespace ra2r::assets {

namespace {
bool read_all(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}
} // namespace

bool Palette::load(const std::filesystem::path& path, std::string* error) {
    std::vector<uint8_t> data;
    if (!read_all(path, data)) {
        if (error) *error = "cannot read palette: " + path.string();
        return false;
    }
    return load(data.data(), data.size(), error);
}

bool Palette::load(const uint8_t* data, size_t size, std::string* error) {
    // JASC-PAL 文本
    if (size >= 4 && data[0] == 'J' && data[1] == 'A' && data[2] == 'S' && data[3] == 'C') {
        // 解析 "R G B" 行
        size_t pos = 0;
        int count = 0;
        auto skip_line = [&]() {
            while (pos < size && data[pos] != '\n') ++pos;
            if (pos < size) ++pos;
        };
        skip_line(); // JASC-PAL
        skip_line(); // 0100
        skip_line(); // 256
        while (pos < size && count < kColors) {
            int r = 0, g = 0, b = 0;
            auto read_num = [&](int& v) {
                v = 0;
                while (pos < size && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r')) ++pos;
                while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
                    v = v * 10 + (data[pos] - '0');
                    ++pos;
                }
            };
            read_num(r);
            read_num(g);
            read_num(b);
            rgb[count * 3 + 0] = static_cast<uint8_t>(r * 4);
            rgb[count * 3 + 1] = static_cast<uint8_t>(g * 4);
            rgb[count * 3 + 2] = static_cast<uint8_t>(b * 4);
            ++count;
            skip_line();
        }
        loaded = count > 0;
        return loaded;
    }
    // 二进制：256 × 3 字节（6-6-6，放大到 8-8-8）
    if (size < kColors * 3) {
        if (error) *error = "palette too small";
        return false;
    }
    for (int i = 0; i < kColors; ++i) {
        rgb[i * 3 + 0] = static_cast<uint8_t>(data[i * 3 + 0] * 4);
        rgb[i * 3 + 1] = static_cast<uint8_t>(data[i * 3 + 1] * 4);
        rgb[i * 3 + 2] = static_cast<uint8_t>(data[i * 3 + 2] * 4);
    }
    loaded = true;
    return true;
}

void Palette::to_rgba(uint8_t idx, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
    if (idx == 0) {
        r = g = b = a = 0;
        return;
    }
    r = rgb[idx * 3 + 0];
    g = rgb[idx * 3 + 1];
    b = rgb[idx * 3 + 2];
    a = 255;
}

} // namespace ra2r::assets
