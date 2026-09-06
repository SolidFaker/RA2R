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
#include <cstring>

namespace ra2r::render {

namespace {

// ── 光照常量：方向光固定于视图空间（模型旋转时高光稳定）──
struct DirectionalLight {
    float x, y, z;
};
const DirectionalLight kLight = [] {
    const float inv = 1.0f / std::sqrt(0.35f * 0.35f + 0.35f * 0.35f + 0.87f * 0.87f);
    return DirectionalLight{0.35f * inv, -0.35f * inv, 0.87f * inv};
}();

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
Projection make_projection(const float m[12], float yaw, float pitch, float scale) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float px = 2.0f * scale, py = scale, pz = 2.0f * scale; // 2:1 斜投影比例
    auto proj_vec = [&](float gx, float gy, float gz, float& ox, float& oy, float& od) {
        const float wx = m[0] * gx + m[1] * gy + m[2] * gz;
        const float wy = m[4] * gx + m[5] * gy + m[6] * gz;
        const float wz = m[8] * gx + m[9] * gy + m[10] * gz;
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
                    RasterImage& out, std::vector<float>* shared_zbuf = nullptr);

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
    const float* hva_;
    float cy_, sy_, cp_, sp_; // yaw/pitch 三角函数缓存
    int w_, h_;
    RasterImage& out_;
    std::vector<float>* zbuf_; // 共享深度缓冲（整模合成）；无共享时用自有
    std::vector<float> zbuf_own_;
};

VoxelRasterizer::VoxelRasterizer(const assets::VxlSection& section, const Projection& proj,
                                 const assets::VxlFile& vxl, const float hva[12], float yaw,
                                 float pitch, RasterImage& out, std::vector<float>* shared_zbuf)
    : section_(section), proj_(proj), pal_(vxl.palette()), hva_(hva),
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
    int r = static_cast<int>(pal_[color * 3] * 4 * shade);
    int g = static_cast<int>(pal_[color * 3 + 1] * 4 * shade);
    int b = static_cast<int>(pal_[color * 3 + 2] * 4 * shade);
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
        const float fy = y + 0.5f;
        for (int x = minx; x <= maxx; ++x) {
            const float fx = x + 0.5f;
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
    const Projection proj = make_projection(hva_m, view.yaw, view.pitch, view.scale);

    std::vector<Voxel> voxels;
    float min_x, max_x, min_y, max_y;
    collect_voxels(section, proj, voxels, min_x, max_x, min_y, max_y);
    if (voxels.empty()) return out;

    const int margin = static_cast<int>(view.scale * 4.0f);
    out.w = static_cast<int>(max_x - min_x) + margin * 2;
    out.h = static_cast<int>(max_y - min_y) + margin * 2;
    out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    // 底盘中心（体素盒 (sx/2, sy/2, 0) 经 HVA+旋转+投影）在光栅中的位置 = 世界锚点。
    // 以底盘中心为锚：单位绕自身中心转向，放置时贴合所在格中心（载具贴地语义）。
    if (anchor_x || anchor_y) {
        float ax, ay, ad;
        proj.project(static_cast<float>(section.sx) * 0.5f,
                     static_cast<float>(section.sy) * 0.5f, 0.0f, ax, ay, ad);
        if (anchor_x) *anchor_x = ax - min_x + margin;
        if (anchor_y) *anchor_y = ay - min_y + margin;
    }

    VoxelRasterizer rasterizer(section, proj, vxl, hva_m, view.yaw, view.pitch, out);
    rasterizer.run(voxels, -min_x + margin, -min_y + margin);
    return out;
}

RasterImage rasterize_voxel_model(const assets::VxlFile& vxl, const assets::HvaFile* hva,
                                  int hva_frame, const VoxelView& view, float* anchor_x,
                                  float* anchor_y) {
    RasterImage out;
    if (vxl.sections().empty()) return out;
    // 1) 各节投影/收集体素，求全局包围盒
    struct Sec {
        const assets::VxlSection* s;
        Projection proj;
        std::vector<Voxel> vx;
        float hva_m[12];
    };
    std::vector<Sec> secs;
    secs.reserve(vxl.sections().size());
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (const auto& section : vxl.sections()) {
        Sec sd;
        sd.s = &section;
        load_hva_matrix(hva, section, hva_frame, sd.hva_m);
        sd.proj = make_projection(sd.hva_m, view.yaw, view.pitch, view.scale);
        float a, b, c, d;
        collect_voxels(section, sd.proj, sd.vx, a, b, c, d);
        if (sd.vx.empty()) continue;
        min_x = std::min(min_x, a);
        max_x = std::max(max_x, b);
        min_y = std::min(min_y, c);
        max_y = std::max(max_y, d);
        secs.push_back(std::move(sd));
    }
    if (secs.empty()) return out;
    const int margin = static_cast<int>(view.scale * 4.0f);
    out.w = static_cast<int>(max_x - min_x) + margin * 2;
    out.h = static_cast<int>(max_y - min_y) + margin * 2;
    out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    // 锚点 = 主体节（体素最多，通常为模型本体）包围盒底中心经 HVA+旋转+投影
    const Sec* body = &secs[0];
    for (const auto& s : secs)
        if (s.vx.size() > body->vx.size()) body = &s;
    if (anchor_x || anchor_y) {
        float ax, ay, ad;
        body->proj.project(static_cast<float>(body->s->sx) * 0.5f,
                           static_cast<float>(body->s->sy) * 0.5f, 0.0f, ax, ay, ad);
        if (anchor_x) *anchor_x = ax - min_x + margin;
        if (anchor_y) *anchor_y = ay - min_y + margin;
    }
    // 2) 共享深度缓冲顺序绘制各节（跨节遮挡正确）
    std::vector<float> zbuf(static_cast<size_t>(out.w) * out.h, -1e30f);
    for (const auto& s : secs) {
        VoxelRasterizer rz(*s.s, s.proj, vxl, s.hva_m, view.yaw, view.pitch, out, &zbuf);
        rz.run(s.vx, -min_x + margin, -min_y + margin);
    }
    return out;
}

} // namespace ra2r::render
