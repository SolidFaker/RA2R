#pragma once
// RA2R — M3 模拟层：世界状态与 15Hz 逻辑帧（确定性纪律见 docs/PLAN.md §4）
//
// 坐标 = 引擎格空间 (col,row)（与 MapFile.cell 一致）。单位位置 = 当前格 +
// 段内进度 frac（0..255，向段终点格线性插值，渲染侧换算像素）。引擎 4 邻接
// 步的屏幕投影长度不等（行步 33.5px、列步 60px = 地图空间对角步），故
// 行进速率按段型折算，保持恒定屏幕速率。
//
// 模拟层只做整数/定点运算与固定遍历序（确定性）；选中的 UI 状态留在 stage。
// M3 战斗为"基础"档：武器只有 Damage/ROF/Range（曼哈顿格距），完整武器
// 语义（Verses/弹头/抛射体）留待 M5。
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ra2r/assets/map_file.h"

namespace ra2r::sim {

constexpr int kFracMax = 256;

// 单位指令（M3 基础档：移动/攻击/护卫/巡逻/采集）
enum SimOrder : uint8_t {
    kOrderNone = 0,
    kOrderMove = 1,
    kOrderAttackUnit = 2,    // target = units 下标
    kOrderAttackBuilding = 3,// target = buildings 下标
    kOrderGuard = 4,         // target = units 下标（跟随友军）
    kOrderPatrol = 5,        // waypoints 循环
    kOrderHarvest = 6,       // 采矿车自动循环（采矿 → 精炼厂卸货）
};

// 基础武器（stage 从 rulesmd 解析注入；缺省 = 默认手枪档）
struct SimWeapon {
    int damage = 25;
    int rof = 30;   // 冷却（逻辑帧）
    int range = 1;  // 射程（格，曼哈顿距离，向上取整）
};

struct SimUnit {
    uint32_t id = 0;
    std::string owner; // [Houses] 键名（地图 House 名）
    std::string type;  // rulesmd 类型名（如 "MTNK"）
    int kind = 1;      // 1=载具 2=步兵（与渲染 PlacedObject.kind 同约定）
    int col = 0, row = 0;           // 当前格
    int next_col = 0, next_row = 0; // 段终点格（静止时 = 当前格）
    int frac = 0;                   // 段内进度 0..255
    uint8_t dir = 0;                // 朝向 0..7（渲染 ×32）
    int hp = 256;
    bool alive = true;
    SimWeapon weapon;
    uint8_t order = kOrderNone;
    int target = -1;               // 攻击/护卫目标下标（按 order 语义）
    int cooldown = 0;
    int speed = 68; // 每逻辑帧 frac 增量（行步基准 ≈4 格/秒 @15Hz）
    std::vector<std::pair<int, int>> path; // 剩余途经格（不含当前格与段终点）
    std::vector<std::pair<int, int>> waypoints; // 巡逻点（kOrderPatrol 循环）
    size_t wp_idx = 0;
    // 采矿（kOrderHarvest；M3 基础档：等量矿石，精确数值待 M4 规则库）
    bool is_miner = false;
    int cargo = 0;
    int capacity = 20;
    int mine_clock = 0; // 采集节拍（每 5 帧采 1 单位）
};

struct SimBuilding {
    uint32_t id = 0;
    std::string owner;
    std::string type;
    int col = 0, row = 0; // 存储格（顶格，引擎坐标）
    int rx = 0, ry = 0;   // 存储格（地图空间，地基解除阻挡用）
    int fw = 1, fh = 1;   // 地基尺寸（地图空间格）
    std::vector<std::pair<int, int>> footprint_cells; // 地基引擎格（邻接判定/寻路目标用）
    int hp = 256;
    bool alive = true;
    bool is_refinery = false;
    uint8_t dir = 0; // 地图存储朝向（0..255；炮塔 SHP 帧/体素 yaw 用）
    bool rect_footprint = false; // true = 引擎矩形地基（现场建造）；false = 地图空间地基换算
    // 建造（M3 基础档：Cost/2 帧工期、半透明+进度条表现；原版生长动画待 M4）
    bool under_construction = false;
    int build_ticks = 0;   // 已建造帧
    int build_total = 150; // 总建造帧
    int cost = 300;
    int power = 0; // >0 产电、<0 耗电（rulesmd Power=）
    // 配件动画（油井摇臂/工厂门等 ActiveAnim）：逻辑帧时钟，1 帧/逻辑帧
    bool has_anim = false;
    uint32_t anim_clock = 0;
};

// 爆炸（渲染事件：格 + 持续逻辑帧；stage 用 EXPLOMED 帧序列播放）
struct SimExplosion {
    int col = 0, row = 0;
    int total = 30;
    int elapsed = 0;
};

struct SimWorld {
    int w = 0, h = 0;
    int min_d = 0, min_s = 0; // 地图空间 → 引擎格线性映射基准（建筑地基换算）
    std::vector<uint8_t> blocked; // 1 = 不可通过（建筑地基；出界另行判定）
    std::vector<SimBuilding> buildings;
    std::vector<SimUnit> units;
    std::vector<SimExplosion> explosions;
    uint32_t next_id = 1;
    // 资源（M3 基础档：矿石格存量 + 各 House 资金）
    std::vector<int16_t> ore; // 引擎格矿石量（0=无矿）
    std::map<std::string, int64_t> credits; // House → 资金
    std::map<std::string, int> power_net;   // House → 净电力（产 − 耗）

    // 从地图装载：建筑/载具/步兵进入 buildings/units；建筑地基格标记 blocked。
    // footprint 回调给出建筑的地基尺寸（地图格空间 fw×fh；stage 从 rulesmd
    // Foundation= 解析，缺省 1×1）；weapon 回调给出单位类型的武器；
    // miner/refinery 回调给出采矿车（容量）与精炼厂标记（rulesmd
    // Harvester=/Capacity=/Refinery=）；ore_at 回调给出格矿石存量
    // （stage 按 OverlayPack 类型 + rulesmd OverlayTypes 名 TIB/GEM 判定）。
    // 返回是否有模拟内容。
    bool load_map(const assets::MapFile& map,
                  const std::function<void(const std::string&, int&, int&)>& footprint,
                  const std::function<void(const std::string&, SimWeapon&)>& weapon,
                  const std::function<void(const std::string&, bool&, int&)>& miner,
                  const std::function<bool(const std::string&)>& refinery,
                  const std::function<int(const std::string&)>& power_of,
                  const std::function<int16_t(int, int)>& ore_at,
                  const std::function<bool(int, int)>& terrain_block);

    // 推进一逻辑帧；返回是否有单位移动/转向/开火（渲染侧据此刷新）
    bool tick();

    // 指令：单位 idx 移动到格 (tc,tr)（为当前格/无路径时原地停止）。
    // 目标不可达返回 false 并保持原指令。
    bool issue_move(size_t unit_idx, int tc, int tr);
    // 攻击敌方单位/建筑（进入射程后驻停开火）；护卫跟随友军单位
    bool issue_attack_unit(size_t unit_idx, size_t target_idx);
    bool issue_attack_building(size_t unit_idx, size_t building_idx);
    bool issue_guard(size_t unit_idx, size_t target_idx);
    // 追加巡逻点（首个点立即生效；到达末点后循环回第一点）
    void add_waypoint(size_t unit_idx, int tc, int tr);

    // 变更单位所属 House（1v1 演示脚本用）；新 House 无资金条目时按 $10000 补种
    void set_unit_owner(size_t idx, const std::string& owner);

    // 建造：为 House 在顶格 (col,row) 起造建筑（地基 = 引擎空间 fw×fh 矩形，
    // M3 基础档近似，原版菱形地基换算待 M4）。校验：格内无阻挡、资金足够
    // （立即扣款）。成功返回 true，建筑进入 under_construction。
    bool issue_build(const std::string& owner, const std::string& type, int col, int row,
                     int fw, int fh, int cost, int build_total, int power);

    // 内部：段推进（到达落格 + 余量进下一段）；朝目标格寻路（失败驻停）；
    // 目标格被阻挡（建筑自身地基）时改选最近可达邻格（确定性固定邻序）
    bool advance_segment(SimUnit& u);
    bool set_move_target(SimUnit& u, int tc, int tr);
    bool set_move_target_near(SimUnit& u, int tc, int tr);

    // 单位是否在行进中（移动/追赶段）
    bool unit_moving(size_t i) const {
        return i < units.size() && units[i].alive &&
               (units[i].frac != 0 || !units[i].path.empty() ||
                units[i].next_col != units[i].col || units[i].next_row != units[i].row);
    }

    // 视觉状态哈希（渲染节流用）：单位位置/朝向/血量/指令 + 建筑状态/建造
    // 进度 + 爆炸；不含对画面无影响的量（如矿车 cargo）。确定性遍历。
    uint64_t visual_hash() const;
};

} // namespace ra2r::sim
