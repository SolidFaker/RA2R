#pragma once
// RA2R — PCX（PC Paintbrush，ZSoft）位图读取。
// RA2/YR mix 内的 PCX 均为 UI 美术，两种变体：
//   24 位真彩（3 平面 × 8 位，无调色板，如 ALOADLG.PCX 载入画面）；
//   8 位索引色（1 平面，文件末尾 769 字节 = 0x0C 标记 + 256×RGB 调色板）。
// RLE 行程压缩（0xC0 前缀 = 计数字节）。格式细节见 docs/formats/pcx.md。
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ra2r::assets {

class PcxFile {
public:
    bool open(const uint8_t* data, size_t size, std::string* error = nullptr);
    bool open(const std::filesystem::path& path, std::string* error = nullptr);

    bool is_open() const { return w_ > 0; }
    int width() const { return w_; }
    int height() const { return h_; }
    int planes() const { return planes_; }
    int bits_per_pixel() const { return bpp_; }
    bool has_palette() const { return palette_ok_; }

    // 解码为 RGBA32（w×h×4）。8 位索引色走内嵌调色板（缺失时灰度渐变兜底）；
    // 24 位 RGB（3 平面）直出。不支持的位深/平面组合返回 false。
    bool decode_rgba(std::vector<uint8_t>& out, std::string* error = nullptr) const;

private:
    std::vector<uint8_t> data_;
    int w_ = 0, h_ = 0;
    int bpp_ = 0, planes_ = 0;
    int bytes_per_line_ = 0;
    std::array<uint8_t, 768> palette_{};  // 256×RGB（仅 8 位 1 平面时有效）
    bool palette_ok_ = false;
};

} // namespace ra2r::assets
