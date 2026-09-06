#pragma once
// RA2R — 等距网格坐标变换（M2 渲染模块，纯函数无状态）
#include <cmath>
//
// RA2 等距约定（60×30 瓦片菱形，砖墙式排布）：
//   格 (cx, cy) 的瓦片包围盒左上角 = (cx·60 + (cy&1)·30, cy·15)——
//   水平行内菱形角挨角（60px 间距），奇数行右移半宽（30px）、下移半高（15px），
//   相邻行菱形共享斜边铺满平面；W×H 格的地图呈**轴对齐矩形**（边缘锯齿状）。
//   锚点：官方图 IsoMapPack5 (col,row)=(rx−ry+c)/2, rx+ry+c2 网格即此排布
//   （Egypt Size=0,0,119,82 与 FinalAlert2 显示一致，docs/DEBUGGING 3.14）。
//   高度（height 0..15）每级抬 15px（kHeightLevelPx）。
namespace ra2r::render {

// 高度换算：地图/瓦片高度字节的 1 级 = 15 屏幕像素（半瓦高）。
// 实测依据：官方图悬崖底格面向上扩展 60px 恰对应顶侧邻居 +4 级（23 图 24/24 样本，
// 见 docs/DEBUGGING.md §3.11）；悬崖/斜坡帧的扩展区均按 15px/级设计。
constexpr int kHeightLevelPx = 15;

struct IsometricGrid {
    int tile_w = 60;
    int tile_h = 30;

    // 格 (cx, cy) → 瓦片包围盒左上角（px 原点为地图左上角，可为负）
    void cell_to_pixel(int cx, int cy, int& px, int& py) const {
        px = cx * tile_w + (cy & 1) * (tile_w / 2);
        py = cy * (tile_h / 2);
    }

    // 屏幕坐标 → 格坐标（点选/视口裁剪用；返回包含该点的格，无归属返回 -1）
    // 相邻行瓦片在垂直方向交错各占一半，py/15 有两个候选行，逐候选做菱形包含测试。
    void pixel_to_cell(int px, int py, int& cx, int& cy) const {
        auto hit = [&](int tx, int ty) {
            const double dx = std::fabs(px - (tx + tile_w / 2.0)) / (tile_w / 2.0);
            const double dy = std::fabs(py - (ty + tile_h / 2.0)) / (tile_h / 2.0);
            return dx + dy <= 1.0;
        };
        const int row0 = py / (tile_h / 2);
        for (int r = row0; r >= row0 - 1; --r) {
            if (r < 0) continue;
            const int c0 = (px - (r & 1) * (tile_w / 2)) / tile_w;
            for (int c = c0; c <= c0 + 1; ++c) {
                int bx, by;
                cell_to_pixel(c, r, bx, by);
                if (hit(bx, by)) {
                    cx = c;
                    cy = r;
                    return;
                }
            }
        }
        cx = -1;
        cy = -1;
    }

    // 全图 W×H 格的渲染包围盒（含悬崖扩展与高度抬升）：
    //   砖墙矩形 W·60+30 × H·15+15；左/上再留悬崖与高度余量。
    void map_bounds(int w, int h, int max_height, int& bw, int& bh, int& ox, int& oy) const {
        const int min_x = -tile_w / 2;
        const int max_x = (w - 1) * tile_w + ((h - 1) & 1) * (tile_w / 2) + tile_w;
        const int min_y = -max_height * kHeightLevelPx - tile_h / 2;
        const int max_y = (h - 1) * (tile_h / 2) + tile_h + tile_h; // 悬崖扩展可达一瓦高
        bw = max_x - min_x;
        bh = max_y - min_y;
        ox = -min_x;
        oy = -min_y;
    }
};

} // namespace ra2r::render
