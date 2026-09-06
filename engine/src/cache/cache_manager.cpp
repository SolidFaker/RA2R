// RA2R — 规范缓存实现（接口见 cache_manager.h）
#include "ra2r/cache/cache_manager.h"

#include <cstdio>
#include <fstream>

#include "ra2r/core/crc32.h"

namespace ra2r::cache {

namespace {
// 原子写：写临时文件后 rename（进程内并发/崩溃半写防护）
bool write_atomic(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp);
    return !ec;
}
} // namespace

bool CacheManager::open(const std::filesystem::path& root, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        std::filesystem::create_directories(root, ec);
        if (ec) {
            if (error) *error = "cannot create cache root: " + root.string();
            return false;
        }
    }
    root_ = root;
    return true;
}

std::filesystem::path CacheManager::entry_path(const uint8_t* src, size_t size,
                                               uint16_t schema) const {
    const uint32_t key = ra2r::core::crc32(src, size);
    char name[32];
    std::snprintf(name, sizeof(name), "%08X_v%u.bin", key, schema);
    return root_ / name;
}

bool CacheManager::get(const uint8_t* src, size_t size, uint16_t schema,
                       std::vector<uint8_t>& out) const {
    std::ifstream f(entry_path(src, size, schema), std::ios::binary);
    if (!f) {
        ++misses_;
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    ++hits_;
    return !out.empty();
}

bool CacheManager::put(const uint8_t* src, size_t size, uint16_t schema,
                       const std::vector<uint8_t>& out) {
    if (!write_atomic(entry_path(src, size, schema), out)) return false;
    ++writes_;
    return true;
}

} // namespace ra2r::cache
