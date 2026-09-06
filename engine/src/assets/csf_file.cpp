// RA2R — CSF 实现（格式经本地 ra2.csf/ra2md.csf 全文件实测验证：
// 4479/4479 与 5211/5211 条目精确走通）
#include "ra2r/assets/csf_file.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "ra2r/core/endian.h"

namespace ra2r::assets {

namespace {
constexpr uint32_t kMagicRts = 0x53545220;  // 'RTS '（LE 存储为 " RTS"）
constexpr uint32_t kMagicWrts = 0x53545257; // 'WRTS'（含 extra 音效引用）

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

// UTF-16LE 单元 → 文本；每单元应用 ~c（游戏的字串反变换）
std::string units_to_text(const uint8_t* p, size_t units) {
    std::string out;
    out.reserve(units);
    for (size_t i = 0; i < units; ++i) {
        const uint32_t c = ~static_cast<uint16_t>(p[i * 2] | (p[i * 2 + 1] << 8)) & 0xFFFF;
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}
} // namespace

bool CsfFile::open(const uint8_t* data, size_t size, std::string* error) {
    entries_.clear();
    if (size < 24 || data[0] != ' ' || data[1] != 'F' || data[2] != 'S' || data[3] != 'C') {
        if (error) *error = "not a CSF file";
        return false;
    }
    const uint32_t version = core::read_u32_le(data + 4);
    const uint32_t count = core::read_u32_le(data + 8);
    language_ = core::read_u32_le(data + 20);
    if (version != 3) {
        if (error) *error = "unsupported CSF version (expect 3)";
        return false;
    }
    size_t pos = 24;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 8 > size || core::read_u32_le(data + pos) != 0x4C424C20) { // 'LBL '
            if (error) *error = "CSF label magic mismatch";
            return false;
        }
        const uint32_t flags = core::read_u32_le(data + pos + 4);
        pos += 8;
        if (pos + 4 > size) {
            if (error) *error = "CSF name length out of range";
            return false;
        }
        const uint32_t name_len = core::read_u32_le(data + pos);
        pos += 4;
        if (name_len > 4096 || pos + name_len > size) {
            if (error) *error = "CSF name out of range";
            return false;
        }
        const std::string name(reinterpret_cast<const char*>(data + pos), name_len);
        pos += name_len;
        CsfEntry entry;
        if (flags & 1) {
            if (pos + 8 > size) {
                if (error) *error = "CSF value header out of range";
                return false;
            }
            const uint32_t magic = core::read_u32_le(data + pos);
            const uint32_t vlen = core::read_u32_le(data + pos + 4);
            pos += 8;
            if (magic != kMagicRts && magic != kMagicWrts) {
                if (error) *error = "CSF unknown value magic";
                return false;
            }
            if (vlen > 65536 || pos + static_cast<size_t>(vlen) * 2 > size) {
                if (error) *error = "CSF value out of range";
                return false;
            }
            entry.has_value = true;
            entry.value = units_to_text(data + pos, vlen);
            pos += static_cast<size_t>(vlen) * 2;
            if (magic == kMagicWrts) {
                if (pos + 4 > size) {
                    if (error) *error = "CSF extra length out of range";
                    return false;
                }
                const uint32_t extra_len = core::read_u32_le(data + pos);
                pos += 4;
                if (extra_len > 4096 || pos + extra_len > size) {
                    if (error) *error = "CSF extra out of range";
                    return false;
                }
                entry.extra.assign(reinterpret_cast<const char*>(data + pos), extra_len);
                pos += extra_len;
            }
        }
        entries_[lower(name)] = std::move(entry);
    }
    if (pos != size) {
        if (error) *error = "CSF trailing data";
        return false;
    }
    return true;
}

bool CsfFile::open(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open csf: " + path.string();
        return false;
    }
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return open(reinterpret_cast<const uint8_t*>(data.data()), data.size(), error);
}

const CsfEntry* CsfFile::get(std::string_view label) const {
    const auto it = entries_.find(lower(label));
    return it == entries_.end() ? nullptr : &it->second;
}

} // namespace ra2r::assets
