#pragma once
// RA2R — 规则数据库：rulesmd/artmd 全量载入内存 + 类型化查询（M4 全键建模地基）
//
// 载入后：INI 原始节全量保留（rules()/art()，任意键可查），同时对渲染与
// 模拟当前需要的字段做类型化建模——单位类型统一抽象（建筑/载具/步兵/
// 飞行器 = UnitTypeDef）、武器、建筑配件（ActiveAnim 系列 / Bib 底座 /
// 炮塔）。后续 M4 的完整键位建模在此模块上扩展。
#include <map>
#include <string>

#include "ra2r/core/ini_file.h"

namespace ra2r::assets {

// 国家（rulesmd [Countries] 列表 + 国家节；遭遇战阵营选择）
struct CountryDef {
    std::string name;      // 节名，如 "Americans"
    std::string ui_name;   // UIName=（CSF 键，如 "Name:Americans"）
    std::string display;   // Name=（英文显示名）
    std::string side;      // Side=（[Sides] 键：GDI/Nod/ThirdSide/Civilian/Mutant）
    std::string color;     // Color=（[Colors] 名）
    std::string prefix;    // Prefix=（语音/图标前缀）
    std::string suffix;    // Suffix=（"Allied"/"Soviet"）
    bool multiplay = false; // Multiplay=yes（可选国家）
};

// 颜色（rulesmd [Colors]：H,S,V 各 0..255；V = 阵营色最大亮度）
struct ColorDef {
    std::string name;
    int h = 0, s = 0, v = 0;
};

// 阵营色重映射色带：16 色，替换单位调色盘索引 16..31（ModEnc Remap 语义）。
// 由 [Colors] 的 H,S,V 生成（见 sim::house_color_ramp）。
struct HouseRamp {
    uint8_t rgb[16][3] = {};
};

// 单位类型统一抽象（建筑/载具/步兵/飞行器共用；name = rulesmd 类型名）
struct UnitTypeDef {
    std::string name;  // rulesmd 类型名（如 "YAGGUN"）
    std::string image; // 美术名（artmd/rulesmd Image=，缺省 = name）
    int kind = 0;      // 渲染 kind：0=建筑 1=载具 2=步兵
    // 通用
    std::string primary;   // Primary= 武器名（载具/步兵/防御建筑）
    std::string secondary; // Secondary= 副武器名（按目标护甲选择；M5.1）
    std::string armor;     // Armor= 护甲名（armor_index 换算；M5.1）
    bool harvester = false;
    int capacity = 20; // 采矿容量（Harvester=yes 时）
    // 运动物理（rulesmd；载具/步兵）
    int speed = 0;            // Speed=（0 = 未给，用 kind 默认）
    int rot = 0;              // ROT=（每逻辑帧 1/256 圈的转向步长；0 = 立即转向）
    int turret_rot = 0;       // TurretROT=（0 = 用 ROT）
    bool accelerates = false; // Accelerates=yes → 按 AccelerationFactor 加速到 Speed
    double accel_factor = 0.0; // AccelerationFactor=（每帧 + max_speed×factor）
    double decel_factor = 0.0; // DeaccelerationFactor=（0 = 不减速、瞬间停止）
    // 建筑
    int fw = 1, fh = 1;          // Foundation（地图格空间）
    // artmd Height=（单位：格）：建筑高度——选中时白色虚线框的高度，以及飞行单位
    //（基洛夫空艇/火箭飞行兵等）飞越该建筑时需要升高的量。0 = 未提供（回退精灵可见高）
    int height_cells = 0;
    int cost = 300;
    int power = 0;               // >0 产电、<0 耗电（rulesmd Power=）
    bool bib = false;            // rulesmd Bib=yes → <image>BB.SHP 底座平台
    bool refinery = false;       // rulesmd Refinery=yes
    bool turret = false;         // rulesmd Turret=yes
    bool turret_voxel = false;   // rulesmd TurretAnimIsVoxel=true（炮塔为体素）
    std::string turret_anim;     // rulesmd TurretAnim=（缺省 <image>TUR）
    int turret_x = 0, turret_y = 0; // rulesmd TurretAnimX/Y（像素近似，M3 档）
    int turret_za = 0;             // TurretAnimZAdjust=（勒普顿，正=上抬）
    // 配件动画（artmd ActiveAnim 系列；键缺失时回退 Image= 的节）
std::string anim;      // ActiveAnim=
std::string anim_dmg;  // ActiveAnimDamaged=（残血 <50% 切换）
std::string anim_two;  // ActiveAnimTwo=
std::string anim_three;// ActiveAnimThree=
std::string anim_two_dmg;   // ActiveAnimTwoDamaged=（战争工厂流水线等）
std::string anim_three_dmg; // ActiveAnimThreeDamaged=
bool anim_ysort = false; // ActiveAnimYSort 非空（原版：相对其它对象的排序偏移）
// IdleAnim=（常驻装置：苏联/尤里建造厂机械臂 NACNST_C、战争工厂摇臂等）
// / ProductionAnim=（生产中的动画：机械臂摆动）；受损变体各自独立段
std::string idle_anim;
std::string idle_anim_dmg;
std::string idle_two;       // IdleAnimTwo=（第二常驻装置，如 YAGRND 双摇臂）
std::string idle_two_dmg;   // IdleAnimTwoDamaged=
std::string prod_anim;
std::string prod_anim_dmg;
    std::string special;     // SpecialAnim=（常驻配件，如光棱塔棱镜 GAPRIS_A）
    std::string special_dmg; // SpecialAnimDamaged=
    // ── 遭遇战 / 科技树（rulesmd；阶段 M4 建造流程）──
    std::vector<std::string> owner;   // Owner=（可选国家名；空 = 任意阵营）
    std::vector<std::string> prereq;  // Prerequisite=（类型名或 POWER/PROC/RADAR 等组名）
    int tech_level = 0;               // TechLevel=（-1 = 不可建造）
    int strength = 256;               // Strength=（建造完成血量）
    std::string build_cat;            // BuildCat=（侧边栏分类：Power/Defense/Combat/…）
    std::string deploys_into;         // DeploysInto=（MCV → 建造厂）
    std::string undeploys_into;       // UndeploysInto=（建造厂 → MCV）
    bool construction_yard = false;   // ConstructionYard=yes
    std::string factory;              // Factory=（BuildingType/UnitType/InfantryType）
    bool weapons_factory = false;     // WeaponsFactory=yes
    bool radar = false;               // Radar=yes
    bool powered = false;             // Powered=yes（断电停摆）
    std::string deploy_sound;         // DeploySound=（展开音效，如 PlaceBuilding）
    // 建造/展开动画（artmd Buildup=；DemandLoadBuildup/FreeBuildup 只影响加载时机）
    std::string buildup;              // Buildup=（如 GACNSTMK）
    bool free_buildup = false;        // FreeBuildup=true
};

struct WeaponDef {
    std::string name;
    int damage = 25;
    int rof = 30;   // 冷却（逻辑帧）
    int range = 1;  // 射程（格，曼哈顿，向上取整；M3 基础档）
    // ── M5.1 战斗语义 ──
    std::string warhead;   // Warhead= 弹头名（[Warheads] 节）
    std::string projectile;// Projectile= 抛射体名（[Projectiles] 节；M5.2）
    int burst = 1;         // Burst= 连发数
    bool can_aa = true;    // 抛射体 AA=（可打空中）
    bool can_ag = true;    // 抛射体 AG=（可打地面）
};

// 弹头（rulesmd [Warheads]）：Verses 为对 11 类护甲的百分比（顺序见 sim::SimArmor）。
// 顺序自证：rulesmd [AP] 注释"让 plate 几乎免疫" → 第 3 项 = plate。
struct WarheadDef {
    std::string name;
    int verses[11] = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100};
    int cell_spread_x100 = 0; // CellSpread= ×100
    int percent_at_max = 100; // PercentAtMax=（%）
    int prone_damage = 100;   // ProneDamage=（%）
    int inf_death = 0;        // InfDeath=
    bool em_effect = false;   // EMEffect=yes（EMP 弹头；M5.6）
    bool mind_control = false;// MindControl=yes（M5.6）
    bool teleport = false;    // Teleport=yes（M5.6）
    bool iron_curtain = false;// IronCurtain=yes（M5.6）
    int rad_level = 0;        // RadLevel=（辐射强度；M5.6）
};

// 护甲名 → 原版 11 类下标（大小写不敏感；未知 = none(0)）
int armor_index(const std::string& name);

class RulesDB {
public:
    // rulesmd/artmd 字节全量解析；失败返回 false 并置 error
    bool load(const uint8_t* rulesmd, size_t rules_n, const uint8_t* artmd, size_t art_n,
              std::string* error);

    // 类型化查询（未收录返回 nullptr）
    const UnitTypeDef* unit(const std::string& name) const;
    const WeaponDef* weapon(const std::string& name) const;
    const WarheadDef* warhead(const std::string& name) const;
    // 国家 / 颜色（遭遇战阵营与阵营色）
    const CountryDef* country(const std::string& name) const;
    const ColorDef* color(const std::string& name) const;
    const std::vector<CountryDef>& countries() const { return countries_; }
    const std::vector<ColorDef>& colors() const { return colors_; }
    // [Sides]：阵营 → 国家名列表（GDI/Nod/ThirdSide/Civilian/Mutant）
    const std::vector<std::string>& side_countries(const std::string& side) const;
    std::vector<std::string> side_names() const;
    // [General] Prerequisite<组名>=（POWER/PROC/RADAR/FACTORY/BARRACKS/TECH）：
    // 前置条件里的组名 → 满足该组的具体建筑类型表（未收录返回空表）
    std::vector<std::string> prereq_group(const std::string& token) const;

    // 全量原始节（M4 全键建模/未类型化键的查询入口）
    const core::IniFile& rules() const { return rules_; }
    const core::IniFile& art() const { return art_; }

    size_t unit_count() const { return units_.size(); }
    size_t weapon_count() const { return weapons_.size(); }

private:
    core::IniFile rules_;
    core::IniFile art_;
    std::map<std::string, UnitTypeDef> units_;
    std::map<std::string, WeaponDef> weapons_;
    std::map<std::string, WarheadDef> warheads_;
    std::vector<CountryDef> countries_;
    std::map<std::string, size_t> country_index_; // 大写名 → countries_ 下标
    std::vector<ColorDef> colors_;
    std::map<std::string, size_t> color_index_;
    std::map<std::string, std::vector<std::string>> sides_; // [Sides] 节
};

} // namespace ra2r::assets
