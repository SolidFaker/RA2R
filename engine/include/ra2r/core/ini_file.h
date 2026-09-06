#pragma once
// RA2R — Westwood INI 解析（节/键不区分大小写；支持重复键；';' 注释）
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ra2r::core {

class IniFile {
public:
    // 保留插入顺序的键值对（含重复键）
    using Pairs = std::vector<std::pair<std::string, std::string>>;

    bool parse(const uint8_t* data, size_t size, std::string* error = nullptr);
    bool parse_file(const std::filesystem::path& path, std::string* error = nullptr);

    const std::vector<std::string>& section_names() const { return section_names_; }
    bool has_section(std::string_view name) const;
    const Pairs& section(std::string_view name) const; // 不存在时返回空表
    std::string get(std::string_view section, std::string_view key,
                    std::string_view def = {}) const;
    std::vector<std::string> get_all(std::string_view section, std::string_view key) const;
    bool get_bool(std::string_view section, std::string_view key, bool def = false) const;
    int get_int(std::string_view section, std::string_view key, int def = 0) const;

private:
    struct Section {
        std::string name; // 原始大小写
        Pairs pairs;
    };
    std::vector<Section> sections_;
    std::vector<std::string> section_names_;
};

} // namespace ra2r::core
