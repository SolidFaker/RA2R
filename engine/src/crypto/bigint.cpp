// RA2R — 最小大整数实现（学校乘法 + 逐位移位减法求模）。
#include "ra2r/crypto/bigint.h"

#include <cstring>

namespace ra2r::crypto {

BigInt BigInt::from_bytes_le(const uint8_t* p, int n) {
    BigInt r;
    r.limbs = (n + 3) / 4;
    if (r.limbs > kMaxLimbs) r.limbs = kMaxLimbs;
    for (int i = 0; i < r.limbs; ++i) {
        uint32_t w = 0;
        for (int b = 0; b < 4; ++b) {
            const int idx = i * 4 + b;
            if (idx < n) w |= static_cast<uint32_t>(p[idx]) << (8 * b);
        }
        r.v[i] = w;
    }
    while (r.limbs > 1 && r.v[r.limbs - 1] == 0) {
        --r.limbs;
    }
    return r;
}

int BigInt::to_bytes_le(uint8_t* out, int cap) const {
    int n = limbs * 4;
    if (n > cap) n = cap;
    for (int i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(v[i / 4] >> (8 * (i % 4)));
    }
    // 去掉高位零字节（与参考实现一致：RSA 输出尾部零被裁剪）
    while (n > 1 && out[n - 1] == 0) {
        --n;
    }
    return n;
}

int BigInt::compare(const BigInt& a, const BigInt& b) {
    if (a.limbs != b.limbs) return a.limbs < b.limbs ? -1 : 1;
    for (int i = a.limbs - 1; i >= 0; --i) {
        if (a.v[i] != b.v[i]) return a.v[i] < b.v[i] ? -1 : 1;
    }
    return 0;
}

BigInt BigInt::mul_mod(const BigInt& a, const BigInt& b, const BigInt& m) {
    // 1. 学校乘法
    uint32_t tmp[2 * kMaxLimbs + 1] = {};
    for (int i = 0; i < a.limbs; ++i) {
        uint64_t carry = 0;
        for (int j = 0; j < b.limbs; ++j) {
            const uint64_t cur = static_cast<uint64_t>(tmp[i + j]) +
                                 static_cast<uint64_t>(a.v[i]) * b.v[j] + carry;
            tmp[i + j] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
        }
        uint64_t acc = static_cast<uint64_t>(tmp[i + b.limbs]) + carry;
        tmp[i + b.limbs] = static_cast<uint32_t>(acc);
        if (acc >> 32) tmp[i + b.limbs + 1] += static_cast<uint32_t>(acc >> 32);
    }
    int n = a.limbs + b.limbs;
    while (n > 1 && tmp[n - 1] == 0) {
        --n;
    }

    // 2. 逐位移位减法求模（320 位规模下足够快）
    uint32_t rem[2 * kMaxLimbs] = {};
    int rem_limbs = 0;
    for (int i = n * 32 - 1; i >= 0; --i) {
        // rem <<= 1
        uint64_t carry = 0;
        for (int k = 0; k < rem_limbs; ++k) {
            const uint64_t cur = (static_cast<uint64_t>(rem[k]) << 1) | carry;
            rem[k] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
        }
        if (carry) rem[rem_limbs++] = 1;
        // rem |= 当前位（移位后 bit0 必为 0）
        rem[0] |= (tmp[i / 32] >> (i % 32)) & 1;
        if (rem_limbs == 0) rem_limbs = 1;

        // 若 rem >= m 则 rem -= m
        bool ge = rem_limbs > m.limbs;
        if (!ge && rem_limbs == m.limbs) {
            ge = true;
            for (int k = m.limbs - 1; k >= 0; --k) {
                if (rem[k] != m.v[k]) {
                    ge = rem[k] > m.v[k];
                    break;
                }
            }
        }
        if (ge) {
            uint64_t borrow = 0;
            for (int k = 0; k < m.limbs; ++k) {
                const uint64_t cur = static_cast<uint64_t>(rem[k]) - m.v[k] - borrow;
                rem[k] = static_cast<uint32_t>(cur);
                borrow = (cur >> 32) & 1;
            }
            for (int k = m.limbs; k < rem_limbs && borrow; ++k) {
                const uint64_t cur = static_cast<uint64_t>(rem[k]) - borrow;
                rem[k] = static_cast<uint32_t>(cur);
                borrow = (cur >> 32) & 1;
            }
            while (rem_limbs > 1 && rem[rem_limbs - 1] == 0) {
                --rem_limbs;
            }
        }
    }

    BigInt out;
    out.limbs = rem_limbs;
    if (out.limbs > kMaxLimbs) out.limbs = kMaxLimbs;
    std::memcpy(out.v, rem, sizeof(uint32_t) * out.limbs);
    while (out.limbs > 1 && out.v[out.limbs - 1] == 0) {
        --out.limbs;
    }
    return out;
}

BigInt BigInt::pow_mod(const BigInt& base, uint32_t exp, const BigInt& m) {
    BigInt result;
    result.v[0] = 1;
    result.limbs = 1;
    BigInt b = base;
    uint32_t e = exp;
    while (e) {
        if (e & 1) result = mul_mod(result, b, m);
        b = mul_mod(b, b, m);
        e >>= 1;
    }
    return result;
}

} // namespace ra2r::crypto
