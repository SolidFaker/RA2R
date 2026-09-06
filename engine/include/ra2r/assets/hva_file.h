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
    // 12 个 float（3 行×4 列，行主序：前 3 列旋转/缩放，第 4 列平移）
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
