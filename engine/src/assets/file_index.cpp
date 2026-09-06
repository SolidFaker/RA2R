// RA2R — 全 MIX 树名称索引实现（接口见 file_index.h）
#include "ra2r/assets/file_index.h"

#include <algorithm>
#include <cstdio>
#include <functional>

#include "ra2r/assets/name_db.h"

namespace ra2r::assets {

namespace {
// 名字归一化：MIX 名匹配按 CRC（大写混淆）天然大小写不敏感，
// 索引键统一大写（XCC 名库存小写）
std::string upper_name(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}
} // namespace

bool FileIndex::build(const std::filesystem::path& dir, std::string* error) {
    mixes.clear();
    by_name.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        if (error) *error = "gamedir not found: " + dir.string();
        return false;
    }
    for (auto& de : std::filesystem::directory_iterator(dir, ec)) {
        if (!de.is_regular_file()) continue;
        std::string fn = de.path().filename().string();
        std::transform(fn.begin(), fn.end(), fn.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".MIX") != 0) continue;
        MixFile m;
        std::string e2;
        if (!m.open(de.path(), &e2) || !m.structure_valid()) continue;
        enrich_names(m);
        mixes.push_back(std::move(m));
    }
    // 全局名库：XCC db + 各 mix 本地名（walk 过程中持续扩充）
    MixFile dbmix;
    std::string from;
    try_load_xcc_database(dbmix, &from);
    std::map<uint32_t, std::string> gnames;
    for (const auto& [id, n] : dbmix.names()) gnames.emplace(id, n);
    std::printf("[index] XCC 名库: %s (%zu 名), 顶层 mix %zu 个\n",
                from.empty() ? "未找到" : from.c_str(), dbmix.names().size(), mixes.size());
    for (size_t i = 0; i < mixes.size(); ++i) {
        for (const auto& [id, n] : mixes[i].names()) gnames.emplace(id, n);
    }
    // 递归索引（快照条目向量后递归，mixes push_back 安全；池序号寻址）
    const size_t top_count = mixes.size();
    std::function<void(size_t, int)> walk = [&](size_t mi, int depth) {
        const std::vector<MixEntry> es = mixes[mi].entries(); // 快照
        for (const auto& e : es) {
            const std::string* nm = mixes[mi].name_of(e.id);
            std::string name;
            if (nm) name = *nm;
            if (name.empty()) {
                const auto git = gnames.find(e.id);
                if (git != gnames.end()) name = git->second;
            }
            if (!name.empty()) {
                by_name.emplace(upper_name(name), std::make_pair(mi, e.id));
            }
            if (depth >= 3 || e.size < 12) continue;
            std::vector<uint8_t> d;
            if (!mixes[mi].read_entry(e, d)) continue;
            if (!MixFile::looks_like_mix(d.data(), d.size())) continue;
            MixFile probe;
            std::string perr;
            if (!probe.open(d.data(), d.size(), &perr) || !probe.structure_valid()) continue;
            const size_t child = mixes.size();
            mixes.push_back(std::move(probe));
            enrich_names(mixes[child]);
            for (const auto& [pid, pn] : mixes[child].names()) gnames.emplace(pid, pn);
            walk(child, depth + 1);
        }
    };
    for (size_t i = 0; i < top_count; ++i) walk(i, 0);
    return !by_name.empty();
}

bool FileIndex::read(const std::string& name, std::vector<uint8_t>& out) const {
    const auto it = by_name.find(upper_name(name));
    if (it == by_name.end()) return false;
    const MixEntry* e = mixes[it->second.first].find(it->second.second);
    return e && mixes[it->second.first].read_entry(*e, out);
}

} // namespace ra2r::assets
