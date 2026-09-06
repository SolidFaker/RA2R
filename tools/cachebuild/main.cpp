// RA2R cachebuild — M2 缓存构建工具（docs/design/cache-layer.md 的起步实现）
//
// 用法: cachebuild <游戏目录> [--root <缓存根>]
//   递归扫描全部 MIX，解码所有地形瓦片（.TEM/.SNO/.URB/.DES/.LUN/.UBN/.TMP
//   的 TMP 变体）并把每帧像素/深度序列化进规范缓存（CacheManager）。
//   默认缓存根 = 当前目录下 cache/。
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ra2r/assets/mix_file.h"
#include "ra2r/assets/name_db.h"
#include "ra2r/cache/cache_manager.h"
#include "ra2r/core/win_unicode.h"
#include "ra2r/render/terrain_tile.h"

namespace fs = std::filesystem;
using ra2r::assets::MixFile;
using ra2r::render::TerrainTile;

namespace {

bool is_tile_ext(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = name.substr(dot + 1);
    return ext == "TEM" || ext == "SNO" || ext == "URB" || ext == "DES" || ext == "LUN" ||
           ext == "UBN" || ext == "TMP";
}

// 收集目录下全部顶层 .mix
std::vector<MixFile> load_mixes(const fs::path& dir) {
    std::vector<MixFile> out;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file()) continue;
        std::string fn = de.path().filename().string();
        std::transform(fn.begin(), fn.end(), fn.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".MIX") != 0) continue;
        MixFile m;
        std::string err;
        if (!m.open(de.path(), &err) || !m.structure_valid()) continue;
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace

static int run(int argc, char** argv) {
    const char* gamedir = nullptr;
    const char* root = "cache";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--root") == 0 && i + 1 < argc) root = argv[++i];
        else if (!gamedir) gamedir = argv[i];
    }
    if (!gamedir) {
        std::printf("usage: cachebuild <gamedir> [--root <cache-root>]\n");
        return 1;
    }
    // ── 名称索引（XCC 名库 + 本地名，与 mapview 同法）──
    auto mixes = load_mixes(gamedir);
    std::printf("顶层 mix: %zu 个\n", mixes.size());
    std::map<uint32_t, std::string> gnames;
    {
        MixFile db;
        std::string from;
        ra2r::assets::try_load_xcc_database(db, &from);
        for (const auto& [id, n] : db.names()) gnames.emplace(id, n);
        std::printf("XCC 名库: %s (%zu 名)\n", from.empty() ? "未找到" : from.c_str(),
                    db.names().size());
    }
    // 遍历（含嵌套）收集瓦片条目：扁平 mix 池（顶层 + 嵌套追加），
    // 条目记 (池序号, id)；快照递归避免 push_back 使迭代器失效
    struct TileSrc {
        size_t mix; // mix 池序号
        uint32_t id;
        std::string name;
    };
    std::vector<TileSrc> tiles;
    std::vector<MixFile> pool; // = mixes 拷贝（顶层）+ 嵌套探针
    pool.reserve(mixes.size() * 2);
    for (auto& m : mixes) pool.push_back(std::move(m));
    for (size_t i = 0; i < pool.size(); ++i) {
        for (const auto& [id, n] : pool[i].names()) gnames.emplace(id, n);
    }
    {
        const size_t top_count = pool.size();
        std::function<void(size_t, int)> walk = [&](size_t mi, int depth) {
            const auto es = pool[mi].entries(); // 快照
            for (const auto& e : es) {
                const std::string* nm = pool[mi].name_of(e.id);
                std::string name;
                if (nm) name = *nm;
                if (name.empty()) {
                    const auto git = gnames.find(e.id);
                    if (git != gnames.end()) name = git->second;
                }
                if (!name.empty()) {
                    std::transform(name.begin(), name.end(), name.begin(),
                                   [](unsigned char c) {
                                       return static_cast<char>(std::toupper(c));
                                   });
                }
                if (!name.empty() && is_tile_ext(name)) {
                    tiles.push_back({mi, e.id, name});
                    continue;
                }
                if (depth >= 3 || e.size < 12) continue;
                std::vector<uint8_t> d;
                if (!pool[mi].read_entry(e, d)) continue;
                if (!MixFile::looks_like_mix(d.data(), d.size())) continue;
                MixFile probe;
                std::string perr;
                if (!probe.open(d.data(), d.size(), &perr) || !probe.structure_valid()) continue;
                const size_t child = pool.size();
                pool.push_back(std::move(probe));
                ra2r::assets::enrich_names(pool[child]);
                for (const auto& [pid, pn] : pool[child].names()) gnames.emplace(pid, pn);
                walk(child, depth + 1);
            }
        };
        for (size_t i = 0; i < top_count; ++i) walk(i, 0);
    }
    std::printf("瓦片条目: %zu\n", tiles.size());

    // ── 解码入缓存 ──
    ra2r::cache::CacheManager cache;
    std::string err;
    if (!cache.open(root, &err)) {
        std::fprintf(stderr, "cache open failed: %s\n", err.c_str());
        return 1;
    }
    int built = 0, failed = 0;
    for (const auto& t : tiles) {
        std::vector<uint8_t> raw;
        const auto* e = pool[t.mix].find(t.id);
        if (!e || !pool[t.mix].read_entry(*e, raw)) {
            ++failed;
            continue;
        }
        TerrainTile tile;
        std::string terr;
        if (!tile.open(raw.data(), raw.size(), &terr)) {
            ++failed;
            continue;
        }
        // 缓存每个非空帧
        for (int i = 0; i < tile.frame_count(); ++i) {
            const auto& fr = tile.frame(i);
            if (fr.bounds_w <= 0 || fr.bounds_h <= 0) continue;
            std::vector<uint8_t> blob;
            TerrainTile::serialize_frame(fr, blob);
            // 帧级键：源字节 + 帧号（拼入源数据尾部）
            std::vector<uint8_t> key = raw;
            key.push_back(static_cast<uint8_t>(i));
            key.push_back(static_cast<uint8_t>(i >> 8));
            if (cache.get(key.data(), key.size(), TerrainTile::kFrameSchema, blob)) {
                continue; // 已缓存
            }
            cache.put(key.data(), key.size(), TerrainTile::kFrameSchema, blob);
        }
        ++built;
    }
    std::printf("完成: 瓦片 %d, 失败 %d; 缓存写 %llu, 命中 %llu, 未命中 %llu\n", built, failed,
                static_cast<unsigned long long>(cache.writes()),
                static_cast<unsigned long long>(cache.hits()),
                static_cast<unsigned long long>(cache.misses()));
    return failed == 0 ? 0 : 1;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) { return ra2r::core::run_wide(run, argc, wargv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
