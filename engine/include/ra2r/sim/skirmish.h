#pragma once
// RA2R — 遭遇战流程（M4）：开局生成 / 基地车展开 / 科技树过滤 / 阵营色
//
// 机制照 OpenRA（mods/yr 的 SpawnMPUnits + MPStartUnits：BaseActor 落在出生点、
// SupportActors 在 InnerSupportRadius..OuterSupportRadius 环形散布），
// 数值全部取自游戏自己的 rulesmd.ini / artmd.ini：
//   基地车   [AMCV]/[SMCV]/[PCV]  DeploysInto=（GACNST/NACNST/YACNST）
//   建造厂   ConstructionYard=yes + artmd Buildup=（展开/建造动画）
//   科技树   Owner= / Prerequisite=（含 [General] Prerequisite<组名>= 组）/
//            TechLevel= / ConstructionYard=
//   阵营色   [Colors] H,S,V（V = 最大亮度，越暗越饱和，见 docs/formats/palettes.md）
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ra2r/assets/rules_db.h"
#include "ra2r/sim/sim_world.h"

namespace ra2r::sim {

// 遭遇战玩家配置
struct SkirmishPlayerCfg {
    std::string house;   // sim 的 owner 键（如 "Player"/"Opponent"）
    std::string country; // rulesmd [Countries] 名（如 "Americans"）
    std::string color;   // [Colors] 名（空 = 用国家节的 Color=）
    int start_class = 2; // OpenRA MPStartUnits.Class：0=none 1=light 2=medium 3=heavy
};

struct SkirmishCfg {
    SkirmishPlayerCfg player;
    SkirmishPlayerCfg opponent;
    int64_t credits = 10000; // 遭遇战初始资金
    int tech_level = 10;     // 科技等级（TechLevel= 大于此值的建筑不可造）
    int seed = 20240906;     // 开局兵力散布的确定性随机种子
};

// 单位工厂：把 rulesmd 类型名变成 SimUnit 的属性（stage 注入）
struct UnitFactory {
    std::function<int(const std::string&)> kind_of;                  // 1=载具 2=步兵
    std::function<void(const std::string&, SimWeapon&)> weapon;      // Primary= → 伤害/射速/射程
    std::function<void(const std::string&, bool&, int&)> miner;      // Harvester=/Capacity=
    std::function<void(const std::string&, UnitMotion&)> motion;    // Speed=/ROT=/Accelerates= 等
};

// 开局兵力方案（OpenRA MPStartUnits 一行）
struct StartUnitPlan {
    std::string base_actor;           // 基地车（BaseActor）
    std::vector<std::string> support; // 支援单位（SupportActors，按序）
    int inner_radius = 3;             // InnerSupportRadius（格）
    int outer_radius = 5;             // OuterSupportRadius（格）
};

// 按国家所属阵营（[Countries] Side=）与档位取开局兵力方案；
// 未知国家/档位返回空方案（base_actor 为空）
StartUnitPlan start_unit_plan(const assets::RulesDB& rules, const std::string& country,
                              int start_class);

// 在出生点生成基地车 + 开局兵力；返回生成单位数（0 = 失败）。
// base_fw/base_fh = 基地车展开后建造厂的地基（如 4×4）：出生格按"该格自身可通行
// 且以它为中心能放下完整地基"选择（原地不行则向外找最近可展开格）——避免基地车
// 落在水面/树丛/窄地导致展开失败（原版遭遇战出生点由地图保证，这里做兜底）。
int spawn_start(SimWorld& world, const assets::RulesDB& rules, const std::string& house,
                const std::string& country, int start_class, int col, int row,
                const UnitFactory& f, uint32_t seed, int base_fw = 1, int base_fh = 1);

// 展开基地车 → 建造厂：地基以基地车格为中心（4×4 → 左上 = 车格 −1,−1），
// 中心不可放置时按固定邻序试近旁落点（校验忽略车体自身占格）；成功后移除
// 车体、建造厂进入展开动画（build_total = Buildup 帧数）。返回新建筑 id（0 = 失败）。
uint32_t deploy_mcv(SimWorld& world, size_t unit_idx, const std::string& building_type, int fw,
                    int fh, int cost, int power, int buildup_frames, int max_hp);

// 收起建筑 → 单位（rulesmd UndeploysInto= 语义，如建造厂 → 基地车）：仅对已建成
// 建筑有效；立即解除地基阻挡并在建筑位置（地基中心 → 锚点 → 地基格，取空位）
// 生成单位，朝向沿用建筑朝向；建筑按"出售"路径无爆炸移除。返回新单位 id
// （0 = 失败：建筑不存在/在建/无空位）。unit_type 属性由 UnitFactory 注入。
uint32_t pack_building(SimWorld& world, size_t building_idx, const std::string& unit_type,
                       const UnitFactory& f);

// 建造前置条件判定（返回未满足原因；空 = 可造）
struct BuildCheck {
    bool ok = false;
    std::string reason;
};
BuildCheck check_buildable(const SimWorld& world, const assets::RulesDB& rules,
                           const std::string& owner, const std::string& country, int tech_level,
                           const assets::UnitTypeDef& t);

// 玩家可造建筑列表（保持 rulesmd [BuildingTypes] 顺序）
std::vector<const assets::UnitTypeDef*> buildable_buildings(const SimWorld& world,
                                                            const assets::RulesDB& rules,
                                                            const std::string& owner,
                                                            const std::string& country,
                                                            int tech_level);

// 阵营色 ramp（16 色，替换单位调色盘 16..31；ModEnc [Colors] 语义）
struct HouseRamp {
    uint8_t rgb[16][3] = {};
};
HouseRamp house_color_ramp(const assets::ColorDef& c);

} // namespace ra2r::sim
