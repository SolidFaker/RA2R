// RA2R — 规则数据库实现（接口见 rules_db.h）
#include "ra2r/assets/rules_db.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ra2r::assets {

namespace {
// "yes/true" 宽松判定（INI 大小写不敏感）
bool is_yes(const std::string& v) {
    for (char c : v) {
        if (c == ' ' || c == '\t') continue;
        return c == 'y' || c == 'Y' || c == 't' || c == 'T' || c == '1';
    }
    return false;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// 逗号分隔列表（去空白、去空项；保留原大小写）
std::vector<std::string> split_list(const std::string& v) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= v.size()) {
        const size_t c = v.find(',', pos);
        std::string t = v.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        if (!t.empty()) out.push_back(t);
        if (c == std::string::npos) break;
        pos = c + 1;
    }
    return out;
}
} // namespace

bool RulesDB::load(const uint8_t* rulesmd, size_t rules_n, const uint8_t* artmd, size_t art_n,
                   std::string* error) {
    units_.clear();
    weapons_.clear();
    if (!rules_.parse(rulesmd, rules_n, error)) return false;
    if (!art_.parse(artmd, art_n, error)) return false;

    // 单位类型：四张类型表 → 统一 UnitTypeDef（建筑/载具/步兵/飞行器）
    const auto collect = [&](const char* sec, int kind) {
        for (const auto& [k, name] : rules_.section(sec)) {
            (void)k;
            if (name.empty()) continue;
            UnitTypeDef& u = units_[upper(name)]; // 键统一大写（INI 大小写不敏感语义）
            u.name = name;
            u.kind = kind;
            // Image=（artmd 优先，rulesmd 兜底）
            u.image = art_.get(name, "Image", "");
            if (u.image.empty()) u.image = rules_.get(name, "Image", "");
            if (u.image.empty()) u.image = name;
            // 通用
            u.primary = rules_.get(name, "Primary", "");
            u.harvester = is_yes(rules_.get(name, "Harvester", "no"));
            u.capacity = std::atoi(rules_.get(name, "Capacity", "20").c_str());
            if (u.capacity < 1) u.capacity = 1;
            // 遭遇战 / 科技树（三类单位通用；建筑用于侧边栏过滤）
            u.owner = split_list(rules_.get(name, "Owner", ""));
            u.prereq = split_list(rules_.get(name, "Prerequisite", ""));
            u.tech_level = std::atoi(rules_.get(name, "TechLevel", "0").c_str());
            u.strength = std::atoi(rules_.get(name, "Strength", "256").c_str());
            if (u.strength < 1) u.strength = 1;
            u.build_cat = rules_.get(name, "BuildCat", "");
            u.deploys_into = rules_.get(name, "DeploysInto", "");
            u.undeploys_into = rules_.get(name, "UndeploysInto", "");
            u.construction_yard = is_yes(rules_.get(name, "ConstructionYard", "no"));
            u.factory = rules_.get(name, "Factory", "");
            u.weapons_factory = is_yes(rules_.get(name, "WeaponsFactory", "no"));
            u.radar = is_yes(rules_.get(name, "Radar", "no"));
            u.powered = is_yes(rules_.get(name, "Powered", "no"));
            u.deploy_sound = rules_.get(name, "DeploySound", "");
            u.buildup = art_.get(name, "Buildup", "");
            if (u.buildup.empty() && u.image != name) u.buildup = art_.get(u.image, "Buildup", "");
            u.free_buildup = is_yes(art_.get(name, "FreeBuildup", "no"));
            if (kind == 0) {
                // 建筑字段；地基优先 rulesmd（游戏地基），artmd 兜底（美术地基更大）
                const std::string found =
                    !rules_.get(name, "Foundation", "").empty()
                        ? rules_.get(name, "Foundation", "")
                        : art_.get(name, "Foundation", "");
                std::sscanf(found.c_str(), "%dx%d", &u.fw, &u.fh);
                if (u.fw < 1) u.fw = 1;
                if (u.fh < 1) u.fh = 1;
                u.cost = std::atoi(rules_.get(name, "Cost", "300").c_str());
                u.power = std::atoi(rules_.get(name, "Power", "0").c_str());
                u.bib = is_yes(rules_.get(name, "Bib", "no"));
                u.refinery = is_yes(rules_.get(name, "Refinery", "no"));
                u.turret = is_yes(rules_.get(name, "Turret", "no"));
                u.turret_voxel = is_yes(rules_.get(name, "TurretAnimIsVoxel", "no"));
                u.turret_anim = rules_.get(name, "TurretAnim", "");
                if (u.turret && u.turret_anim.empty()) u.turret_anim = u.image + "TUR";
                u.turret_x = std::atoi(rules_.get(name, "TurretAnimX", "0").c_str());
                u.turret_y = std::atoi(rules_.get(name, "TurretAnimY", "0").c_str());
                // TurretAnimZAdjust=（勒普顿；正=上抬）。屏幕 y 抬升量 = z·格高/256
                u.turret_za = std::atoi(rules_.get(name, "TurretAnimZAdjust", "0").c_str());
                // 配件动画（artmd；键缺失时回退 Image= 美术节——如 ATESLA 的
                // Image=GAPRIS，棱镜/摇臂等键在 [GAPRIS] 节）
                const auto aget = [&](const char* key) {
                    std::string v = art_.get(name, key, "");
                    if (!v.empty()) return v;
                    if (u.image != name) return art_.get(u.image, key, "");
                    return std::string();
                };
                u.anim = aget("ActiveAnim");
                u.anim_dmg = aget("ActiveAnimDamaged");
                u.anim_two = aget("ActiveAnimTwo");
                u.anim_three = aget("ActiveAnimThree");
                u.anim_ysort = !aget("ActiveAnimYSort").empty();
                u.special = aget("SpecialAnim");
                u.special_dmg = aget("SpecialAnimDamaged");
            }
        }
    };
    collect("BuildingTypes", 0);
    collect("VehicleTypes", 1);
    collect("AircraftTypes", 1);
    collect("InfantryTypes", 2);

    // ── 国家（[Countries] 顺序表 + 各国家节）──
    countries_.clear();
    country_index_.clear();
    for (const auto& [k, cname] : rules_.section("Countries")) {
        (void)k;
        if (cname.empty()) continue;
        CountryDef c;
        c.name = cname;
        c.ui_name = rules_.get(cname, "UIName", "");
        c.display = rules_.get(cname, "Name", cname);
        c.side = rules_.get(cname, "Side", "");
        c.color = rules_.get(cname, "Color", "");
        c.prefix = rules_.get(cname, "Prefix", "");
        c.suffix = rules_.get(cname, "Suffix", "");
        c.multiplay = is_yes(rules_.get(cname, "Multiplay", "no"));
        country_index_[upper(cname)] = countries_.size();
        countries_.push_back(std::move(c));
    }
    // ── 颜色（[Colors]：名=H,S,V）──
    colors_.clear();
    color_index_.clear();
    for (const auto& [cname, hsv] : rules_.section("Colors")) {
        if (cname.empty()) continue;
        int h = 0, s = 0, v = 0;
        if (std::sscanf(hsv.c_str(), "%d,%d,%d", &h, &s, &v) != 3) continue;
        ColorDef c;
        c.name = cname;
        c.h = std::clamp(h, 0, 255);
        c.s = std::clamp(s, 0, 255);
        c.v = std::clamp(v, 0, 255);
        color_index_[upper(cname)] = colors_.size();
        colors_.push_back(std::move(c));
    }
    // ── 阵营（[Sides]：GDI/Nod/ThirdSide/Civilian/Mutant → 国家名列表）──
    sides_.clear();
    for (const auto& [side, list] : rules_.section("Sides"))
        sides_[upper(side)] = split_list(list);
    // 武器：标准 [WeaponTypes] 索引节（mod 可能删除）→ 兜底按单位引用名
    // （Primary=/Weapon1..N= 直接指向武器节）收集，逐节类型化（M3 基础档）
    std::map<std::string, bool> wnames;
    for (const auto& [k, wname] : rules_.section("WeaponTypes")) {
        (void)k;
        if (!wname.empty()) wnames[wname] = true;
    }
    const auto add_ref = [&](const std::string& w) {
        if (!w.empty()) wnames[w] = true;
    };
    for (const auto& [name, u] : units_) {
        (void)name;
        add_ref(u.primary);
        for (const char* key : {"Secondary", "Weapon1", "Weapon2", "Weapon3", "Weapon4",
                                "Weapon5", "Weapon6", "EliteWeapon1", "EliteWeapon2",
                                "EliteWeapon3", "EliteWeapon4", "EliteWeapon5",
                                "EliteWeapon6"}) {
            add_ref(rules_.get(u.name, key, ""));
        }
    }
    for (const auto& [wname, dummy] : wnames) {
        (void)dummy;
        if (!rules_.has_section(wname)) continue;
        WeaponDef& w = weapons_[upper(wname)];
        w.name = wname;
        w.damage = std::atoi(rules_.get(wname, "Damage", "25").c_str());
        w.rof = std::atoi(rules_.get(wname, "ROF", "30").c_str());
        if (w.rof < 1) w.rof = 1;
        const float rng =
            static_cast<float>(std::atof(rules_.get(wname, "Range", "1").c_str()));
        w.range = rng > 0.0f ? static_cast<int>(std::ceil(rng)) : 1;
    }
    return true;
}

const UnitTypeDef* RulesDB::unit(const std::string& name) const {
    const auto it = units_.find(upper(name));
    return it != units_.end() ? &it->second : nullptr;
}

const WeaponDef* RulesDB::weapon(const std::string& name) const {
    const auto it = weapons_.find(upper(name));
    return it != weapons_.end() ? &it->second : nullptr;
}

const CountryDef* RulesDB::country(const std::string& name) const {
    const auto it = country_index_.find(upper(name));
    return it != country_index_.end() ? &countries_[it->second] : nullptr;
}

const ColorDef* RulesDB::color(const std::string& name) const {
    const auto it = color_index_.find(upper(name));
    return it != color_index_.end() ? &colors_[it->second] : nullptr;
}

const std::vector<std::string>& RulesDB::side_countries(const std::string& side) const {
    static const std::vector<std::string> kEmpty;
    const auto it = sides_.find(upper(side));
    return it != sides_.end() ? it->second : kEmpty;
}

std::vector<std::string> RulesDB::side_names() const {
    std::vector<std::string> out;
    out.reserve(sides_.size());
    for (const auto& [k, v] : sides_) {
        (void)v;
        out.push_back(k);
    }
    return out;
}

std::vector<std::string> RulesDB::prereq_group(const std::string& token) const {
    // [General] Prerequisite<组名>=：POWER/PROC/RADAR/FACTORY/BARRACKS/TECH…
    // （PrerequisiteProcAlternate=SMIN 也归入 PROC 组）
    const std::string key = "Prerequisite" + token;
    std::vector<std::string> out = split_list(rules_.get("General", key, ""));
    if (upper(token) == "PROC") {
        for (auto& t : split_list(rules_.get("General", "PrerequisiteProcAlternate", "")))
            out.push_back(std::move(t));
    }
    return out;
}

} // namespace ra2r::assets
