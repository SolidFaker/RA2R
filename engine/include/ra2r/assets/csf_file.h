#pragma once
// RA2R — CSF 字串表（RA2/YR 实测格式）
// 结构：' FSC' 头 + 条目（" LBL" + flags + [len+ASCII名] + 可选值）
//   值魔数 'RTS '（0x53545220）= UTF-16LE 值（每单元 ~c 变换）；
//          'WRTS'（0x53545257）= 值 + extra 音效引用（ASCII）
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace ra2r::assets {

struct CsfEntry {
    std::string value;   // 值（已做 ~c 反变换；数据类标签为原始码元的 UTF-16 文本）
    std::string extra;   // 音效引用（仅 'WRTS' 条目）
    bool has_value = false;
};

class CsfFile {
public:
    bool open(const uint8_t* data, size_t size, std::string* error = nullptr);
    bool open(const std::filesystem::path& path, std::string* error = nullptr);

    // 大小写不敏感查找；返回 nullptr 表示不存在
    const CsfEntry* get(std::string_view label) const;
    const std::map<std::string, CsfEntry>& all() const { return entries_; }
    size_t size() const { return entries_.size(); }
    uint32_t language() const { return language_; }

private:
    std::map<std::string, CsfEntry> entries_; // 键统一小写
    uint32_t language_ = 0;
};

} // namespace ra2r::assets
