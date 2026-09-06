// RA2R assetcheck — M1 批量资产校验器
// 用法:
//   assetcheck <mix 或目录> [--quick] [--verbose] [--max-depth N]
// 功能: 递归校验 MIX 树内所有条目(按扩展名/魔数分派到各解析器), 输出统计与失败明细
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "ra2r/assets/aud_file.h"
#include "ra2r/assets/csf_file.h"
#include "ra2r/assets/fnt_file.h"
#include "ra2r/assets/hva_file.h"
#include "ra2r/assets/lcw.h"
#include "ra2r/assets/map_file.h"
#include "ra2r/assets/mix_file.h"
#include "ra2r/assets/name_db.h"
#include "ra2r/assets/pal_file.h"
#include "ra2r/assets/shp_file.h"
#include "ra2r/assets/vxl_file.h"
#include "ra2r/core/endian.h"
#include "ra2r/core/ini_file.h"
#include "ra2r/core/win_unicode.h"
#include "ra2r/render/terrain_tile.h"

namespace fs = std::filesystem;
using namespace ra2r::assets;

namespace {
bool g_quick = false;
bool g_verbose = false;
int g_max_depth = 4;
// 跨 MIX 累积的名称库（与 mixbrowser 同法：嵌套 mix 内条目靠全局名解析扩展名）
std::map<uint32_t, std::string> g_gnames;

struct Stats {
    uint64_t files = 0;    // 解析的条目/文件总数
    uint64_t ok = 0;
    uint64_t fail = 0;
    uint64_t skip = 0;    // 已知但未实现的格式
    uint64_t mixes = 0;   // 嵌套 MIX 数
    void add(const Stats& s) {
        files += s.files; ok += s.ok; fail += s.fail; skip += s.skip; mixes += s.mixes;
    }
};

std::string upper(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return v;
}

// 结果类型
enum class R { Ok, Fail, Skip, NestedMix };

// 校验一个条目数据；返回结果与说明
R check_entry(const std::string& name, const std::vector<uint8_t>& d, std::string& note,
              const char** type_out) {
    // 嵌套 MIX（魔数/结构判别优先，防止把 MIX 当别的格式误报）
    if (MixFile::looks_like_mix(d.data(), d.size())) {
        MixFile probe;
        std::string err;
        if (probe.open(d.data(), d.size(), &err) && probe.structure_valid()) {
            if (type_out) *type_out = "MIX";
            return R::NestedMix;
        }
        // 长得像 MIX 但结构非法 → 按名字后缀继续分派（可能真不是 MIX）
    }
    std::string ext;
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) ext = upper(name.substr(dot));
    std::string err;

    if (ext == ".SHP") {
        if (type_out) *type_out = "SHP";
        ShpFile f;
        if (!f.open(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        if (f.frame_count() == 0) { note = "0 frames"; return R::Fail; }
        if (!g_quick) {
            // 全帧解码（RLE），校验每帧尺寸
            for (uint32_t i = 0; i < f.frame_count(); ++i) {
                std::vector<uint8_t> idx;
                std::string e2;
                if (!f.decode_frame(static_cast<int>(i), idx, &e2)) {
                    note = "frame " + std::to_string(i) + ": " + e2;
                    return R::Fail;
                }
                const auto& fh = f.frame(static_cast<int>(i));
                if (idx.size() != static_cast<size_t>(fh.cx) * fh.cy) {
                    note = "frame " + std::to_string(i) + " size mismatch";
                    return R::Fail;
                }
            }
        }
        return R::Ok;
    }
    if (ext == ".VXL") {
        if (type_out) *type_out = "VXL";
        VxlFile f;
        if (!f.open(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        if (f.sections().empty()) { note = "no sections"; return R::Fail; }
        return R::Ok;
    }
    if (ext == ".HVA") {
        if (type_out) *type_out = "HVA";
        HvaFile f;
        if (!f.open(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        if (f.frame_count() == 0 || f.section_count() == 0) { note = "empty"; return R::Fail; }
        return R::Ok;
    }
    if (ext == ".PAL") {
        if (type_out) *type_out = "PAL";
        Palette f;
        if (d.size() >= 4 && std::memcmp(d.data(), "JASC", 4) == 0) {
            if (!f.load(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        } else if (d.size() != 768 && d.size() != 1024) {
            note = "bad pal size " + std::to_string(d.size());
            return R::Fail;
        }
        return R::Ok;
    }
    if (ext == ".AUD" || ext == ".WAV") {
        if (type_out) *type_out = ext == ".AUD" ? "AUD" : "WAV";
        AudFile f;
        if (!f.open(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        std::vector<int16_t> pcm;
        if (!f.decode_pcm16(pcm, &err)) { note = "decode: " + err; return R::Fail; }
        if (pcm.empty()) { note = "0 samples"; return R::Fail; }
        return R::Ok;
    }
    if (ext == ".CSF") {
        if (type_out) *type_out = "CSF";
        CsfFile f;
        if (!f.open(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        if (f.size() == 0) { note = "0 strings"; return R::Fail; }
        return R::Ok;
    }
    if (ext == ".INI" || ext == ".TXT") {
        if (type_out) *type_out = "INI";
        ra2r::core::IniFile ini;
        if (!ini.parse(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        return R::Ok;
    }
    if (ext == ".MAP" || ext == ".YRM" || ext == ".YRO" || ext == ".MMX") {
        if (type_out) *type_out = "MAP";
        MapFile f;
        if (!f.open(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        return R::Ok;
    }
    if (ext == ".FNT") {
        if (type_out) *type_out = "FNT";
        FntFile f;
        if (!f.open(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        if (f.is_unicode() ? f.glyph_count() == 0 : f.ts_char_count() == 0) {
            note = "0 glyphs";
            return R::Fail;
        }
        return R::Ok;
    }
    if (ext == ".PCX") {
        // Westwood PCX：仅头校验（0x0A 魔数 + 版本/编码 + 尺寸合理）
        if (type_out) *type_out = "PCX";
        if (d.size() < 128 || d[0] != 0x0A) { note = "bad pcx header"; return R::Fail; }
        const int w = ra2r::core::read_u16_le(d.data() + 8) -
                      ra2r::core::read_u16_le(d.data() + 4) + 1;
        const int h = ra2r::core::read_u16_le(d.data() + 10) -
                      ra2r::core::read_u16_le(d.data() + 6) + 1;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
            note = "bad pcx dims";
            return R::Fail;
        }
        return R::Ok;
    }
    if (ext == ".CPS") {
        // CPS：头 [u16 size][u16][u16 w][u16 h] + 原始像素
        if (type_out) *type_out = "CPS";
        if (d.size() < 8) { note = "cps too small"; return R::Fail; }
        const uint16_t fsize = ra2r::core::read_u16_le(d.data());
        const uint16_t w = ra2r::core::read_u16_le(d.data() + 4);
        const uint16_t h = ra2r::core::read_u16_le(d.data() + 6);
        if (fsize != d.size() || w == 0 || h == 0 || w > 4096 || h > 4096) {
            note = "bad cps header";
            return R::Fail;
        }
        return R::Ok;
    }
    // 地形瓦片：RA2 的瓦片文件按剧场用 .TEM/.SNO/.URB/.DES/.LUN/.UBN 后缀
    // （.TMP 是 TS 惯例）。同后缀下两种格式：
    //   a) TMP 模板（未压缩：模板头 + 帧偏移 + 菱形像素）——地面瓦片
    //   b) SHP 包装（u16 0 + w/h + 帧数）——覆盖物瓦片（弹坑/履带/燃烧建筑等）
    if (ext == ".TMP" || ext == ".TEM" || ext == ".SNO" || ext == ".URB" ||
        ext == ".DES" || ext == ".LUN" || ext == ".UBN") {
        ra2r::render::TerrainTile tmp;
        std::string tmp_err;
        if (tmp.open(d.data(), d.size(), &tmp_err) && tmp.frame_count() > 0) {
            if (type_out) *type_out = "TMP";
            return R::Ok;
        }
        ShpFile shp;
        std::string shp_err;
        if (shp.open(d.data(), d.size(), &shp_err) && shp.frame_count() > 0) {
            if (type_out) *type_out = "SHP";
            return R::Ok;
        }
        if (type_out) *type_out = "TILE";
        note = "tmp: " + tmp_err + "; shp: " + shp_err;
        return R::Fail;
    }
    // ── 名字无法解析时的内容兜底 ──
    if (d.size() >= 4 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F') {
        if (type_out) *type_out = "WAV";
        AudFile f;
        if (!f.open(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        std::vector<int16_t> pcm;
        if (!f.decode_pcm16(pcm, &err)) { note = "decode: " + err; return R::Fail; }
        return R::Ok;
    }
    if (d.size() >= 4 && std::memcmp(d.data(), "JASC", 4) == 0) {
        if (type_out) *type_out = "PAL";
        return R::Ok;
    }
    if (d.size() == 768 || d.size() == 1024) {
        if (type_out) *type_out = "PAL";
        return R::Ok;
    }
    if (d.size() >= 8 && d[0] == 0 && d[1] == 0) {
        if (type_out) *type_out = "SHP";
        ShpFile f;
        if (f.open(d.data(), d.size(), &err)) return R::Ok;
        note = err;
        return R::Fail;
    }
    if (d.size() >= 4 && d[0] == ' ' && d[1] == 'F' && d[2] == 'S' && d[3] == 'C') {
        if (type_out) *type_out = "CSF";
        CsfFile f;
        if (!f.open(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        return R::Ok;
    }
    if (d.size() >= 16 && std::memcmp(d.data(), "Voxel Animation", 16) == 0) {
        if (type_out) *type_out = "VXL";
        VxlFile f;
        if (!f.open(d.data(), d.size(), &err)) { note = err; return R::Fail; }
        return R::Ok;
    }
    if (!d.empty()) {
        // 文本/INI（首个非空白字符为 '['）
        size_t i = 0;
        while (i < d.size() && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n' ||
                                d[i] == 0xEF || d[i] == 0xBB || d[i] == 0xBF)) {
            ++i;
        }
        if (i < d.size() && d[i] == '[') {
            if (type_out) *type_out = "INI";
            ra2r::core::IniFile ini;
            if (!ini.parse(d.data(), d.size(), &err)) { note = err; return R::Fail; }
            return R::Ok;
        }
    }
    if (type_out) *type_out = "?";
    note = "unrecognized";
    return R::Skip;
}

void check_mix(MixFile& mix, const std::string& label, int depth, Stats& st) {
    const auto& es = mix.entries();
    st.files += es.size();
    for (const auto& e : es) {
        std::vector<uint8_t> d;
        if (!mix.read_entry(e, d)) {
            ++st.fail;
            std::fprintf(stderr, "FAIL %s/entry %08X: read failed\n", label.c_str(), e.id);
            continue;
        }
        std::string note, name;
        const std::string* nm = mix.name_of(e.id);
        if (nm) name = *nm;
        if (name.empty()) {
            const auto git = g_gnames.find(e.id);
            if (git != g_gnames.end()) name = git->second;
        }
        if (name.empty()) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08X", e.id);
            name = buf;
        }
        const char* type = nullptr;
        const R r = check_entry(name, d, note, &type);
        if (r == R::Ok) {
            ++st.ok;
            if (g_verbose) std::printf("OK   %s/%s (%s)\n", label.c_str(), name.c_str(), type);
        } else if (r == R::Skip) {
            ++st.skip;
            if (g_verbose) std::printf("SKIP %s/%s (%s: %s)\n", label.c_str(), name.c_str(),
                                       type, note.c_str());
        } else if (r == R::NestedMix) {
            ++st.mixes;
            if (depth + 1 <= g_max_depth) {
                MixFile probe;
                std::string err;
                if (probe.open(d.data(), d.size(), &err) && probe.structure_valid()) {
                    ra2r::assets::enrich_names(probe);
                    for (const auto& [id, n] : probe.names()) g_gnames.emplace(id, n);
                    check_mix(probe, label + "/" + name, depth + 1, st);
                    continue;
                }
            }
            ++st.skip; // 深度上限或打开失败
        } else {
            ++st.fail;
            std::fprintf(stderr, "FAIL %s/%s (%s): %s\n", label.c_str(), name.c_str(), type,
                         note.c_str());
        }
    }
}

// 递归收集名字库（含嵌套 MIX；第二遍校验时条目名已齐）
void collect_names_recursive(MixFile& mix, int depth) {
    for (const auto& e : mix.entries()) {
        if (e.size < 12) continue;
        std::vector<uint8_t> d;
        if (!mix.read_entry(e, d)) continue;
        if (!MixFile::looks_like_mix(d.data(), d.size())) continue;
        MixFile probe;
        std::string err;
        if (!probe.open(d.data(), d.size(), &err) || !probe.structure_valid()) continue;
        ra2r::assets::enrich_names(probe);
        for (const auto& [id, n] : probe.names()) g_gnames.emplace(id, n);
        if (depth + 1 <= g_max_depth) collect_names_recursive(probe, depth + 1);
    }
}

int check_path(const fs::path& p, Stats& st, bool collect_only) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        // 两遍：先收集全树名字库，再校验（嵌套 mix 内条目靠全局名解析扩展名）
        for (int pass = 0; pass < 2; ++pass) {
            for (auto& de : fs::directory_iterator(p, ec)) {
                if (!de.is_regular_file()) continue;
                const std::string fn = de.path().filename().string();
                const std::string u = upper(fn);
                if (u.size() >= 4 && u.compare(u.size() - 4, 4, ".MIX") == 0) {
                    check_path(de.path(), st, pass == 0);
                }
            }
        }
        return 0;
    }
    MixFile mix;
    std::string err;
    if (!mix.open(p, &err)) {
        if (!collect_only) {
            std::fprintf(stderr, "FAIL open %s: %s\n", p.string().c_str(), err.c_str());
            ++st.fail;
        }
        return 1;
    }
    if (!mix.structure_valid()) {
        if (!collect_only) {
            std::fprintf(stderr, "FAIL structure %s\n", p.string().c_str());
            ++st.fail;
        }
        return 1;
    }
    ra2r::assets::enrich_names(mix);
    for (const auto& [id, n] : mix.names()) g_gnames.emplace(id, n);
    if (collect_only) {
        collect_names_recursive(mix, 0);
        return 0;
    }
    std::printf("[assetcheck] %s: %u entries, %zu names\n", p.string().c_str(),
                mix.file_count(), mix.names().size());
    check_mix(mix, p.filename().string(), 0, st);
    return 0;
}

int run(int argc, char** argv) {
    std::vector<const char*> paths;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) g_quick = true;
        else if (std::strcmp(argv[i], "--verbose") == 0) g_verbose = true;
        else if (std::strcmp(argv[i], "--max-depth") == 0 && i + 1 < argc)
            g_max_depth = std::atoi(argv[++i]);
        else paths.push_back(argv[i]);
    }
    if (paths.empty()) {
        std::printf("usage: assetcheck <mix|dir>... [--quick] [--verbose] [--max-depth N]\n");
        return 1;
    }
    // XCC 官方名库（可选）：大幅提升条目名覆盖率
    {
        MixFile tmp;
        std::string from;
        if (ra2r::assets::try_load_xcc_database(tmp, &from)) {
            for (const auto& [id, n] : tmp.names()) g_gnames.emplace(id, n);
            std::printf("[assetcheck] XCC 名库: %s -> %zu 名\n", from.c_str(),
                        tmp.names().size());
        } else {
            std::printf("[assetcheck] 未找到 XCC 名库（tools/fetch_references.ps1 可获取）\n");
        }
    }
    Stats st;
    // 目录模式在 check_path 内部做两遍（collect→check）；单 MIX 路径这里补 collect 遍
    for (const char* p : paths) {
        std::error_code ec;
        if (!fs::is_directory(fs::path(p), ec)) check_path(fs::path(p), st, true);
    }
    for (const char* p : paths) check_path(fs::path(p), st, false);
    std::printf("────────────────────────────────────────\n");
    std::printf("总计: 条目 %llu | 通过 %llu | 失败 %llu | 跳过 %llu | 嵌套MIX %llu\n",
                static_cast<unsigned long long>(st.files),
                static_cast<unsigned long long>(st.ok),
                static_cast<unsigned long long>(st.fail),
                static_cast<unsigned long long>(st.skip),
                static_cast<unsigned long long>(st.mixes));
    return st.fail == 0 ? 0 : 1;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) { return ra2r::core::run_wide(run, argc, wargv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
