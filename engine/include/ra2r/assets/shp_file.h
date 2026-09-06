#pragma once
// RA2R — TS/RA2 SHP 精灵动画读取与帧解码
// 布局：8 字节头 [0,w,h,frames] + 24 字节/帧头 + 帧数据
// 压缩：帧头 flags bit1 → 逐扫描线 RLE（0x00+计数=透明游程，非零=字面像素）
#include <cstdint>
#include <string>
#include <vector>

namespace ra2r::assets {

struct ShpFrameHeader {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t cx = 0;
    uint16_t cy = 0;
    uint8_t flags = 0;
    uint32_t offset = 0; // 帧数据绝对文件偏移；0 = 空帧
};

class ShpFile {
public:
    bool open(const uint8_t* data, size_t size, std::string* error);

    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }
    uint16_t frame_count() const { return static_cast<uint16_t>(frames_.size()); }
    const ShpFrameHeader& frame(int i) const { return frames_[i]; }

    // 解码一帧为 cx×cy 调色板索引（行优先）
    bool decode_frame(int i, std::vector<uint8_t>& out, std::string* error = nullptr) const;

private:
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::vector<ShpFrameHeader> frames_;
};

} // namespace ra2r::assets
