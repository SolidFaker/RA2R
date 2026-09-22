// RA2R — 遭遇战流程实现（接口见 skirmish.h）
#include "ra2r/sim/skirmish.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ra2r::sim {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool list_has(const std::vector<std::string>& v, const std::string& name) {
    for (const auto& s : v)
        if (same_name(s, name)) return true;
    return false;
}

// 确定性 LCG（开局兵力散布；与地图种子无关，固定种子可复现）
uint32_t lcg(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 9;
}

// 按 rulesmd 类型名造一个 SimUnit（属性由 UnitFactory 从 rules 注入）
uint32_t make_unit(SimWorld& world, const std::string& house, const std::string& type, int col,
                   int row, const UnitFactory& f) {
    SimWeapon w;
    if (f.weapon) f.weapon(type, w);
    bool miner = false;
    int cap = 20;
    if (f.miner) f.miner(type, miner, cap);
    int kind = f.kind_of ? f.kind_of(type) : 1;
    if (kind != 1 && kind != 2) kind = 1;
    UnitMotion mo;
    mo.max_speed = kind == 2 ? 51 : 68;
    if (f.motion) f.motion(type, mo);
    SimWeapon sec;
    if (f.secondary) f.secondary(type, sec);
    const uint32_t id = world.spawn_unit(house, type, kind, col, row, 0, w, miner, cap,
                                        kind == 2 ? 51 : 68, mo);
    if (id != 0) { // M5.1：护甲 + 副武器（spawn_unit 只注入主武器/护甲回调）
        SimUnit& u = world.units.back();
        if (sec.damage > 0 && sec.range > 0) {
            u.weapon2 = sec;
            u.has_secondary = true;
        }
    }
    return id;
}

// 格是否可用（图内 + 无地形/建筑阻挡）
bool cell_free(const SimWorld& world, int col, int row) {
    if (col < 0 || row < 0 || col >= world.w || row >= world.h) return false;
    return world.blocked[static_cast<size_t>(row) * world.w + col] == 0;
}

// 地基锚点：以车格为中心在地图空间对齐（与 deploy_mcv 同一换算）
void deploy_anchor(const SimWorld& world, int col, int row, int fw, int fh, int& ac, int& ar) {
    int mrx = 0, mry = 0;
    world.cell_to_map(col, row, mrx, mry);
    world.map_to_cell(mrx - (fw - 1) / 2, mry - (fh - 1) / 2, ac, ar);
}

// (col,row) 是否可作为基地车格：本格可通行，且以它为中心能放下完整地基
bool deployable_at(const SimWorld& world, int col, int row, int fw, int fh) {
    if (!cell_free(world, col, row)) return false;
    int ac = 0, ar = 0;
    deploy_anchor(world, col, row, fw, fh, ac, ar);
    return world.can_place(ac, ar, fw, fh);
}

// 从出生点起按固定环序找最近可展开格（基地车落点兜底）；找不到返回 false
bool nearest_deployable(const SimWorld& world, int col, int row, int fw, int fh, int& out_col,
                        int& out_row) {
    const int maxr = std::max(world.w, world.h);
    for (int rad = 0; rad <= maxr; ++rad) {
        for (int dy = -rad; dy <= rad; ++dy)
            for (int dx = -rad; dx <= rad; ++dx) {
                if (rad > 0 && std::abs(dx) != rad && std::abs(dy) != rad) continue;
                if (deployable_at(world, col + dx, row + dy, fw, fh)) {
                    out_col = col + dx;
                    out_row = row + dy;
                    return true;
                }
            }
    }
    return false;
}

// 玩家是否拥有任一建造厂（rulesmd ConstructionYard=yes 的建成建筑）
bool has_conyard(const SimWorld& world, const assets::RulesDB& rules, const std::string& owner) {
    for (const auto& b : world.buildings) {
        if (!b.alive || b.under_construction || b.owner != owner) continue;
        const auto* t = rules.unit(b.type);
        if (t && t->construction_yard) return true;
    }
    return false;
}

} // namespace

// ── OpenRA MPStartUnits 表（mods/yr/rules/world.yaml MPStartUnits@*；单位名 = rulesmd 类型名）──
// 三阵营（[Sides] GDI/Nod/ThirdSide）× 四档（none/light/medium/heavy），
// BaseActor = 基地车，SupportActors = 开局兵力；半径取自 Inner/OuterSupportRadius。
StartUnitPlan start_unit_plan(const assets::RulesDB& rules, const std::string& country,
                              int start_class) {
    StartUnitPlan plan;
    plan.inner_radius = 3;
    plan.outer_radius = 5;
    const auto* c = rules.country(country);
    const std::string side = c ? upper(c->side) : std::string();
    // 档位 → 支援兵力（与 OpenRA yaml 逐项一致；none = 只有基地车）
    struct Row {
        const char* base;
        const char* light;
        const char* medium;
        const char* heavy;
    };
    static const Row kRows[3] = {
        // GDI（盟军）
        {"AMCV", "DOG,E1,E1", "DOG,E1,E1,E1,MTNK,ENGINEER",
         "DOG,E1,E1,E1,E1,MTNK,MTNK,FV,ENGINEER"},
        // Nod（苏军）
        {"SMCV", "DOG,E2,E2,E2", "DOG,E2,E2,E2,E2,HTNK,ENGINEER",
         "DOG,E2,E2,E2,E2,E2,HTNK,HTNK,HTK,ENGINEER"},
        // ThirdSide（尤里）
        {"PCV", "BRUTE,INIT,INIT,INIT", "BRUTE,INIT,INIT,INIT,INIT,HTNK,ENGINEER",
         "BRUTE,INIT,INIT,INIT,INIT,INIT,HTNK,HTNK,HTK,ENGINEER"},
    };
    int row = -1;
    if (side == "GDI") row = 0;
    else if (side == "NOD") row = 1;
    else if (side == "THIRDSIDE") row = 2;
    if (row < 0) return plan;
    const Row& r = kRows[row];
    plan.base_actor = r.base;
    const char* sup = nullptr;
    switch (start_class) {
        case 0: break;
        case 1: sup = r.light; break;
        case 3: sup = r.heavy; break;
        default: sup = r.medium; break;
    }
    if (sup) {
        const char* p = sup;
        while (*p) {
            const char* q = std::strchr(p, ',');
            const size_t n = q ? static_cast<size_t>(q - p) : std::strlen(p);
            plan.support.emplace_back(p, n);
            if (!q) break;
            p = q + 1;
        }
    }
    return plan;
}

int spawn_start(SimWorld& world, const assets::RulesDB& rules, const std::string& house,
                const std::string& country, int start_class, int col, int row,
                const UnitFactory& f, uint32_t seed, int base_fw, int base_fh) {
    const StartUnitPlan plan = start_unit_plan(rules, country, start_class);
    if (plan.base_actor.empty()) return 0;
    if (!world.credits.count(house)) world.credits[house] = 10000;
    // 基地车：出生点（不可放置或无法展开时取最近可展开格）
    int bc = col, br = row;
    if (!deployable_at(world, bc, br, base_fw, base_fh) &&
        !nearest_deployable(world, col, row, base_fw, base_fh, bc, br))
        return 0;
    if (!make_unit(world, house, plan.base_actor, bc, br, f)) return 0;
    int placed = 1;
    // 支援兵力：以基地车为心的环形 [inner, outer] 内确定性散布（不与已放格重叠）
    std::vector<std::pair<int, int>> cand;
    for (int dy = -plan.outer_radius; dy <= plan.outer_radius; ++dy)
        for (int dx = -plan.outer_radius; dx <= plan.outer_radius; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 < plan.inner_radius * plan.inner_radius ||
                d2 > plan.outer_radius * plan.outer_radius)
                continue;
            const int x = bc + dx, y = br + dy;
            if (cell_free(world, x, y)) cand.emplace_back(x, y);
        }
    uint32_t rng = seed ? seed : 1u;
    for (const std::string& type : plan.support) {
        if (cand.empty()) break;
        // 随机取一个候选（确定性），取走后从表里删除
        const size_t pick = lcg(rng) % cand.size();
        const auto [x, y] = cand[pick];
        cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(pick));
        if (make_unit(world, house, type, x, y, f)) ++placed;
    }
    return placed;
}

uint32_t deploy_mcv(SimWorld& world, size_t unit_idx, const std::string& building_type, int fw,
                    int fh, int cost, int power, int buildup_frames, int max_hp) {
    if (unit_idx >= world.units.size()) return 0;
    const SimUnit u = world.units[unit_idx]; // 拷贝：spawn 后 units 可能重分配
    const std::string owner = u.owner;
    // 地基以基地车格为中心：在地图空间对齐（锚 = 中心 −(fw−1)/2, −(fh−1)/2），
    // 再换算回引擎锚格——引擎网格的"矩形"在砖墙排布里不是地基形状。
    int base_col = 0, base_row = 0;
    deploy_anchor(world, u.col, u.row, fw, fh, base_col, base_row);
    static const int kOff[9][2] = {{0, 0},  {1, 0},  {-1, 0}, {0, 1}, {0, -1},
                                   {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    for (const auto& o : kOff) {
        const int c = base_col + o[0], r = base_row + o[1];
        // 车体自身占住地基中心格之一 → 展开校验忽略自己（展开后车体移除）
        if (!world.can_place(c, r, fw, fh, u.id)) continue;
        const uint32_t id = world.spawn_building(owner, building_type, c, r, fw, fh, cost, power,
                                                true, buildup_frames, max_hp, u.id);
        if (id) {
            world.remove_unit(unit_idx);
            return id;
        }
    }
    return 0;
}

uint32_t pack_building(SimWorld& world, size_t building_idx, const std::string& unit_type,
                       const UnitFactory& f) {
    if (building_idx >= world.buildings.size()) return 0;
    // 拷贝：spawn_unit 可能让 units 重分配，建筑随后只做"标记移除"
    const SimBuilding b = world.buildings[building_idx];
    if (!b.alive || b.under_construction) return 0;
    // 立即解除地基阻挡：落点判定与打包后寻路都要立即可用（tick 清扫会再解除一次，幂等）
    const auto in_bounds = [&](int c, int r) {
        return c >= 0 && r >= 0 && c < world.w && r < world.h;
    };
    for (const auto& c : b.footprint_cells)
        if (in_bounds(c.first, c.second))
            world.blocked[static_cast<size_t>(c.second) * world.w + c.first] = 0;
    world.note_nav_change(); // 直接改 blocked → 让流场缓存失效
    const auto cell_has_unit = [&](int c, int r) {
        for (const auto& u : world.units)
            if (u.alive && u.col == c && u.row == r) return true;
        return false;
    };
    // 落点：地基中心（展开时的对称位置，重新展开可放回原处）→ 锚点 → 地基格
    int mrx = 0, mry = 0;
    world.cell_to_map(b.col, b.row, mrx, mry);
    int cc = 0, cr = 0;
    world.map_to_cell(mrx + (b.fw - 1) / 2, mry + (b.fh - 1) / 2, cc, cr);
    int sc = -1, sr = -1;
    if (in_bounds(cc, cr) && !cell_has_unit(cc, cr)) {
        sc = cc;
        sr = cr;
    } else if (in_bounds(b.col, b.row) && !cell_has_unit(b.col, b.row)) {
        sc = b.col;
        sr = b.row;
    } else {
        for (const auto& c : b.footprint_cells)
            if (in_bounds(c.first, c.second) && !cell_has_unit(c.first, c.second)) {
                sc = c.first;
                sr = c.second;
                break;
            }
    }
    if (sc < 0) return 0;
    // 建筑移除：sold 标记 = 无爆炸；tick 清扫时统一重映射引用
    world.buildings[building_idx].alive = false;
    world.buildings[building_idx].sold = true;
    const uint32_t id = make_unit(world, b.owner, unit_type, sc, sr, f);
    if (id) {
        for (auto& u : world.units)
            if (u.id == id) {
                u.dir = static_cast<uint8_t>(b.dir / 32); // 朝向沿用建筑（0..255 → 0..7）
                break;
            }
    }
    return id;
}

BuildCheck check_buildable(const SimWorld& world, const assets::RulesDB& rules,
                           const std::string& owner, const std::string& country, int tech_level,
                           const assets::UnitTypeDef& t) {
    BuildCheck out;
    // 0. 必须在 rulesmd 有定义（[BuildingTypes] 里只有 artmd 的装饰/民用件排除）
    if (!rules.rules().has_section(t.name)) {
        out.reason = "非可建造类型";
        return out;
    }
    // 1. 阵营（Owner=；空表 = 非玩家可建造，如中立建筑）
    if (t.owner.empty() || !list_has(t.owner, country)) {
        out.reason = "本阵营不可建造";
        return out;
    }
    // 2. 科技等级（TechLevel=-1 = 永不建造，如建造厂只能由基地车展开）
    if (t.tech_level < 0) {
        out.reason = "不可建造（只能由基地车展开）";
        return out;
    }
    if (t.tech_level > tech_level) {
        out.reason = "科技等级不足";
        return out;
    }
    // 3. 建造厂（ConstructionYard=yes 建筑；原版隐含前置）
    if (!has_conyard(world, rules, owner)) {
        out.reason = "需要建造厂";
        return out;
    }
    // 4. Prerequisite=（全部满足；组名 POWER/PROC/RADAR/… 任一类型即可）
    for (const std::string& token : t.prereq) {
        const std::vector<std::string> group = rules.prereq_group(token);
        if (!group.empty()) {
            bool any = false;
            for (const std::string& g : group)
                if (world.has_building(owner, g, true)) {
                    any = true;
                    break;
                }
            if (!any) {
                out.reason = "缺少前置：" + token;
                return out;
            }
            continue;
        }
        if (!world.has_building(owner, token, true)) {
            out.reason = "缺少前置：" + token;
            return out;
        }
    }
    out.ok = true;
    return out;
}

std::vector<const assets::UnitTypeDef*> buildable_buildings(const SimWorld& world,
                                                            const assets::RulesDB& rules,
                                                            const std::string& owner,
                                                            const std::string& country,
                                                            int tech_level) {
    std::vector<const assets::UnitTypeDef*> out;
    // 保持 [BuildingTypes] 顺序（obj_lists 已排序，改用类型名查 rules）
    for (const auto& [k, name] : rules.rules().section("BuildingTypes")) {
        (void)k;
        if (name.empty()) continue;
        const auto* t = rules.unit(name);
        if (!t || t->kind != 0) continue;
        if (check_buildable(world, rules, owner, country, tech_level, *t).ok)
            out.push_back(t);
    }
    return out;
}

HouseRamp house_color_ramp(const assets::ColorDef& c) {
    // ModEnc [Colors]：H 恒定；V = 该色最大亮度；越暗越饱和。
    // 16 档：i=0 最暗（S→255）→ i=15 最亮（V，S）。HSV → RGB（Westwood 0..255 域）。
    HouseRamp out;
    const float h = static_cast<float>(c.h) * 360.0f / 256.0f;
    const float vmax = static_cast<float>(c.v);
    const float smin = static_cast<float>(c.s);
    for (int i = 0; i < 16; ++i) {
        const float t = static_cast<float>(i + 1) / 16.0f; // 亮度比例
        const float v = vmax * t;
        const float s = smin + (255.0f - smin) * (1.0f - t); // 越暗越饱和
        // HSV → RGB（h 度）
        const float cc = v * s / 255.0f;
        const float x = cc * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
        const float m = v - cc;
        float r = 0, g = 0, b = 0;
        if (h < 60) { r = cc; g = x; }
        else if (h < 120) { r = x; g = cc; }
        else if (h < 180) { g = cc; b = x; }
        else if (h < 240) { g = x; b = cc; }
        else if (h < 300) { r = x; b = cc; }
        else { r = cc; b = x; }
        const auto cl = [](float f) {
            const int k = static_cast<int>(std::lround(f));
            return static_cast<uint8_t>(k < 0 ? 0 : (k > 255 ? 255 : k));
        };
        out.rgb[i][0] = cl(r + m);
        out.rgb[i][1] = cl(g + m);
        out.rgb[i][2] = cl(b + m);
    }
    return out;
}

} // namespace ra2r::sim
