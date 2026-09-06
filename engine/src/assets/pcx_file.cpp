// RA2R — PCX（PC Paintbrush）位图解码。
// 头部 128 字节：魔数 0x0A / 版本 / 编码(1=RLE) / 位深 / 边界框 / 平面数 /
// 每平面每行字节数；数据按"逐行 × 逐平面"排列，RLE 行程压缩。
// 调色板：8 位 1 平面时位于文件末尾 769 字节（0x0C + 256×RGB）；缺失则灰度。
#include "ra2r/assets/pcx_file.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace ra2r::assets {

namespace {

constexpr uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// 解码一行 RLE 数据到 out（长度 bytes_per_line）；流耗尽返回 false（不视为致命错误）
bool decode_line(const std::vector<uint8_t>& data, size_t& pos, uint8_t* out, int len) {
    int n = 0;
    while (n < len) {
        if (pos >= data.size()) return false;
        const uint8_t c = data[pos++];
        int count = 1;
        uint8_t value = c;
        if ((c & 0xC0) == 0xC0) {  // 计数字节：低 6 位 = 重复次数
            count = c & 0x3F;
            if (pos >= data.size()) return false;
            value = data[pos++];
        }
        if (n + count > len) count = len - n;  // 超出行长的数据截断保护
        std::memset(out + n, value, static_cast<size_t>(count));
        n += count;
    }
    return true;
}

}  // namespace

bool PcxFile::open(const uint8_t* data, size_t size, std::string* error) {
    data_.clear();
    w_ = h_ = 0;
    bpp_ = planes_ = 0;
    bytes_per_line_ = 0;
    palette_ok_ = false;
    if (!data || size < 128) {
        if (error) *error = "数据过短（<128 字节头部）";
        return false;
    }
    if (data[0] != 0x0A) {
        if (error) *error = "魔数不是 PCX（0x0A）";
        return false;
    }
    const int version = data[1];
    const int encoding = data[2];
    bpp_ = data[3];
    const int xmax = read_u16_le(data + 8);
    const int ymax = read_u16_le(data + 10);
    planes_ = data[65];
    bytes_per_line_ = read_u16_le(data + 66);
    w_ = xmax + 1;
    h_ = ymax + 1;
    if (encoding != 1) {
        if (error) *error = "仅支持 RLE 编码（encoding!=1）";
        return false;
    }
    if (version < 2 || version > 5) {
        if (error) *error = "未知版本 " + std::to_string(version);
        return false;
    }
    if (w_ <= 0 || h_ <= 0 || bytes_per_line_ < (planes_ > 1 ? w_ : (w_ * bpp_ + 7) / 8)) {
        if (error) *error = "头部尺寸字段非法";
        return false;
    }
    // 8 位 1 平面：文件末尾 769 字节（0x0C + 256×RGB）
    if (bpp_ == 8 && planes_ == 1 && size >= 128 + 769 && data[size - 769] == 0x0C) {
        const uint8_t* pal = data + size - 768;
        for (int i = 0; i < 768; ++i) palette_[i] = pal[i];
        palette_ok_ = true;
    }
    data_.assign(data, data + size);
    return true;
}

bool PcxFile::open(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "无法打开文件: " + path.string();
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    return open(buf.data(), buf.size(), error);
}

bool PcxFile::decode_rgba(std::vector<uint8_t>& out, std::string* error) const {
    out.clear();
    if (w_ <= 0 || h_ <= 0) {
        if (error) *error = "未打开";
        return false;
    }
    // RA2 场景只出现这两种变体；其余组合明确报错
    const bool indexed = (bpp_ == 8 && planes_ == 1);
    const bool rgb24 = (bpp_ == 8 && planes_ == 3);
    if (!indexed && !rgb24) {
        if (error) *error = "不支持的 PCX 变体（位深 " + std::to_string(bpp_) + " × 平面 " +
                            std::to_string(planes_) + "）";
        return false;
    }
    // 无调色板的 8 位索引图 → 灰度渐变兜底（与 SHP 灰度预览一致）
    static std::array<uint8_t, 768> kGray = [] {
        std::array<uint8_t, 768> g{};
        for (int i = 0; i < 256; ++i) {
            g[i * 3] = g[i * 3 + 1] = g[i * 3 + 2] = static_cast<uint8_t>(i);
        }
        return g;
    }();
    const uint8_t* pal = palette_ok_ ? palette_.data() : kGray.data();

    out.resize(static_cast<size_t>(w_) * h_ * 4, 0);
    std::vector<uint8_t> line(static_cast<size_t>(bytes_per_line_));
    size_t pos = 128;  // 数据区起点
    // 实测 RA2 的 ALOADLG.PCX 数据流比头部声明的行数少 16 行（西木工具产物），
    // 流耗尽后的剩余行保持透明黑（out 已清零），不视为致命错误
    for (int y = 0; y < h_; ++y) {
        for (int p = 0; p < planes_; ++p) {
            if (!decode_line(data_, pos, line.data(), bytes_per_line_)) return true;
            for (int x = 0; x < w_; ++x) {
                uint8_t* d = &out[(static_cast<size_t>(y) * w_ + x) * 4];
                if (indexed) {
                    const uint8_t idx = line[x];
                    d[0] = pal[idx * 3];
                    d[1] = pal[idx * 3 + 1];
                    d[2] = pal[idx * 3 + 2];
                    d[3] = 255;
                } else {
                    d[p] = line[x];  // 平面 0/1/2 = R/G/B
                    if (p == 2) d[3] = 255;
                }
            }
        }
    }
    return true;
}

} // namespace ra2r::assets
