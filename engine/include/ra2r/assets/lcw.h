#pragma once
// RA2R — Westwood LCW (Format80) 解压（规格见 docs/formats/shp.md §4）
#include <cstdint>
#include <vector>

namespace ra2r::assets {

// 解压到 out；dst_capacity 为输出缓冲上限（防炸弹）。返回实际输出字节数；失败返回 -1。
// 说明：回拷读取"尚未写入的区域"按 0 处理（与原引擎预清零缓冲一致）。
int lcw_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_capacity);

inline int lcw_decompress(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst,
                          size_t dst_capacity) {
    dst.resize(dst_capacity);
    const int n = lcw_decompress(src.data(), src.size(), dst.data(), dst_capacity);
    if (n >= 0) dst.resize(static_cast<size_t>(n));
    else dst.clear();
    return n;
}

} // namespace ra2r::assets
