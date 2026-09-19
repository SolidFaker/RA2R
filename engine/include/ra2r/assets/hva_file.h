#pragma once
// RA2R — HVA 体素动画（帧 × section 的 3×4 变换矩阵，frame-major）
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ra2r::assets {

class HvaFile {
public:
    bool open(const uint8_t* data, size_t size, std::string* error = nullptr);
    bool open(const std::filesystem::path& path, std::string* error = nullptr);

    bool is_open() const { return !matrices_.empty(); }
    uint32_t frame_count() const { return frame_count_; }
    uint32_t section_count() const { return section_count_; }
    const std::vector<std::string>& section_names() const { return names_; }
    // 12 个 float（3 行×4 列**行主序**：前 3 列旋转/缩放，第 4 列平移 3/7/11）。
    // 文件内是列主序（每 4 float 一列），解析时已转置；
    // 布局依据 OpenRA `HvaReader`：ids={0,4,8,12, 1,5,9,13, 2,6,10,14} 把文件
    // 序列写入 4×4 行主序矩阵（列→行），据此 3×3 旋转必须转置使用。
    const float* matrix(uint32_t frame, uint32_t section) const {
        return matrices_.data() +
               (static_cast<size_t>(frame) * section_count_ + section) * 12;
    }

private:
    uint32_t frame_count_ = 0;
    uint32_t section_count_ = 0;
    std::vector<std::string> names_;
    std::vector<float> matrices_;
};

} // namespace ra2r::assets
