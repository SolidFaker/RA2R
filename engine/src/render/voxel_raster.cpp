// RA2R — 体素模型软件光栅实现
//
// 结构（按 code-organization.md 拆分）：
//   load_hva_matrix   — 按 section 名取 HVA 3×4 矩阵
//   Projection        — 线性全变换（格坐标 → 屏幕 + 深度）与投影基向量
//   collect_voxels    — 体素投影 + 紧凑包围盒
//   VoxelRasterizer   — 面剔除、着色、逐像素 z-buffer 光栅
//
// 渲染管线说明见 voxel_raster.h；算法规格见 docs/DEBUGGING.md §3.5。
#include "ra2r/render/voxel_raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ra2r::render {

namespace {

// ── 光照常量：方向光固定于视图空间（模型旋转时高光稳定）──
struct DirectionalLight {
    float x, y, z;
};
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) —— sqrt 不抛异常，纯常量
const DirectionalLight kLight = [] {
    const float inv = 1.0f / std::sqrt(0.35f * 0.35f + 0.35f * 0.35f + 0.87f * 0.87f);
    return DirectionalLight{0.35f * inv, -0.35f * inv, 0.87f * inv};
}();

// HVA 平移分量单位换算：文件里的平移量是"1/16 体素"尺度。实测依据：
//   YAGGUN 炮管 t=(111.5,164.5,479.6) → /16 = (7.0,10.3,30.0)，z=30 恰为炮塔枢轴高度；
//   YTNKTUR 炮塔 t_z=256.7 → 16.0，而 YTNK 车体高 15 体素（炮塔正落在车体顶）；
//   1TNKBARL 炮管 t_z=139.3 → 8.7，1TNK 车体高 11（炮管在炮塔高度）。
// 全库 139 个非零平移 section 按此换算均落在模型尺寸内。
constexpr float kHvaTranslationScale = 1.0f / 16.0f;

// 取 HVA 矩阵（按 section 名匹配；无 HVA/无同名 → 单位矩阵）
void load_hva_matrix(const assets::HvaFile* hva, const assets::VxlSection& section,
                     int hva_frame, float m[12]) {
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(m, identity, sizeof(identity));
    if (!hva || !hva->is_open() || hva_frame < 0 ||
        static_cast<uint32_t>(hva_frame) >= hva->frame_count()) {
        return;
    }
    for (uint32_t i = 0; i < hva->section_count(); ++i) {
        if (hva->section_names()[i] != section.name) continue;
        const float* hm = hva->matrix(static_cast<uint32_t>(hva_frame), i);
        std::memcpy(m, hm, 12 * sizeof(float));
        // 平移换算用**节自身 det**（limb footer Scale；恒定 1/16 只是 0.083≈1/12
        // 文件的巧合值）：实测 YAGGUN det=0.056 时炮管平移 z=479.6×0.056=26.9
        // 恰落进座圈 z 22.6..39.7；按 1/16 换算则浮空（26.9→30 偏差尚可，
        // 但 det 相差 1.5 倍的文件会明显错位——单位语义即 det）
        const float det = section.det > 0.0f ? section.det : kHvaTranslationScale;
        m[3] *= det;
        m[7] *= det;
        m[11] *= det;
        return;
    }
}

// 线性全变换：格向量 → HVA 3×3 → yaw/pitch 旋转 → 斜投影。
// 屏幕坐标 px = (u-v2)·pitch_x, py = (u+v2)·pitch_y - w2·pitch_z；
// 深度 = w2（越大越近），是屏幕坐标的线性函数（z-buffer 的平面方程前提）。
struct Projection {
    float exx, exy, exd; // 格基向量 (1,0,0) 的屏幕/深度分量
    float eyx, eyy, eyd; //             (0,1,0)
    float ezx, ezy, ezd; //             (0,0,1)
    float tx, ty, td;    // HVA 平移分量的投影

    void project(float gx, float gy, float gz, float& px, float& py, float& depth) const;
};

void Projection::project(float gx, float gy, float gz, float& px, float& py,
                         float& depth) const {
    px = exx * gx + eyx * gy + ezx * gz + tx;
    py = exy * gx + eyy * gy + ezy * gz + ty;
    depth = exd * gx + eyd * gy + ezd * gz + td;
}

// 由 HVA 矩阵与视角参数构造投影
Projection make_projection(const float m[12], float yaw, float pitch, float scale, float tilt) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float px = 2.0f * scale, py = scale, pz = 2.0f * scale; // 2:1 斜投影比例
    // 车体俯仰（上下坡）：绕模型横向轴 y 旋转（车头 = +x）。**在 HVA 之后**，
    // 即先摆好各节位置再整体倾斜——炮塔/炮管跟着底盘一起倾。
    const float ct = std::cos(tilt), st = std::sin(tilt);
    const auto tilt_vec = [&](float& x, float& y, float& z) {
        (void)y; // 绕横向轴 y 旋转：y 分量不变
        const float nx = x * ct + z * st;
        const float nz = -x * st + z * ct;
        x = nx;
        z = nz;
    };
    auto proj_vec = [&](float gx, float gy, float gz, float& ox, float& oy, float& od) {
        float wx = m[0] * gx + m[1] * gy + m[2] * gz;
        float wy = m[4] * gx + m[5] * gy + m[6] * gz;
        float wz = m[8] * gx + m[9] * gy + m[10] * gz;
        if (tilt != 0.0f) tilt_vec(wx, wy, wz);
        const float u = wx * cy - wy * sy;
        const float v1 = wx * sy + wy * cy;
        const float v2 = v1 * cp - wz * sp;
        const float w2 = v1 * sp + wz * cp;
        ox = (u - v2) * px;
        oy = (u + v2) * py - w2 * pz;
        od = w2;
    };
    Projection p{};
    proj_vec(1, 0, 0, p.exx, p.exy, p.exd);
    proj_vec(0, 1, 0, p.eyx, p.eyy, p.eyd);
    proj_vec(0, 0, 1, p.ezx, p.ezy, p.ezd);
    proj_vec(m[3], m[7], m[11], p.tx, p.ty, p.td); // 平移分量经同一线性部分
    return p;
}

// 同上，但把节的包围盒映射烘焙进基向量：体素【索引】i 的模型坐标 =
//   min + (i + 0.5)·(max − min)/size（XCC 语义；各轴步长不同也正确）。
// 不做该映射时三件（底盘/炮塔/炮管）都会叠在原点——炮塔埋进底盘。
// 烘焙后 project() 仍吃索引，底盘观感与旧行为一致（步长 ≈ 1）。
Projection make_section_projection(const assets::VxlSection& s, const float m[12], float yaw,
                                   float pitch, float scale, float tilt) {
    Projection p = make_projection(m, yaw, pitch, scale, tilt);
    const float* mn = s.min;
    const float* mx = s.max;
    const float step[3] = {
        s.sx > 0 ? (mx[0] - mn[0]) / s.sx : 1.0f,
        s.sy > 0 ? (mx[1] - mn[1]) / s.sy : 1.0f,
        s.sz > 0 ? (mx[2] - mn[2]) / s.sz : 1.0f,
    };
    const float org[3] = {mn[0] + 0.5f * step[0], mn[1] + 0.5f * step[1],
                          mn[2] + 0.5f * step[2]};
    // 基向量 ×步长；原点偏移并入平移（保持线性投影不变式）
    const float b[3][3] = {{p.exx, p.exy, p.exd}, {p.eyx, p.eyy, p.eyd},
                           {p.ezx, p.ezy, p.ezd}};
    p.exx = b[0][0] * step[0];
    p.exy = b[0][1] * step[0];
    p.exd = b[0][2] * step[0];
    p.eyx = b[1][0] * step[1];
    p.eyy = b[1][1] * step[1];
    p.eyd = b[1][2] * step[1];
    p.ezx = b[2][0] * step[2];
    p.ezy = b[2][1] * step[2];
    p.ezd = b[2][2] * step[2];
    p.tx += b[0][0] * org[0] + b[1][0] * org[1] + b[2][0] * org[2];
    p.ty += b[0][1] * org[0] + b[1][1] * org[1] + b[2][1] * org[2];
    p.td += b[0][2] * org[0] + b[1][2] * org[1] + b[2][2] * org[2];
    return p;
}

// 体素：屏幕原点 + 深度 + 格坐标 + 调色板索引
struct Voxel {
    float ox, oy, depth;
    uint8_t wx, wy, wz;
    uint8_t color;
};

// 投影全部体素，并求覆盖立方体 8 角的紧凑包围盒
void collect_voxels(const assets::VxlSection& section, const Projection& proj,
                    std::vector<Voxel>& voxels, float& min_x, float& max_x, float& min_y,
                    float& max_y) {
    voxels.clear();
    voxels.reserve(static_cast<size_t>(section.sx) * section.sy * section.sz);
    min_x = min_y = 1e9f;
    max_x = max_y = -1e9f;
    auto upd = [&](float x, float y) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    };
    for (uint16_t z = 0; z < section.sz; ++z) {
        for (uint16_t y = 0; y < section.sy; ++y) {
            for (uint16_t x = 0; x < section.sx; ++x) {
                const auto& v = section.at(x, y, z);
                if (v.color == 0) continue; // 索引 0 = 空体素
                float ox, oy, depth;
                proj.project(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                             ox, oy, depth);
                // 8 角：O + {0,ex,ey,ez} 组合
                upd(ox, oy);
                upd(ox + proj.exx, oy + proj.exy);
                upd(ox + proj.eyx, oy + proj.eyy);
                upd(ox + proj.ezx, oy + proj.ezy);
                upd(ox + proj.exx + proj.eyx, oy + proj.exy + proj.eyy);
                upd(ox + proj.exx + proj.ezx, oy + proj.exy + proj.ezy);
                upd(ox + proj.eyx + proj.ezx, oy + proj.eyy + proj.ezy);
                upd(ox + proj.exx + proj.eyx + proj.ezx, oy + proj.exy + proj.eyy + proj.ezy);
                voxels.push_back({ox, oy, depth, static_cast<uint8_t>(x),
                                  static_cast<uint8_t>(y), static_cast<uint8_t>(z), v.color});
            }
        }
    }
}

// 逐像素 z-buffer 光栅器：面剔除 + 着色 + 三角形平面方程 z-test
class VoxelRasterizer {
public:
    VoxelRasterizer(const assets::VxlSection& section, const Projection& proj,
                    const assets::VxlFile& vxl, const float hva[12], float yaw, float pitch,
                    RasterImage& out, std::vector<float>* shared_zbuf = nullptr,
                    const uint8_t* remap = nullptr);

    void run(const std::vector<Voxel>& voxels, float off_x, float off_y);

private:
    // 几何面法线经 HVA+yaw/pitch 旋转后与固定光点积 → 亮度系数
    float face_shade(float n0, float n1, float n2) const;
    // 调色板索引 → 着色后 RGB
    std::array<int, 3> face_rgb(uint8_t color, float shade) const;
    // 带深度的三角形光栅（平面方程 d = A·x + B·y + C；0.35px 边容差防裂缝）
    void fill_triangle(float ax, float ay, float ad, float bx, float by, float bd, float cx,
                       float cy, float cd, const std::array<int, 3>& rgb);

    const assets::VxlSection& section_;
    const Projection& proj_;
    const std::array<uint8_t, 768>& pal_;
    const uint8_t* remap_; // 阵营色重映射段（16×RGB；nullptr = 原样）
    const float* hva_;
    float cy_, sy_, cp_, sp_; // yaw/pitch 三角函数缓存
    int w_, h_;
    RasterImage& out_;
    std::vector<float>* zbuf_; // 共享深度缓冲（整模合成）；无共享时用自有
    std::vector<float> zbuf_own_;
};

VoxelRasterizer::VoxelRasterizer(const assets::VxlSection& section, const Projection& proj,
                                 const assets::VxlFile& vxl, const float hva[12], float yaw,
                                 float pitch, RasterImage& out, std::vector<float>* shared_zbuf,
                                 const uint8_t* remap)
    : section_(section), proj_(proj), pal_(vxl.palette()), remap_(remap), hva_(hva),
      cy_(std::cos(yaw)), sy_(std::sin(yaw)), cp_(std::cos(pitch)), sp_(std::sin(pitch)),
      w_(out.w), h_(out.h), out_(out), zbuf_(shared_zbuf),
      zbuf_own_(shared_zbuf ? std::vector<float>()
                            : std::vector<float>(static_cast<size_t>(out.w) * out.h,
                                                 -1e30f)) {
    if (!zbuf_) zbuf_ = &zbuf_own_;
}

float VoxelRasterizer::face_shade(float n0, float n1, float n2) const {
    const float hx = hva_[0] * n0 + hva_[1] * n1 + hva_[2] * n2;
    const float hy = hva_[4] * n0 + hva_[5] * n1 + hva_[6] * n2;
    const float hz = hva_[8] * n0 + hva_[9] * n1 + hva_[10] * n2;
    const float u = hx * cy_ - hy * sy_;
    const float v1 = hx * sy_ + hy * cy_;
    const float v2 = v1 * cp_ - hz * sp_;
    const float w2 = v1 * sp_ + hz * cp_;
    const float d = u * kLight.x + v2 * kLight.y + w2 * kLight.z;
    return 0.55f + 0.45f * std::max(0.0f, d);
}

std::array<int, 3> VoxelRasterizer::face_rgb(uint8_t color, float shade) const {
    // VXL 内嵌调色板是 **8 位** RGB（实测 YAGGUN：索引 15 = 255、20 = 195、13 = 87；
    // 同索引在 UNITURB.PAL 里是 252/192/84 的 6 位值 ×4）。早期版本按 .PAL 的
    // 6 位约定再 ×4，导致体素整体过曝（炮管发白、暗部丢层次）。
    // 索引 16..31 = 原版 Remap 段：有阵营色时整段替换（VXL 内嵌盘同为 8 位）
    const uint8_t* src = pal_.data() + color * 3;
    if (remap_ && color >= 16 && color <= 31) src = remap_ + (color - 16) * 3;
    const int r = static_cast<int>(src[0] * shade);
    const int g = static_cast<int>(src[1] * shade);
    const int b = static_cast<int>(src[2] * shade);
    return {std::min(255, r), std::min(255, g), std::min(255, b)};
}

void VoxelRasterizer::fill_triangle(float ax, float ay, float ad, float bx, float by, float bd,
                                    float cx, float cy, float cd,
                                    const std::array<int, 3>& rgb) {
    const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (std::fabs(area) < 1e-6f) return; // 退化三角形
    // 深度平面方程：d = A·x + B·y + C（由 3 角解 2×2 线性系）
    const float denom = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    const float A = ((bd - ad) * (cy - ay) - (cd - ad) * (by - ay)) / denom;
    const float B = ((bx - ax) * (cd - ad) - (cx - ax) * (bd - ad)) / denom;
    const float C = ad - A * ax - B * ay;
    const int minx = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
    const int maxx = std::min(w_ - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
    const int miny = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
    const int maxy = std::min(h_ - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
    for (int y = miny; y <= maxy; ++y) {
        const float fy = static_cast<float>(y) + 0.5f;
        for (int x = minx; x <= maxx; ++x) {
            const float fx = static_cast<float>(x) + 0.5f;
            // 边函数（像素中心；0.35px 容差让相邻面共享棱无缝，
            // 覆盖正确性由 z-test 保证）
            const float w0 = (cx - bx) * (fy - by) - (cy - by) * (fx - bx);
            const float w1 = (ax - cx) * (fy - cy) - (ay - cy) * (fx - cx);
            const float w2 = (bx - ax) * (fy - ay) - (by - ay) * (fx - ax);
            const float eps = 0.35f;
            const bool inside = area > 0 ? (w0 >= -eps && w1 >= -eps && w2 >= -eps)
                                         : (w0 <= eps && w1 <= eps && w2 <= eps);
            if (!inside) continue;
            const float d = A * fx + B * fy + C;
            const size_t i = static_cast<size_t>(y) * w_ + x;
            if (d <= (*zbuf_)[i]) continue; // 更远/同深不覆盖
            (*zbuf_)[i] = d;
            uint8_t* p = out_.rgba.data() + i * 4;
            p[0] = static_cast<uint8_t>(rgb[0]);
            p[1] = static_cast<uint8_t>(rgb[1]);
            p[2] = static_cast<uint8_t>(rgb[2]);
            p[3] = 255;
        }
    }
}

void VoxelRasterizer::run(const std::vector<Voxel>& voxels, float off_x, float off_y) {
    // 面类型元数据：序 0=-x 1=+x 2=-y 3=+y 4=底 5=顶
    // 角索引：0=C00 1=C10 2=C01 3=C11 4=T00 5=T10 6=T01 7=T11
    static constexpr int kFaceIdx[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4},
                                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
    // 邻面方向（内部面剔除：邻居非空则不画该面）
    static constexpr int kFaceNbr[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                           {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
    // 各轴向面的屏幕面积（投影退化检测：侧对相机时跳过）
    const float area[3] = {std::fabs(proj_.eyx * proj_.ezy - proj_.eyy * proj_.ezx),
                           std::fabs(proj_.exx * proj_.ezy - proj_.exy * proj_.ezx),
                           std::fabs(proj_.exx * proj_.eyy - proj_.exy * proj_.eyx)};
    const float sh[6] = {face_shade(-1, 0, 0), face_shade(1, 0, 0), face_shade(0, -1, 0),
                         face_shade(0, 1, 0),  face_shade(0, 0, -1), face_shade(0, 0, 1)};
    auto empty = [&](int x, int y, int z) {
        if (x < 0 || y < 0 || z < 0 || x >= section_.sx || y >= section_.sy || z >= section_.sz)
            return true;
        return section_.at(x, y, z).color == 0;
    };
    for (const Voxel& v : voxels) {
        const float X = v.ox + off_x, Y = v.oy + off_y;
        // 8 角：屏幕坐标 + 深度（基向量线性分量累加，零额外变换成本）
        const float cx[8] = {X, X + proj_.exx, X + proj_.eyx, X + proj_.exx + proj_.eyx,
                             X + proj_.ezx, X + proj_.exx + proj_.ezx,
                             X + proj_.eyx + proj_.ezx,
                             X + proj_.exx + proj_.eyx + proj_.ezx};
        const float cy[8] = {Y, Y + proj_.exy, Y + proj_.eyy, Y + proj_.exy + proj_.eyy,
                             Y + proj_.ezy, Y + proj_.exy + proj_.ezy,
                             Y + proj_.eyy + proj_.ezy,
                             Y + proj_.exy + proj_.eyy + proj_.ezy};
        const float cd[8] = {v.depth, v.depth + proj_.exd, v.depth + proj_.eyd,
                             v.depth + proj_.exd + proj_.eyd,
                             v.depth + proj_.ezd, v.depth + proj_.exd + proj_.ezd,
                             v.depth + proj_.eyd + proj_.ezd,
                             v.depth + proj_.exd + proj_.eyd + proj_.ezd};
        const std::array<std::array<int, 3>, 6> shades = {
            face_rgb(v.color, sh[0]), face_rgb(v.color, sh[1]), face_rgb(v.color, sh[2]),
            face_rgb(v.color, sh[3]), face_rgb(v.color, sh[4]), face_rgb(v.color, sh[5])};
        for (int f = 0; f < 6; ++f) {
            if (!empty(v.wx + kFaceNbr[f][0], v.wy + kFaceNbr[f][1], v.wz + kFaceNbr[f][2]))
                continue;
            if (area[f / 2] < 0.1f) continue;
            const int* idx = kFaceIdx[f];
            fill_triangle(cx[idx[0]], cy[idx[0]], cd[idx[0]], cx[idx[1]], cy[idx[1]], cd[idx[1]],
                          cx[idx[2]], cy[idx[2]], cd[idx[2]], shades[f]);
            fill_triangle(cx[idx[0]], cy[idx[0]], cd[idx[0]], cx[idx[2]], cy[idx[2]], cd[idx[2]],
                          cx[idx[3]], cy[idx[3]], cd[idx[3]], shades[f]);
        }
    }
}

} // namespace

RasterImage rasterize_voxel_section(const assets::VxlFile& vxl, const assets::HvaFile* hva,
                                    int limb, int hva_frame, const VoxelView& view,
                                    float* anchor_x, float* anchor_y) {
    RasterImage out;
    if (limb < 0 || limb >= static_cast<int>(vxl.sections().size())) return out;
    const assets::VxlSection& section = vxl.sections()[limb];

    float hva_m[12];
    load_hva_matrix(hva, section, hva_frame, hva_m);
    const Projection proj =
        make_section_projection(section, hva_m, view.yaw, view.pitch, view.scale, view.tilt);

    std::vector<Voxel> voxels;
    float min_x, max_x, min_y, max_y;
    collect_voxels(section, proj, voxels, min_x, max_x, min_y, max_y);
    if (voxels.empty()) return out;

    const int margin = static_cast<int>(view.scale * 4.0f);
    out.w = static_cast<int>(max_x - min_x) + margin * 2;
    out.h = static_cast<int>(max_y - min_y) + margin * 2;
    out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    // 锚点 = 模型原点 (0,0,0) 在光栅中的位置（与整模版本同语义）
    if (anchor_x) *anchor_x = static_cast<float>(margin) - min_x;
    if (anchor_y) *anchor_y = static_cast<float>(margin) - min_y;

    VoxelRasterizer rasterizer(section, proj, vxl, hva_m, view.yaw, view.pitch, out, nullptr,
                               view.remap);
    rasterizer.run(voxels, -min_x + static_cast<float>(margin),
                   -min_y + static_cast<float>(margin));
    return out;
}

RasterImage rasterize_voxel_parts(const VoxelPart* parts, size_t count, const VoxelView& view,
                                  float* anchor_x, float* anchor_y, float* body_x,
                                  float* body_y, float* origin_x, float* origin_y) {
    RasterImage out;
    if (!parts || count == 0) return out;
    // 1) 各部件各节投影/收集体素，求全局包围盒
    struct Sec {
        const assets::VxlSection* s = nullptr;
        const assets::VxlFile* vxl = nullptr;
        Projection proj{};
        std::vector<Voxel> vx;
        float hva_m[12] = {};
    };
    std::vector<Sec> secs;
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (size_t pi = 0; pi < count; ++pi) {
        const VoxelPart& p = parts[pi];
        if (!p.vxl) continue;
        for (const auto& section : p.vxl->sections()) {
            Sec sd;
            sd.s = &section;
            sd.vxl = p.vxl;
            load_hva_matrix(p.hva, section, p.hva_frame, sd.hva_m);
            sd.proj = make_section_projection(section, sd.hva_m, view.yaw, view.pitch,
                                              view.scale, view.tilt);
            float a, b, c, d;
            collect_voxels(section, sd.proj, sd.vx, a, b, c, d);
            if (sd.vx.empty()) continue;
            min_x = std::min(min_x, a);
            max_x = std::max(max_x, b);
            min_y = std::min(min_y, c);
            max_y = std::max(max_y, d);
            secs.push_back(std::move(sd));
        }
    }
    if (secs.empty()) return out;
    const int margin = static_cast<int>(view.scale * 4.0f);
    out.w = static_cast<int>(max_x - min_x) + margin * 2;
    out.h = static_cast<int>(max_y - min_y) + margin * 2;
    out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    // 锚点 = 模型原点 (0,0,0) 在光栅中的像素位置（模型空间原点经视角变换
    // 落在屏幕 (0,0)，光栅整体偏移 (-min_x+margin, -min_y+margin)）。
    if (anchor_x) *anchor_x = static_cast<float>(margin) - min_x;
    if (anchor_y) *anchor_y = static_cast<float>(margin) - min_y;
    // 主体节"炮塔座锚点"：主体节（体素最多）包围盒中心 (x,y) + 该节最低 z
    // （即炮塔坐在底座上的那个点）。取主体节而非整模包围盒，保证朝向变化时
    // 锚点稳定（炮管旋转会让整模 AABB 中心摆动 ±27px）。
    if (body_x || body_y) {
        const Sec* body = &secs[0];
        for (const auto& s : secs)
            if (s.vx.size() > body->vx.size()) body = &s;
        float bx, by, bd;
        // 包围盒中心/底 → 索引空间：i = ((mn+mx)/2 − mn − 0.5·step)/step
        const auto idx_c = [](float mn, float mx, int n) {
            const float step = n > 0 ? (mx - mn) / static_cast<float>(n) : 1.0f;
            return (mx - mn) * 0.5f / step - 0.5f;
        };
        body->proj.project(idx_c(body->s->min[0], body->s->max[0], body->s->sx),
                           idx_c(body->s->min[1], body->s->max[1], body->s->sy), 0.0f, bx, by,
                           bd);
        if (body_x) *body_x = bx - min_x + static_cast<float>(margin);
        if (body_y) *body_y = by - min_y + static_cast<float>(margin);
    }
    // 模型原点 (0,0,0)【模型空间】在光栅中的位置：rules TurretAnimX/Y 等
    // 游戏内偏移都以模型原点为参照（炮塔枢轴）。索引 → 模型：
    // i = (0 − mn − 0.5·step)/step。
    // **必须用不含 HVA 平移的投影**：HVA 平移只是"该节几何相对模型原点的摆位"
    // （如 YAGGUN 炮管 t=(6.2,9.1,26.7) 抬到枢轴高度），原点锚必须固定在模型
    // 坐标系原点。早期用 secs[0].proj（含该节 HVA 平移）计算，整个光栅随首节
    // 平移漂移 R·t——YAGGUN 首节=炮管，实测偏 (+15.4,−25.2)px@scale0.5，是
    // "盖特炮塔与底座错位"的主因之一；GTGCAN/BARL 平移极小故未暴露。
    if (origin_x || origin_y) {
        if (!secs.empty()) {
            const auto& s0 = *secs[0].s;
            const auto idx_of = [](float mn, float mx, int n) {
                const float step = n > 0 ? (mx - mn) / static_cast<float>(n) : 1.0f;
                return step > 0 ? (0.0f - mn - 0.5f * step) / step : 0.0f;
            };
            const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            const Projection pure =
                make_section_projection(s0, identity, view.yaw, view.pitch, view.scale, view.tilt);
            float ox, oy, od;
            pure.project(idx_of(s0.min[0], s0.max[0], s0.sx),
                         idx_of(s0.min[1], s0.max[1], s0.sy),
                         idx_of(s0.min[2], s0.max[2], s0.sz), ox, oy, od);
            if (origin_x) *origin_x = ox - min_x + static_cast<float>(margin);
            if (origin_y) *origin_y = oy - min_y + static_cast<float>(margin);
        } else {
            if (origin_x) *origin_x = static_cast<float>(margin) - min_x;
            if (origin_y) *origin_y = static_cast<float>(margin) - min_y;
        }
    }
    // 2) 共享深度缓冲顺序绘制各节（跨节、跨部件遮挡正确）
    std::vector<float> zbuf(static_cast<size_t>(out.w) * out.h, -1e30f);
    for (const auto& s : secs) {
        VoxelRasterizer rz(*s.s, s.proj, *s.vxl, s.hva_m, view.yaw, view.pitch, out, &zbuf,
                       view.remap);
        rz.run(s.vx, -min_x + static_cast<float>(margin),
               -min_y + static_cast<float>(margin));
    }
    return out;
}

RasterImage rasterize_voxel_model(const assets::VxlFile& vxl, const assets::HvaFile* hva,
                                  int hva_frame, const VoxelView& view, float* anchor_x,
                                  float* anchor_y, float* body_x, float* body_y) {
    const VoxelPart part{&vxl, hva, hva_frame};
    return rasterize_voxel_parts(&part, 1, view, anchor_x, anchor_y, body_x, body_y);
}

} // namespace ra2r::render
