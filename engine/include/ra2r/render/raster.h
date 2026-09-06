#pragma once
// RA2R — 软件光栅通用类型（M2 渲染模块公共基底）
//
// 约定（docs/design/code-organization.md）：
// - 左上原点、行主序、RGBA8 直存；所有光栅组件共享本结构
// - 引擎侧渲染只产出 RasterImage，由各前端（SDL 纹理/BMP/PNG）自行适配
#include <cstdint>
#include <vector>

namespace ra2r::render {

// RGBA8 像素帧（w/h 为像素尺寸；空帧 = 无内容）
struct RasterImage {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba;

    bool empty() const { return rgba.empty(); }
};

} // namespace ra2r::render
