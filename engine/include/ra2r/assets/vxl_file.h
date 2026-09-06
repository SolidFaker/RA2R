#pragma once
// RA2R — TS/RA2 VXL 体素模型读取（规格见 docs/formats/vxl.md）
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ra2r::assets {

struct VxlVoxel {
    uint8_t color = 0;  // 调色板索引（0 = 空）
    uint8_t normal = 0; // 法线索引（RA2: 0..243）
};

struct VxlSection {
    std::string name;
    uint8_t sx = 0, sy = 0, sz = 0;
    uint8_t normal_type = 0; // 2=TS, 4=RA2
    float det = 0;
    float min[3] = {}, max[3] = {};
    // 体素网格，索引 [x + sx*(y + sy*z)]
    std::vector<VxlVoxel> voxels;

    const VxlVoxel& at(int x, int y, int z) const {
        return voxels[static_cast<size_t>(x) + static_cast<size_t>(sx) * (y + sy * z)];
    }
};

class VxlFile {
public:
    bool open(const uint8_t* data, size_t size, std::string* error);

    const std::array<uint8_t, 768>& palette() const { return palette_; }
    const std::vector<VxlSection>& sections() const { return sections_; }

    // 法线方向表：normal_type 2=TS(36条), 4=RA2(244条)
    static const float* normal(uint8_t normal_type, int idx);
    static constexpr int kNormalCountRA2 = 244;
    static constexpr int kNormalCountTS = 36;

private:
    std::array<uint8_t, 768> palette_{};
    std::vector<VxlSection> sections_;
};

} // namespace ra2r::assets
