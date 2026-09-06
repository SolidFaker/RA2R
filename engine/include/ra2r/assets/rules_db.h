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

// 单位类型统一抽象（建筑/载具/步兵/飞行器共用；name = rulesmd 类型名）
struct UnitTypeDef {
    std::string name;  // rulesmd 类型名（如 "YAGGUN"）
    std::string image; // 美术名（artmd/rulesmd Image=，缺省 = name）
    int kind = 0;      // 渲染 kind：0=建筑 1=载具 2=步兵
    // 通用
    std::string primary; // Primary= 武器名（载具/步兵/防御建筑）
    bool harvester = false;
    int capacity = 20; // 采矿容量（Harvester=yes 时）
    // 建筑
    int fw = 1, fh = 1;          // Foundation（地图格空间）
    int cost = 300;
    int power = 0;               // >0 产电、<0 耗电（rulesmd Power=）
    bool bib = false;            // rulesmd Bib=yes → <image>BB.SHP 底座平台
    bool refinery = false;       // rulesmd Refinery=yes
    bool turret = false;         // rulesmd Turret=yes
    bool turret_voxel = false;   // rulesmd TurretAnimIsVoxel=true（炮塔为体素）
    std::string turret_anim;     // rulesmd TurretAnim=（缺省 <image>TUR）
    int turret_x = 0, turret_y = 0; // rulesmd TurretAnimX/Y（像素近似，M3 档）
    // 配件动画（artmd ActiveAnim 系列；键缺失时回退 Image= 的节）
    std::string anim;      // ActiveAnim=
    std::string anim_dmg;  // ActiveAnimDamaged=（血量<50% 切换）
    std::string anim_two;  // ActiveAnimTwo=
    std::string anim_three;// ActiveAnimThree=
    bool anim_ysort = false; // ActiveAnimYSort 非空 → 画在建筑身后
    std::string special;     // SpecialAnim=（常驻配件，如光棱塔棱镜 GAPRIS_A）
    std::string special_dmg; // SpecialAnimDamaged=
};

struct WeaponDef {
    std::string name;
    int damage = 25;
    int rof = 30;   // 冷却（逻辑帧）
    int range = 1;  // 射程（格，曼哈顿，向上取整；M3 基础档）
};

class RulesDB {
public:
    // rulesmd/artmd 字节全量解析；失败返回 false 并置 error
    bool load(const uint8_t* rulesmd, size_t rules_n, const uint8_t* artmd, size_t art_n,
              std::string* error);

    // 类型化查询（未收录返回 nullptr）
    const UnitTypeDef* unit(const std::string& name) const;
    const WeaponDef* weapon(const std::string& name) const;

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
};

} // namespace ra2r::assets
