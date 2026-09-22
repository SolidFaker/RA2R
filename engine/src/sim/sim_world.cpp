// RA2R — M3 模拟层实现（接口见 sim_world.h）
#include "ra2r/sim/sim_world.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <utility>

#include "ra2r/sim/pathfind.h"

namespace ra2r::sim {

namespace {
// 向下取整的 /2（负数也正确：引擎格 ↔ 地图空间的 floor 语义）
int floor_div2(int v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); }

// 屏幕 8 向**单位**向量（×256）：45° = (229,±114)（(30,15)/33.54），
// 水平 = (±256,0)（60px），垂直 = (0,±256)（30px）。
// 必须用归一化向量做点积：屏幕 8 向的规范长度不等（60/33.5/30px），用未归一
// 的 kDirV 时 45° 步的 dot 会输给水平步（60·30+0·15=1800 > 30·30+15·15=1125），
// 对角移动被误判成"朝右"——VXL 车头只剩水平/竖直、步兵走序列也放错朝向帧。
constexpr int kDirU[8][2] = {{229, -114}, {256, 0},   {229, 114},  {0, 256},
                             {-229, 114}, {-256, 0},  {-229, -114}, {0, -256}};

// 屏幕增量 → 最近朝向（8 向；argmax 与单位方向向量的点积，纯整数确定性）
uint8_t dir_of_screen(int dx, int dy) {
    int best = 0;
    long long best_dot = INT64_MIN;
    for (int k = 0; k < 8; ++k) {
        const long long dot = static_cast<long long>(dx) * kDirU[k][0] +
                              static_cast<long long>(dy) * kDirU[k][1];
        if (dot > best_dot) {
            best_dot = dot;
            best = k;
        }
    }
    return static_cast<uint8_t>(best);
}

// 段的屏幕方向（8 向）。砖墙格 (c,r) 的屏幕位置 = (60c + 30(r&1), 15r)
// （见 IsometricGrid）；8 邻接步覆盖屏幕 8 向：45° 步 (0,±1)/(±1,±1)、
// 水平 (±1,0)、垂直 (0,±2)
uint8_t dir_of(int c, int r, int nc, int nr) {
    const int dx = 60 * (nc - c) + 30 * ((nr & 1) - (r & 1));
    const int dy = 15 * (nr - r);
    return dir_of_screen(dx, dy);
}

// 朝目标格的 8 向朝向（任意距离，站立/攻击面向用）
uint8_t dir_toward(int c, int r, int tc, int tr) {
    const int dx = 60 * (tc - c) + 30 * ((tr & 1) - (r & 1));
    const int dy = 15 * (tr - r);
    return dir_of_screen(dx, dy);
}

int manhattan(int c, int r, int tc, int tr) { return std::abs(c - tc) + std::abs(r - tr); }

// 每单位确定性伪随机流（步兵 idle 动作选择/间隔；纯整数，跨平台一致）
uint32_t unit_rand(SimUnit& u) {
    u.idle_rng = u.idle_rng * 1664525u + 1013904223u;
    return u.idle_rng >> 16;
}

// idle 动作等待 = 0.5~2× 平均间隔（原版 IdleActionFrequency 语义），单位逻辑帧
int idle_wait_ticks(SimUnit& u, int freq) {
    if (freq < 1) return 1;
    const uint32_t r = unit_rand(u) % static_cast<uint32_t>(freq + 1); // 0..freq
    return freq / 2 + static_cast<int>(r) * 3 / 2;                     // 0.5f..2f
}

// 到建筑任意地基格的最小曼哈顿距离；并给出最近格（out 可空）
int dist_to_building(const SimBuilding& b, int col, int row, int* out_col = nullptr,
                     int* out_row = nullptr) {
    int best = INT_MAX;
    for (const auto& c : b.footprint_cells) {
        const int d = manhattan(col, row, c.first, c.second);
        if (d < best) {
            best = d;
            if (out_col) *out_col = c.first;
            if (out_row) *out_row = c.second;
        }
    }
    return best == INT_MAX ? manhattan(col, row, b.col, b.row) : best;
}

// 最近有矿格（由近及远环形扫描，确定性）；无矿返回 false
bool find_nearest_ore(const std::vector<int16_t>& ore, int w, int h, int col, int row, int& tc,
                      int& tr) {
    // 防御：矿石格栅缺失/尺寸不符（无矿地图）直接无矿（否则越界读）
    if (ore.size() < static_cast<size_t>(w) * static_cast<size_t>(h)) return false;
    const int maxr = std::max(w, h);
    for (int rad = 0; rad <= maxr; ++rad) {
        for (int dy = -rad; dy <= rad; ++dy) {
            for (int dx = -rad; dx <= rad; ++dx) {
                if (std::abs(dx) != rad && std::abs(dy) != rad) continue; // 只扫环
                const int x = col + dx, y = row + dy;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                if (ore[static_cast<size_t>(y) * w + x] > 0) {
                    tc = x;
                    tr = y;
                    return true;
                }
            }
        }
    }
    return false;
}
} // namespace

bool SimWorld::load_map(const assets::MapFile& map,
                        const std::function<void(const std::string&, int&, int&)>& footprint,
                        const std::function<void(const std::string&, SimWeapon&)>& weapon,
                        const std::function<void(const std::string&, bool&, int&)>& miner,
                        const std::function<bool(const std::string&)>& refinery,
                        const std::function<int(const std::string&)>& power_of,
                        const std::function<int16_t(int, int)>& ore_at,
                        const std::function<bool(int, int)>& terrain_block,
                        const std::function<void(const std::string&, UnitMotion&)>&
                            motion) {
    w = map.cell_w();
    h = map.cell_h();
    min_d = map.min_d();
    min_s = map.min_s();
    blocked.assign(static_cast<size_t>(w) * h, 0);
    buildings.clear();
    units.clear();
    explosions.clear();
    credits.clear();
    power_net.clear();
    ore.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            ore[static_cast<size_t>(y) * w + x] = ore_at(x, y);
            // 水面/悬崖等不可通行地形（M3 基础档按 TileSet 分类名判定）
            if (terrain_block(x, y)) blocked[static_cast<size_t>(y) * w + x] = 1;
        }
    // 覆盖物阻挡建造（矿石除外：矿石采完即空，归 ore 动态判定；桥梁/围墙/栅栏
    // 等覆盖物一律不可建造）。矿石由 ore_at 回调给出（TIB*/GEM*），故此处只标
    // "有覆盖物且非矿石"的格。与 blocked 分离：覆盖物不阻挡通行，只挡建造。
    no_build.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (ore[static_cast<size_t>(y) * w + x] > 0) continue;
            int rx = 0, ry = 0;
            cell_to_map(x, y, rx, ry);
            if (map.overlay_type(rx, ry) != 0xFF) no_build[static_cast<size_t>(y) * w + x] = 1;
        }
    // 覆盖物阻挡建造（矿石格除外：矿石可由采矿车采完，采完后可建；其余覆盖物
    // ——桥梁/围墙/栅栏等——一律不可建造）。矿石的判定复用 ore_at 回调
    // （TIB*/GEM* 归矿石），故这里只处理"有覆盖物但不是矿石"的格。
    no_build.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (ore[static_cast<size_t>(y) * w + x] > 0) continue;
            int rx = 0, ry = 0;
            cell_to_map(x, y, rx, ry);
            if (map.overlay_type(rx, ry) != 0xFF) no_build[static_cast<size_t>(y) * w + x] = 1;
        }
    note_nav_change(); // 新地图的 blocked 全量重建 → 流场缓存失效
    next_id = 1;
    // 各 House 初始资金（遭遇战 $10000，M3 基础档）
    const auto seed_credits = [&](const std::string& owner) {
        if (!owner.empty() && !credits.count(owner)) credits[owner] = 10000;
    };
    // 建筑：实体 + 地基格阻挡（地基 = 地图空间 fw×fh → 引擎格换算）
    for (const auto& b : map.buildings()) {
        SimBuilding sb;
        sb.id = next_id++;
        sb.owner = b.owner;
        sb.type = b.id;
        sb.col = b.cx;
        sb.row = b.cy;
        sb.dir = b.dir;
        sb.rx = b.rx;
        sb.ry = b.ry;
        sb.hp = b.health;
        int fw = 1, fh = 1;
        footprint(b.id, fw, fh);
        if (fw < 1) fw = 1;
        if (fh < 1) fh = 1;
        sb.fw = fw;
        sb.fh = fh;
        sb.is_refinery = refinery(b.id);
        sb.power = power_of(b.id);
        sb.weapon = {};
        weapon(b.id, sb.weapon); // 防御建筑 Primary= 武器（无武器 = 空）
        sb.turret_dir = sb.dir;
        seed_credits(b.owner);
        for (int j = 0; j < fh; ++j) {
            for (int i = 0; i < fw; ++i) {
                const int rx = b.rx + i;
                const int ry = b.ry + j;
                const int col = (rx - ry - map.min_d()) / 2;
                const int row = rx + ry - map.min_s();
                if (col >= 0 && row >= 0 && col < w && row < h) {
                    blocked[static_cast<size_t>(row) * w + col] = 1;
                    sb.footprint_cells.emplace_back(col, row);
                }
            }
        }
        buildings.push_back(std::move(sb));
    }
    // 载具（[Units]）
    for (const auto& u : map.units()) {
        SimUnit su;
        su.id = next_id++;
        su.owner = u.owner;
        su.type = u.id;
        su.kind = 1;
        su.col = su.next_col = u.cx;
        su.row = su.next_row = u.cy;
        su.subcell = 0; // 载具占整格
        su.dir = u.dir;
        su.turret_dir = su.dir;
        {
            UnitMotion mo;
            mo.max_speed = su.speed;
            if (motion) motion(u.id, mo);
            su.speed = mo.max_speed;
            su.vel = mo.accel_step > 0 ? 0 : mo.max_speed;
            su.rot_step = mo.rot_step;
            su.turret_rot = mo.turret_rot > 0 ? mo.turret_rot : mo.rot_step;
            su.accel_step = mo.accel_step;
            su.decel_step = mo.decel_step;
        }
        su.hp = u.health;
        weapon(u.id, su.weapon);
        miner(u.id, su.is_miner, su.capacity);
        seed_credits(u.owner);
        units.push_back(std::move(su));
    }
    // 步兵（[Infantry]）
    for (const auto& n : map.infantry()) {
        SimUnit su;
        su.id = next_id++;
        su.owner = n.owner;
        su.type = n.id;
        su.kind = 2;
        su.col = su.next_col = n.cx;
        su.row = su.next_row = n.cy;
        su.subcell = n.subcell <= 4 ? n.subcell : 0; // 地图原始子格（0..4 五格位）
        su.dir = n.dir;
        su.idle_rng = su.id * 1664525u + 12345u; // 步兵 idle 动作随机流播种
        su.hp = n.health;
        su.speed = 51; // 步兵 ≈3 格/秒
        {
            UnitMotion mo;
            mo.max_speed = su.speed;
            if (motion) motion(n.id, mo);
            su.speed = mo.max_speed;
            su.vel = mo.accel_step > 0 ? 0 : mo.max_speed;
            su.rot_step = mo.rot_step;
            su.turret_rot = mo.turret_rot > 0 ? mo.turret_rot : mo.rot_step;
            su.accel_step = mo.accel_step;
            su.decel_step = mo.decel_step;
        }
        weapon(n.id, su.weapon);
        miner(n.id, su.is_miner, su.capacity);
        seed_credits(n.owner);
        units.push_back(std::move(su));
    }
    return !units.empty() || !buildings.empty();
}

// 运动物理推进（rulesmd 语义）：① 车身按 ROT 转向当前段方向；**转向与移动互斥**
// ——朝向与目标方向偏差 >16 时先原地转向（速度目标 0：暂停或按减速系数
// 大幅减速），偏差 ≤ 16（半个 45° 扇区）才允许向目标格推进；② 速度按
// Accelerates/AccelerationFactor 加速到 Speed 上限，末段（path 为空）且
// DeaccelerationFactor>0 时随剩余距离线性减速（保证仍能到达，下限 2 frac/帧）；
// 系数为 0 = 立即到位（**不是不能移动**）；对准且有活动段时每帧
// 至少推进 1 frac，保证不会卡死；③ 炮塔按 TurretROT（缺省 ROT）转向：
// 攻击/护卫目标优先，否则跟随行进方向。返回本帧车体朝向是否变化。
bool SimWorld::segment_aligned(const SimUnit& u) const {
    if (u.next_col == u.col && u.next_row == u.row) return false;
    const int want = dir_of(u.col, u.row, u.next_col, u.next_row) * 32;
    int diff = want - static_cast<int>(u.dir);
    while (diff > 128) diff -= 256;
    while (diff < -128) diff += 256;
    return std::abs(diff) <= kMoveAlignTol;
}

bool SimWorld::update_motion(SimUnit& u) {
    const bool seg = u.next_col != u.col || u.next_row != u.row;
    const int want = seg ? dir_of(u.col, u.row, u.next_col, u.next_row) * 32 : -1;
    const auto rotate = [](uint8_t& facing, int target, int rate) -> bool {
        int diff = target - static_cast<int>(facing);
        while (diff > 128) diff -= 256;
        while (diff < -128) diff += 256;
        if (diff == 0) return false;
        const int max_step = rate > 0 ? rate : std::abs(diff); // 0 = 立即转向
        const int step = std::min(std::abs(diff), max_step);
        facing = static_cast<uint8_t>((static_cast<int>(facing) + (diff > 0 ? step : -step)) & 255);
        return diff != 0 && step < std::abs(diff); // 仍有差值 → 转向中
    };
    bool turned = false;
    if (want >= 0) {
        const uint8_t before = u.dir;
        rotate(u.dir, want, u.rot_step);
        turned = u.dir != before;
    }
    // 炮塔：攻击/护卫目标优先，否则行进方向（驻停且无目标时不动）
    int want_tur = want;
    if ((u.order == kOrderAttackUnit || u.order == kOrderGuard) && u.target >= 0 &&
        u.target < static_cast<int>(units.size()) && units[static_cast<size_t>(u.target)].alive) {
        const SimUnit& t = units[static_cast<size_t>(u.target)];
        want_tur = dir_of(u.col, u.row, t.col, t.row) * 32;
    }
    if (want_tur >= 0) rotate(u.turret_dir, want_tur, u.turret_rot);
    // 速度目标：转向中 0；末段可减速则随剩余距离线性下降；否则上限
    int target = u.speed;
    if (!seg || !segment_aligned(u)) {
        target = 0; // 无活动段，或朝向偏差 >16（原地转向：暂停/大幅减速）
    } else if (u.path.empty() && u.decel_step > 0) {
        target = std::max(2, u.speed * (kFracMax - u.frac) / kFracMax);
    }
    if (u.vel < target) {
        u.vel = std::min(target, u.vel + (u.accel_step > 0 ? u.accel_step : u.speed));
    } else if (u.vel > target) {
        u.vel = std::max(target, u.vel - (u.decel_step > 0 ? u.decel_step : u.speed));
    }
    return turned;
}

// 段推进（到达落格 + 余量进下一段；所有订单共用）
bool SimWorld::advance_segment(SimUnit& u) {
    if (u.next_col == u.col && u.next_row == u.row) {
        u.frac = 0;
        return false;
    }
    if (!segment_aligned(u)) return false; // 朝向偏差 >16：原地转向，暂不推进
    // 步长折算（恒定屏幕速率 = 33.5px/speed 单位）：45° 步 33.5px → speed；
    // 屏幕水平 60px → speed·143/256；屏幕垂直 30px → speed·286/256
    // （OpenRA 同思路：位置按世界距离推进，方向不同的步长按屏幕投影折算）
    const int dc = u.next_col - u.col;
    const int dr = u.next_row - u.row;
    // 段屏幕长度（×10；仅用于跨段余量换算，与上面折算保持同一基准）
    const auto seg_len = [](int ddc, int ddr) -> int {
        if (ddc == 0 && (ddr == 2 || ddr == -2)) return 300; // 垂直 30px
        if (ddc != 0 && ddr == 0) return 600;                // 水平 60px
        return 335;                                          // 45° 33.5px
    };
    int len_old = seg_len(dc, dr);
    int inc;
    if (dc == 0 && (dr == 2 || dr == -2)) inc = (u.vel * 286) / 256;
    else if (dc != 0 && dr == 0) inc = (u.vel * 143) / 256;
    else inc = u.vel;
    // 保证不卡死：只要还有活动段，每帧至少推进 1 frac
    //（刚启动或系数极小时速度为 0/极低，也不能冻住）。
    if (inc <= 0) inc = 1;
    // 推进本帧增量，并在**同一帧内**结算跨过的格心（frac 恒 < 256）：
    // 若把 >256 的残留留到下一帧，渲染位置会越过格心再被拉回（每格一次抖动）。
    // 跨段时余量按新旧段长度换算（frac 是"段内百分比"，同余量=同屏幕距离）。
    u.frac += inc;
    while (u.frac >= kFracMax) {
        // 格占用门禁（原版：一格 1 载具 或 ≤3 步兵）：目标格已满 → 停在段
        // 边界等位（frac 钳在 255，下一逻辑帧重试）；等位超时（30 帧）→ 把
        // 占位单位当临时障碍绕行重规划，避免互相堵死。
        if (free_subcell(u.next_col, u.next_row, u.kind, u.id) < 0) {
            u.frac -= inc; // 撤销本帧增量：停在格内原位置（不越界进占格）
            if (u.frac < 0) u.frac = 0;
            // 动态障碍（车辆/人物）：**立即**为被挡单位重算绕行路径（首帧就重算）；
            // 仍被挡则每 5 帧再试（避免每帧一次全图 Dijkstra 拖慢逻辑帧）
            if (u.wait_ticks == 0 || u.wait_ticks % 5 == 0) replan_around_units(u);
            ++u.wait_ticks;
            break;
        }
        u.wait_ticks = 0;
        const int rem = u.frac - kFracMax; // 旧段余量（旧段单位）
        u.prev_col = u.col; // 渲染转角平滑用（上一格 = 本段起点）
        u.prev_row = u.row;
        u.col = u.next_col;
        u.row = u.next_row;
        u.frac = rem;
        if (u.path.empty()) {
            u.frac = 0; // 到达终点：清段内余量（否则 unit_moving 恒真、走路动画停不下来）
            u.vel = 0; // 到达 → 速度归零（DeaccelerationFactor=0 即瞬间停止）
            u.next_col = u.col;
            u.next_row = u.row;
            if (u.kind == 2) { // 步兵落位到格内空闲子格（等腰三角分布）
                const int cs = free_subcell(u.col, u.row, 2, u.id);
                if (cs >= 0) u.subcell = static_cast<uint8_t>(cs);
            }
            if (u.order == kOrderMove) u.order = kOrderNone; // 移动完成 → 空闲
            break;
        }
        u.next_col = u.path.front().first;
        u.next_row = u.path.front().second;
        u.path.erase(u.path.begin());
        u.next2_col = u.path.empty() ? -1 : u.path.front().first;
        u.next2_row = u.path.empty() ? -1 : u.path.front().second;
        const int len_new = seg_len(u.next_col - u.col, u.next_row - u.row);
        if (len_new != len_old) {
            u.frac = (rem * len_old + len_new / 2) / len_new;
            len_old = len_new;
        }
    }
    return true;
}

// 单位渲染屏幕偏移（格内插值 + 转角平滑；与 stage 绘制同源，供测试复用）。
// 二次 B 样条：控制点 = 相邻格中心（prev/col/next/next2），缺失的邻居按
// 直线外推 → 直线段精确、转弯处自然切角、端点落在格心。
void unit_render_offset(const SimUnit& u, int& off_x, int& off_y) {
    const auto cell_px = [](int c, int r) {
        return std::pair<int, int>{c * 60 + (r & 1) * 30, r * 15};
    };
    const std::pair<int, int> tp = cell_px(u.col, u.row);
    const std::pair<int, int> np = cell_px(u.next_col, u.next_row);
    const int tx = tp.first, ty = tp.second;
    const int nx = np.first, ny = np.second;
    off_x = ((nx - tx) * u.frac) / kFracMax;
    off_y = ((ny - ty) * u.frac) / kFracMax;
    if (u.next_col == u.col && u.next_row == u.row) return; // 无活动段
    std::pair<int, int> qp = cell_px(u.prev_col, u.prev_row);
    if (u.prev_col == u.col && u.prev_row == u.row) { // 无上一段 → 直线外推
        qp = {2 * tx - nx, 2 * ty - ny};
    }
    std::pair<int, int> wp{0, 0};
    if (u.next2_col >= 0) { // 段终点之后那格：重算路径时保持不变
        wp = cell_px(u.next2_col, u.next2_row);
    } else if (!u.path.empty()) { // 回退：取剩余路径首格
        wp = cell_px(u.path.front().first, u.path.front().second);
    } else { // 路径终点 → 直线外推（端点落在格心）
        wp = {2 * nx - tx, 2 * ny - ty};
    }
    const int m0x = qp.first + tx, m0y = qp.second + ty;
    const int m1x = tx + nx, m1y = ty + ny;
    const int m2x = nx + wp.first, m2y = ny + wp.second;
    const long long f = u.frac; // 0..255（kFracMax=256）
    long long sx2 = 0, sy2 = 0; // 2× 平滑位置
    if (f < 128) { // 前半段：过当前格心（控制点 tx,ty）
        const long long v = f + 128, ca = 256 - v, cb = v;
        sx2 = (ca * ca * m0x + 2 * ca * cb * (2LL * tx) + cb * cb * m1x) / 65536;
        sy2 = (ca * ca * m0y + 2 * ca * cb * (2LL * ty) + cb * cb * m1y) / 65536;
    } else { // 后半段：过下一格心（控制点 nx,ny）
        const long long v = f - 128, ca = 256 - v, cb = v;
        sx2 = (ca * ca * m1x + 2 * ca * cb * (2LL * nx) + cb * cb * m2x) / 65536;
        sy2 = (ca * ca * m1y + 2 * ca * cb * (2LL * ny) + cb * cb * m2y) / 65536;
    }
    off_x = static_cast<int>((sx2 - 2LL * tx) / 2);
    off_y = static_cast<int>((sy2 - 2LL * ty) / 2);
}

// 目标槽位表：目标格优先，再按固定环序取周围可走格（确定性）；最多 slots 个。
// 目标被建筑/水面挡住或编队人数多于 1 时，单位按流场就近落到这些槽位。
std::vector<std::pair<int, int>> SimWorld::nav_sources(int tc, int tr, int slots) const {
    return nav_sources_in(blocked, tc, tr, slots);
}

std::vector<std::pair<int, int>> SimWorld::nav_sources_in(const std::vector<uint8_t>& nav,
                                                          int tc, int tr, int slots) const {
    std::vector<std::pair<int, int>> out;
    if (slots < 1) slots = 1;
    if (slots > 16) slots = 16;
    const auto walkable = [&](int c, int r) {
        if (c < 0 || r < 0 || c >= w || r >= h) return false;
        const size_t i = static_cast<size_t>(r) * w + c;
        return i >= nav.size() || nav[i] == 0;
    };
    if (walkable(tc, tr)) out.emplace_back(tc, tr);
    static const int kRing[16][2] = {{1, 0},  {-1, 0},  {0, 1},   {0, -1}, {1, 1},   {-1, 1},
                                     {1, -1}, {-1, -1}, {2, 0},   {-2, 0}, {0, 2},   {0, -2},
                                     {2, 1},  {-2, 1},  {2, -1},  {-2, -1}};
    for (const auto& d : kRing) {
        if (static_cast<int>(out.size()) >= slots) break;
        const int c = tc + d[0], r = tr + d[1];
        if (!walkable(c, r)) continue;
        bool dup = false;
        for (const auto& e : out)
            if (e.first == c && e.second == r) {
                dup = true;
                break;
            }
        if (!dup) out.emplace_back(c, r);
    }
    return out;
}

const FlowField* SimWorld::flow_for(int tc, int tr, int slots) {
    if (slots < 1) slots = 1;
    if (slots > 16) slots = 16;
    for (size_t i = 0; i < flow_cache.size(); ++i) {
        FlowCacheEntry& e = flow_cache[i];
        if (e.tc == tc && e.tr == tr && e.slots == slots && e.version == nav_version) {
            if (i + 1 != flow_cache.size()) {
                FlowCacheEntry hit = std::move(e);
                flow_cache.erase(flow_cache.begin() + static_cast<std::ptrdiff_t>(i));
                flow_cache.push_back(std::move(hit));
            }
            return &flow_cache.back().field;
        }
    }
    std::vector<std::pair<int, int>> sources = nav_sources(tc, tr, slots);
    if (sources.empty()) return nullptr;
    FlowCacheEntry entry;
    entry.tc = tc;
    entry.tr = tr;
    entry.slots = slots;
    entry.version = nav_version;
    if (!build_flow_field(blocked, w, h, (min_s + min_d) & 1, sources, entry.field))
        return nullptr;
    if (flow_cache.size() >= 8) flow_cache.erase(flow_cache.begin());
    flow_cache.push_back(std::move(entry));
    return &flow_cache.back().field;
}

// 用已建好的流场给单位布置路径（失败 → 清路径并驻停，返回是否可达）。
// 移动中重下令（反复右键改目标 / 编队反复改点）：从当前段**终点**续接并保留
// 段内进度 frac，否则 frac 归零会让单位视觉上"复位到格心"（OpenRA 同思路：
// 重寻路不改变已走位置，只是替换剩余路径）。目标不可达时也先走完当前这一步再停。
bool SimWorld::set_move_target_field(SimUnit& u, const FlowField* f, int tc, int tr) {
    u.dest_col = tc;
    u.dest_row = tr;
    u.wait_ticks = 0;
    const bool mid = u.frac > 0 && (u.next_col != u.col || u.next_row != u.row);
    const int sc = mid ? u.next_col : u.col;
    const int sr = mid ? u.next_row : u.row;
    const auto at_goal = [&](int c, int r) {
        if (!f) return false;
        const size_t i = static_cast<size_t>(r) * w + c;
        return i < f->goal.size() && f->goal[i] != 0;
    };
    const bool start_at_goal = at_goal(sc, sr);
    std::vector<std::pair<int, int>> path =
        (f && !start_at_goal) ? flow_path(*f, sc, sr) : std::vector<std::pair<int, int>>{};
    if (mid) {
        u.path = std::move(path); // 当前段（col/next/frac/prev）保持不动
        // 转角平滑控制点：本段原有值优先保持（画面不跳变）；
        // 原先没有（路径即将走完）时用新路径的下一格
        if (u.next2_col < 0 && !u.path.empty()) {
            u.next2_col = u.path.front().first;
            u.next2_row = u.path.front().second;
        }
        return !u.path.empty() || start_at_goal;
    }
    if (path.empty()) {
        u.path.clear();
        u.next_col = u.col;
        u.next_row = u.row;
        u.frac = 0;
        return start_at_goal;
    }
    u.path = std::move(path);
    u.prev_col = u.col; // 该段为路径起点（渲染端按直线外推，转角平滑端点精确）
    u.prev_row = u.row;
    u.next_col = u.path.front().first;
    u.next_row = u.path.front().second;
    u.path.erase(u.path.begin());
    u.next2_col = u.path.empty() ? -1 : u.path.front().first;
    u.next2_row = u.path.empty() ? -1 : u.path.front().second;
    u.frac = 0;
    return true;
}

// 单目标移动：流场（目标 + 必要时周围可走格）→ 布置路径
bool SimWorld::set_move_target(SimUnit& u, int tc, int tr) {
    return set_move_target_field(u, flow_for(tc, tr, 1), tc, tr);
}

// 段边界被单位堵住超时 → 绕行重规划：把"对本人不可入"的单位格当临时障碍
// （原 blocked + 占位单位），目标槽位也只取临时路网上的可走格。**从当前格**
// 重规划（段终点被占，不能从段终点续接），成功则清掉旧段、从格心重新起步。
bool SimWorld::replan_around_units(SimUnit& u) {
    if (u.dest_col < 0) return false;
    std::vector<uint8_t> nav = blocked;
    for (const auto& o : units) {
        if (!o.alive || o.id == u.id) continue;
        if (free_subcell(o.col, o.row, u.kind, u.id) >= 0) continue; // 该格对本人可入
        const size_t i = static_cast<size_t>(o.row) * w + o.col;
        if (i < nav.size()) nav[i] = 1;
    }
    const std::vector<std::pair<int, int>> slots = nav_sources_in(nav, u.dest_col, u.dest_row, 1);
    if (slots.empty()) return false;
    FlowField f;
    if (!build_flow_field(nav, w, h, (min_s + min_d) & 1, slots, f)) return false;
    const auto at_goal = [&](int c, int r) {
        const size_t i = static_cast<size_t>(r) * w + c;
        return i < f.goal.size() && f.goal[i] != 0;
    };
    if (at_goal(u.col, u.row)) { // 已站到槽位（段终点被占而已）→ 原地驻停
        u.path.clear();
        u.next_col = u.col;
        u.next_row = u.row;
        u.frac = 0;
        return true;
    }
    std::vector<std::pair<int, int>> path = flow_path(f, u.col, u.row);
    if (path.empty()) return false;
    // **不弹回格心**：段终点被占才重算，此时把单位已走的屏幕位移投影到
    // 新段方向上作为新 frac（方向接近时位置几乎不变；硬转弯也≤半个格），
    // prev 保留（来向）交给渲染端做转角平滑，col 不动 —— 无闪现。
    const int n2c = path.front().first, n2r = path.front().second;
    const auto pix_delta = [&](int c0, int r0, int c1, int r1) {
        return std::pair<int, int>{ (c1 - c0) * 60 + ((r1 & 1) - (r0 & 1)) * 30,
                                     (r1 - r0) * 15 };
    };
    const auto [d1x, d1y] = pix_delta(u.col, u.row, u.next_col, u.next_row);
    const auto [d2x, d2y] = pix_delta(u.col, u.row, n2c, n2r);
    const long long num = static_cast<long long>(d1x) * d2x +
                          static_cast<long long>(d1y) * d2y;
    const long long den = static_cast<long long>(d2x) * d2x +
                          static_cast<long long>(d2y) * d2y;
    int frac2 = den > 0 ? static_cast<int>(num * u.frac / den) : 0;
    frac2 = std::clamp(frac2, 0, kFracMax - 1);
    u.path = std::move(path);
    u.path.erase(u.path.begin());
    u.next_col = n2c;
    u.next_row = n2r;
    u.next2_col = u.path.empty() ? -1 : u.path.front().first;
    u.next2_row = u.path.empty() ? -1 : u.path.front().second;
    u.frac = frac2;
    u.wait_ticks = 0;
    return true;
}

// 编队移动：为**每个单位**分配一个"尽量接近点击格、且不违反格占用约束"的
// 目的地（步兵同格最多 3、载具独占、互斥），再各自以流场点到点寻路。
// 记账表 = 现有单位 + 本次已分配；搜索序 = 最大范数环（近到远）+ 环内固定序。
size_t SimWorld::issue_move_group(const std::vector<size_t>& unit_idx, int tc, int tr) {
    std::vector<size_t> valid;
    valid.reserve(unit_idx.size());
    for (const size_t i : unit_idx)
        if (i < units.size() && units[i].alive) valid.push_back(i);
    if (valid.empty()) return 0;
    struct CellUse {
        int inf = 0;
        bool veh = false;
    };
    std::map<size_t, CellUse> use;
    const auto key = [&](int c, int r) { return static_cast<size_t>(r) * w + c; };
    for (const auto& o : units) {
        if (!o.alive) continue;
        CellUse& cu = use[key(o.col, o.row)];
        if (o.kind == 2) ++cu.inf;
        else cu.veh = true;
    }
    size_t ordered = 0;
    for (const size_t i : valid) {
        SimUnit& u = units[i];
        int bc = -1, br = -1;
        for (int rad = 0; rad <= 8 && bc < 0; ++rad) {
            for (int dy = -rad; dy <= rad && bc < 0; ++dy)
                for (int dx = -rad; dx <= rad && bc < 0; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != rad) continue;
                    const int c = tc + dx, r = tr + dy;
                    if (c < 0 || r < 0 || c >= w || r >= h) continue;
                    const size_t ci = key(c, r);
                    if (ci < blocked.size() && blocked[ci]) continue; // 地形/建筑阻挡
                    const auto it = use.find(ci);
                    const CellUse cu = it == use.end() ? CellUse{} : it->second;
                    if (u.kind == 2) {
                        if (cu.veh || cu.inf >= 3) continue; // 步兵：载具互斥 + 最多 3
                    } else if (cu.veh || cu.inf > 0) {
                        continue; // 载具：独占整格
                    }
                    bc = c;
                    br = r;
                }
        }
        u.order = kOrderMove;
        u.target = -1;
        if (bc < 0) { // 8 环内无空位（极罕见）→ 原地驻停
            u.path.clear();
            u.next_col = u.col;
            u.next_row = u.row;
            u.frac = 0;
            ++ordered;
            continue;
        }
        CellUse& cu = use[key(bc, br)];
        if (u.kind == 2) ++cu.inf;
        else cu.veh = true;
        set_move_target_field(u, flow_for(bc, br, 1), bc, br);
        ++ordered;
    }
    return ordered;
}


bool SimWorld::tick() {
    bool changed = false;
    for (auto& u : units) { // 固定索引序遍历（确定性）
        if (!u.alive) continue;
        if (u.cooldown > 0) --u.cooldown;
        const bool stand = u.frac == 0 && u.next_col == u.col && u.next_row == u.row;
        if (u.is_miner && (u.order == kOrderNone || u.order == kOrderHarvest)) {
            // 采矿循环：空闲自动采集；满载 → 最近精炼厂卸货 → 资金
            u.order = kOrderHarvest;
            if (u.cargo >= u.capacity) {
                const bool ref_ok = u.target >= 0 && u.target < static_cast<int>(buildings.size()) &&
                                    buildings[static_cast<size_t>(u.target)].alive &&
                                    buildings[static_cast<size_t>(u.target)].is_refinery;
                if (!ref_ok) {
                    u.target = -1;
                    for (int i = 0; i < static_cast<int>(buildings.size()); ++i) {
                        if (!buildings[i].alive || !buildings[i].is_refinery) continue;
                        if (u.target < 0 ||
                            manhattan(u.col, u.row, buildings[i].col, buildings[i].row) <
                                manhattan(u.col, u.row, buildings[u.target].col,
                                          buildings[u.target].row))
                            u.target = i;
                    }
                }
                if (u.target < 0) {
                    continue; // 无精炼厂：驻停（满仓）
                }
                const SimBuilding& b = buildings[static_cast<size_t>(u.target)];
                int tc = b.col, tr = b.row;
                const int d = dist_to_building(b, u.col, u.row, &tc, &tr);
                if (d <= 1) {
                    if (stand) {
                        credits[u.owner] += u.cargo;
                        u.cargo = 0;
                        u.target = -1;
                        changed = true;
                    }
                } else if (stand) {
                    // 仅在驻停（段走完）时（重）寻路，避免段中反复重置进度
                    if (!set_move_target(u, tc, tr)) {
                        u.target = -1;
                        continue;
                    }
                    changed = true;
                }
            } else {
                const size_t oi = static_cast<size_t>(u.row) * w + u.col;
                if (ore[oi] > 0 && stand) {
                    // 采集节拍：每 5 帧 1 单位（整数确定性）
                    if (++u.mine_clock >= 5) {
                        u.mine_clock = 0;
                        ore[oi] = static_cast<int16_t>(ore[oi] - 1);
                        u.cargo += 1;
                        changed = true;
                    }
                } else if (ore[oi] <= 0 && stand) {
                    int tc = 0, tr = 0;
                    if (find_nearest_ore(ore, w, h, u.col, u.row, tc, tr)) {
                        if (set_move_target(u, tc, tr)) changed = true;
                    } // 无矿可采：原地驻停
                }
            }
        } else if (u.order == kOrderAttackUnit || u.order == kOrderGuard) {
            const bool attacking = u.order == kOrderAttackUnit;
            const bool target_ok =
                u.target >= 0 && u.target < static_cast<int>(units.size()) &&
                units[static_cast<size_t>(u.target)].alive;
            if (!target_ok) {
                u.order = kOrderNone;
                u.path.clear();
                continue;
            }
            const SimUnit& t = units[static_cast<size_t>(u.target)];
            if (manhattan(u.col, u.row, t.col, t.row) <= u.weapon.range) {
                // 进射程：撤销走向目标格的最后一步并驻停
                if (u.next_col == t.col && u.next_row == t.row) {
                    u.next_col = u.col;
                    u.next_row = u.row;
                    u.frac = 0;
                }
                u.path.clear();
                if (stand) {
                    // 转向交给炮塔（TurretROT/ROT），车身保持行进朝向
                    u.turret_dir = static_cast<uint8_t>(
                        dir_toward(u.col, u.row, t.col, t.row) * 32);
                    if (attacking && u.cooldown == 0) {
                        units[static_cast<size_t>(u.target)].hp -= u.weapon.damage;
                        u.cooldown = u.weapon.rof;
                        changed = true;
                    }
                    continue; // 驻停开火
                }
            } else if (stand) {
                // 仅在驻停时（重）寻路（目标移动则下段修正；段中不重置进度）
                if (!set_move_target(u, t.col, t.row)) {
                    u.order = kOrderNone; // 目标不可达 → 放弃
                    continue;
                }
                changed = true;
            }
        } else if (u.order == kOrderAttackBuilding) {
            const bool target_ok =
                u.target >= 0 && u.target < static_cast<int>(buildings.size()) &&
                buildings[static_cast<size_t>(u.target)].alive;
            if (!target_ok) {
                u.order = kOrderNone;
                u.path.clear();
                continue;
            }
            const SimBuilding& b = buildings[static_cast<size_t>(u.target)];
            int tc = b.col, tr = b.row;
            const int d = dist_to_building(b, u.col, u.row, &tc, &tr);
            if (d <= u.weapon.range) {
                // 进射程：撤销走向地基格的最后一步并驻停
                if (dist_to_building(b, u.next_col, u.next_row) == 0) {
                    u.next_col = u.col;
                    u.next_row = u.row;
                    u.frac = 0;
                }
                u.path.clear();
                if (stand) {
                    // 转向交给炮塔（车身保持行进朝向）
                    u.turret_dir = static_cast<uint8_t>(
                        dir_toward(u.col, u.row, tc, tr) * 32);
                    if (u.cooldown == 0) {
                        buildings[static_cast<size_t>(u.target)].hp -= u.weapon.damage;
                        u.cooldown = u.weapon.rof;
                        changed = true;
                    }
                    continue;
                }
            } else if (stand) {
                if (!set_move_target(u, tc, tr)) {
                    u.order = kOrderNone;
                    continue;
                }
                changed = true;
            }
        } else if (u.order == kOrderPatrol) {
            if (u.waypoints.empty()) {
                u.order = kOrderNone;
            } else {
                const auto& wp = u.waypoints[u.wp_idx];
                if (u.col == wp.first && u.row == wp.second && stand) {
                    u.wp_idx = (u.wp_idx + 1) % u.waypoints.size();
                    if (set_move_target(u, u.waypoints[u.wp_idx].first,
                                        u.waypoints[u.wp_idx].second))
                        changed = true;
                } else if (stand) {
                    if (set_move_target(u, wp.first, wp.second)) changed = true;
                }
            }
        }
        const bool turned = update_motion(u);
        if (turned) changed = true;
        if (advance_segment(u)) changed = true;
    }
    // ── 步兵 idle 动作调度（原版 IdleActionFrequency 语义）──
    // 静止且无指令时，按 0.5~2× 均值间隔随机播放 Idle1/Idle2（sim 只发布触发，
    // 动画长度由渲染层按 artmd 序列帧数截断）；确定性：每单位独立 LCG 流。
    for (size_t i = 0; i < units.size(); ++i) {
        SimUnit& u = units[i];
        if (u.kind != 2 || !u.alive) continue;
        if (unit_moving(i)) {
            if (u.idle_kind) { // 开始移动：打断 idle 动作，停下后重掷等待
                u.idle_kind = 0;
                u.idle_wait = -1;
                changed = true;
            }
            continue;
        }
        if (u.idle_kind) {
            if (logic_ticks >= u.idle_start + static_cast<uint32_t>(kIdleAnimBusyTicks)) {
                u.idle_kind = 0;
                u.idle_wait = idle_wait_ticks(u, idle_freq_ticks);
                changed = true;
            }
            continue;
        }
        if (idle_freq_ticks <= 0) continue; // rules 关闭 idle 动作
        if (u.idle_wait < 0) u.idle_wait = idle_wait_ticks(u, idle_freq_ticks);
        if (u.idle_wait > 0) {
            --u.idle_wait;
            continue;
        }
        u.idle_kind = (unit_rand(u) & 1u) ? 1 : 2;
        u.idle_start = static_cast<uint32_t>(logic_ticks);
        changed = true;
    }
    // ── 建造进度（固定序遍历；完成补满血）──
    for (auto& b : buildings) {
        if (b.alive && b.under_construction) {
            if (++b.build_ticks >= b.build_total) {
                b.under_construction = false;
                b.hp = b.max_hp;
                changed = true;
            }
        }
        // 配件动画时钟（油井摇臂/工厂门等；1 帧/逻辑帧，15fps 原版节拍）
        if (b.alive && b.has_anim) {
            ++b.anim_clock;
            changed = true;
        }
    }
    // ── 电力净值（产 − 耗；建造中不供电）+ 低电惩罚 ──
    // RA2 语义：净电力 < 0 时该 House 生产减速（此处取建造推进 ×1/2，
    // 奇数帧不推进，确定性）；低电影响武器 ROF/雷达留 M5。
    power_net.clear();
    for (const auto& b : buildings) {
        if (b.alive && !b.under_construction) power_net[b.owner] += b.power;
    }
    ++logic_ticks;
    // ── 建造队列推进（遭遇战：排队 → 进度 → 待放置；低电 ×1/2）──
    for (auto& [owner, item] : build_queue) {
        (void)owner;
        if (item.ready) continue;
        const auto pit = power_net.find(owner);
        const bool low_power = pit != power_net.end() && pit->second < 0;
        if (low_power && (logic_ticks & 1)) continue; // 低电：隔帧推进
        if (++item.ticks >= item.total) {
            item.ready = true;
            changed = true;
        }
    }
    // ── 修理推进（M4）：回血 + 等比扣款；满血自动停 ──
    // 修理经济：RepairRate = Cost 的 2%/秒（rulesmd [General] 缺省档），
    // 即回满全程花费 ≈ Cost·(1−hp/max)/1；每帧回血 repair_step_hp
    // （stage 按 max_hp/总修理帧换算），扣款 = Cost/max_hp·step（亚元累计由
    // 整数除法天然截断，确定性）。低电时修理同样减半（RA2 原版语义）。
    for (auto& b : buildings) {
        if (!b.alive || !b.repairing || b.under_construction) continue;
        if (b.hp >= b.max_hp) {
            b.repairing = false;
            continue;
        }
        const auto pit = power_net.find(b.owner);
        const bool low_power = pit != power_net.end() && pit->second < 0;
        if (low_power && (logic_ticks & 1)) continue;
        b.hp = std::min(b.max_hp, b.hp + b.repair_step_hp);
        if (b.max_hp > 0 && b.cost > 0) {
            const int64_t step_cost =
                (static_cast<int64_t>(b.cost) * b.repair_step_hp + b.max_hp - 1) / b.max_hp;
            auto cit = credits.find(b.owner);
            if (cit != credits.end() && cit->second >= step_cost) {
                cit->second -= step_cost;
            } else if (cit != credits.end()) {
                cit->second = 0;
                b.repairing = false; // 资金耗尽停修
            }
        }
        changed = true;
    }
    // ── 防御建筑攻击（Primary= 有武器者）：手动目标 + 自动索敌 + 炮塔转向开火 ──
    // 目标失效即清；无目标每 15 帧扫射程内最近敌单位（确定性固定序遍历）；
    // 炮塔每帧最多转 16/256 圈（≈16°），大致对准（±16）且射程内才开火；
    // 开火 = 扣血 + ROF 冷却（单位死亡沿用后面的死亡整批结算）。
    for (auto& b : buildings) {
        if (!b.alive || b.weapon.damage <= 0 || b.under_construction) continue;
        if (b.cooldown > 0) --b.cooldown;
        const auto target_alive = [&]() -> bool {
            if (b.target < 0 || b.target >= static_cast<int>(units.size())) return false;
            const SimUnit& t = units[static_cast<size_t>(b.target)];
            return t.alive && t.owner != b.owner;
        };
        if (!target_alive()) {
            b.target = -1;
            // 自动索敌：非玩家 House（地图上的 Neutral/Special 陈设）不自动开火
            const bool neutral_house = b.owner == "Neutral" || b.owner == "Special";
            if (!neutral_house && --b.acquire_clock <= 0) {
                b.acquire_clock = 15;
                int best = -1, best_d = INT_MAX;
                for (size_t i = 0; i < units.size(); ++i) {
                    if (!units[i].alive || units[i].owner == b.owner) continue;
                    const int d = manhattan(b.col, b.row, units[i].col, units[i].row);
                    if (d <= b.weapon.range && d < best_d) {
                        best_d = d;
                        best = static_cast<int>(i);
                    }
                }
                if (best >= 0) {
                    b.target = best;
                    changed = true;
                }
            }
        }
        if (b.target < 0) continue;
        const SimUnit& t = units[static_cast<size_t>(b.target)];
        const int want = static_cast<int>(dir_toward(b.col, b.row, t.col, t.row)) * 32;
        int diff = want - static_cast<int>(b.turret_dir);
        while (diff > 128) diff -= 256;
        while (diff < -128) diff += 256;
        if (diff != 0) {
            const int step = std::clamp(diff, -16, 16);
            b.turret_dir = static_cast<uint8_t>((static_cast<int>(b.turret_dir) + step) & 255);
            changed = true;
        }
        const bool in_range = manhattan(b.col, b.row, t.col, t.row) <= b.weapon.range;
        if (in_range && std::abs(diff) <= 16 && b.cooldown == 0) {
            units[static_cast<size_t>(b.target)].hp -= b.weapon.damage;
            b.cooldown = b.weapon.rof > 0 ? b.weapon.rof : 1;
            changed = true;
        }
    }
    // ── 死亡结算（固定序：先标记，后整批移除并重映射目标下标）──
    bool any_death = false;
    for (auto& u : units) {
        if (u.alive && u.hp <= 0) {
            u.alive = false;
            explosions.push_back({u.col, u.row, 30, 0});
            any_death = true;
        }
    }
    for (auto& b : buildings) {
        if (b.alive && b.hp <= 0) {
            b.alive = false;
            if (!b.sold) explosions.push_back({b.col, b.row, 45, 0}); // 出售无爆炸
            any_death = true;
        } else if (!b.alive) {
            any_death = true; // 出售等已置 dead 的也要进清扫（解除阻挡）
        }
    }
    if (any_death) {
        std::vector<SimUnit> nu;
        nu.reserve(units.size());
        std::vector<int> remap_u(units.size(), -1);
        for (size_t i = 0; i < units.size(); ++i) {
            if (units[i].alive) {
                remap_u[i] = static_cast<int>(nu.size());
                nu.push_back(std::move(units[i]));
            }
        }
        units = std::move(nu);
        std::vector<SimBuilding> nb;
        nb.reserve(buildings.size());
        std::vector<int> remap_b(buildings.size(), -1);
        for (size_t i = 0; i < buildings.size(); ++i) {
            if (buildings[i].alive) {
                remap_b[i] = static_cast<int>(nb.size());
                nb.push_back(std::move(buildings[i]));
            } else {
                // 解除地基阻挡（footprint_cells 对地图装载/现场建造统一有效）
                for (const auto& c : buildings[i].footprint_cells) {
                    if (c.first >= 0 && c.second >= 0 && c.first < w && c.second < h)
                        blocked[static_cast<size_t>(c.second) * w + c.first] = 0;
                }
                note_nav_change(); // 地基解阻 → 路网变化
            }
        }
        buildings = std::move(nb);
        for (auto& u : units) {
            if ((u.order == kOrderAttackUnit || u.order == kOrderGuard) && u.target >= 0 &&
                u.target < static_cast<int>(remap_u.size())) {
                u.target = remap_u[static_cast<size_t>(u.target)];
                if (u.target < 0) {
                    u.order = kOrderNone;
                    u.path.clear();
                }
            }
            if (u.order == kOrderAttackBuilding && u.target >= 0 &&
                u.target < static_cast<int>(remap_b.size())) {
                u.target = remap_b[static_cast<size_t>(u.target)];
                if (u.target < 0) {
                    u.order = kOrderNone;
                    u.path.clear();
                }
            }
        }
        changed = true;
    }
    // 爆炸计时
    for (auto& e : explosions) ++e.elapsed;
    if (!explosions.empty()) {
        explosions.erase(std::remove_if(explosions.begin(), explosions.end(),
                                        [](const SimExplosion& e) {
                                            return e.elapsed >= e.total;
                                        }),
                         explosions.end());
    }
    return changed;
}

bool SimWorld::issue_move(size_t unit_idx, int tc, int tr) {
    if (unit_idx >= units.size()) return false;
    SimUnit& u = units[unit_idx];
    u.order = kOrderMove;
    u.target = -1;
    if (u.col == tc && u.row == tr) {
        u.path.clear();
        u.next_col = u.col;
        u.next_row = u.row;
        u.frac = 0;
        return true;
    }
    return set_move_target(u, tc, tr);
}

bool SimWorld::issue_attack_unit(size_t unit_idx, size_t target_idx) {
    if (unit_idx >= units.size() || target_idx >= units.size()) return false;
    SimUnit& u = units[unit_idx];
    u.order = kOrderAttackUnit;
    u.target = static_cast<int>(target_idx);
    const SimUnit& t = units[target_idx];
    if (manhattan(u.col, u.row, t.col, t.row) <= u.weapon.range) {
        u.path.clear();
        u.next_col = u.col;
        u.next_row = u.row;
        u.frac = 0;
        return true;
    }
    if (!set_move_target(u, t.col, t.row)) {
        u.order = kOrderNone;
        return false;
    }
    return true;
}

bool SimWorld::issue_attack_building(size_t unit_idx, size_t building_idx) {
    if (unit_idx >= units.size() || building_idx >= buildings.size()) return false;
    SimUnit& u = units[unit_idx];
    u.order = kOrderAttackBuilding;
    u.target = static_cast<int>(building_idx);
    const SimBuilding& b = buildings[building_idx];
    if (manhattan(u.col, u.row, b.col, b.row) <= u.weapon.range) {
        u.path.clear();
        u.next_col = u.col;
        u.next_row = u.row;
        u.frac = 0;
        return true;
    }
    if (!set_move_target(u, b.col, b.row)) {
        u.order = kOrderNone;
        return false;
    }
    return true;
}

bool SimWorld::issue_guard(size_t unit_idx, size_t target_idx) {
    if (unit_idx >= units.size() || target_idx >= units.size()) return false;
    SimUnit& u = units[unit_idx];
    u.order = kOrderGuard;
    u.target = static_cast<int>(target_idx);
    const SimUnit& t = units[target_idx];
    if (manhattan(u.col, u.row, t.col, t.row) <= u.weapon.range) {
        u.path.clear();
        u.next_col = u.col;
        u.next_row = u.row;
        u.frac = 0;
        return true;
    }
    if (!set_move_target(u, t.col, t.row)) {
        u.order = kOrderNone;
        return false;
    }
    return true;
}

// 停止（S 键）：清指令/目标/路径，原地驻停（保留采矿车自动采矿的 kOrderHarvest
// 语义：停采矿车 = 清路径，下一 tick 会重新回到采矿循环）
bool SimWorld::stop_unit(size_t unit_idx) {
    if (unit_idx >= units.size() || !units[unit_idx].alive) return false;
    SimUnit& u = units[unit_idx];
    u.order = kOrderNone;
    u.target = -1;
    u.waypoints.clear();
    u.wp_idx = 0;
    u.path.clear();
    u.next_col = u.col;
    u.next_row = u.row;
    u.frac = 0;
    return true;
}

void SimWorld::add_waypoint(size_t unit_idx, int tc, int tr) {    if (unit_idx >= units.size()) return;
    SimUnit& u = units[unit_idx];
    u.waypoints.push_back({tc, tr});
    if (u.order != kOrderPatrol) {
        u.order = kOrderPatrol;
        u.target = -1;
        u.wp_idx = 0;
        if (u.col != tc || u.row != tr) set_move_target(u, tc, tr);
    }
}

void SimWorld::set_unit_owner(size_t idx, const std::string& owner) {
    if (idx >= units.size()) return;
    units[idx].owner = owner;
    if (!credits.count(owner)) credits[owner] = 10000;
}

bool SimWorld::issue_build(const std::string& owner, const std::string& type, int col, int row,
                           int fw, int fh, int cost, int build_total, int power) {
    if (!can_place(col, row, fw, fh)) return false;
    auto it = credits.find(owner);
    if (it == credits.end() || it->second < cost) return false;
    it->second -= cost; // 立即扣款
    return spawn_building(owner, type, col, row, fw, fh, cost, power, true, build_total, 256) != 0;
}

// ── 遭遇战流程（M4）──

void SimWorld::cell_to_map(int col, int row, int& rx, int& ry) const {
    const int sum = row + min_s;              // rx + ry
    const int e = (row + min_s - min_d) & 1;  // rx - ry - min_d 的奇偶位
    const int diff = 2 * col + min_d + e;     // rx - ry
    rx = (sum + diff) / 2;
    ry = (sum - diff) / 2;
}

void SimWorld::map_to_cell(int rx, int ry, int& col, int& row) const {
    row = rx + ry - min_s;
    col = floor_div2(rx - ry - min_d);
}

void SimWorld::foundation_cells(int col, int row, int fw, int fh,
                                std::vector<std::pair<int, int>>& out) const {
    out.clear();
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    // 地图空间 (rx+i, ry+j) → 引擎格：行 = row+i+j，列 = col + floor((e+i-j)/2)
    const int e = (row + min_s - min_d) & 1;
    out.reserve(static_cast<size_t>(fw) * fh);
    for (int j = 0; j < fh; ++j)
        for (int i = 0; i < fw; ++i)
            out.emplace_back(col + floor_div2(e + i - j), row + i + j);
}

// 格占用：一格 = 1 载具 或 最多 3 个步兵（子格 0..2）。返回可用子格号（-1 = 满）。
// 步兵计数含地图装载的单位（其 subcell 可能为 3..4 的原版五格位，照常计数）。
int SimWorld::free_subcell(int col, int row, int kind, uint32_t ignore_unit_id) const {
    if (col < 0 || row < 0 || col >= w || row >= h) return -1;
    int infantry = 0;
    bool slot_used[3] = {false, false, false};
    for (const auto& u : units) {
        if (!u.alive || (ignore_unit_id && u.id == ignore_unit_id)) continue;
        if (u.col != col || u.row != row) continue;
        if (u.kind != 2 || kind != 2) return -1; // 载具占满整格（含与步兵互斥）
        ++infantry;
        if (u.subcell < 3) slot_used[u.subcell] = true;
    }
    if (kind != 2) return 0; // 载具：格内无任何单位 → 占用整格
    if (infantry >= 3) return -1;
    for (int s = 0; s < 3; ++s)
        if (!slot_used[s]) return s;
    return -1;
}

bool SimWorld::cell_buildable(int col, int row, uint32_t ignore_unit_id) const {
    if (col < 0 || row < 0 || col >= w || row >= h) return false;
    const size_t i = static_cast<size_t>(row) * w + col;
    if (i < blocked.size() && blocked[i]) return false; // 地形/建筑地基
    if (i < no_build.size() && no_build[i]) return false; // 覆盖物（桥梁/围墙等）
    if (i < ore.size() && ore[i] > 0) return false;       // 矿石（采完后可建）
    for (const auto& u : units) {
        if (!u.alive || (ignore_unit_id && u.id == ignore_unit_id)) continue;
        if (u.col == col && u.row == row) return false; // 单位占格
    }
    return true;
}

bool SimWorld::can_place(int col, int row, int fw, int fh, uint32_t ignore_unit_id) const {
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    const int e = (row + min_s - min_d) & 1;
    for (int j = 0; j < fh; ++j)
        for (int i = 0; i < fw; ++i) {
            const int x = col + floor_div2(e + i - j);
            const int y = row + i + j;
            if (!cell_buildable(x, y, ignore_unit_id)) return false;
        }
    return true;
}

uint32_t SimWorld::spawn_unit(const std::string& owner, const std::string& type, int kind,
                              int col, int row, uint8_t dir, const SimWeapon& weapon, bool is_miner,
                              int capacity, int speed, const UnitMotion& motion) {
    if (kind < 1) kind = 1;
    const int sc = free_subcell(col, row, kind); // 一格 1 载具 或 ≤3 步兵
    if (sc < 0) return 0;                        // 该格已满 → 生成失败
    SimUnit u;
    u.id = next_id++;
    u.idle_rng = u.id * 1664525u + 12345u; // 步兵 idle 动作随机流播种
    u.owner = owner;
    u.type = type;
    u.kind = kind;
    u.col = u.next_col = col;
    u.row = u.next_row = row;
    u.subcell = static_cast<uint8_t>(sc);
    u.dir = dir;
    u.turret_dir = dir;
    u.weapon = weapon;
    u.is_miner = is_miner;
    u.capacity = capacity > 0 ? capacity : 20;
    u.speed = motion.max_speed > 0 ? motion.max_speed
                                     : (speed > 0 ? speed : (kind == 2 ? 51 : 68));
    u.rot_step = motion.rot_step;
    u.turret_rot = motion.turret_rot > 0 ? motion.turret_rot : motion.rot_step;
    u.accel_step = motion.accel_step;
    u.decel_step = motion.decel_step;
    u.vel = u.accel_step > 0 ? 0 : u.speed; // 加速型从静止起步，否则等速
    if (!credits.count(owner)) credits[owner] = 10000;
    units.push_back(std::move(u));
    return units.back().id;
}

bool SimWorld::remove_unit(size_t idx) {
    if (idx >= units.size()) return false;
    units.erase(units.begin() + static_cast<std::ptrdiff_t>(idx));
    // 目标下标重映射（与死亡结算同规则：指向被移除单位的指令清空）
    for (auto& u : units) {
        if (u.target < 0) continue;
        if (u.order == kOrderAttackUnit || u.order == kOrderGuard) {
            if (u.target == static_cast<int>(idx)) {
                u.order = kOrderNone;
                u.target = -1;
                u.path.clear();
            } else if (u.target > static_cast<int>(idx)) {
                --u.target;
            }
        }
    }
    return true;
}

uint32_t SimWorld::spawn_building(const std::string& owner, const std::string& type, int col,
                                  int row, int fw, int fh, int cost, int power,
                                  bool under_construction, int build_total, int max_hp,
                                  uint32_t ignore_unit_id) {
    if (!can_place(col, row, fw, fh, ignore_unit_id)) return 0;
    SimBuilding b;
    b.id = next_id++;
    b.owner = owner;
    b.type = type;
    b.col = col;
    b.row = row;
    b.fw = fw;
    b.fh = fh;
    cell_to_map(col, row, b.rx, b.ry); // 地图空间锚（顶格）
    b.cost = cost;
    b.power = power;
    b.max_hp = max_hp > 0 ? max_hp : 256;
    b.under_construction = under_construction;
    b.build_total = build_total > 0 ? build_total : 1;
    b.build_ticks = 0;
    b.hp = under_construction ? 1 : b.max_hp;
    foundation_cells(col, row, fw, fh, b.footprint_cells);
    for (const auto& c : b.footprint_cells)
        blocked[static_cast<size_t>(c.second) * w + c.first] = 1;
    note_nav_change(); // 新地基阻挡 → 路网变化
    if (!credits.count(owner)) credits[owner] = 10000;
    buildings.push_back(std::move(b));
    return buildings.back().id;
}

bool SimWorld::queue_build(const std::string& owner, const std::string& type, int cost,
                           int total_ticks) {
    if (build_queue.count(owner)) return false; // 单队列：已有在建项
    auto it = credits.find(owner);
    if (it == credits.end() || it->second < cost) return false;
    it->second -= cost; // RA2 在排队时扣款
    BuildQueueItem item;
    item.type = type;
    item.cost = cost;
    item.total = total_ticks > 0 ? total_ticks : 1;
    build_queue[owner] = std::move(item);
    return true;
}

bool SimWorld::cancel_build(const std::string& owner, int refund_percent) {
    const auto it = build_queue.find(owner);
    if (it == build_queue.end()) return false;
    const int64_t refund = static_cast<int64_t>(it->second.cost) * refund_percent / 100;
    credits[owner] += refund;
    build_queue.erase(it);
    return true;
}

bool SimWorld::toggle_repair(size_t building_idx) {
    if (building_idx >= buildings.size()) return false;
    SimBuilding& b = buildings[building_idx];
    if (!b.alive || b.under_construction || b.hp >= b.max_hp) return false;
    b.repairing = !b.repairing;
    // 步长 ≈ 满修 300 逻辑帧（15fps ≈ 20s，接近原版修理节奏）
    b.repair_step_hp = std::max(1, b.max_hp / 300);
    return b.repairing;
}

int64_t SimWorld::sell_building(size_t building_idx, int refund_percent) {
    if (building_idx >= buildings.size()) return -1;
    SimBuilding& b = buildings[building_idx];
    if (!b.alive || b.under_construction) return -1;
    // 退款 = Cost × RefundPercent% × 残血比（原版打折语义的近似：卖伤楼折价）
    const int64_t hp_frac = b.max_hp > 0 ? (b.hp * 100 / b.max_hp) : 0;
    const int64_t refund =
        static_cast<int64_t>(b.cost) * refund_percent / 100 * hp_frac / 100;
    credits[b.owner] += refund;
    b.alive = false; // 死亡结算统一解除阻挡并整批移除
    b.sold = true;   // 静默移除（无爆炸）
    b.repairing = false;
    return refund;
}

bool SimWorld::build_ready(const std::string& owner) const {
    const auto it = build_queue.find(owner);
    return it != build_queue.end() && it->second.ready;
}

// ── 建筑配置 / 防御攻击（M4）──

bool SimWorld::configure_building(uint32_t id, int dir, const SimWeapon& weapon,
                                  bool is_refinery) {
    for (auto& b : buildings) {
        if (b.id != id) continue;
        b.dir = static_cast<uint8_t>(dir & 255);
        b.turret_dir = b.dir;
        b.weapon = weapon;
        b.is_refinery = is_refinery;
        return true;
    }
    return false;
}

bool SimWorld::issue_build_attack(size_t building_idx, size_t unit_idx) {
    if (building_idx >= buildings.size() || unit_idx >= units.size()) return false;
    SimBuilding& b = buildings[building_idx];
    const SimUnit& t = units[unit_idx];
    if (!b.alive || b.weapon.damage <= 0) return false;
    if (!t.alive || t.owner == b.owner) return false; // 只能打敌方
    b.target = static_cast<int>(unit_idx);
    return true;
}

bool SimWorld::stop_build_attack(size_t building_idx) {
    if (building_idx >= buildings.size()) return false;
    buildings[building_idx].target = -1;
    buildings[building_idx].acquire_clock = 0;
    return true;
}

bool SimWorld::take_ready_build(const std::string& owner, std::string* type) {
    const auto it = build_queue.find(owner);
    if (it == build_queue.end() || !it->second.ready) return false;
    if (type) *type = it->second.type;
    build_queue.erase(it);
    return true;
}

bool SimWorld::has_building(const std::string& owner, const std::string& type,
                            bool completed_only) const {
    for (const auto& b : buildings) {
        if (!b.alive || b.owner != owner) continue;
        if (completed_only && b.under_construction) continue;
        if (b.type == type) return true;
    }
    return false;
}

uint64_t SimWorld::visual_hash() const {
    uint64_t hash = 1469598103934665603ull; // FNV-1a
    const auto mix = [&hash](uint64_t v) {
        hash ^= v;
        hash *= 1099511628211ull;
    };
    for (const auto& u : units) {
        mix(u.id);
        mix((static_cast<uint64_t>(u.col + 4096) << 32) |
            static_cast<uint64_t>(u.row + 4096));
        mix((static_cast<uint64_t>(u.next_col + 4096) << 32) |
            static_cast<uint64_t>(u.next_row + 4096));
        mix(u.frac);
        mix(u.dir);
        mix(u.turret_dir);
        mix(u.subcell);
        mix((static_cast<uint64_t>(u.next2_col + 4096) << 32) |
            static_cast<uint64_t>(u.next2_row + 4096));
        mix(u.hp);
        mix(u.alive ? 1ull : 0ull);
        mix(u.order);
        mix(u.cooldown);
        // 步兵 idle 动作：相位每 3 帧一档（触发/结束/帧推进都需重绘）
        mix(u.idle_kind ? static_cast<uint64_t>(u.idle_kind) * 65536u +
                              (logic_ticks - u.idle_start) / 3 + 1
                        : 0ull);
    }
    for (const auto& b : buildings) {
        mix(b.id);
        mix(b.hp);
        mix(b.alive ? 1ull : 0ull);
        mix(b.under_construction ? 1ull : 0ull);
        mix(b.build_ticks);
        if (b.has_anim) mix(b.anim_clock); // 配件动画帧变化触发重绘
        if (b.weapon.damage > 0) {        // 防御建筑炮塔转向/开火需重绘
            mix(b.turret_dir);
            mix(b.target < 0 ? 0ull : 1ull);
            mix(b.cooldown);
        }
    }
    mix(explosions.size());
    for (const auto& e : explosions) mix(static_cast<uint64_t>(e.elapsed));
    // 建造队列（进度条/就绪提示需触发重绘）
    for (const auto& [owner, item] : build_queue) {
        (void)owner;
        mix(item.ticks);
        mix(item.ready ? 1ull : 0ull);
    }
    return hash;
}

} // namespace ra2r::sim
