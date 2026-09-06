#pragma once
// RA2R — 规范缓存（CacheManager，docs/design/cache-layer.md 的起步实现）
//
// 起步版为"源字节 → 解码产物"的 Blob 缓存：
//   键 = CRC32(源字节) + 格式 schema 版本（解析器改动时 +1 使旧缓存失效）；
//   文件 = <root>/<crc32:08X>_v<schema>.bin（原子写：临时文件 + rename）。
// M2 先接入地形瓦片；SHP 帧/VXL 网格/规则库按设计稿后续扩展。
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ra2r::cache {

class CacheManager {
public:
    // 打开缓存根（不存在则创建）；error 输出失败原因
    bool open(const std::filesystem::path& root, std::string* error = nullptr);

    // 命中：src 的 crc32 + schema 对应条目存在 → 填 out 并返回 true
    bool get(const uint8_t* src, size_t size, uint16_t schema, std::vector<uint8_t>& out) const;
    // 写入：解码产物（原子写）
    bool put(const uint8_t* src, size_t size, uint16_t schema, const std::vector<uint8_t>& out);

    // 统计（本轮会话内 put 次数）
    uint64_t writes() const { return writes_; }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

private:
    std::filesystem::path entry_path(const uint8_t* src, size_t size, uint16_t schema) const;

    std::filesystem::path root_;
    uint64_t writes_ = 0;
    mutable uint64_t hits_ = 0, misses_ = 0;
};

} // namespace ra2r::cache
