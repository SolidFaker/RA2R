// RA2R — Blowfish 实现（算法规范见 Schneier《Applied Cryptography》第 14 章）。
// P 数组与 S 盒取自圆周率 π 的前 8336 个十六进制位（由 tools/gen_pi_tables.py
// 用 BBP 公式独立计算生成，见 engine/src/crypto/pi_tables.inc）。
#include "ra2r/crypto/blowfish.h"

#include <cstring>
#include <utility>

#include "pi_tables.inc"

namespace ra2r::crypto {

namespace {
inline uint32_t be_load(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline void be_store(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
} // namespace

uint32_t Blowfish::f(uint32_t x) const {
    const uint8_t a = static_cast<uint8_t>(x >> 24);
    const uint8_t b = static_cast<uint8_t>(x >> 16);
    const uint8_t c = static_cast<uint8_t>(x >> 8);
    const uint8_t d = static_cast<uint8_t>(x);
    return ((s_[0][a] + s_[1][b]) ^ s_[2][c]) + s_[3][d];
}

void Blowfish::crypt_block(uint32_t& l, uint32_t& r, bool encrypt) const {
    if (encrypt) {
        for (int i = 0; i < 16; ++i) {
            l ^= p_[i];
            r ^= f(l);
            std::swap(l, r);
        }
        std::swap(l, r);
        r ^= p_[16];
        l ^= p_[17];
    } else {
        for (int i = 17; i > 1; --i) {
            l ^= p_[i];
            r ^= f(l);
            std::swap(l, r);
        }
        std::swap(l, r);
        r ^= p_[1];
        l ^= p_[0];
    }
}

void Blowfish::set_key(const uint8_t* key, int key_len) {
    if (key_len < 1) key_len = 1;
    if (key_len > kMaxKeyLen) key_len = kMaxKeyLen;

    // 1. 初始化 P 数组与 S 盒（π 十六进制位）。
    std::memcpy(p_, kPiP, sizeof(p_));
    for (int i = 0; i < 4; ++i) {
        std::memcpy(s_[i], kPiS[i], sizeof(s_[i]));
    }

    // 2. 密钥与 P 数组循环异或（密钥按大端 32 位字解释）。
    int j = 0;
    for (int i = 0; i < 18; ++i) {
        uint32_t w = 0;
        for (int b = 0; b < 4; ++b) {
            w = (w << 8) | key[j % key_len];
            ++j;
        }
        p_[i] ^= w;
    }

    // 3. 用全零块反复加密，刷新 P 数组与 S 盒。
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) {
        crypt_block(l, r, true);
        p_[i] = l;
        p_[i + 1] = r;
    }
    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i < 256; i += 2) {
            crypt_block(l, r, true);
            s_[k][i] = l;
            s_[k][i + 1] = r;
        }
    }
}

void Blowfish::encrypt_block(const uint8_t in[kBlockSize], uint8_t out[kBlockSize]) const {
    uint32_t l = be_load(in);
    uint32_t r = be_load(in + 4);
    crypt_block(l, r, true);
    be_store(out, l);
    be_store(out + 4, r);
}

void Blowfish::decrypt_block(const uint8_t in[kBlockSize], uint8_t out[kBlockSize]) const {
    uint32_t l = be_load(in);
    uint32_t r = be_load(in + 4);
    crypt_block(l, r, false);
    be_store(out, l);
    be_store(out + 4, r);
}

} // namespace ra2r::crypto
