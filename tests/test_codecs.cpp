// RA2R 编解码器单元测试：LCW(Format80) / LZO1X / CRC32 / 大整数（Blowfish 密钥派生用）
// 全部纯逻辑，不依赖游戏素材。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ra2r/assets/lcw.h"
#include "ra2r/assets/mix_file.h"
#include "ra2r/assets/lzo1x.h"
#include "ra2r/core/crc32.h"

using namespace ra2r;

// ── LCW（Format80）：命令逐条构造，验证解码语义 ──────────────────────────────

namespace {
std::vector<uint8_t> lcw_run(const std::vector<uint8_t>& src, size_t cap) {
    std::vector<uint8_t> out(cap, 0xAA);
    const int n = assets::lcw_decompress(src.data(), src.size(), out.data(), cap);
    if (n < 0) return {};
    out.resize(static_cast<size_t>(n));
    return out;
}
} // namespace

TEST(Lcw, LiteralCopyCommand) {
    // 0x81..0xBF：字面量 count = op & 0x3F
    std::vector<uint8_t> src = {0x83, 'A', 'B', 'C', 0x80};
    EXPECT_EQ(lcw_run(src, 16), (std::vector<uint8_t>{'A', 'B', 'C'}));
}

TEST(Lcw, LongFillCommand) {
    // 0xFE + u16 count + value
    std::vector<uint8_t> src = {0xFE, 0x05, 0x00, 0x2A, 0x80};
    EXPECT_EQ(lcw_run(src, 16), (std::vector<uint8_t>{0x2A, 0x2A, 0x2A, 0x2A, 0x2A}));
}

TEST(Lcw, ShortCopyRelativeAndLongCopyAbsolute) {
    // 短回拷：count = (op>>4)+3；op=0x00 → count=3，offset=0x0002 → 从 op-2 回拷
    // 字面量 "AB" 后回拷 3 字节 → "AB" + "ABA" = "ABABA"
    std::vector<uint8_t> src = {0x82, 'A', 'B', 0x00, 0x02, 0x80};
    EXPECT_EQ(lcw_run(src, 16), (std::vector<uint8_t>{'A', 'B', 'A', 'B', 'A'}));
    // 长回拷（绝对偏移 u16，自输出缓冲起点）：count=3 offset=0 → 前 3 字节
    std::vector<uint8_t> src2 = {0x83, 'X', 'Y', 'Z', 0xFF, 0x03, 0x00, 0x00, 0x00, 0x80};
    EXPECT_EQ(lcw_run(src2, 16), (std::vector<uint8_t>{'X', 'Y', 'Z', 'X', 'Y', 'Z'}));
}

TEST(Lcw, MediumCopyAbsolute) {
    // 0xC0..0xFD：count = (op&0x3F)+3，offset = 后续 u16（自输出缓冲起点）
    // 0xC0 → count = 3，offset = 4 → 从 dst[4] 回拷；dst[4..] 尚未写入 = 0
    // 先写 7 字节字面量，再从 offset 0 回拷 3 字节
    std::vector<uint8_t> src = {0x87, 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 0xC0, 0x00,
                                0x00, 0x80};
    EXPECT_EQ(lcw_run(src, 32),
              (std::vector<uint8_t>{'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'P', 'Q', 'R'}));
}

TEST(Lcw, TruncatedStreamFailsGracefully) {
    // 字面量声明 4 字节但流截断 → -1（不越界读）
    std::vector<uint8_t> bad = {0x84, 'A', 'B'};
    std::vector<uint8_t> out(64, 0);
    EXPECT_EQ(assets::lcw_decompress(bad.data(), bad.size(), out.data(), out.size()), -1);
    // 容量不足 → 失败而非越界
    std::vector<uint8_t> ok = {0xFE, 0xFF, 0x00, 0x11, 0x80};
    std::vector<uint8_t> small(10, 0);
    EXPECT_EQ(assets::lcw_decompress(ok.data(), ok.size(), small.data(), small.size()), -1);
}

TEST(Lcw, VectorOverloadResizes) {
    std::vector<uint8_t> src = {0x83, 1, 2, 3, 0x80};
    std::vector<uint8_t> dst;
    const int n = assets::lcw_decompress(src, dst, 8);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(dst.size(), 3u); // 接口按实际输出长度收缩
    EXPECT_EQ(dst[0], 1);
    EXPECT_EQ(dst[2], 3);
}

// ── LZO1X：字面量流 + 最小匹配 ───────────────────────────────────────────────

TEST(Lzo1x, LiteralRunThenEof) {
    // 首字节 = 17 + t（t ≥ 4 走 first_literal_run）；t=4 → 拷 4 字节字面量
    // 随后 M2 匹配（t=0x11 ≥ 16，bit3=0，t&7=1 无扩展字节）且偏移 0
    // → m_pos == op → eof_found（canonical LZO1X 的流结束哨兵）
    std::vector<uint8_t> src = {static_cast<uint8_t>(17 + 4), 'Z', 'Z', 'Z', 'Z',
                                0x11, 0x00, 0x00};
    std::vector<uint8_t> dst(16, 0);
    const int n = assets::lzo1x_decompress(src.data(), src.size(), dst.data(), dst.size());
    ASSERT_EQ(n, 4);
    EXPECT_EQ(dst[0], 'Z');
    EXPECT_EQ(dst[3], 'Z');
}

TEST(Lzo1x, ShortFirstLiteralRun) {
    // 首字节 17+2=19（t<4）：拷贝 2 字节后读操作码；0x11/0x00/0x00 同上结束
    std::vector<uint8_t> src = {static_cast<uint8_t>(17 + 2), 'A', 'B', 0x11, 0x00, 0x00};
    std::vector<uint8_t> dst(16, 0);
    const int n = assets::lzo1x_decompress(src.data(), src.size(), dst.data(), dst.size());
    ASSERT_EQ(n, 2);
    EXPECT_EQ(dst[0], 'A');
    EXPECT_EQ(dst[1], 'B');
}

TEST(Lzo1x, EmptyAndTruncatedInput) {
    std::vector<uint8_t> dst(16, 0);
    EXPECT_EQ(assets::lzo1x_decompress(nullptr, 0, dst.data(), dst.size()), -1);
    std::vector<uint8_t> trunc = {0xFF};
    EXPECT_EQ(assets::lzo1x_decompress(trunc.data(), trunc.size(), dst.data(), dst.size()), -1);
}

TEST(Lzo1x, OutputOverrunFails) {
    // 声明 18 字节字面量但输出缓冲只有 4 → 失败
    std::vector<uint8_t> src(20, 'A');
    src[0] = static_cast<uint8_t>(17 + 18);
    std::vector<uint8_t> dst(4, 0);
    EXPECT_EQ(assets::lzo1x_decompress(src.data(), src.size(), dst.data(), dst.size()), -1);
}

// ── CRC32（MIX 名哈希、缓存键）──────────────────────────────────────────────

TEST(Crc32, KnownVectors) {
    const std::string s1 = "123456789";
    EXPECT_EQ(core::crc32(reinterpret_cast<const uint8_t*>(s1.data()), s1.size()),
              0xCBF43926u); // 标准 CRC-32 测试向量
    EXPECT_EQ(core::crc32(nullptr, 0), 0u);
    const std::string s2 = "The quick brown fox jumps over the lazy dog";
    EXPECT_EQ(core::crc32(reinterpret_cast<const uint8_t*>(s2.data()), s2.size()),
              0x414FA339u);
}

TEST(Crc32, DeterministicAndOrderSensitive) {
    const std::string a = "GAPOWR";
    const std::string b = "GAPOWRMK";
    const uint32_t ha = core::crc32(reinterpret_cast<const uint8_t*>(a.data()), a.size());
    const uint32_t hb = core::crc32(reinterpret_cast<const uint8_t*>(b.data()), b.size());
    EXPECT_EQ(ha, core::crc32(reinterpret_cast<const uint8_t*>(a.data()), a.size()));
    EXPECT_NE(ha, hb);
}

// ── MIX 名哈希（大小写不敏感 + 4 字节对齐填充）──────────────────────────────

TEST(MixNameHash, CaseInsensitiveAndPadded) {
    const uint32_t h1 = assets::MixFile::crc32_of_name("rulesmd.ini");
    const uint32_t h2 = assets::MixFile::crc32_of_name("RULESMD.INI");
    const uint32_t h3 = assets::MixFile::crc32_of_name("RulesMd.Ini");
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1, h3);
    // 斜杠方向归一化
    EXPECT_EQ(assets::MixFile::crc32_of_name("a/b.dat"),
              assets::MixFile::crc32_of_name("a\\b.dat"));
    // 不同名不同哈希
    EXPECT_NE(h1, assets::MixFile::crc32_of_name("artmd.ini"));
    // 长度 4 的整数倍时不加填充（与 3 字节名区分）
    EXPECT_NE(assets::MixFile::crc32_of_name("abcd"), assets::MixFile::crc32_of_name("abc"));
}
