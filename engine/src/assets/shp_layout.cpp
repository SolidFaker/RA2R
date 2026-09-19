// RA2R — SHP 分段布局自判实现（接口见 shp_layout.h）
#include "ra2r/assets/shp_layout.h"

#include <algorithm>
#include <vector>

namespace ra2r::assets {

namespace {
// 帧像素贴到画布索引图（0 = 透明），用于比较两帧的形状差异
bool decode_canvas(const ShpFile& shp, int f, std::vector<uint8_t>& out) {
    std::vector<uint8_t> px;
    if (!shp.decode_frame(f, px)) return false;
    const auto& fr = shp.frame(f);
    out.assign(static_cast<size_t>(shp.width()) * shp.height(), 0);
    for (uint32_t y = 0; y < fr.cy; ++y) {
        for (uint32_t x = 0; x < fr.cx; ++x) {
            const uint8_t c = px[static_cast<size_t>(y) * fr.cx + x];
            if (!c) continue;
            const int cx = static_cast<int>(fr.x) + static_cast<int>(x);
            const int cy = static_cast<int>(fr.y) + static_cast<int>(y);
            if (cx < 0 || cy < 0 || cx >= static_cast<int>(shp.width()) ||
                cy >= static_cast<int>(shp.height()))
                continue;
            out[static_cast<size_t>(cy) * shp.width() + cx] = c;
        }
    }
    return true;
}

// 两帧差异率 = 不同像素数 / max(墨迹像素数)
double frame_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t diff = 0, ia = 0, ib = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i]) ++ia;
        if (b[i]) ++ib;
        if (a[i] != b[i]) ++diff;
    }
    const size_t m = std::max(ia, ib);
    return m ? static_cast<double>(diff) / static_cast<double>(m) : 0.0;
}

// 帧区间 [from, to) 是否全为阴影/空帧
bool all_shadow(const ShpFile& shp, int from, int to) {
    for (int f = from; f < to; ++f) {
        const char k = shp_frame_kind(shp, f);
        if (k == 'C' || k == '?') return false;
    }
    return true;
}
} // namespace

char shp_frame_kind(const ShpFile& shp, int frame) {
    std::vector<uint8_t> px;
    if (!shp.decode_frame(frame, px)) return '?';
    bool any = false, only1 = true;
    for (uint8_t c : px) {
        if (!c) continue;
        any = true;
        if (c != 1) {
            only1 = false;
            break;
        }
    }
    return !any ? 'E' : (only1 ? 'S' : 'C');
}

ShpLayout shp_layout(const ShpFile& shp) {
    ShpLayout out;
    const int n = static_cast<int>(shp.frame_count());
    out.frames = n;
    if (n <= 0) return out;
    if (n < 2) {
        out.seg_len = n;
        return out;
    }
    // 阴影段（后半）必须全为阴影/空帧，否则整段视为一个循环动画
    if (!all_shadow(shp, n / 2, n)) {
        out.seg_len = n;
        return out;
    }
    if (n % 4 == 0) {
        std::vector<uint8_t> f0, fq, fh;
        if (decode_canvas(shp, 0, f0) && decode_canvas(shp, n / 4 - 1, fq) &&
            decode_canvas(shp, n / 2 - 1, fh)) {
            const double seam4 = frame_diff(fq, f0); // L=n/4 时空闲段接缝
            const double seam2 = frame_diff(fh, f0); // L=n/2 时接缝
            if (seam4 <= seam2) {
                out.seg_len = n / 4;
                out.has_damaged = true;
                out.shadow_start = n / 2;
                out.shadow_len = n / 2;
                return out;
            }
        }
    }
    out.seg_len = n / 2;
    out.shadow_start = n / 2;
    out.shadow_len = n / 2;
    return out;
}

int shp_shadow_start(const ShpFile& shp) {
    const int n = static_cast<int>(shp.frame_count());
    if (n < 2 || n % 2 != 0) return -1;
    return all_shadow(shp, n / 2, n) ? n / 2 : -1;
}

} // namespace ra2r::assets
