// RA2R — LCW (Format80) 实现（命令表见 docs/formats/shp.md §4.2/§4.4）
#include "ra2r/assets/lcw.h"

#include <cstring>

#include "ra2r/core/endian.h"

namespace ra2r::assets {

int lcw_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_capacity) {
    size_t ip = 0;   // 输入位置
    size_t op = 0;   // 输出位置
    const uint8_t* dst_base = dst;

    auto read_u16 = [&](size_t pos) {
        return pos + 2 <= src_len ? core::read_u16_le(src + pos) : 0u;
    };
    auto put = [&](uint8_t v) -> bool {
        if (op >= dst_capacity) return false;
        dst[op++] = v;
        return true;
    };
    // 从输出缓冲回拷（越界/未写区域按 0）
    auto copy_back = [&](size_t abs_pos, size_t count) -> bool {
        for (size_t i = 0; i < count; ++i) {
            const uint8_t v = abs_pos + i < op ? dst[abs_pos + i] : 0;
            if (!put(v)) return false;
        }
        return true;
    };

    while (ip < src_len) {
        const uint8_t opc = src[ip++];
        if ((opc & 0x80) == 0) {
            // 命令 0：短回拷（相对偏移 12 位）
            if (ip >= src_len) return -1;
            const size_t count = (opc >> 4) + 3;
            const size_t offset = ((opc & 0x0F) << 8) | src[ip++];
            if (offset > op) {
                // 相对回拷：dst - offset；未写区域按 0
                const size_t rel = op - offset;
                if (!copy_back(rel, count)) return -1;
            } else {
                if (!copy_back(op - offset, count)) return -1;
            }
        } else if (opc == 0x80) {
            // 流结束
            break;
        } else if ((opc & 0x40) == 0) {
            // 命令 1：字面量拷贝
            const size_t count = opc & 0x3F;
            for (size_t i = 0; i < count; ++i) {
                if (ip >= src_len || !put(src[ip++])) return -1;
            }
        } else if (opc == 0xFE) {
            // 命令 3：长填充
            if (ip + 3 > src_len) return -1;
            const size_t count = read_u16(ip);
            ip += 2;
            const uint8_t v = src[ip++];
            for (size_t i = 0; i < count; ++i) {
                if (!put(v)) return -1;
            }
        } else if (opc == 0xFF) {
            // 命令 4：长回拷（绝对偏移）
            if (ip + 4 > src_len) return -1;
            const size_t count = read_u16(ip);
            ip += 2;
            const size_t offset = read_u16(ip);
            ip += 2;
            if (!copy_back(offset, count)) return -1;
        } else {
            // 命令 2：中等回拷（绝对偏移 u16）
            if (ip + 2 > src_len) return -1;
            const size_t count = (opc & 0x3F) + 3;
            const size_t offset = read_u16(ip);
            ip += 2;
            if (!copy_back(offset, count)) return -1;
        }
    }
    return static_cast<int>(op);
}

} // namespace ra2r::assets
