// RA2R — HVA 实现（头 24B: id[16]+n_frames+n_sections；名字表 n×16；
// 矩阵 (frame×n_sections+section)×12 float32 —— 文件内为**列主序 3×4**
// （每 4 个 float = 一列：3 个旋转/缩放 + 1 个平移分量），解析时转成行主序）
#include "ra2r/assets/hva_file.h"

#include <cstring>
#include <fstream>

#include "ra2r/core/endian.h"

namespace ra2r::assets {

namespace {
float read_f32_le(const uint8_t* p) {
    uint32_t v = core::read_u32_le(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
} // namespace

bool HvaFile::open(const uint8_t* data, size_t size, std::string* error) {
    names_.clear();
    matrices_.clear();
    frame_count_ = section_count_ = 0;
    if (size < 24) {
        if (error) *error = "HVA too small";
        return false;
    }
    frame_count_ = core::read_u32_le(data + 16);
    section_count_ = core::read_u32_le(data + 20);
    const size_t names_start = 24;
    const size_t mats_start = names_start + 16ull * section_count_;
    const size_t mats_size = 12ull * frame_count_ * section_count_ * 4; // 3×4 矩阵
    if (section_count_ == 0 || frame_count_ == 0 || mats_start + mats_size > size) {
        if (error) *error = "HVA size fields out of range";
        return false;
    }
    names_.resize(section_count_);
    for (uint32_t i = 0; i < section_count_; ++i) {
        names_[i].assign(reinterpret_cast<const char*>(data + names_start + 16ull * i), 16);
        names_[i] = names_[i].c_str();
    }
    // 文件内为列主序 3×4：每 4 个 float 一列（3 个旋转/缩放 + 1 个平移分量）。
    // 映射到行主序 3×4（对应 OpenRA HvaReader 的
    // ids={0,4,8,12, 1,5,9,13, 2,6,10,14}：file[k] → 4×4 行主序的 ids[k]）：
    //   file[col*4+row] → R[row*4+col]（旋转，col/row 0..2）
    //   file[3]/[7]/[11] → R[3]/[7]/[11]（平移 tx/ty/tz）
    const size_t mats = static_cast<size_t>(frame_count_) * section_count_;
    matrices_.resize(12ull * mats);
    for (size_t e = 0; e < mats; ++e) {
        const uint8_t* src = data + mats_start + e * 48;
        float* dst = matrices_.data() + e * 12;
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row)
                dst[row * 4 + col] = read_f32_le(src + (col * 4 + row) * 4);
        dst[3] = read_f32_le(src + 3 * 4);
        dst[7] = read_f32_le(src + 7 * 4);
        dst[11] = read_f32_le(src + 11 * 4);
    }
    return true;
}

bool HvaFile::open(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open hva: " + path.string();
        return false;
    }
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return open(reinterpret_cast<const uint8_t*>(data.data()), data.size(), error);
}

} // namespace ra2r::assets
