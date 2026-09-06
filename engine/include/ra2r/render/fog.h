#pragma once
// RA2R — 战争迷雾/黑幕渲染状态（M2 渲染模块）
//
// RA2 语义：格状态 1=可见、2..4=迷雾（探索过但不在视野，逐级加深）、
// 5=黑幕（未探索，全黑）。渲染时状态映射为 PaletteLut 光照等级：
// 可见=当前光照，迷雾=加深（20..24），黑幕=全黑（31 级纯黑）。
#include <cstdint>
#include <vector>

#include "ra2r/assets/map_file.h"

namespace ra2r::render {

class ShroudMap {
public:
    static constexpr uint8_t kVisible = 1;
    static constexpr uint8_t kBlack = 5;

    // 从地图 [Shroud] 节构建（无节 = 全可见）
    static ShroudMap from_map(const assets::MapFile& map);
    // 合成演示：以 (cx,cy) 为圆心 radius 半径可见，外两圈迷雾，再外黑幕
    static ShroudMap demo(int w, int h, int cx, int cy, int radius);

    uint8_t state(int cx, int cy) const {
        if (cx < 0 || cy < 0 || cx >= w_ || cy >= h_) return kBlack;
        return states_[static_cast<size_t>(cy) * w_ + cx];
    }
    // 状态 → 光照等级（PaletteLut 0..31；黑幕 31 = 纯黑）
    static uint8_t to_light(uint8_t state);

private:
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> states_;
};

} // namespace ra2r::render
