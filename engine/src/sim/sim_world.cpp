// RA2R — M3 模拟层实现（接口见 sim_world.h）
#include "ra2r/sim/sim_world.h"

#include <algorithm>
#include <cstdlib>

#include "ra2r/sim/pathfind.h"

namespace ra2r::sim {

namespace {
// 段方向 → 朝向索引（0=NE..7=NW，渲染侧 ×32 得 dir 字节）。
// 列步 = 屏幕水平（E/W）；行步的屏幕斜向由起点行奇偶决定（砖墙 30px 错位）：
//   行奇 → 下行是 SW、上行是 NE；行偶 → 下行是 SE、上行是 NW。
uint8_t dir_of(int c, int r, int nc, int nr) {
    if (nc > c) return 1; // E
    if (nc < c) return 5; // W
    if (nr > r) return (r & 1) ? 4 : 2;  // SW / SE
    return (r & 1) ? 0 : 7;              // NE / NW
}

// 朝目标格的 8 向朝向（任意距离）：取与屏幕向量点积最大的规范方向
// （纯整数、确定性；dir 0..7 规范向量 = 各朝向的屏幕单位向量）
uint8_t dir_toward(int c, int r, int tc, int tr) {
    const int dx = 60 * (tc - c) + 30 * ((tr & 1) - (r & 1));
    const int dy = 15 * (tr - r);
    static const int kDirV[8][2] = {{30, -15}, {60, 0},  {30, 15},  {0, 30},
                                    {-30, 15}, {-60, 0}, {-30, -15}, {0, -30}};
    int best = 0;
    long long best_dot = INT64_MIN;
    for (int k = 0; k < 8; ++k) {
        const long long dot = static_cast<long long>(dx) * kDirV[k][0] +
                              static_cast<long long>(dy) * kDirV[k][1];
        if (dot > best_dot) {
            best_dot = dot;
            best = k;
        }
    }
    return static_cast<uint8_t>(best);
}

int manhattan(int c, int r, int tc, int tr) { return std::abs(c - tc) + std::abs(r - tr); }

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
                        const std::function<bool(int, int)>& terrain_block) {
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
        su.dir = u.dir / 32;
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
        su.dir = n.dir / 32;
        su.hp = n.health;
        su.speed = 51; // 步兵 ≈3 格/秒
        weapon(n.id, su.weapon);
        miner(n.id, su.is_miner, su.capacity);
        seed_credits(n.owner);
        units.push_back(std::move(su));
    }
    return !units.empty() || !buildings.empty();
}

// 段推进（到达落格 + 余量进下一段；所有订单共用）
bool SimWorld::advance_segment(SimUnit& u) {
    bool changed = false;
    while (u.frac >= kFracMax) {
        u.col = u.next_col;
        u.row = u.next_row;
        u.frac -= kFracMax;
        changed = true;
        if (u.path.empty()) {
            u.next_col = u.col;
            u.next_row = u.row;
            if (u.order == kOrderMove) u.order = kOrderNone; // 移动完成 → 空闲（采矿车恢复自动采集）
            break;
        }
        u.next_col = u.path.front().first;
        u.next_row = u.path.front().second;
        u.path.erase(u.path.begin());
        u.dir = dir_of(u.col, u.row, u.next_col, u.next_row);
    }
    if (u.next_col == u.col && u.next_row == u.row) {
        u.frac = 0;
        return changed;
    }
    // 行步（地图轴步，33.5px）基准速率；列步（地图对角步，60px）按
    // 33.5/60 ≈ 143/256 折算，保持恒定屏幕速率
    const int inc = (u.next_col != u.col) ? (u.speed * 143) / 256 : u.speed;
    u.frac += inc;
    return true;
}

// 朝目标格寻路（失败 → 清路径并驻停，返回是否可达）
bool SimWorld::set_move_target(SimUnit& u, int tc, int tr) {
    std::vector<std::pair<int, int>> path = find_path(blocked, w, h, u.col, u.row, tc, tr);
    if (path.empty()) {
        u.path.clear();
        u.next_col = u.col;
        u.next_row = u.row;
        u.frac = 0;
        return false;
    }
    u.path = std::move(path);
    u.next_col = u.path.front().first;
    u.next_row = u.path.front().second;
    u.path.erase(u.path.begin());
    u.frac = 0;
    u.dir = dir_of(u.col, u.row, u.next_col, u.next_row);
    return true;
}

// 目标格被阻挡（建筑自身地基）时，在 8 邻格中选路径最短的可达格（确定性）
bool SimWorld::set_move_target_near(SimUnit& u, int tc, int tr) {
    const bool blocked_target = tc < 0 || tr < 0 || tc >= w || tr >= h ||
                                blocked[static_cast<size_t>(tr) * w + tc] != 0;
    if (!blocked_target) return set_move_target(u, tc, tr);
    static const int kN[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    std::vector<std::pair<int, int>> best;
    for (const auto& d : kN) {
        const int nc = tc + d[0], nr = tr + d[1];
        if (nc < 0 || nr < 0 || nc >= w || nr >= h) continue;
        if (blocked[static_cast<size_t>(nr) * w + nc]) continue;
        std::vector<std::pair<int, int>> path =
            find_path(blocked, w, h, u.col, u.row, nc, nr);
        if (path.empty()) continue;
        if (best.empty() || path.size() < best.size()) best = std::move(path);
    }
    if (best.empty()) {
        u.path.clear();
        u.next_col = u.col;
        u.next_row = u.row;
        u.frac = 0;
        return false;
    }
    u.path = std::move(best);
    u.next_col = u.path.front().first;
    u.next_row = u.path.front().second;
    u.path.erase(u.path.begin());
    u.frac = 0;
    u.dir = dir_of(u.col, u.row, u.next_col, u.next_row);
    return true;
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
                    if (!set_move_target_near(u, tc, tr)) {
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
                    u.dir = dir_toward(u.col, u.row, t.col, t.row);
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
                    u.dir = dir_toward(u.col, u.row, tc, tr);
                    if (u.cooldown == 0) {
                        buildings[static_cast<size_t>(u.target)].hp -= u.weapon.damage;
                        u.cooldown = u.weapon.rof;
                        changed = true;
                    }
                    continue;
                }
            } else if (stand) {
                if (!set_move_target_near(u, tc, tr)) {
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
        if (advance_segment(u)) changed = true;
    }
    // ── 建造进度（固定序遍历；完成补满血）──
    for (auto& b : buildings) {
        if (b.alive && b.under_construction) {
            if (++b.build_ticks >= b.build_total) {
                b.under_construction = false;
                b.hp = 256;
                changed = true;
            }
        }
        // 配件动画时钟（油井摇臂/工厂门等；1 帧/逻辑帧，15fps 原版节拍）
        if (b.alive && b.has_anim) {
            ++b.anim_clock;
            changed = true;
        }
    }
    // ── 电力净值（产 − 耗；建造中不供电；低电惩罚 M4）──
    power_net.clear();
    for (const auto& b : buildings) {
        if (b.alive && !b.under_construction) power_net[b.owner] += b.power;
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
            explosions.push_back({b.col, b.row, 45, 0});
            any_death = true;
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
                // 解除地基阻挡
                for (int j = 0; j < buildings[i].fh; ++j) {
                    for (int k = 0; k < buildings[i].fw; ++k) {
                        int col = 0, row = 0;
                        if (buildings[i].rect_footprint) {
                            col = buildings[i].col + k;
                            row = buildings[i].row + j;
                        } else {
                            const int rx = buildings[i].rx + k;
                            const int ry = buildings[i].ry + j;
                            col = (rx - ry - min_d) / 2;
                            row = rx + ry - min_s;
                        }
                        if (col >= 0 && row >= 0 && col < w && row < h)
                            blocked[static_cast<size_t>(row) * w + col] = 0;
                    }
                }
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

void SimWorld::add_waypoint(size_t unit_idx, int tc, int tr) {
    if (unit_idx >= units.size()) return;
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
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    // 地基校验（引擎空间矩形近似；原版菱形地基换算待 M4）
    for (int j = 0; j < fh; ++j) {
        for (int i = 0; i < fw; ++i) {
            const int x = col + i, y = row + j;
            if (x < 0 || y < 0 || x >= w || y >= h) return false;
            if (blocked[static_cast<size_t>(y) * w + x]) return false;
        }
    }
    auto it = credits.find(owner);
    if (it == credits.end() || it->second < cost) return false;
    it->second -= cost; // 立即扣款
    SimBuilding b;
    b.id = next_id++;
    b.owner = owner;
    b.type = type;
    b.col = col;
    b.row = row;
    b.fw = fw;
    b.fh = fh;
    b.rect_footprint = true;
    b.under_construction = true;
    b.build_total = build_total > 0 ? build_total : 1;
    b.cost = cost;
    b.power = power;
    b.hp = 1; // 建造中 1 血，完成补满
    for (int j = 0; j < fh; ++j)
        for (int i = 0; i < fw; ++i) {
            blocked[static_cast<size_t>(row + j) * w + (col + i)] = 1;
            b.footprint_cells.emplace_back(col + i, row + j);
        }
    buildings.push_back(std::move(b));
    return true;
}

uint64_t SimWorld::visual_hash() const {
    uint64_t h = 1469598103934665603ull; // FNV-1a
    const auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    for (const auto& u : units) {
        mix(u.id);
        mix((static_cast<uint64_t>(u.col + 4096) << 32) |
            static_cast<uint64_t>(u.row + 4096));
        mix((static_cast<uint64_t>(u.next_col + 4096) << 32) |
            static_cast<uint64_t>(u.next_row + 4096));
        mix(u.frac);
        mix(u.dir);
        mix(u.hp);
        mix(u.alive ? 1ull : 0ull);
        mix(u.order);
        mix(u.cooldown);
    }
    for (const auto& b : buildings) {
        mix(b.id);
        mix(b.hp);
        mix(b.alive ? 1ull : 0ull);
        mix(b.under_construction ? 1ull : 0ull);
        mix(b.build_ticks);
        if (b.has_anim) mix(b.anim_clock); // 配件动画帧变化触发重绘
    }
    mix(explosions.size());
    for (const auto& e : explosions) mix(static_cast<uint64_t>(e.elapsed));
    return h;
}

} // namespace ra2r::sim
