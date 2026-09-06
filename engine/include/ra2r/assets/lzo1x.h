#pragma once
// RA2R — LZO1X 解压（IsoMapPack5 使用；格式为公开规范，实现自 lzo1x 文档化算法）
#include <cstdint>

namespace ra2r::assets {

// 解压到 dst（需预分配 dst_capacity，通常为解压长度）。
// 返回实际输出字节数；失败返回 -1。
int lzo1x_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_capacity);

} // namespace ra2r::assets
