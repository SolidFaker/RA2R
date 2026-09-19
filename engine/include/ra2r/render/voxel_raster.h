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
    // 阵营色重映射（可选）：48 字节 = 16 × RGB，替换 VXL 内嵌调色盘索引 16..31
    //（原版 Remap 段，见 assets::HouseRamp）
    const uint8_t* remap = nullptr;
};

// 光栅化单个 section 为 RGBA 帧。
//   vxl / limb      — 模型与 section 序号
//   hva / hva_frame — 动画数据；hva 为 nullptr 时按恒等变换（单位矩阵）
//   view            — 观察参数（yaw/pitch/scale）
//   anchor_x/anchor_y（可选输出）— **模型原点 (0,0,0)** 在光栅内的像素位置，
//     用于把模型对齐到世界锚点（单位所在格中心 / 建筑顶格锚点 + TurretAnimX/Y）。
// 返回内容为"模型本体的紧凑包围盒帧"（不含透明边距），调用方自行定位绘制。
RasterImage rasterize_voxel_section(const assets::VxlFile& vxl, const assets::HvaFile* hva,
                                    int limb, int hva_frame, const VoxelView& view,
                                    float* anchor_x = nullptr, float* anchor_y = nullptr);

// 光栅化整个体素模型（全部 section 合成，跨节共享深度缓冲）：
// 炮塔体素（如 YAGGUN 3 节：双枪管 + 主体）与多节载具需整模绘制。
// 锚点输出：
//   anchor_x/anchor_y — 模型原点 (0,0,0) 在光栅中的像素位置（载具用）；
//   body_x/body_y     — 主体节（体素最多）的"座锚点"：包围盒中心 (x,y) + 该节
//     最低 z（炮塔坐在底座上的接触点）。取主体节而非整模包围盒，保证炮管旋转
//     时锚点稳定（整模 AABB 中心会随炮管朝向摆动 ±27px）。
RasterImage rasterize_voxel_model(const assets::VxlFile& vxl, const assets::HvaFile* hva,
                                  int hva_frame, const VoxelView& view,
                                  float* anchor_x = nullptr, float* anchor_y = nullptr,
                                  float* body_x = nullptr, float* body_y = nullptr);

// 一个体素模型部件（VXL + 可选 HVA 与帧号）
struct VoxelPart {
    const assets::VxlFile* vxl = nullptr;
    const assets::HvaFile* hva = nullptr;
    int hva_frame = 0;
};

// 多部件整模光栅化（共享同一深度缓冲与同一模型坐标系）：
// 炮塔 + 炮管是两个独立 VXL（原版命名约定：炮塔 `<TurretAnim>.VXL`、
// 炮管 `<Image>BARL.VXL`，如巨炮 GTGCANTUR + GTGCANBARL、坦克 GTNKTUR + GTNKBARL），
// 两者坐标同系、必须一起做 z-test，否则互相穿插。锚点语义同单模型版本
// （body_* 取全部部件中体素最多的那节）。
RasterImage rasterize_voxel_parts(const VoxelPart* parts, size_t count, const VoxelView& view,
                                  float* anchor_x = nullptr, float* anchor_y = nullptr,
                                  float* body_x = nullptr, float* body_y = nullptr,
                                  float* origin_x = nullptr, float* origin_y = nullptr);

} // namespace ra2r::render
