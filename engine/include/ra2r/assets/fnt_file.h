#pragma once
// RA2R — RA2/YR 字体：game.fnt（'fonT' Unicode 位图字体）与 TS 旧式 .fnt
// （规格见 docs/formats/fnt.md）
#include <cstdint>
#include <string>
#include <vector>

namespace ra2r::assets {

enum class FntKind { Unknown, Unicode, Ts };

struct FntGlyph {
    uint8_t width = 0; // 显示宽度（像素）
    // 1bpp 位图：lines() 行 × stride_bytes 字节/行；每字节最高位 = 最左像素
    const uint8_t* data = nullptr;
    int stride_bytes = 0;
    int lines = 0;
    bool pixel(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= lines || !data) return false;
        const uint8_t b = data[static_cast<size_t>(y) * stride_bytes + x / 8];
        return (b >> (7 - (x & 7))) & 1;
    }
};

class FntFile {
public:
    bool open(const uint8_t* data, size_t size, std::string* error = nullptr);

    bool is_open() const { return !data_.empty(); }
    FntKind kind() const { return kind_; }
    bool is_unicode() const { return kind_ == FntKind::Unicode; }

    uint32_t ideograph_width() const { return ideograph_width_; }
    uint32_t stride() const { return stride_; }
    uint32_t lines() const { return lines_; }
    uint32_t font_height() const { return font_height_; }
    uint32_t glyph_count() const { return count_; }
    uint32_t symbol_data_size() const { return symbol_data_size_; }
    // TS 变体：字符数（0..c_chars）与整字体高度
    uint32_t ts_char_count() const { return ts_c_chars_ + 1; }
    int ts_height() const { return ts_cy_; }

    // 码点 → 字形索引（0 = 空；否则为 UnicodeTable[cp] - 1）
    // 紧凑 'FONt' 变体无全表：视为全部为空
    uint16_t table_entry(uint32_t cp) const {
        return cp < unicode_table_.size() ? unicode_table_[cp] : 0;
    }
    bool has_glyph(uint32_t cp) const { return table_entry(cp) != 0; }

    // Unicode 变体：取字形（无字形返回空 width=0 的字形）
    FntGlyph glyph(uint32_t cp) const {
        const uint16_t e = table_entry(cp);
        FntGlyph g;
        if (e == 0 || static_cast<size_t>(e) - 1 >= glyphs_.size()) return g;
        const auto& raw = glyphs_[static_cast<size_t>(e) - 1];
        g.width = raw[0];
        g.data = raw.data() + 1;
        g.stride_bytes = static_cast<int>(stride_);
        g.lines = static_cast<int>(lines_);
        return g;
    }

    // TS 变体：字符 ch（0..255 内，超出或越界返回空）
    FntGlyph ts_glyph(int ch) const {
        FntGlyph g;
        if (kind_ != FntKind::Ts || ch < 0 || ch >= static_cast<int>(ts_char_count())) return g;
        g.width = ts_cx_[ch];
        g.data = data_.data() + ts_img_ofs_[ch];
        g.stride_bytes = (ts_cx_[ch] + 7) / 8; // 行宽按像素取整
        g.lines = ts_cy_table_[ch];
        return g;
    }

private:
    std::vector<uint8_t> data_;
    FntKind kind_ = FntKind::Unknown;
    // Unicode 变体
    uint32_t ideograph_width_ = 0;
    uint32_t stride_ = 0;
    uint32_t lines_ = 0;
    uint32_t font_height_ = 0;
    uint32_t count_ = 0;
    uint32_t symbol_data_size_ = 0;
    std::vector<uint16_t> unicode_table_;            // 65536 项
    std::vector<std::vector<uint8_t>> glyphs_;       // 每项 = [width][bitmap]
    // TS 变体
    uint32_t ts_c_chars_ = 0;
    int ts_cy_ = 0;
    std::vector<uint8_t> ts_cx_;                     // 每字符宽
    std::vector<uint8_t> ts_cy_table_;               // 每字符高
    std::vector<uint32_t> ts_img_ofs_;               // 每字符位图偏移
};

} // namespace ra2r::assets
