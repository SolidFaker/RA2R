#pragma once
// RA2R — Blowfish 分组密码（按 Schneier 公开算法规范实现，块序为大端）
// 用途：解密 RA2/YR 加密 MIX 的头部与索引。
#include <cstdint>

namespace ra2r::crypto {

class Blowfish {
public:
    static constexpr int kBlockSize = 8;   // 64 位块
    static constexpr int kMaxKeyLen  = 56; // 最大密钥长度

    // key_len 取值 1..56
    void set_key(const uint8_t* key, int key_len);

    void encrypt_block(const uint8_t in[kBlockSize], uint8_t out[kBlockSize]) const;
    void decrypt_block(const uint8_t in[kBlockSize], uint8_t out[kBlockSize]) const;

private:
    uint32_t f(uint32_t x) const;
    void crypt_block(uint32_t& l, uint32_t& r, bool encrypt) const;

    uint32_t p_[18] = {};
    uint32_t s_[4][256] = {};
};

} // namespace ra2r::crypto
