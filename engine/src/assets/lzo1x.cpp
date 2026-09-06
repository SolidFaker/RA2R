// RA2R — LZO1X 解压（按公开文档化的 LZO1X 格式实现；结构对照 Oberhumer lzo1x 解压语义）
#include "ra2r/assets/lzo1x.h"

namespace ra2r::assets {

int lzo1x_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_capacity) {
    const uint8_t* ip = src;
    const uint8_t* ip_end = src + src_len;
    uint8_t* op = dst;
    const uint8_t* op_end = dst + dst_capacity;
    const uint8_t* m_pos = nullptr;

    auto need = [&](size_t n) -> bool { return static_cast<size_t>(ip_end - ip) >= n; };
    auto room = [&](size_t n) -> bool { return static_cast<size_t>(op_end - op) >= n; };

    if (!need(1) || !room(1)) return -1;
    uint32_t t = 0;

    // 窥视首字节：仅在 >17 时消耗（canonical 语义）
    if (*ip > 17) {
        t = *ip++ - 17;
        if (t < 4) {
            // 短首段：直接从输入拷贝
            if (!need(t) || !room(t)) return -1;
            while (t-- > 0) *op++ = *ip++;
            if (!need(1)) return -1;
            t = *ip++;
            goto match;
        }
        if (!need(t) || !room(t)) return -1;
        while (t-- > 0) *op++ = *ip++;
        goto first_literal_run;
    }

    for (;;) {
        if (!need(1)) return -1;
        t = *ip++;
        if (t >= 16) goto match;

        // 字面量段：长度 t+3（t==0 时扩展）
        if (t == 0) {
            while (need(1) && *ip == 0) {
                t += 255;
                ++ip;
            }
            if (!need(1)) return -1;
            t += 15 + *ip++;
        }
        if (!need(t + 3) || !room(t + 3)) return -1;
        t += 3;
        while (t-- > 0) *op++ = *ip++;

    first_literal_run:
        if (!need(1)) return -1;
        t = *ip++;
        if (t >= 16) goto match;

        // 首短匹配：offset = 1 + 0x800 + (t>>2) + (ip[0]<<2)，拷贝 3 字节
        if (!need(1)) return -1;
        m_pos = op - (1 + 0x800);
        m_pos -= t >> 2;
        m_pos -= *ip++ << 2;
        if (m_pos < dst || !room(3)) return -1;
        *op++ = *m_pos++;
        *op++ = *m_pos++;
        *op++ = *m_pos;
        goto match_done;

    match:
        // 内层 match 循环：break 仅退出本层，回到外层继续字面量/EOF
        for (;;) {
            if (t >= 64) {
                // M1 长匹配
                if (!need(1)) return -1;
                m_pos = op - 1;
                m_pos -= (t >> 2) & 7;
                m_pos -= *ip++ << 3;
                t = (t >> 5) - 1;
                if (m_pos < dst || !room(static_cast<size_t>(t) + 2)) return -1;
                *op++ = *m_pos++;
                *op++ = *m_pos++;
                while (t-- > 0) *op++ = *m_pos++;
                goto match_done;
            } else if (t >= 32) {
                // M3 匹配
                t &= 31;
                if (t == 0) {
                    while (need(1) && *ip == 0) {
                        t += 255;
                        ++ip;
                    }
                    if (!need(1)) return -1;
                    t += 31 + *ip++;
                }
                if (!need(2)) return -1;
                m_pos = op - 1;
                m_pos -= (static_cast<uint32_t>(ip[0]) | (static_cast<uint32_t>(ip[1]) << 8)) >> 2;
                ip += 2;
            } else if (t >= 16) {
                // M2 匹配
                if (!need(2)) return -1;
                m_pos = op;
                m_pos -= (t & 8) << 11;
                t &= 7;
                if (t == 0) {
                    while (need(1) && *ip == 0) {
                        t += 255;
                        ++ip;
                    }
                    if (!need(1)) return -1;
                    t += 7 + *ip++;
                }
                m_pos -= (static_cast<uint32_t>(ip[0]) | (static_cast<uint32_t>(ip[1]) << 8)) >> 2;
                ip += 2;
                if (m_pos == op) goto eof_found;
                m_pos -= 0x4000;
            } else {
                // 短匹配：拷贝 2 字节
                if (!need(1)) return -1;
                m_pos = op - 1;
                m_pos -= t >> 2;
                m_pos -= *ip++ << 2;
                if (m_pos < dst || !room(2)) return -1;
                *op++ = *m_pos++;
                *op++ = *m_pos;
                goto match_done;
            }
            if (m_pos < dst || !room(static_cast<size_t>(t) + 2)) return -1;
            // 拷贝 t+2 字节
            *op++ = *m_pos++;
            *op++ = *m_pos++;
            while (t-- > 0) *op++ = *m_pos++;

        match_done:
            // 尾部扩展：由偏移低 2 位编码 0..3 个额外字节——
            // 注意：canonical LZO1X 的这些字节来自【输入流】而非匹配位置
            if (ip < src + 2) return -1;
            t = static_cast<uint32_t>(ip[-2]) & 3;
            if (t == 0) break;
            if (!need(t) || !room(t)) return -1;
            while (t-- > 0) *op++ = *ip++;
            // 读取下一个操作码，继续内层 match 循环
            if (!need(1)) return -1;
            t = *ip++;
        }
    }

eof_found:
    return static_cast<int>(op - dst);
}

} // namespace ra2r::assets
