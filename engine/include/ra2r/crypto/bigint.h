#pragma once
// RA2R — 最小大整数（u32 肢、小端序），仅用于 MIX 头部 Blowfish 密钥的 RSA 解封：
// block.modpow(65537, public_modulus)，块长 40 字节（320 位）。
#include <cstdint>

namespace ra2r::crypto {

struct BigInt {
    static constexpr int kMaxLimbs = 12; // 384 位，覆盖 320 位运算需求
    uint32_t v[kMaxLimbs] = {};
    int limbs = 0; // 有效肢数（小端序）

    static BigInt from_bytes_le(const uint8_t* p, int n);
    // 输出为小端字节，去掉高位零；返回字节数（全部为零时返回 1，输出 0x00）。
    int to_bytes_le(uint8_t* out, int cap) const;

    static int compare(const BigInt& a, const BigInt& b); // a<b:-1 a==b:0 a>b:1
    static BigInt mul_mod(const BigInt& a, const BigInt& b, const BigInt& m);
    static BigInt pow_mod(const BigInt& base, uint32_t exp, const BigInt& m);
};

} // namespace ra2r::crypto
