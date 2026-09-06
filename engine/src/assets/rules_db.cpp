// RA2R — 规则数据库实现（接口见 rules_db.h）
#include "ra2r/assets/rules_db.h"

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
                // 配件动画（artmd；键缺失时回退 Image= 美术节——如 ATESLA 的
                // Image=GAPRIS，棱镜/摇臂等键在 [GAPRIS] 节）
                const auto aget = [&](const char* key) {
                    const std::string v = art_.get(name, key, "");
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

} // namespace ra2r::assets
