#pragma once
// RA2R — MIX 归档读取（RA2/YR 格式）。
// 加密布局（实测 ra2.mix / ra2md.mix，旗标 0x00030000）：
//   [4B flags][80B RSA 封装的 Blowfish 密钥][Blowfish-ECB: 2B count + 4B size + 索引（8B 对齐补零）]
//   正文从 90 + 12*count + padding 开始；正文文件数据不加密。
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ra2r::assets {

struct MixEntry {
    uint32_t id     = 0;
    uint32_t offset = 0; // 相对正文起点
    uint32_t size   = 0;
};

class MixFile {
public:
    static constexpr uint32_t kFlagHasChecksum = 0x00010000;
    static constexpr uint32_t kFlagEncrypted   = 0x00020000;

    bool open(const std::filesystem::path& path, std::string* error);
    bool open(const uint8_t* data, size_t size, std::string* error);

    // 结构自洽校验：真实 MIX 精确满足 body_start + data_size + (校验和?20:0) == 文件大小
    bool structure_valid() const {
        if (data_.empty()) return false;
        const uint64_t expect = static_cast<uint64_t>(body_start_) + data_size_ +
                                (has_checksum() ? 20 : 0);
        return expect == data_.size();
    }

    // 判定一段数据是否为 MIX（用于嵌套结构遍历）
    static bool looks_like_mix(const uint8_t* data, size_t size);

    bool is_open() const { return !data_.empty(); }
    uint32_t flags() const { return flags_; }
    bool is_encrypted() const { return (flags_ & kFlagEncrypted) != 0; }
    bool has_checksum() const { return (flags_ & kFlagHasChecksum) != 0; }
    uint16_t file_count() const { return file_count_; }
    uint32_t data_size() const { return data_size_; }
    uint32_t body_start() const { return body_start_; }
    const std::vector<MixEntry>& entries() const { return entries_; }

    // 读取条目原始字节
    bool read_entry(const MixEntry& e, std::vector<uint8_t>& out) const;
    // 只读条目前 n 字节（避免整文件拷贝，用于扫描）
    bool read_head(const MixEntry& e, uint8_t* buf, size_t n) const;
    const MixEntry* find(uint32_t id) const;
    // 名称 → 条目（RA2 文件名混淆 + CRC32）
    const MixEntry* find_by_name(std::string_view name) const;
    // 本地文件名数据库（local mix database.dat）
    const std::string* name_of(uint32_t id) const;
    const std::map<uint32_t, std::string>& names() const { return names_; }
    // 手工扩充名称库（名称 → id 由 crc32_of_name 计算）
    void add_name(const std::string& name) { names_[crc32_of_name(name)] = name; }

    // 文件名 → 索引 id：大写 + 4 字节对齐混淆 + 标准 CRC-32（与 zlib 一致）
    static uint32_t crc32_of_name(std::string name);

private:
    bool parse(std::string* error);

    uint32_t flags_      = 0;
    uint16_t file_count_ = 0;
    uint32_t data_size_  = 0;
    uint32_t body_start_ = 0;
    std::vector<uint8_t> data_; // 整个文件的内存映像（M0 够用，后续流式化）
    std::vector<MixEntry> entries_;
    std::map<uint32_t, std::string> names_;
};

} // namespace ra2r::assets
