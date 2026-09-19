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

// 基础武器（stage 从 rulesmd 解析注入）
struct SimWeapon {
    // damage = 0 且 range = 0 = **无武器**（默认即"无"；有武器必须显式注入
    // rulesmd Primary=）。旧默认 25/1 会把"没配武器"的建筑/单位变成 25 伤害的
    // 近战武器（防御建筑误开火），已改为零值。
    int damage = 0;
    int rof = 30;   // 冷却（逻辑帧）
    int range = 0;  // 射程（格，曼哈顿距离，向上取整）
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
    // 上一格（渲染转角平滑用；= 当前格表示该段是路径起点）
    int prev_col = 0, prev_row = 0;
    std::vector<std::pair<int, int>> path; // 剩余途经格（不含当前格与段终点）
    std::vector<std::pair<int, int>> waypoints; // 巡逻点（kOrderPatrol 循环）
    size_t wp_idx = 0;
    // 采矿（kOrderHarvest；M3 基础档：等量矿石，精确数值待 M4 规则库）
    bool is_miner = false;
    int cargo = 0;
    int capacity = 20;
    int mine_clock = 0; // 采集节拍（每 5 帧采 1 单位）
    // 步兵 idle 动作（原版 IdleActionFrequency：静止时按 0.5~2× 均值随机间隔
    // 播放 Idle1/Idle2；纯渲染表现，sim 只发布"动作已触发"事件，动画长度由
    // 渲染层按序列帧数截断）。确定性：每单位独立 LCG 流。
    uint8_t idle_kind = 0;   // 0=无 1=Idle1 2=Idle2
    uint32_t idle_start = 0; // 触发时逻辑帧（渲染相位基准）
    int idle_wait = -1;      // 距下次 idle 动作等待逻辑帧（-1 = 静止后重掷）
    uint32_t idle_rng = 0;   // 确定性伪随机源（spawn 时按 id 播种）
};

// idle 动作忙期（渲染层最长 idle 序列 26 帧 × Walk/Idle 速率 3 ≈ 78 帧）：
// sim 在此期间不重复触发，动画实际长度由渲染层按 artmd Length 截断。
inline constexpr int kIdleAnimBusyTicks = 78;

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
    int max_hp = 256; // 建造完成血量（rulesmd Strength=）
    int power = 0; // >0 产电、<0 耗电（rulesmd Power=）
    // 配件动画（油井摇臂/工厂门等 ActiveAnim）：逻辑帧时钟，1 帧/逻辑帧
    bool has_anim = false;
    uint32_t anim_clock = 0;
    // 修理（M4）：true = 逐帧回血并扣款（修理速率 = Cost·0.02/秒@15fps，
    // rulesmd [General] RepairRate 缺省档；满血自动关）
    bool repairing = false;
    int repair_step_hp = 1; // 每次结算回血量（调用方按 max_hp/工期换算传入）
    bool sold = false;      // 出售移除（死亡结算跳过爆炸）
    // ── 防御建筑攻击（rulesmd Primary= 有武器者；stage 落成时注入）──
    // 手动指定目标（issue_build_attack）+ 无目标时自动索敌（每 15 帧扫最近敌
    // 单位）；炮塔按 turn rate 转向目标后再开火；单位死亡沿用死亡整批结算。
    SimWeapon weapon;
    int target = -1;        // 攻击目标单位下标（-1 = 无）
    int cooldown = 0;       // ROF 冷却（逻辑帧）
    int acquire_clock = 0;  // 自动索敌节拍（0 → 复位为 15）
    uint8_t turret_dir = 0; // 炮塔朝向字节 0..255（渲染；初始 = 建筑朝向）
};

// 爆炸（渲染事件：格 + 持续逻辑帧；stage 用 EXPLOMED 帧序列播放）
struct SimExplosion {
    int col = 0, row = 0;
    int total = 30;
    int elapsed = 0;
};

// 建造队列项（RA2 侧边栏：排队 → 进度 → 待放置）。
// 遭遇战流程：选中建筑 → queue_build（立即扣款）→ tick 推进 → ready →
// 玩家在地图放置（spawn_building）后清空。
struct BuildQueueItem {
    std::string type;   // rulesmd 类型名
    int ticks = 0;      // 已用逻辑帧
    int total = 1;      // 总逻辑帧
    int cost = 0;       // 已扣款
    bool ready = false; // 建造完成，等待放置
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
    uint64_t logic_ticks = 0; // 全局逻辑帧（低电减半的奇偶节拍用）
    // 步兵 idle 动作平均间隔（逻辑帧；stage 从 [General] IdleActionFrequency
    // 折算，YR = .15 分钟 → 135 帧 @15Hz；0 = 禁用 idle 动作）
    int idle_freq_ticks = 135;
    // 建造队列（每 House 单队列；遭遇战流程）
    std::map<std::string, BuildQueueItem> build_queue;

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

    // ── 遭遇战流程（M4：开局生成 / 基地车展开 / 建造队列）──
    // 生成单位（不走地图加载路径）：返回新单位 id（0 = 失败）。owner 无资金条目时
    // 按 $10000 补种；格被占/出界不拒绝（调用方负责选点）。
    uint32_t spawn_unit(const std::string& owner, const std::string& type, int kind, int col,
                        int row, uint8_t dir, const SimWeapon& w, bool is_miner, int capacity,
                        int speed);
    // 移除单位（基地车展开消耗车体）；返回是否移除
    bool remove_unit(size_t idx);
    // 生成建筑（基地车展开 / 建造队列放置）：地基矩形校验 + 阻挡标记。
    // under_construction = true 时进入建造/展开动画（build_total = 动画帧数）；
    // 完成后血量 = max_hp（rulesmd Strength=）。
    uint32_t spawn_building(const std::string& owner, const std::string& type, int col, int row,
                            int fw, int fh, int cost, int power, bool under_construction,
                            int build_total, int max_hp);
    // 地基是否可放置（矩形、全图内、无阻挡）
    bool can_place(int col, int row, int fw, int fh) const;

    // 建造队列（立即扣款；同一 House 单队列）
    bool queue_build(const std::string& owner, const std::string& type, int cost, int total_ticks);
    // 取消排队（按 rulesmd [General] RefundPercent 默认 50% 退款）
    bool cancel_build(const std::string& owner, int refund_percent = 50);
    bool build_ready(const std::string& owner) const;
    // 取出待放置项（放置成功后调用）；无待放置项返回 false
    bool take_ready_build(const std::string& owner, std::string* type);

    // ── 修理 / 出售（M4）──
    // 修理切换：repairing = true 时建筑按 RepairRate（rulesmd [General] 缺省 0.02·Cost
    // 每秒）逐帧回血并等比扣款，满血自动停。返回当前修理态（切换后）。
    bool toggle_repair(size_t building_idx);
    // 出售：立即移除建筑并退款（满血按 [General] RefundPercent；打折率随残血线性），
    // 解除地基阻挡。返回退款额（失败 = -1）。
    int64_t sell_building(size_t building_idx, int refund_percent = 50);

    // ── 建筑配置 / 防御攻击（M4）──
    // 落成后由 stage 按 rulesmd 注入：朝向 dir（0..255，也作为炮塔初始朝向）、
    // Primary 武器（无武器传空 = 纯建筑）与精炼厂标记（Refinery=yes；采矿车
    // 卸货目标判定用）。返回是否找到该建筑。
    bool configure_building(uint32_t id, int dir, const SimWeapon& w, bool is_refinery = false);
    // 命令建筑攻击单位（防御建筑；仅同/敌我校验，射程外会先转向等待）。
    bool issue_build_attack(size_t building_idx, size_t unit_idx);
    // 停止建筑攻击（清目标，恢复自动索敌）
    bool stop_build_attack(size_t building_idx);

    // 已完成建筑查询（科技树前置条件判定；completed_only = 只算建成建筑）
    bool has_building(const std::string& owner, const std::string& type,
                      bool completed_only = true) const;
    // 建造厂（ConstructionYard=yes 由 stage 判定并传类型名）
    bool has_conyard(const std::string& owner, const std::string& conyard_type) const {
        return has_building(owner, conyard_type, true);
    }

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
