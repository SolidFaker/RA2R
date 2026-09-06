// RA2R — 战争迷雾/黑幕渲染状态实现（接口见 fog.h）
#include "ra2r/render/fog.h"

#include <algorithm>
#include <cmath>

namespace ra2r::render {

ShroudMap ShroudMap::from_map(const assets::MapFile& map) {
    ShroudMap s;
    s.w_ = map.cell_w();
    s.h_ = map.cell_h();
    s.states_ = map.shroud(); // 无 [Shroud] 节时 MapFile 返回全 1（可见）
    return s;
}

ShroudMap ShroudMap::demo(int w, int h, int cx, int cy, int radius) {
    ShroudMap s;
    s.w_ = w;
    s.h_ = h;
    s.states_.assign(static_cast<size_t>(w) * h, kBlack);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = static_cast<float>(x - cx);
            const float dy = static_cast<float>(y - cy);
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist <= radius) {
                s.states_[static_cast<size_t>(y) * w + x] = kVisible;
            } else if (dist <= radius + 4) {
                // 外圈两级迷雾：2 或 3（按距离细分）
                s.states_[static_cast<size_t>(y) * w + x] =
                    dist <= radius + 2 ? 2 : 3;
            } else if (dist <= radius + 8) {
                s.states_[static_cast<size_t>(y) * w + x] = 4;
            }
            // 其余保持黑幕
        }
    }
    return s;
}

uint8_t ShroudMap::to_light(uint8_t state) {
    switch (state) {
        case 1: return 0;  // 可见：全亮
        case 2: return 12; // 轻度迷雾
        case 3: return 18; // 中度迷雾
        case 4: return 24; // 重度迷雾
        default: return 31; // 黑幕：纯黑
    }
}

} // namespace ra2r::render
