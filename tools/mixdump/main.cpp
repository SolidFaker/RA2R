// RA2R mixdump — M0 工具：列出 / 提取 / 递归查找 RA2(YR) MIX 内容。
// 用法：
//   mixdump [--names <names.txt>] <mix> list
//   mixdump [--names <names.txt>] <mix> extract <hex-id|名称> <输出文件>
//   mixdump [--names <names.txt>] <mix> extract-all <输出目录>
//   mixdump [--names <names.txt>] <mix> find <名称> <输出文件>   (自动钻入嵌套 MIX)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ra2r/assets/mix_file.h"
#include "ra2r/core/win_unicode.h"

using ra2r::assets::MixFile;
using ra2r::assets::MixEntry;

namespace fs = std::filesystem;

static void print_usage() {
    std::printf("usage:\n"
                "  mixdump [--names <file>] <mix> list\n"
                "  mixdump [--names <file>] <mix> extract <hex-id|name> <out-file>\n"
                "  mixdump [--names <file>] <mix> extract-all <out-dir>\n"
                "  mixdump [--names <file>] <mix> find <name> <out-file>\n"
                "  mixdump [--names <file>] <mix> scan [--from-ini <ini>]... [names...]\n");
}

// 名称 → id 解析（可选外部名称库，行分隔或 NUL 分隔）
static std::vector<std::string> g_extra_names;

static void load_extra_names(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string token;
    auto flush = [&]() {
        while (!token.empty() && (token.back() == '\r' || token.back() == ' ' || token.back() == '\t')) {
            token.pop_back();
        }
        if (token.size() >= 3) g_extra_names.push_back(token);
        token.clear();
    };
    for (char c : data) {
        if (c == '\n' || c == '\r' || c == '\0') flush();
        else token.push_back(c);
    }
    flush();
}

static std::string trim(std::string s) {
    const auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    return s;
}

// 从 rules/art INI 生成候选资产名：节名 + 资源引用键的值 × 常见扩展名
static std::set<std::string> build_candidates_from_ini(const char* path) {
    static const std::set<std::string> kListHeaders = {
        "GENERAL","AUDIOVISUAL","COMBATDAMAGE","RADIATION","IQ","SPECIALWEAPONS","AI",
        "POWERUPS","LANDTYPES","INFANTRYTYPES","VEHICLETYPES","AIRCRAFTTYPES","BUILDINGTYPES",
        "ANIMATIONS","VOXELANIMS","TERRAINTYPES","SMUDGETYPES","OVERLAYTYPES","PARTICLESYSTEMS",
        "PARTICLES","JUMPJETCONTROLS","SUPERWEAPONTYPES","WARHEADS","WEAPONS","ARMORTYPES","HOUSES",
        "COUNTRIES","SIDES","SPEECH","MPLAYER","FILM","SIDEBAR","DIALOG","SONGLIST","SOUNDLIST",
        "EVA","TUTORIAL","CONTROLS","VETERANLEVELS","MULTIPLAYERDIALOG","CREDITS","TIBSYSTEMS",
        "BUILDINGANIMS","BUILDINGTIMES","AARH","RANKING","PROJECTILES","PARASITETYPES",
        "GAMEPLAY","DIFFICULTY","THEME","ANIMATIONLENGTH","ANIMATIONSPEED","ARMCLEAN",
    };
    std::set<std::string> bases;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t e = line.find(']');
            if (e != std::string::npos) {
                std::string sec = trim(line.substr(1, e - 1));
                if (kListHeaders.find(sec) == kListHeaders.end()) bases.insert(sec);
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        const size_t semi = val.find(';');
        if (semi != std::string::npos) val = trim(val.substr(0, semi));
        static const std::set<std::string> kAssetKeys = {
            "IMAGE","CAMEO","VOXEL","ALPHAIMAGE","CURSOR","TURRET","BARREL","IDLEANIM",
            "POWERUP1ANIM","POWERUP2ANIM","POWERUP3ANIM","FIREANIM","DOORANIM","FLYINGFRAMES",
        };
        if (kAssetKeys.count(key) && val.size() >= 2 && val.size() <= 16 &&
            val.find(' ') == std::string::npos && val.find(',') == std::string::npos &&
            val.find('<') == std::string::npos) {
            bases.insert(val);
        }
    }
    return bases;
}

static const MixEntry* resolve(MixFile& mix, const std::string& spec) {
    if (spec.size() == 8) {
        bool hex = true;
        uint32_t id = 0;
        for (char c : spec) {
            uint32_t v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else { hex = false; break; }
            id = (id << 4) | v;
        }
        if (hex) return mix.find(id);
    }
    return mix.find_by_name(spec);
}

static void hex_dump_head(const std::vector<uint8_t>& data, size_t n) {
    n = n < data.size() ? n : data.size();
    for (size_t i = 0; i < n; ++i) {
        std::printf("%02x ", data[i]);
        if (i % 16 == 15) std::printf("\n");
    }
    if (n % 16) std::printf("\n");
}

// 递归查找：先在本层按名命中（若是嵌套 MIX 则下钻），未命中则 DFS 遍历全部嵌套 MIX。
static bool find_recursive(MixFile& mix, const std::string& name, int depth,
                           std::vector<uint8_t>& out, uint64_t* scanned) {
    if (depth > 10) return false;
    const MixEntry* e = mix.find_by_name(name);
    if (e) {
        std::vector<uint8_t> data;
        if (!mix.read_entry(*e, data)) {
            std::printf("  [depth %d] 读取失败 id=%08X\n", depth, e->id);
            return false;
        }
        const std::string* nm = mix.name_of(e->id);
        std::printf("  [depth %d] 命中 id=%08X %s size=%u\n", depth, e->id,
                    nm ? nm->c_str() : "", e->size);
        if (MixFile::looks_like_mix(data.data(), data.size())) {
            std::printf("    -> 疑似嵌套 MIX，尝试下钻...\n");
            MixFile sub;
            std::string error;
            if (sub.open(data.data(), data.size(), &error) && sub.structure_valid()) {
                return find_recursive(sub, name, depth + 1, out, scanned);
            }
            std::printf("    -> 不是有效 MIX（%s），按目标文件提取\n", error.c_str());
        }
        out = std::move(data);
        return true;
    }
    // 本层未命中：遍历所有嵌套 MIX
    for (const auto& entry : mix.entries()) {
        if (*scanned >= 5000000) {
            std::printf("  [depth %d] 扫描条目超限，中止\n", depth);
            return false;
        }
        ++*scanned;
        std::vector<uint8_t> data;
        if (!mix.read_entry(entry, data)) continue;
        if (!MixFile::looks_like_mix(data.data(), data.size())) continue;
        MixFile sub;
        std::string error;
        if (!sub.open(data.data(), data.size(), &error) || !sub.structure_valid()) continue;
        const std::string* nm = mix.name_of(entry.id);
        std::printf("  [depth %d] 进入嵌套 MIX id=%08X %s (%u 条目)\n", depth, entry.id,
                    nm ? nm->c_str() : "", sub.file_count());
        if (find_recursive(sub, name, depth + 1, out, scanned)) return true;
    }
    return false;
}

// DFS 全树扫描：对每个条目检查候选 id，并钻入所有嵌套 MIX。
static void scan_recursive(MixFile& mix, const std::vector<uint32_t>& crcs,
                           const std::vector<std::string>& names, int depth,
                           std::vector<std::pair<uint32_t, std::string>>& hits,
                           uint64_t* scanned) {
    if (depth > 12) return;
    std::fprintf(stderr, "[depth %d] enter mix: %u entries\n", depth, mix.file_count());
    int skip_head = 0, skip_look = 0, skip_read = 0, skip_open = 0;
    for (const auto& entry : mix.entries()) {
        if (*scanned >= 2000000) return;
        ++*scanned;
        if (*scanned % 50000 == 0) {
            std::fprintf(stderr, "... scanned %llu entries\n",
                         static_cast<unsigned long long>(*scanned));
        }
        const auto it = std::lower_bound(crcs.begin(), crcs.end(), entry.id);
        if (it != crcs.end() && *it == entry.id) {
            const size_t idx = static_cast<size_t>(it - crcs.begin());
            hits.emplace_back(entry.id, names[idx]);
            std::printf("HIT %08X %-24s depth=%d size=%u\n", entry.id, names[idx].c_str(),
                        depth, entry.size);
        }
        uint8_t head[96];
        if (entry.size < sizeof(head)) { ++skip_head; continue; }
        if (!mix.read_head(entry, head, sizeof(head))) { ++skip_read; continue; }
        if (!MixFile::looks_like_mix(head, sizeof(head))) { ++skip_look; continue; }
        std::vector<uint8_t> data;
        if (!mix.read_entry(entry, data)) { ++skip_read; continue; }
        MixFile sub;
        std::string error;
        if (!sub.open(data.data(), data.size(), &error) || !sub.structure_valid()) {
            ++skip_open;
            continue;
        }
        scan_recursive(sub, crcs, names, depth + 1, hits, scanned);
    }
    if (skip_head || skip_look || skip_read || skip_open) {
        std::fprintf(stderr, "[depth %d] skips: small=%d look=%d read=%d open=%d\n",
                     depth, skip_head, skip_look, skip_read, skip_open);
    }
}

static int run(int argc, char** argv) {
    int a = 1;
    while (a < argc && std::strcmp(argv[a], "--names") == 0 && a + 1 < argc) {
        load_extra_names(argv[a + 1]);
        a += 2;
    }
    if (argc - a < 2) {
        print_usage();
        return 1;
    }
    const char* mix_path = argv[a];
    const char* cmd = argv[a + 1];

    MixFile mix;
    std::string error;
    if (!mix.open(mix_path, &error)) {
        std::fprintf(stderr, "mix open failed: %s\n", error.c_str());
        return 1;
    }

    std::printf("mix: %s\n", mix_path);
    std::printf("flags=0x%08x (encrypted=%d checksum=%d) count=%u data_size=%u body_start=%u\n",
                mix.flags(), mix.is_encrypted(), mix.has_checksum(),
                mix.file_count(), mix.data_size(), mix.body_start());

    if (std::strcmp(cmd, "list") == 0) {
        size_t shown = 0;
        const size_t cap = 400;
        for (const auto& e : mix.entries()) {
            if (shown++ >= cap) break;
            const std::string* name = mix.name_of(e.id);
            const char* tag = "";
            for (const auto& n : g_extra_names) {
                if (MixFile::crc32_of_name(n) == e.id) { tag = n.c_str(); break; }
            }
            std::printf("%08X  off=%10u  size=%10u  %s%s\n", e.id, e.offset, e.size,
                        name ? name->c_str() : "", tag);
        }
        if (mix.entries().size() > cap) {
            std::printf("... (%u more entries)\n",
                        static_cast<unsigned>(mix.entries().size() - cap));
        }
        std::printf("names resolved: %u / %u\n",
                    static_cast<unsigned>(mix.names().size()),
                    static_cast<unsigned>(mix.entries().size()));
    } else if (std::strcmp(cmd, "extract") == 0 && argc - a >= 4) {
        const MixEntry* e = resolve(mix, argv[a + 2]);
        if (!e) {
            std::fprintf(stderr, "entry not found: %s\n", argv[a + 2]);
            return 1;
        }
        std::vector<uint8_t> data;
        if (!mix.read_entry(*e, data)) {
            std::fprintf(stderr, "read failed for %08X\n", e->id);
            return 1;
        }
        std::ofstream out(argv[a + 3], std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        std::printf("extracted %08X (%u bytes) -> %s\n", e->id, e->size, argv[a + 3]);
        std::printf("--- head bytes ---\n");
        hex_dump_head(data, 64);
    } else if (std::strcmp(cmd, "find") == 0 && argc - a >= 4) {
        std::vector<uint8_t> data;
        uint64_t scanned = 0;
        if (!find_recursive(mix, argv[a + 2], 0, data, &scanned)) {
            std::fprintf(stderr, "find failed: %s\n", argv[a + 2]);
            return 1;
        }
        std::ofstream out(argv[a + 3], std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        std::printf("found %s (%u bytes) -> %s\n", argv[a + 2],
                    static_cast<unsigned>(data.size()), argv[a + 3]);
        std::printf("--- head bytes ---\n");
        hex_dump_head(data, 64);
    } else if (std::strcmp(cmd, "scan") == 0) {
        // 组装候选名：--from-ini + 显式名称 + --names 库
        std::set<std::string> bases;
        for (int i = a + 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--from-ini") == 0 && i + 1 < argc) {
                const auto got = build_candidates_from_ini(argv[i + 1]);
                bases.insert(got.begin(), got.end());
                std::printf("from-ini %s: %u bases\n", argv[i + 1],
                            static_cast<unsigned>(got.size()));
                ++i;
            } else if (argv[i][0] != '-') {
                bases.insert(argv[i]);
            }
        }
        bases.insert(g_extra_names.begin(), g_extra_names.end());
        static const char* kSuffixes[] = {".SHP", ".VXL", ".PAL", ".TMP", ".HVA", ".WAV", ".AUD"};
        std::vector<std::pair<uint32_t, std::string>> cand;
        for (const auto& b : bases) {
            for (const char* s : kSuffixes) {
                cand.emplace_back(MixFile::crc32_of_name(b + s), b + s);
            }
        }
        std::sort(cand.begin(), cand.end());
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
        std::vector<uint32_t> crcs;
        std::vector<std::string> names;
        crcs.reserve(cand.size());
        names.reserve(cand.size());
        for (const auto& p : cand) {
            crcs.push_back(p.first);
            names.push_back(p.second);
        }
        std::printf("scan candidates: %zu\n", cand.size());
        std::vector<std::pair<uint32_t, std::string>> hits;
        uint64_t scanned = 0;
        scan_recursive(mix, crcs, names, 0, hits, &scanned);
        std::printf("scanned %llu entries, %zu hits\n",
                    static_cast<unsigned long long>(scanned), hits.size());
    } else if (std::strcmp(cmd, "scanmagic") == 0 && argc - a >= 3) {
        // 按内容魔数扫描全树：参数为十六进制字节串（如 "566f78656c20416e696d6174696f6e"）
        std::string hex = argv[a + 2];
        std::vector<uint8_t> magic;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
            if (hi < 0 || lo < 0) { std::fprintf(stderr, "bad hex: %s\n", hex.c_str()); return 1; }
            magic.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        std::printf("scanmagic: %s (%zu bytes)\n", hex.c_str(), magic.size());
        uint64_t scanned = 0;
        std::vector<std::tuple<uint32_t, uint32_t, int, int>> hits; // id,size,depth,parent
        auto walk = [&](auto&& self, MixFile& cur, int depth) -> void {
            for (const auto& entry : cur.entries()) {
                if (++scanned > 2000000) return;
                uint8_t head[16] = {};
                const size_t n = entry.size < 16 ? entry.size : 16;
                if (cur.read_head(entry, head, n) &&
                    magic.size() <= n &&
                    std::memcmp(head, magic.data(), magic.size()) == 0) {
                    hits.emplace_back(entry.id, entry.size, depth, cur.file_count());
                    std::printf("MAGICHIT %08X size=%u depth=%d\n", entry.id, entry.size, depth);
                }
                uint8_t mh[96];
                if (entry.size < sizeof(mh)) continue;
                if (!cur.read_head(entry, mh, sizeof(mh))) continue;
                if (!MixFile::looks_like_mix(mh, sizeof(mh))) continue;
                std::vector<uint8_t> data;
                if (!cur.read_entry(entry, data)) continue;
                MixFile sub;
                std::string sub_error;
                if (!sub.open(data.data(), data.size(), &sub_error) || !sub.structure_valid())
                    continue;
                self(self, sub, depth + 1);
            }
        };
        walk(walk, mix, 0);
        std::printf("scanmagic done: %llu entries, %zu hits\n",
                    static_cast<unsigned long long>(scanned), hits.size());
    } else if (std::strcmp(cmd, "crc") == 0 && argc - a >= 3) {
        for (int i = a + 2; i < argc; ++i) {
            std::printf("%s -> %08X\n", argv[i], MixFile::crc32_of_name(argv[i]));
        }
    } else if (std::strcmp(cmd, "extract-all") == 0 && argc - a >= 3) {
        fs::create_directories(argv[a + 2]);
        size_t ok = 0;
        for (const auto& e : mix.entries()) {
            std::vector<uint8_t> data;
            if (!mix.read_entry(e, data)) continue;
            char fname[64];
            const std::string* name = mix.name_of(e.id);
            if (name && name->size() < 40) {
                std::snprintf(fname, sizeof(fname), "%s", name->c_str());
            } else {
                std::snprintf(fname, sizeof(fname), "%08X.bin", e.id);
            }
            std::ofstream out(fs::path(argv[a + 2]) / fname, std::ios::binary);
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
            ++ok;
        }
        std::printf("extracted %zu / %u entries -> %s\n", ok, mix.file_count(), argv[a + 2]);
    } else {
        print_usage();
        return 1;
    }
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) { return ra2r::core::run_wide(run, argc, wargv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
