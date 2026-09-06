#pragma once
// RA2R — 体素模型软件光栅（M2 从 mixbrowser 提入引擎的渲染组件）
//
// 管线：VXL section 体素 → HVA 3×4 矩阵 → yaw/pitch 旋转 → 斜投影
//       → 立方体面（内部面剔除 + 精确投影平行四边形）
//       → 逐像素深度缓冲 z-test（参考 XCC Mixer image_z 方案；
//         OpenRA 用 GPU 深度缓冲，见 third_party/reference/VoxelLoader.cs）
//
// 相关规格：docs/formats/vxl.md、docs/formats/hva.md、docs/DEBUGGING.md §3.5
#include "ra2r/assets/hva_file.h"
#include "ra2r/assets/vxl_file.h"
#include "ra2r/render/raster.h"

namespace ra2r::render {

// 观察参数（等距摄像机占位：M2 后续统一到 camera.h，先保留最小集）
struct VoxelView {
    float yaw = 0.0f;                          // 绕 z 轴旋转（弧度）
    float pitch = 35.264f * 3.14159265f / 180.0f; // 绕 x 轴俯仰（默认等距俯角）
    float scale = 3.0f;                        // 每体素像素尺寸（可小数；0.5 ≈ 原版一格观感）
};

// 光栅化单个 section 为 RGBA 帧。
//   vxl / limb      — 模型与 section 序号
//   hva / hva_frame — 动画数据；hva 为 nullptr 时按恒等变换（单位矩阵）
//   view            — 观察参数（yaw/pitch/scale）
//   anchor_x/anchor_y（可选输出）— 光栅内网格原点 (0,0,0) 的像素位置
//     （经 HVA 平移+旋转+投影），用于把模型对齐到世界锚点（如单位所在格）。
// 返回内容为"模型本体的紧凑包围盒帧"（不含透明边距），调用方自行定位绘制。
RasterImage rasterize_voxel_section(const assets::VxlFile& vxl, const assets::HvaFile* hva,
                                    int limb, int hva_frame, const VoxelView& view,
                                    float* anchor_x = nullptr, float* anchor_y = nullptr);

// 光栅化整个体素模型（全部 section 合成，跨节共享深度缓冲）：
// 炮塔体素（如 YAGGUN 3 节：双枪管 + 主体）与多节载具需整模绘制；
// 锚点 = 主体节（体素最多）包围盒底中心经 HVA+旋转+投影后的位置。
RasterImage rasterize_voxel_model(const assets::VxlFile& vxl, const assets::HvaFile* hva,
                                  int hva_frame, const VoxelView& view,
                                  float* anchor_x = nullptr, float* anchor_y = nullptr);

} // namespace ra2r::render
