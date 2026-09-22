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
#include "ra2r/sim/pathfind.h"

namespace ra2r::sim {

constexpr int kFracMax = 256;
// 转向/移动互斥阈值（用户更正）：车体朝向与目标方向偏差 ≤ 16
//（半个 45° 扇区）才允许推进；偏差更大时先原地转向。
constexpr int kMoveAlignTol = 16;

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

// 运动物理参数（rulesmd；stage 注入。0 = 无物理 = 原行为：瞬间起停/转向）
struct UnitMotion {
    int max_speed = 68;  // Speed= 标定后的 frac/逻辑帧上限
    int rot_step = 0;    // ROT=（每帧转向步长，0..255 圆周；0 = 立即转向）
    int turret_rot = 0;  // TurretROT=（0 = 用 ROT）
    int accel_step = 0;  // AccelerationFactor×上限（0 = 立即到上限）
    int decel_step = 0;  // DeaccelerationFactor×上限（0 = 瞬间停止）
};

struct SimUnit {
    uint32_t id = 0;
    std::string owner; // [Houses] 键名（地图 House 名）
    std::string type;  // rulesmd 类型名（如 "MTNK"）
    int kind = 1;      // 1=载具 2=步兵（与渲染 PlacedObject.kind 同约定）
    int col = 0, row = 0;           // 当前格
    int next_col = 0, next_row = 0; // 段终点格（静止时 = 当前格）
    int frac = 0;                   // 段内进度 0..255
    // 格内子格（步兵 0..2 = 等腰三角分布；载具恒 0）。一格 = 1 载具 **或**
    // 最多 3 个步兵（见 free_subcell）；移动中渲染端按格心绘制（子格仅在驻停时生效）
    uint8_t subcell = 0;
    uint8_t dir = 0;                // 车体朝向 0..255（渲染直用；8 向扇区中心 = n×32）
    uint8_t turret_dir = 0;         // 炮塔朝向 0..255（载具；按 TurretROT/ROT 转向）
    int vel = 0;                    // 当前速度（frac/逻辑帧，0..speed）
    int rot_step = 0;               // ROT=（0 = 立即转向）
    int turret_rot = 0;             // TurretROT=（0 = 用 rot_step）
    int accel_step = 0;             // AccelerationFactor 换算（0 = 立即加速）
    int decel_step = 0;             // DeaccelerationFactor 换算（0 = 瞬间停止）
    int hp = 256;
    bool alive = true;
    SimWeapon weapon;
    uint8_t order = kOrderNone;
    int target = -1;               // 攻击/护卫目标下标（按 order 语义）
    int cooldown = 0;
    int speed = 68; // 每逻辑帧 frac 增量（行步基准 ≈4 格/秒 @15Hz）
    // 上一格（渲染转角平滑用；= 当前格表示该段是路径起点）
    int prev_col = 0, prev_row = 0;
    // 渲染转角平滑的“下一段再下一格”（= 段终点之后那格；-1 = 无）：
    // **重算路径时不改**，当前段的渲染位置不跳变（不闪现）；
    // 跨格后才换成新路径的下一格。
    int next2_col = -1, next2_row = -1;
    // 移动目的地（格；-1 = 无）。段边界等位超时后用它在"把占位单位当临时障碍"
    // 的路网上绕行重规划（单位互相堵住时的出路）。
    int dest_col = -1, dest_row = -1;
    int wait_ticks = 0; // 段边界等位计时（格占用门禁；满 30 帧触发绕行重规划）
    // 让路冷却（OpenRA Nudge / 原版空闲单位避让）：被请求横挪后 40 帧内不再响应，
    // 避免多车互相"让来让去"抖动。
    int make_way_cd = 0;
    // 当前预订格（next_col/next_row）的预订时刻：多单位争同一格时
    // **先预订者优先**（同时刻则 id 小者优先），后来者立即改道而不是干等 30 帧。
    uint64_t res_tick = 0;
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
    int col = 0, row = 0; // 存储格（地基顶格，引擎坐标）
    int rx = 0, ry = 0;   // 存储格（地图空间；地基换算/调试用）
    int fw = 1, fh = 1;   // 地基尺寸（地图空间格）
    std::vector<std::pair<int, int>> footprint_cells; // 地基引擎格（邻接判定/寻路目标用）
    int hp = 256;
    bool alive = true;
    bool is_refinery = false;
    uint8_t dir = 0; // 地图存储朝向（0..255；炮塔 SHP 帧/体素 yaw 用）
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
    // 覆盖物阻挡（非矿石：桥梁/围墙/栅栏等）：不可在其上建造；矿石格用 ore 动态
    // 判定（采完后可建）。与 blocked 分离——覆盖物不阻挡通行。
    std::vector<uint8_t> no_build;
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
                  const std::function<bool(int, int)>& terrain_block,
                  const std::function<void(const std::string&, UnitMotion&)>& motion =
                      {});

    // 推进一逻辑帧；返回是否有单位移动/转向/开火（渲染侧据此刷新）
    bool tick();

    // 指令：单位 idx 移动到格 (tc,tr)（为当前格/无路径时原地停止）。
    // 目标不可达返回 false 并保持原指令。
    bool issue_move(size_t unit_idx, int tc, int tr);
    // **编队移动**：一组单位共享一次流场构建（目标 = 点击格 + 其周围可走格，
    // 单位各自就近落位），返回成功下达（含就地驻停）的单位数。多选命令用。
    size_t issue_move_group(const std::vector<size_t>& unit_idx, int tc, int tr);
    // 攻击敌方单位/建筑（进入射程后驻停开火）；护卫跟随友军单位
    bool issue_attack_unit(size_t unit_idx, size_t target_idx);
    bool issue_attack_building(size_t unit_idx, size_t building_idx);
    bool issue_guard(size_t unit_idx, size_t target_idx);
    // 停止（S 键）：清指令/目标/路径，原地驻停
    bool stop_unit(size_t unit_idx);
    // 追加巡逻点（首个点立即生效；到达末点后循环回第一点）
    void add_waypoint(size_t unit_idx, int tc, int tr);

    // 变更单位所属 House（1v1 演示脚本用）；新 House 无资金条目时按 $10000 补种
    void set_unit_owner(size_t idx, const std::string& owner);

    // ── 遭遇战流程（M4：开局生成 / 基地车展开 / 建造队列）──
    // 生成单位（不走地图加载路径）：返回新单位 id（0 = 失败）。owner 无资金条目时
    // 按 $10000 补种；格被占/出界不拒绝（调用方负责选点）。
    uint32_t spawn_unit(const std::string& owner, const std::string& type, int kind, int col,
                        int row, uint8_t dir, const SimWeapon& weapon, bool is_miner, int capacity,
                        int speed, const UnitMotion& motion = {});
    // 移除单位（基地车展开消耗车体）；返回是否移除
    bool remove_unit(size_t idx);
    // 生成建筑（基地车展开 / 建造队列放置）：地基矩形校验 + 阻挡标记。
    // under_construction = true 时进入建造/展开动画（build_total = 动画帧数）；
    // 完成后血量 = max_hp（rulesmd Strength=）。
    uint32_t spawn_building(const std::string& owner, const std::string& type, int col, int row,
                            int fw, int fh, int cost, int power, bool under_construction,
                            int build_total, int max_hp, uint32_t ignore_unit_id = 0);
    // ── 地基几何 ──    // 引擎格 ↔ 地图空间格（与 MapFile 的 rx/ry 换算互逆；锚点语义：地基顶格）
    void cell_to_map(int col, int row, int& rx, int& ry) const;
    void map_to_cell(int rx, int ry, int& col, int& row) const;
    // 地基格枚举（引擎坐标；锚 = 顶格）：地图空间 fw×fh 矩形 → 引擎格菱形。
    // **不是引擎网格矩形**：引擎矩形在砖墙排布里是沿行错位的平行四边形，
    // 与地图装载建筑的菱形地基和原版观感都不符（曾致摆放占格呈对角线错位）。
    void foundation_cells(int col, int row, int fw, int fh,
                          std::vector<std::pair<int, int>>& out) const;
    // 地基是否可放置（地图空间菱形、全图内、无阻挡/覆盖物/单位）。
    // ignore_unit_id：忽略该单位所占格（基地车展开时忽略车体自身）。
    bool can_place(int col, int row, int fw, int fh, uint32_t ignore_unit_id = 0) const;
    // 单格是否可建造（放置预览逐格标红/绿用）：图内、非地形/建筑阻挡、无
    // 覆盖物（桥梁/围墙等）、无矿石、无单位（ignore_unit_id 例外同上）。
    bool cell_buildable(int col, int row, uint32_t ignore_unit_id = 0) const;

// 格占用规则（原版）：一格 = 1 载具 **或** 最多 3 个步兵（子格 0..2，渲染
    // 为格内等腰三角分布）。返回 kind 单位在 (col,row) 可用的子格号；
    // -1 = 该格已满/被异类占据。ignore_unit_id：忽略自身（重定位/展开校验用）。
    int free_subcell(int col, int row, int kind, uint32_t ignore_unit_id = 0) const;

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
    bool configure_building(uint32_t id, int dir, const SimWeapon& weapon,
                            bool is_refinery = false);
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

    // ── 导航（多源流场；缓存按 目标格+槽位数+路网版本）──
    // nav_version 在 blocked 变化时自增（建筑落成/移除/地图装载）；流场缓存
    // 随之失效。blocked 变化必须调用 note_nav_change()（外部直接改 blocked 时，
    // 如 skirmish::pack_building 解阻）。
    uint32_t nav_version = 1;
    struct FlowCacheEntry {
        int tc = -1, tr = -1, slots = 0;
        uint32_t version = 0;
        FlowField field;
    };
    std::vector<FlowCacheEntry> flow_cache; // 最近使用（LRU：命中/插入移到末尾）
    void note_nav_change() {
        ++nav_version;
        flow_cache.clear();
    }
    // 取（必要时构建）目标格 (tc,tr) 的流场：slots = 目标槽位数（1 = 仅目标格；
    // 目标格被阻挡/槽位>1 时用"目标 + 周围可走格"作多源，单位就近落位）。
    const FlowField* flow_for(int tc, int tr, int slots);
    // 目标槽位表（确定性：目标格优先，再按环序取周围可走格；最多 slots 个）
    std::vector<std::pair<int, int>> nav_sources(int tc, int tr, int slots) const;
    // 同上，但用调用方给的临时路网（绕行重规划：把占位单位当障碍）
    std::vector<std::pair<int, int>> nav_sources_in(const std::vector<uint8_t>& nav, int tc,
                                                    int tr, int slots) const;
    // 段边界被单位堵住超时 → 用"临时路网（原 blocked + 占位单位格）"重规划到
    // 原目的地；成功返回 true（路径已换，段中续接保留进度）。
    bool replan_around_units(SimUnit& u);

    // 内部：段推进（到达落格 + 余量进下一段）；朝目标格寻路（失败驻停）。
    // 目标格被建筑挡住时由流场多源自动落到最近可达邻格（无需单独分支）。
    // set_move_target_field：已建好的流场 → 给单位布置路径（段中重下令从当前段
    // 终点续接并保留 frac，避免"复位到格心"的闪现）；单/编队指令共用此逻辑。
    bool advance_segment(SimUnit& u);
    bool set_move_target(SimUnit& u, int tc, int tr);
    bool set_move_target_field(SimUnit& u, const FlowField* f, int tc, int tr);
    // 运动物理：转向（ROT，偏差 >16 时原地转向且不推进）→ 加减速（Accelerates/
    // DeaccelerationFactor）→ 炮塔转向（TurretROT）。返回本帧车体朝向是否变化。
    bool update_motion(SimUnit& u);
    // 车体朝向与目标格方向偏差是否 ≤ kMoveAlignTol（对准则允许推进）
    bool segment_aligned(const SimUnit& u) const;
    // 格预订（原版机制）：目标格的挡路者 = 他人当前格或他人**已预订的
    // 下一格**；nullptr = 可入。双方互换（对方当前格=本格且对方预订格=我当前格）
    // 放行，避免正面相遇时双方各自等待对方预订格而死锁。
    SimUnit* blocker_at(SimUnit& u, int col, int row);
    // 让路（OpenRA Nudge）：请"挡路的空闲友军"横挪一格（确定性选择，排除地形
    // 阻挡/被占格/被挡者的当前格与预订格，别挪进对方行车线），返回是否已下达
    bool make_way(SimUnit& blocker, const SimUnit& blocked);
    // 槽位表（nav 路网上目标附近可走格），按离起点 (sc,sr) 由近到远排序
    std::vector<std::pair<int, int>> nav_sources_near(const std::vector<uint8_t>& nav, int sc,
                                                      int sr, int tc, int tr, int slots) const;
    // 单位感知局部规划：他人**静止格 + 行进单位的预订格**当障碍
    //（预订冲突 → 改道）；目标不可站时按"离起点最近"槽位落点。
    // 返回空且 at_slot=false = 无路；at_slot=true = 起点已是可落槽位。
    std::vector<std::pair<int, int>> plan_avoiding_units(const SimUnit& u, int sc, int sr,
                                                         int tc, int tr,
                                                         bool* at_slot = nullptr) const;

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

    // 单位渲染屏幕偏移（格内插值 + 转角平滑；与 stage 绘制同源）：
// 实际绘制位置 = 格心 + (off_x, off_y)。依赖 col/next/frac/prev/next2
// 五个量——重算路径时这些量不变，当前段画面不跳变。
void unit_render_offset(const SimUnit& u, int& off_x, int& off_y);

} // namespace ra2r::sim
