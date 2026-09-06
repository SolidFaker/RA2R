// RA2R — FNT 字体实现（规格见 docs/formats/fnt.md）
#include "ra2r/assets/fnt_file.h"

#include <cstring>

#include "ra2r/core/endian.h"

namespace ra2r::assets {

bool FntFile::open(const uint8_t* data, size_t size, std::string* error) {
    data_.clear();
    glyphs_.clear();
    unicode_table_.clear();
    ts_cx_.clear();
    ts_cy_table_.clear();
    ts_img_ofs_.clear();
    kind_ = FntKind::Unknown;

    // ── 变体 1：'fonT' Unicode 字体（game.fnt；编辑器紧凑字体用 "FONt" 大小写变体）──
    const bool font_magic = size >= 4 && (data[0] == 'f' || data[0] == 'F') &&
                            (data[1] == 'o' || data[1] == 'O') &&
                            (data[2] == 'n' || data[2] == 'N') &&
                            (data[3] == 't' || data[3] == 'T');
    if (size >= 20 && font_magic) {
        ideograph_width_ = core::read_u32_le(data + 4);
        stride_ = core::read_u32_le(data + 8);
        lines_ = core::read_u32_le(data + 12);
        font_height_ = core::read_u32_le(data + 16);
        count_ = core::read_u32_le(data + 20);
        symbol_data_size_ = core::read_u32_le(data + 24);
        if (symbol_data_size_ != 1 + stride_ * lines_) {
            if (error) *error = "fnt symbol data size mismatch";
            return false;
        }
        // 标准 game.fnt：0x2001C 起全 64K Unicode 表 + 字形块
        if (size >= 0x2001C) {
            unicode_table_.resize(0x10000);
            for (size_t i = 0; i < 0x10000; ++i) {
                unicode_table_[i] = core::read_u16_le(data + 0x1C + i * 2);
            }
            const size_t img_off = 0x2001C;
            const size_t need = img_off + static_cast<size_t>(count_) * symbol_data_size_;
            if (size < need) {
                if (error) *error = "fnt image data truncated";
                return false;
            }
            glyphs_.resize(count_);
            for (uint32_t i = 0; i < count_; ++i) {
                const uint8_t* p = data + img_off + static_cast<size_t>(i) * symbol_data_size_;
                glyphs_[i].assign(p, p + symbol_data_size_);
            }
        }
        // 紧凑变体（cache.mix 里的 huge/efnt/map.fnt 等编辑器字体）：
        // 头一致但无全 64K 表；接受但字形表为空（校验器用途）
        kind_ = FntKind::Unicode;
        data_.assign(data, data + size);
        return true;
    }

    // ── 变体 2：TS 旧式 FNT（实测 RA2 cache.mix 字体为 21 字节头：
    //    zero 为 u16；字形偏移表紧接头后 @20，值为相对 image_ofs 的偏移）──
    if (size >= 21) {
        const uint16_t fsize = core::read_u16_le(data);
        const uint16_t id1 = core::read_u16_le(data + 2);
        const uint16_t id2 = core::read_u16_le(data + 4);
        const uint16_t id3 = core::read_u16_le(data + 6);
        const uint16_t cx_ofs = core::read_u16_le(data + 8);
        const uint16_t img_ofs = core::read_u16_le(data + 10);
        const uint16_t cy_ofs = core::read_u16_le(data + 12);
        const uint16_t id4 = core::read_u16_le(data + 14);
        const uint16_t zero = core::read_u16_le(data + 16);
        const uint8_t c_chars = data[18];
        const int8_t cy = static_cast<int8_t>(data[19]);
        if (fsize == size && id1 == 0x0002 && id2 == 0x000E && id3 == 0x0014 && id4 == 0 &&
            zero == 0) {
            const int n = c_chars + 1;
            ts_c_chars_ = c_chars;
            ts_cy_ = cy;
            ts_cx_.resize(n);
            ts_cy_table_.resize(n);
            ts_img_ofs_.resize(n);
            if (cx_ofs + n > size || cy_ofs + static_cast<size_t>(n) * 2 > size ||
                20 + static_cast<size_t>(n) * 2 > size) {
                if (error) *error = "ts fnt tables out of range";
                return false;
            }
            for (int i = 0; i < n; ++i) {
                ts_cx_[i] = data[cx_ofs + i];
                const uint16_t ycy = core::read_u16_le(data + cy_ofs + i * 2);
                ts_cy_table_[i] = static_cast<uint8_t>(ycy >> 8); // 高字节 = 行高
                const uint32_t off =
                    static_cast<uint32_t>(core::read_u16_le(data + 20 + i * 2)) + img_ofs;
                if (off >= size) {
                    if (error) *error = "ts fnt image offset out of range";
                    return false;
                }
                ts_img_ofs_[i] = off;
            }
            kind_ = FntKind::Ts;
            data_.assign(data, data + size);
            return true;
        }
    }

    if (error) *error = "not a RA2 font (neither 'fonT' nor TS fnt)";
    return false;
}

} // namespace ra2r::assets
