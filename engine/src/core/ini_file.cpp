// RA2R — INI 实现
#include "ra2r/core/ini_file.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace ra2r::core {

namespace {
std::string trim(std::string s) {
    const auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    return s;
}
std::string upper(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return r;
}
} // namespace

bool IniFile::parse(const uint8_t* data, size_t size, std::string* error) {
    sections_.clear();
    section_names_.clear();
    std::string text(reinterpret_cast<const char*>(data), size);
    Section* cur = nullptr;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string::npos ? text.size() : nl;
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            if (close == std::string::npos) continue;
            sections_.push_back({});
            cur = &sections_.back();
            cur->name = trim(line.substr(1, close - 1));
            section_names_.push_back(cur->name);
            continue;
        }
        if (!cur) continue; // 节外键忽略
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        // 行内注释：从第一个 ';' 截断（RA2 INI 惯例）
        const size_t semi = val.find(';');
        if (semi != std::string::npos) val = trim(val.substr(0, semi));
        if (key.empty()) continue;
        cur->pairs.emplace_back(std::move(key), std::move(val));
    }
    return true;
}

bool IniFile::parse_file(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open ini: " + path.string();
        return false;
    }
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), error);
}

bool IniFile::has_section(std::string_view name) const {
    const std::string u = upper(name);
    for (const auto& s : sections_) {
        if (upper(s.name) == u) return true;
    }
    return false;
}

const IniFile::Pairs& IniFile::section(std::string_view name) const {
    static const Pairs kEmpty;
    const std::string u = upper(name);
    for (const auto& s : sections_) {
        if (upper(s.name) == u) return s.pairs;
    }
    return kEmpty;
}

std::string IniFile::get(std::string_view section, std::string_view key,
                         std::string_view def) const {
    const std::string u = upper(key);
    for (const auto& [k, v] : IniFile::section(section)) {
        if (upper(k) == u) return v;
    }
    return std::string(def);
}

std::vector<std::string> IniFile::get_all(std::string_view section,
                                          std::string_view key) const {
    std::vector<std::string> out;
    const std::string u = upper(key);
    for (const auto& [k, v] : IniFile::section(section)) {
        if (upper(k) == u) out.push_back(v);
    }
    return out;
}

bool IniFile::get_bool(std::string_view section, std::string_view key, bool def) const {
    const std::string v = get(section, key);
    if (v.empty()) return def;
    return v[0] == 'y' || v[0] == 'Y' || v[0] == '1' || v[0] == 't' || v[0] == 'T';
}

int IniFile::get_int(std::string_view section, std::string_view key, int def) const {
    const std::string v = get(section, key);
    if (v.empty()) return def;
    return std::atoi(v.c_str());
}

} // namespace ra2r::core
