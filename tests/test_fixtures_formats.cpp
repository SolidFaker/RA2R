// RA2R 夹具格式测试（第二批）：PCX / CSF / FNT / AUD——合成夹具驱动，无需游戏素材。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ra2r/assets/aud_file.h"
#include "ra2r/assets/csf_file.h"
#include "ra2r/assets/fnt_file.h"
#include "ra2r/assets/pal_file.h"
#include "ra2r/assets/pcx_file.h"

using namespace ra2r;

namespace {
std::filesystem::path fx_dir() {
    const char* candidates[] = {"tests/fixtures", "../tests/fixtures", "../../tests/fixtures"};
    for (const char* c : candidates)
        if (std::filesystem::is_directory(c)) return c;
    return "tests/fixtures";
}
std::vector<uint8_t> fx(const std::string& name) {
    std::ifstream f(fx_dir() / name, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}
} // namespace

// ── PCX：8 位索引色 + RLE + 内嵌调色板 ──────────────────────────────────────

TEST(FixtureFormats, PcxIndexedRleDecodes) {
    const auto raw = fx("sample.pcx");
    ASSERT_FALSE(raw.empty());
    assets::PcxFile pcx;
    std::string err;
    ASSERT_TRUE(pcx.open(raw.data(), raw.size(), &err)) << err;
    EXPECT_EQ(pcx.width(), 4);
    EXPECT_EQ(pcx.height(), 2);
    EXPECT_EQ(pcx.bits_per_pixel(), 8);
    EXPECT_EQ(pcx.planes(), 1);
    EXPECT_TRUE(pcx.has_palette());
    std::vector<uint8_t> rgba;
    ASSERT_TRUE(pcx.decode_rgba(rgba, &err)) << err;
    ASSERT_EQ(rgba.size(), 4u * 2 * 4);
    // 行 0：索引 1,2,2,3 → 查内嵌调色板
    // palette[1] = (4,12,28)、palette[2] = 纯红 (255,0,0)、palette[3] = (12,36,84)
    EXPECT_EQ(rgba[0], 4);
    EXPECT_EQ(rgba[1], 12);
    EXPECT_EQ(rgba[2], 28);
    EXPECT_EQ(rgba[4], 255); // 索引 2 R
    EXPECT_EQ(rgba[5], 0);
    EXPECT_EQ(rgba[8], 255);
    EXPECT_EQ(rgba[12], 12);
    // 行 1：全索引 4 → palette[4] = (16,48,112)
    for (int x = 0; x < 4; ++x) {
        EXPECT_EQ(rgba[(4 + x) * 4], 16);
        EXPECT_EQ(rgba[(4 + x) * 4 + 1], 48);
        EXPECT_EQ(rgba[(4 + x) * 4 + 2], 112);
    }
    // 坏数据（截断头）→ 失败而非崩溃
    assets::PcxFile bad;
    EXPECT_FALSE(bad.open(raw.data(), 8, &err));
}

// ── CSF 字串表：'RTS ' 反变换 / 'WRTS' 音效引用 / 大小写不敏感 ──────────────

TEST(FixtureFormats, CsfLabelsAndInverseUtf16) {
    const auto raw = fx("sample.csf");
    ASSERT_FALSE(raw.empty());
    assets::CsfFile csf;
    std::string err;
    ASSERT_TRUE(csf.open(raw.data(), raw.size(), &err)) << err;
    EXPECT_EQ(csf.size(), 2u);
    EXPECT_EQ(csf.language(), 0u);
    const auto* e = csf.get("Name:Americans");
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->has_value);
    // 反变换后的 UTF-16 已转为 UTF-8 文本
    EXPECT_EQ(e->value, "America");
    // 大小写不敏感
    EXPECT_NE(csf.get("name:americans"), nullptr);
    EXPECT_NE(csf.get("NAME:AMERICANS"), nullptr);
    EXPECT_EQ(csf.get("nope:xyz"), nullptr);
    // 'WRTS' 条目带音效引用
    const auto* d = csf.get("DESC:E31");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->value, "Demo Map");
    EXPECT_FALSE(d->extra.empty());
}

// ── FNT Unicode 位图字体：码点表 + 字形位 ──────────────────────────────────

TEST(FixtureFormats, FntUnicodeGlyphBits) {
    const auto raw = fx("sample.fnt");
    ASSERT_FALSE(raw.empty());
    assets::FntFile fnt;
    std::string err;
    ASSERT_TRUE(fnt.open(raw.data(), raw.size(), &err)) << err;
    EXPECT_TRUE(fnt.is_unicode());
    EXPECT_EQ(fnt.kind(), assets::FntKind::Unicode);
    EXPECT_EQ(fnt.stride(), 1u);
    EXPECT_EQ(fnt.lines(), 2u);
    EXPECT_EQ(fnt.glyph_count(), 1u);
    EXPECT_TRUE(fnt.has_glyph(0x41));  // 'A'
    EXPECT_FALSE(fnt.has_glyph(0x42)); // 'B'
    const auto g = fnt.glyph(0x41);
    EXPECT_EQ(g.width, 3);
    EXPECT_TRUE(g.pixel(0, 0)); // 0b111
    EXPECT_TRUE(g.pixel(1, 0));
    EXPECT_TRUE(g.pixel(2, 0));
    EXPECT_TRUE(g.pixel(0, 1)); // 0b101
    EXPECT_FALSE(g.pixel(1, 1));
    const auto e = fnt.glyph(0x99); // 无字形 → 空且不崩
    EXPECT_EQ(e.width, 0);
    EXPECT_FALSE(e.pixel(0, 0));
}

// ── PAL：二进制 / JASC 文本 / to_rgba / 错误路径 ────────────────────────────

TEST(FixtureFormats, PalLoadVariantsAndToRgba) {
    // 二进制 768 字节：6-6-6 → ×4
    const auto raw = fx("sample.pal");
    ASSERT_EQ(raw.size(), 768u);
    assets::Palette pal;
    std::string err;
    ASSERT_TRUE(pal.load(raw.data(), raw.size(), &err)) << err;
    EXPECT_TRUE(pal.loaded);
    uint8_t r, g, b, a;
    pal.to_rgba(2, r, g, b, a); // 夹具索引 2 = 纯红（63,0,0）
    EXPECT_EQ(r, 252);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(b, 0);
    EXPECT_EQ(a, 255);
    pal.to_rgba(0, r, g, b, a); // 索引 0 强制透明
    EXPECT_EQ(a, 0);
    // 文件路径加载
    assets::Palette from_file;
    ASSERT_TRUE(from_file.load(fx_dir() / "sample.pal", &err)) << err;
    from_file.to_rgba(2, r, g, b, a);
    EXPECT_EQ(r, 252);
    // 不存在的文件 / 过小的数据 → 失败
    assets::Palette missing;
    EXPECT_FALSE(missing.load(fx_dir() / "no_such.pal", &err));
    assets::Palette small;
    EXPECT_FALSE(small.load(raw.data(), 10, &err));
}

TEST(FixtureFormats, PalJascTextFormat) {
    const std::string text =
        "JASC-PAL\r\n0100\r\n3\r\n63 0 0\r\n0 63 0\r\n0 0 63\r\n";
    assets::Palette pal;
    std::string err;
    ASSERT_TRUE(pal.load(reinterpret_cast<const uint8_t*>(text.data()), text.size(), &err))
        << err;
    EXPECT_TRUE(pal.loaded);
    uint8_t r, g, b, a;
    pal.to_rgba(0, r, g, b, a);
    EXPECT_EQ(a, 0); // 索引 0 强制透明
    pal.to_rgba(1, r, g, b, a); // 第 2 行 (0,63,0) → 纯绿
    EXPECT_EQ(r, 0);
    EXPECT_EQ(g, 252);
    pal.to_rgba(2, r, g, b, a); // 第 3 行 (0,0,63) → 纯蓝
    EXPECT_EQ(b, 252);
}

// ── AUD 无压缩块解码 ────────────────────────────────────────────────────────

TEST(FixtureFormats, AudUncompressedBlocks) {
    const auto raw = fx("sample.aud");
    ASSERT_FALSE(raw.empty());
    assets::AudFile aud;
    std::string err;
    ASSERT_TRUE(aud.open(raw.data(), raw.size(), &err)) << err;
    EXPECT_FALSE(aud.is_wav());
    EXPECT_EQ(aud.rate(), 22050);
    EXPECT_EQ(aud.channels(), 1);
    EXPECT_EQ(aud.compression(), 0);
    std::vector<int16_t> pcm;
    ASSERT_TRUE(aud.decode_pcm16(pcm, &err)) << err;
    ASSERT_EQ(pcm.size(), 6u); // 4 + 2 字节（8 位 → 各 1 样本）
    EXPECT_EQ(pcm[0], 1);
    EXPECT_EQ(pcm[3], 4);
    EXPECT_EQ(pcm[4], 5);
    EXPECT_EQ(pcm[5], 6);
    assets::AudFile bad;
    EXPECT_FALSE(bad.open(raw.data(), 6, &err)); // 截断 → 失败
}
