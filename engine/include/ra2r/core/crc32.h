#pragma once
// RA2R — 标准 CRC-32（init/xorout 0xFFFFFFFF，与 zlib 一致；MIX 名哈希同款）
#include <cstddef>
#include <cstdint>

namespace ra2r::core {

uint32_t crc32(const uint8_t* data, size_t len);

} // namespace ra2r::core
