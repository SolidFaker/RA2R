// RA2R — 规则数据库实现（接口见 rules_db.h）
#include "ra2r/assets/rules_db.h"

#include <algorithm>
#include <cctype>
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

// artmd 取键，回退到 Image= 美术节（缺失返回空）
std::string art_get_fallback(const core::IniFile& art, const std::string& section,
                             const std::string& image, const char* key) {
    std::string v = art.get(section, key, "");
    if (!v.empty()) return v;
    if (image != section) return art.get(image, key, "");
    return {};
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
            u.secondary = rules_.get(name, "Secondary", ""); // M5.1：副武器
            u.armor = rules_.get(name, "Armor", "");         // M5.1：护甲名
            // M5.4：老兵/精英能力 + 精英武器
            u.veteran_abilities = vet_ability_mask(rules_.get(name, "VeteranAbilities", ""));
            u.elite_abilities = vet_ability_mask(rules_.get(name, "EliteAbilities", ""));
            u.elite_primary = rules_.get(name, "ElitePrimary", "");
            u.elite_secondary = rules_.get(name, "EliteSecondary", "");
            // M5.5：运输/进驻（IFV 武器槽 1..N；乘客 IFVMode= 选槽 = 模式+1）
            u.gunner = is_yes(rules_.get(name, "Gunner", "no"));
            u.passengers = std::atoi(rules_.get(name, "Passengers", "0").c_str());
            {
                const std::string m = rules_.get(name, "IFVMode", "");
                u.ifv_mode = m.empty() ? -1 : std::atoi(m.c_str());
            }
            u.weapons.clear();
            u.elite_weapons.clear();
            for (int i = 1; i <= 20; ++i) {
                const std::string w = rules_.get(name, "Weapon" + std::to_string(i), "");
                if (!w.empty()) u.weapons.push_back(w);
                const std::string ew =
                    rules_.get(name, "EliteWeapon" + std::to_string(i), "");
                if (!ew.empty()) u.elite_weapons.push_back(ew);
            }
            u.occupy_weapon = rules_.get(name, "OccupyWeapon", "");
            u.elite_occupy_weapon = rules_.get(name, "EliteOccupyWeapon", "");
            u.can_be_occupied = is_yes(rules_.get(name, "CanBeOccupied", "no"));
            u.max_occupants = std::atoi(rules_.get(name, "MaxNumberOccupants", "0").c_str());
            // M5.7/M5.8：飞行/海军
            u.naval = is_yes(rules_.get(name, "Naval", "no"));
            u.underwater = is_yes(rules_.get(name, "Underwater", "no"));
            u.airport_bound = is_yes(rules_.get(name, "AirportBound", "no"));
            u.fighter = is_yes(rules_.get(name, "Fighter", "no"));
            {
                const std::string nt = rules_.get(name, "NavalTargeting", "");
                const std::string lt = rules_.get(name, "LandTargeting", "");
                u.naval_targeting = nt.empty() ? -1 : std::atoi(nt.c_str());
                u.land_targeting = lt.empty() ? -1 : std::atoi(lt.c_str());
            }
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
            // 运动物理（rulesmd）：Speed=/ROT=/TurretROT=/Accelerates=/
            // AccelerationFactor=/DeaccelerationFactor=（语义见 docs/DEBUGGING.md §3.33）
            u.speed = std::atoi(rules_.get(name, "Speed", "0").c_str());
            u.rot = std::atoi(rules_.get(name, "ROT", "0").c_str());
            u.turret_rot = std::atoi(rules_.get(name, "TurretROT", "0").c_str());
            u.accelerates = is_yes(rules_.get(name, "Accelerates", "no"));
            u.accel_factor = std::atof(rules_.get(name, "AccelerationFactor", "0").c_str());
            u.decel_factor = std::atof(rules_.get(name, "DeaccelerationFactor", "0").c_str());
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
                // artmd Height=（单位：格）：建筑高度——选中虚线框高度 / 飞行单位
                // 飞越升高的量；建筑节缺失时回退 Image= 美术节
                u.height_cells =
                    std::atoi(art_get_fallback(art_, name, u.image, "Height").c_str());
                // 配件动画（artmd；键缺失时回退 Image= 美术节——如 ATESLA 的
                // Image=GAPRIS，棱镜/摇臂等键在 [GAPRIS] 节）
                u.anim = art_get_fallback(art_, name, u.image, "ActiveAnim");
                u.anim_dmg = art_get_fallback(art_, name, u.image, "ActiveAnimDamaged");
                u.anim_two = art_get_fallback(art_, name, u.image, "ActiveAnimTwo");
                u.anim_three = art_get_fallback(art_, name, u.image, "ActiveAnimThree");
                u.anim_two_dmg =
                    art_get_fallback(art_, name, u.image, "ActiveAnimTwoDamaged");
                u.anim_three_dmg =
                    art_get_fallback(art_, name, u.image, "ActiveAnimThreeDamaged");
                u.anim_ysort = !art_get_fallback(art_, name, u.image, "ActiveAnimYSort").empty();
                // 空闲/生产配件动画（苏联建造厂机械臂 IdleAnim=NACNST_C、
                // 战争工厂/精炼厂摇臂等；IdleAnimDamaged/ProductionAnim 一并解析）
                u.idle_anim = art_get_fallback(art_, name, u.image, "IdleAnim");
                u.idle_anim_dmg = art_get_fallback(art_, name, u.image, "IdleAnimDamaged");
                u.idle_two = art_get_fallback(art_, name, u.image, "IdleAnimTwo");
                u.idle_two_dmg = art_get_fallback(art_, name, u.image, "IdleAnimTwoDamaged");
                u.prod_anim = art_get_fallback(art_, name, u.image, "ProductionAnim");
                u.prod_anim_dmg = art_get_fallback(art_, name, u.image, "ProductionAnimDamaged");
                u.special = art_get_fallback(art_, name, u.image, "SpecialAnim");
                u.special_dmg = art_get_fallback(art_, name, u.image, "SpecialAnimDamaged");
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
        for (int i = 1; i <= 20; ++i) { // M5.5：IFV 武器槽最多 20（原版 WeaponCount=17）
            add_ref(rules_.get(u.name, "Weapon" + std::to_string(i), ""));
            add_ref(rules_.get(u.name, "EliteWeapon" + std::to_string(i), ""));
        }
        add_ref(rules_.get(u.name, "Secondary", ""));
        add_ref(rules_.get(u.name, "OccupyWeapon", ""));
        add_ref(rules_.get(u.name, "EliteOccupyWeapon", ""));
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
        // M5.1：弹头 / 抛射体 / 连发（AA/AG 在 M5.2 解析抛射体时回填）
        w.warhead = rules_.get(wname, "Warhead", "");
        w.projectile = rules_.get(wname, "Projectile", "");
        w.burst = std::max(1, std::atoi(rules_.get(wname, "Burst", "1").c_str()));
    }
    // ── M5.1 弹头（[Warheads] 列表 + 逐节字段；Verses 为 11 项百分比）──
    warheads_.clear();
    std::map<std::string, bool> whnames;
    for (const auto& [k, wname] : rules_.section("Warheads")) {
        (void)k;
        if (!wname.empty()) whnames[wname] = true;
    }
    for (const auto& [wname, dummy] : whnames) {
        (void)dummy;
        if (!rules_.has_section(wname)) continue;
        WarheadDef& h = warheads_[upper(wname)];
        h.name = wname;
        const std::string verses = rules_.get(wname, "Verses", "");
        if (!verses.empty()) {
            const std::vector<std::string> parts = split_list(verses);
            for (size_t i = 0; i < parts.size() && i < 11; ++i)
                h.verses[i] = std::atoi(parts[i].c_str());
        }
        // CellSpread 是格数小数（.3 → 30）；PercentAtMax/ProneDamage 是
        // 带 % 的百分比（"50%" → 50）——两者格式不同，分别解析。
        const std::string cs = rules_.get(wname, "CellSpread", "");
        h.cell_spread_x100 =
            cs.empty() ? 0 : static_cast<int>(std::lround(std::atof(cs.c_str()) * 100.0));
        // 百分比字段两种写法都要吃：`ProneDamage=50%`（整数百分比）与
        // `PercentAtMax=.5`（小数比例）——含 '%' 按整数读，否则按比例 ×100。
        const auto pct = [&](const char* key, int def) {
            const std::string v = rules_.get(wname, key, "");
            if (v.empty()) return def;
            if (v.find('%') != std::string::npos) return std::atoi(v.c_str());
            return static_cast<int>(std::lround(std::atof(v.c_str()) * 100.0));
        };
        h.percent_at_max = std::max(0, pct("PercentAtMax", 100));
        h.prone_damage = std::max(0, pct("ProneDamage", 100));
        h.inf_death = std::atoi(rules_.get(wname, "InfDeath", "0").c_str());
        h.em_effect = is_yes(rules_.get(wname, "EMEffect", "no"));
        h.mind_control = is_yes(rules_.get(wname, "MindControl", "no"));
        h.teleport = is_yes(rules_.get(wname, "Teleport", "no"));
        h.iron_curtain = is_yes(rules_.get(wname, "IronCurtain", "no"));
        h.rad_level = std::atoi(rules_.get(wname, "RadLevel", "0").c_str());
    }
    // ── M5.2 抛射体（[Projectiles] 列表 + 逐节字段）──
    projectiles_.clear();
    std::map<std::string, bool> prnames;
    for (const auto& [k, pname] : rules_.section("Projectiles")) {
        (void)k;
        if (!pname.empty()) prnames[pname] = true;
    }
    for (const auto& [pname, dummy] : prnames) {
        (void)dummy;
        if (!rules_.has_section(pname)) continue;
        ProjectileDef& p = projectiles_[upper(pname)];
        p.name = pname;
        p.inviso = is_yes(rules_.get(pname, "Inviso", "no"));
        p.image = rules_.get(pname, "Image", "");
        p.subject_cliffs = is_yes(rules_.get(pname, "SubjectToCliffs", "no"));
        p.subject_elevation = is_yes(rules_.get(pname, "SubjectToElevation", "no"));
        p.subject_walls = is_yes(rules_.get(pname, "SubjectToWalls", "no"));
        p.arcing = is_yes(rules_.get(pname, "Arcing", "no"));
        p.rot = std::atoi(rules_.get(pname, "ROT", "0").c_str());
        p.speed = std::atoi(rules_.get(pname, "Speed", "0").c_str());
        p.aa = is_yes(rules_.get(pname, "AA", "no"));
        p.ag = is_yes(rules_.get(pname, "AG", "yes"));
        p.arm_x10 = static_cast<int>(
            std::lround(std::atof(rules_.get(pname, "Arm", "0").c_str()) * 10.0));
        p.shadow = is_yes(rules_.get(pname, "Shadow", "no"));
        p.acceleration = std::atoi(rules_.get(pname, "Acceleration", "0").c_str());
    }
    // 武器 AA/AG 回填（M5.1 预留字段；无抛射体 = 全支持，保持旧行为）
    for (auto& [k, w] : weapons_) {
        (void)k;
        const auto it = projectiles_.find(upper(w.projectile));
        if (it == projectiles_.end()) continue;
        w.can_aa = it->second.aa;
        w.can_ag = it->second.ag;
    }
    // ── M5.4 老兵参数（[General]；小数 ×100 存整数）──
    {
        const auto gx100 = [&](const char* key, int def) {
            const std::string v = rules_.get("General", key, "");
            return v.empty() ? def
                             : static_cast<int>(std::lround(std::atof(v.c_str()) * 100.0));
        };
        veteran_.ratio_x100 = gx100("VeteranRatio", 300);
        veteran_.combat_x100 = gx100("VeteranCombat", 110);
        veteran_.armor_x100 = gx100("VeteranArmor", 150);
        veteran_.speed_x100 = gx100("VeteranSpeed", 120);
        veteran_.rof_x100 = gx100("VeteranROF", 60);
        veteran_.sight_x100 = gx100("VeteranSight", 100);
        veteran_.cap = std::atoi(rules_.get("General", "VeteranCap", "2").c_str());
        if (veteran_.cap < 0) veteran_.cap = 0;
    }
    // M5.6：特殊武器参数（[General]）
    iron_curtain_frames_ = std::atoi(rules_.get("General", "IronCurtainDuration", "750").c_str());
    if (iron_curtain_frames_ < 1) iron_curtain_frames_ = 1;
    rad_max_ = std::atoi(rules_.get("General", "RadLevelMax", "500").c_str());
    rad_delay_ = std::atoi(rules_.get("General", "RadLevelDelay", "90").c_str());
    if (rad_delay_ < 1) rad_delay_ = 1;
    return true;
}

// 能力名列表 → 位掩码（逗号/空格分隔；大小写不敏感）
uint32_t vet_ability_mask(const std::string& list) {
    uint32_t mask = 0;
    std::string cur;
    const auto flush = [&]() {
        if (cur.empty()) return;
        const std::string u = upper(cur);
        if (u == "FASTER") mask |= kVetFaster;
        else if (u == "STRONGER") mask |= kVetStronger;
        else if (u == "FIREPOWER") mask |= kVetFirepower;
        else if (u == "ROF") mask |= kVetRof;
        else if (u == "SIGHT") mask |= kVetSight;
        else if (u == "SELF_HEAL") mask |= kVetSelfHeal;
        cur.clear();
    };
    for (const char ch : list) {
        if (ch == ',' || ch == ' ' || ch == '\t') flush();
        else cur.push_back(ch);
    }
    flush();
    return mask;
}

// 护甲名 → 原版 11 类下标（顺序自证见 rules_db.h；未知 = none(0)）
int armor_index(const std::string& name) {
    static const char* const kNames[11] = {"none",   "flak",    "plate",     "light",
                                           "medium", "heavy",   "wood",      "steel",
                                           "concrete", "special_1", "special_2"};
    for (int i = 0; i < 11; ++i) {
        const char* a = kNames[i];
        const char* b = name.c_str();
        size_t j = 0;
        for (; a[j] && b[j]; ++j)
            if (std::tolower(static_cast<unsigned char>(a[j])) !=
                std::tolower(static_cast<unsigned char>(b[j])))
                break;
        if (!a[j] && !b[j]) return i;
    }
    return 0;
}

const UnitTypeDef* RulesDB::unit(const std::string& name) const {
    const auto it = units_.find(upper(name));
    return it != units_.end() ? &it->second : nullptr;
}

const WeaponDef* RulesDB::weapon(const std::string& name) const {
    const auto it = weapons_.find(upper(name));
    return it != weapons_.end() ? &it->second : nullptr;
}

const WarheadDef* RulesDB::warhead(const std::string& name) const {
    const auto it = warheads_.find(upper(name));
    return it != warheads_.end() ? &it->second : nullptr;
}

const ProjectileDef* RulesDB::projectile(const std::string& name) const {
    const auto it = projectiles_.find(upper(name));
    return it != projectiles_.end() ? &it->second : nullptr;
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
