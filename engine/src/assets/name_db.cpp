// RA2R — INI 名称库实现
#include "ra2r/assets/name_db.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

#include "ra2r/core/endian.h"
#include "ra2r/core/executable_path.h"
#include "ra2r/core/ini_file.h"

namespace ra2r::assets {

namespace {
const char* kSuffixes[] = {".SHP", ".VXL", ".PAL", ".TMP", ".HVA", ".WAV", ".AUD",
                           ".INI", ".PCX", ".CPS", ".CSF", ".MAP"};

// 与工具侧一致的资源引用键（Image/Cameo/Voxel 等）
bool is_asset_key(const std::string& k) {
    static const std::set<std::string> keys = {
        "IMAGE",      "CAMEO",      "ALTCAMEO",    "VOXEL",    "ALPHAIMAGE", "CURSOR",
        "TURRET",     "BARREL",     "IDLEANIM",    "FIREANIM", "DOORANIM",   "FLYINGFRAMES",
        "POWERUP1ANIM", "POWERUP2ANIM", "POWERUP3ANIM",
    };
    return keys.count(k) > 0;
}

bool is_list_header(const std::string& s) {
    static const std::set<std::string> headers = {
        "GENERAL","AUDIOVISUAL","COMBATDAMAGE","RADIATION","IQ","SPECIALWEAPONS","AI",
        "POWERUPS","LANDTYPES","INFANTRYTYPES","VEHICLETYPES","AIRCRAFTTYPES","BUILDINGTYPES",
        "ANIMATIONS","VOXELANIMS","TERRAINTYPES","SMUDGETYPES","OVERLAYTYPES","PARTICLESYSTEMS",
        "PARTICLES","JUMPJETCONTROLS","SUPERWEAPONTYPES","WARHEADS","WEAPONS","ARMORTYPES","HOUSES",
        "COUNTRIES","SIDES","SPEECH","MPLAYER","FILM","SIDEBAR","DIALOG","SONGLIST","SOUNDLIST",
        "EVA","TUTORIAL","CONTROLS","VETERANLEVELS","MULTIPLAYERDIALOG","CREDITS","TIBSYSTEMS",
        "BUILDINGANIMS","BUILDINGTIMES","AARH","RANKING","PROJECTILES","PARASITETYPES",
        "GAMEPLAY","DIFFICULTY","THEME","ANIMATIONLENGTH","ANIMATIONSPEED","ARMCLEAN","SWATCH",
    };
    return headers.count(s) > 0;
}
} // namespace

std::vector<std::string> collect_names_from_ini(const uint8_t* data, size_t size) {
    core::IniFile ini;
    ini.parse(data, size);
    std::set<std::string> bases;
    for (const auto& sec : ini.section_names()) {
        std::string u(sec);
        std::transform(u.begin(), u.end(), u.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (!is_list_header(u)) bases.insert(sec);
    }
    for (const auto& sn : ini.section_names()) {
        for (const auto& [k, v] : ini.section(sn)) {
            std::string uk(k);
            std::transform(uk.begin(), uk.end(), uk.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (!is_asset_key(uk)) continue;
            if (v.size() < 2 || v.size() > 16 || v.find(' ') != std::string::npos ||
                v.find(',') != std::string::npos || v.find('<') != std::string::npos) {
                continue;
            }
            bases.insert(v);
        }
    }
    return std::vector<std::string>(bases.begin(), bases.end());
}

void enrich_names(MixFile& mix) {
    // 1) 实测验证过的顶层/常见 MIX 名（游戏自身无名字库，此为逆向实测数据）
    static const char* kWellKnown[] = {
        "CACHE.MIX",     "CONQUER.MIX",  "ISOURB.MIX",  "LOCAL.MIX",    "SIDEC01.MIX",
        "SIDEC02.MIX",   "SIDENC01.MIX", "SIDENC02.MIX", "CACHEMD.MIX", "LOCALMD.MIX",
        "ISODES.MIX",    "ISOLUN.MIX",   "EXPANDMD01.MIX", "THEMEMD.MIX", "MULTIMD.MIX",
        "MAPSMD03.MIX",  "LANGMD.MIX",   "LANGUAGE.MIX", "MOVMD03.MIX", "RA2.MIX",
        "RA2MD.MIX",
    };
    for (const char* n : kWellKnown) mix.add_name(n);
    // 2) 规则 INI 推导（节名 + Image=/Cameo=/Voxel= 等 × 常见扩展名）
    //    地形 INI 节名即瓦片名（→ .TMP）；任务 INI 节名即战役地图名（→ .MAP）
    static const char* kRuleInis[] = {"RULES.INI",     "RULESMD.INI", "ART.INI",
                                      "ARTMD.INI",     "AI.INI",       "AIMD.INI",
                                      "BATTLE.INI",    "BATTLEMD.INI", "TEMPERAT.INI",
                                      "TEMPERATMD.INI", "SNOW.INI",    "SNOWMD.INI",
                                      "URBAN.INI",     "URBANMD.INI",  "URBANN.INI",
                                      "URBANNMD.INI",  "LUNAR.INI",    "LUNARMD.INI",
                                      "DESERT.INI",    "DESERTMD.INI", "MISSION.INI",
                                      "MISSIONMD.INI", "MISSIONS.INI", "MISSIONSMD.INI"};
    for (const char* iname : kRuleInis) {
        const MixEntry* e = mix.find_by_name(iname);
        if (!e) continue;
        mix.add_name(iname); // 规则 INI 自身文件名也入名库（此前遗漏，顶层条目无名）
        std::vector<uint8_t> data;
        if (!mix.read_entry(*e, data)) continue;
        const auto bases = collect_names_from_ini(data.data(), data.size());
        for (const auto& b : bases) {
            for (const char* suf : kSuffixes) {
                mix.add_name(b + suf);
            }
        }
    }
}

size_t load_global_names_from_xcc(MixFile& mix, const uint8_t* data, size_t size) {
    size_t added = 0;
    const uint8_t* s = data;
    const uint8_t* end = data + size;
    // 顺序：TD、RA、TS、RA2 四段；只把 RA2 段（第 4 段）并入
    for (int game = 0; game < 4 && s + 4 <= end; ++game) {
        const int32_t count = static_cast<int32_t>(core::read_u32_le(s));
        s += 4;
        for (int32_t i = 0; i < count; ++i) {
            const uint8_t* e = s;
            while (e < end && *e) ++e;
            if (e >= end) return added; // 截断，安全退出
            const std::string name(reinterpret_cast<const char*>(s), e - s);
            s = e + 1;
            e = s;
            while (e < end && *e) ++e;
            if (e >= end) return added;
            s = e + 1; // description 丢弃
            if (game == 3 && !name.empty()) {
                mix.add_name(name);
                ++added;
            }
        }
    }
    return added;
}

bool try_load_xcc_database(MixFile& mix, std::string* loaded_from) {
    std::vector<std::filesystem::path> candidates;
    // exe 目录及向上几级（开发时 exe 在 build/tools/ 下，数据在仓库根 data/）
    const std::filesystem::path exe = core::executable_path();
    if (!exe.empty()) {
        const std::filesystem::path dir = exe.parent_path();
        for (const char* rel : {"", "data/", "../data/", "../../data/", "../../../data/",
                                "../third_party/reference/",
                                "../../third_party/reference/",
                                "../../../third_party/reference/"}) {
            candidates.push_back(dir / (rel + std::string("global mix database.dat")));
        }
    }
    candidates.emplace_back("data/global mix database.dat");
    candidates.emplace_back("third_party/reference/global mix database.dat");
    candidates.emplace_back("global mix database.dat");
    for (const auto& c : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(c, ec)) continue;
        std::ifstream f(c, std::ios::binary);
        if (!f) continue;
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (raw.size() < 16) continue;
        const size_t n = load_global_names_from_xcc(
            mix, reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
        if (n > 0) {
            if (loaded_from) *loaded_from = c.string();
            return true;
        }
    }
    return false;
}

} // namespace ra2r::assets
