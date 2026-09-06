// RA2R mapview 对象层实现（接口见 object_layer.h）
#include "ra2r/render/object_layer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "ra2r/assets/hva_file.h"
#include "ra2r/assets/shp_file.h"
#include "ra2r/assets/vxl_file.h"
#include "ra2r/core/ini_file.h"
#include "ra2r/render/palette_lut.h"
#include "ra2r/render/voxel_raster.h"

namespace ra2r::render {

namespace {

// 通用像素写入（带边界检查；alpha<255 时半透明混合——建造中建筑用）
void put_px(std::vector<uint8_t>& canvas, int bw, int bh, int x, int y, const uint8_t* src,
            uint8_t alpha = 255) {
    if (x < 0 || y < 0 || x >= bw || y >= bh || src[3] == 0) return;
    uint8_t* d = canvas.data() + (static_cast<size_t>(y) * bw + x) * 4;
    if (alpha >= 255) {
        d[0] = src[0];
        d[1] = src[1];
        d[2] = src[2];
        d[3] = 255;
        return;
    }
    const uint8_t ia = static_cast<uint8_t>(255 - alpha);
    d[0] = static_cast<uint8_t>((src[0] * alpha + d[0] * ia) / 255);
    d[1] = static_cast<uint8_t>((src[1] * alpha + d[1] * ia) / 255);
    d[2] = static_cast<uint8_t>((src[2] * alpha + d[2] * ia) / 255);
    d[3] = 255;
}

// SHP 帧 → RGBA（调色板 LUT，光照 0 级）
std::vector<uint8_t> shp_frame_rgba(const ra2r::assets::ShpFile& shp, int frame_i,
                                    const ra2r::render::PaletteLut& lut) {
    std::vector<uint8_t> idx;
    std::string err;
    if (!shp.decode_frame(frame_i, idx, &err)) return {};
    const auto& frame = shp.frame(frame_i);
    std::vector<uint8_t> rgba(static_cast<size_t>(frame.cx) * frame.cy * 4, 0);
    for (size_t p = 0; p < idx.size(); ++p) {
        const uint8_t c = idx[p];
        if (c == 0) continue;
        uint8_t r, g, b, a;
        lut.rgba(c, 0, r, g, b, a);
        uint8_t* d = rgba.data() + p * 4;
        d[0] = r;
        d[1] = g;
        d[2] = b;
        d[3] = 255;
    }
    return rgba;
}

} // namespace

ObjectRenderStats render_objects(const std::vector<PlacedObject>& objs,
                                 const UnitPaletteCfg& cfg,
                                 const ra2r::render::IsometricGrid& grid, const FileLoader& load,
                                 int bw, int bh, int ox, int oy,
                                 std::vector<uint8_t>& canvas, float obj_scale,
                                 ObjectRenderCache* cache) {
    ObjectRenderStats stats;
    // artmd 覆盖表：Image=（美术名）、Foundation=（地基尺寸）、Sequence=（序列节名）；
    // 有缓存时跨帧复用（INI 不随帧变化）
    ra2r::core::IniFile art;
    bool have_art = false;
    std::map<std::string, std::string> art_images;
    if (cache && cache->art_ready) {
        have_art = true;
        art_images = cache->art_images;
    } else {
        const auto* artraw = load("ARTMD.INI");
        if (artraw) {
            std::string err;
            if (art.parse(artraw->data(), artraw->size(), &err)) {
                have_art = true;
                for (const auto& sn : art.section_names()) {
                    const std::string img = art.get(sn, "Image", "");
                    if (!img.empty()) art_images[sn] = img;
                }
            }
        }
        // rulesmd 的 Image= 覆盖（步兵如 E1 → GI 在 rulesmd 定义）
        const auto* rraw = load("RULESMD.INI");
        if (rraw) {
            ra2r::core::IniFile r;
            std::string err;
            if (r.parse(rraw->data(), rraw->size(), &err)) {
                for (const auto& sn : r.section_names()) {
                    const std::string img = r.get(sn, "Image", "");
                    if (!img.empty()) art_images.emplace(sn, img);
                }
            }
        }
        if (cache) {
            cache->art_images = art_images;
            cache->art_ready = true;
        }
    }
    // 单位调色盘（剧场单位盘；建筑/步兵共用）
    ra2r::render::PaletteLut ulut;
    bool have_upal = false;
    {
        const auto* upal = load(cfg.unit_pal);
        if (upal && upal->size() >= 768) {
            ulut.build(upal->data());
            have_upal = true;
        }
    }
    // 对象列表统一按 (cx+cy, cx) 排序
    struct Obj {
        int cx, cy, depth;
        int kind; // 0=建筑 1=载具 2=步兵
        std::string id;
        uint8_t dir;
        uint8_t subcell;
        int height;
        int off_x, off_y; // 格内像素偏移（M3 平滑移动插值；建筑恒 0）
        uint8_t alpha;    // 透明度（建造中建筑半透明）
        int hp;           // 建筑血量（受损帧切换）
        float build_p;    // 建筑建造进度（<0 = 建成）
        int seq;          // 原始序号（确定性决胜：同格对象稳定排序）
    };
    std::vector<Obj> sorted;
    for (size_t i = 0; i < objs.size(); ++i) {
        const auto& o = objs[i];
        sorted.push_back({o.cx, o.cy, o.cx + o.cy, o.kind, o.id, o.dir, o.subcell, o.height,
                          o.off_x, o.off_y, o.alpha, o.hp, o.build_p, static_cast<int>(i)});
    }
    std::sort(sorted.begin(), sorted.end(), [](const Obj& a, const Obj& b) {
        if (a.cy != b.cy) return a.cy < b.cy; // 砖墙行序（后行盖前行）
        if (a.cx != b.cx) return a.cx < b.cx;
        return a.seq < b.seq; // 同格：按调用方给定顺序（确定性）
    });
    for (const Obj& o : sorted) {
        int px, py;
        grid.cell_to_pixel(o.cx, o.cy, px, py);
        const int cx_s = ox + px + grid.tile_w / 2 + o.off_x; // 格中心（包围盒原点 + 半瓦）
        const int cy_s = oy + py + grid.tile_h / 2 - o.height * kHeightLevelPx + o.off_y;
        if (o.kind == 1) {
            // 载具：VXL 体素（可选 HVA 帧 0）；底盘中心对齐单位格中心
            // 投影为 RA2 斜投影（pitch=0：体素 z 轴垂直屏幕，载具贴地不倾倒）
            // 美术名 = artmd/rulesmd Image=（如 AMCV → MCV），缺省用 id
            const auto& img_name = art_images.count(o.id) ? art_images[o.id] : o.id;
            // 体素光栅缓存（按 美术名|朝向|比例16；跨帧复用）
            const int scale16 = static_cast<int>(std::clamp(obj_scale, 0.1f, 4.0f) * 16.0f);
            const std::string vkey = img_name + '|' + std::to_string(o.dir) + '|' +
                                     std::to_string(scale16);
            const ObjectRenderCache::VoxelEntry* vent = nullptr;
            if (cache && cache->voxels.count(vkey)) {
                vent = &cache->voxels[vkey];
            } else {
                const auto* raw = load(img_name + ".VXL");
                if (!raw) {
                    ++stats.skipped;
                    continue;
                }
                ra2r::assets::VxlFile vxl;
                std::string err;
                if (!vxl.open(raw->data(), raw->size(), &err)) {
                    ++stats.skipped;
                    continue;
                }
                ra2r::assets::HvaFile hva;
                bool have_hva = false;
                if (const auto* hraw = load(img_name + ".HVA")) {
                    have_hva = hva.open(hraw->data(), hraw->size());
                }
                ra2r::render::VoxelView view;
                // 朝向：RA2 dir 0=右上(与格平行) 32=右(与水平线平行) 64=右下…，
                // 体素模型车头 = +x 轴（实测 TNKD 炮管向 +x 延伸：x+ 35.5 vs x− 21.5）
                // → yaw = dir/256·2π − π/2（dir0→270°→车头右上，8 向全表吻合）
                view.yaw = o.dir / 256.0f * 6.2831853f - 1.5707963f;
                view.pitch = 0.0f;
                view.scale = std::clamp(obj_scale, 0.1f, 4.0f);
                float ax = 0, ay = 0;
                const auto img = ra2r::render::rasterize_voxel_model(
                    vxl, have_hva ? &hva : nullptr, 0, view, &ax, &ay);
                ObjectRenderCache::VoxelEntry ent;
                ent.w = img.w;
                ent.h = img.h;
                ent.ax = static_cast<int>(ax);
                ent.ay = static_cast<int>(ay);
                ent.rgba = img.rgba;
                if (cache) {
                    vent = &cache->voxels.emplace(vkey, std::move(ent)).first->second;
                } else {
                    // 无缓存：直接绘制
                    for (int y = 0; y < ent.h; ++y) {
                        const uint8_t* s =
                            ent.rgba.data() + static_cast<size_t>(y) * ent.w * 4;
                        for (int x = 0; x < ent.w; ++x) {
                            if (s[x * 4 + 3])
                                put_px(canvas, bw, bh, cx_s - ent.ax + x, cy_s - ent.ay + y,
                                       s + x * 4);
                        }
                    }
                    ++stats.units;
                    continue;
                }
            }
            for (int y = 0; y < vent->h; ++y) {
                const uint8_t* s = vent->rgba.data() + static_cast<size_t>(y) * vent->w * 4;
                for (int x = 0; x < vent->w; ++x) {
                    if (s[x * 4 + 3])
                        put_px(canvas, bw, bh, cx_s - vent->ax + x, cy_s - vent->ay + y,
                               s + x * 4);
                }
            }
            ++stats.units;
        } else if (o.kind == 2) {
            // 步兵：美术名 = artmd/rulesmd Image=（缺省 id）；
            // 序列 = artmd [id] Sequence= → [seq] Guard=Start,Length,Stride
            //（3 参数 = 8 朝向、4 参数 = 1 朝向，OpenRA LegacySequenceImporter 语义）；
            // 朝向帧 = Start + F·Stride，F = (dir+16)/32 % 8（0=N..7=NW）
            const auto& img_name = art_images.count(o.id) ? art_images[o.id] : o.id;
            const auto* raw = load(img_name + ".SHP");
            if (!raw || !have_upal) {
                ++stats.skipped;
                continue;
            }
            ra2r::assets::ShpFile shp;
            std::string err;
            if (!shp.open(raw->data(), raw->size(), &err) || shp.frame_count() == 0) {
                ++stats.skipped;
                continue;
            }
            int seq_start = 0, seq_stride = 1;
            if (have_art) {
                // 序列节查找：artmd [id]，缺省回退美术名节（如 E1 → [GI]）
                const std::string art_sec = art.has_section(o.id) ? o.id : img_name;
                const std::string seq = art.get(art_sec, "Sequence", "");
                if (!seq.empty()) {
                    std::string guard = art.get(seq, "Guard", "");
                    if (guard.empty()) guard = art.get(seq, "Ready", "0,1,1");
                    int len = 1;
                    std::sscanf(guard.c_str(), "%d,%d,%d", &seq_start, &len, &seq_stride);
                }
                // 无 Sequence= 的步兵（动物/平民）默认帧 0..7 为朝向帧（stride 1）
            }
            const int facing = (o.dir + 16) / 32 % 8;
            const int frame_i =
                std::min<int>(shp.frame_count() - 1, seq_start + facing * seq_stride);
            // 步兵帧 RGBA 缓存（按 美术名|帧号；朝向帧跨帧复用）
            const std::string ikey = img_name + '|' + std::to_string(frame_i);
            ObjectRenderCache::BldEntry ient;
            bool have_ient = false;
            if (cache && cache->shp_frames.count(ikey)) {
                ient = cache->shp_frames[ikey];
                have_ient = true;
            } else {
                const auto rgba = shp_frame_rgba(shp, frame_i, ulut);
                if (rgba.empty()) {
                    ++stats.skipped;
                    continue;
                }
                const auto& frame = shp.frame(frame_i);
                ient.rgba = rgba;
                ient.cx = frame.cx;
                ient.cy = frame.cy;
                ient.fx = frame.x;
                ient.fy = frame.y;
                ient.canvas_w = shp.width();
                ient.canvas_h = shp.height();
                if (cache) cache->shp_frames.emplace(ikey, ient);
                have_ient = true;
            }
            if (!have_ient) {
                ++stats.skipped;
                continue;
            }
            // 格内 subcell 偏移（0=中心，1..4=四角近似）
            static const int kSubOff[5][2] = {{0, 0}, {0, -4}, {6, 2}, {-6, 2}, {0, 4}};
            const int sc = std::min<int>(o.subcell, 4);
            // 帧在画布内的位置参与放置（与建筑同语义：美术中心 = 格中心 + 帧中心 − 画布中心）
            const int bx =
                cx_s + ient.fx - ient.canvas_w / 2 + kSubOff[sc][0];
            const int by =
                cy_s + ient.fy - ient.canvas_h / 2 + kSubOff[sc][1];
            for (int y = 0; y < ient.cy; ++y) {
                const uint8_t* s = ient.rgba.data() + static_cast<size_t>(y) * ient.cx * 4;
                for (int x = 0; x < ient.cx; ++x) {
                    if (s[x * 4 + 3]) put_px(canvas, bw, bh, bx + x, by + y, s + x * 4);
                }
            }
            ++stats.infantry;
        } else {
            // 建筑：帧语义按 OpenRA ra2 ^Structure 约定（defaults.yaml）——
            //   idle=帧0、damaged-idle=帧1、make(建造)=帧2；阴影帧 = 3+i
            //   （帧 3/4/5，画在精灵之下）；建造中按 build_p 0..1 缩放 make 帧
            //   （原版"建筑生长"观感）；hp<128 → 受损帧。
            // 原版规则（ra2diy 实测）：本体位置 = 地基"最顶上尖的那一格" =
            // 地图存储的顶格 (cx,cy)，锚点 = 该格中心；精灵以画布中心对齐。
            const auto& img_name = art_images.count(o.id) ? art_images[o.id] : o.id;
            const auto* raw = load(img_name + ".SHP");
            if (!raw || !have_upal) {
                ++stats.skipped;
                continue;
            }
            ra2r::assets::ShpFile shp;
            std::string err;
            if (!shp.open(raw->data(), raw->size(), &err) || shp.frame_count() == 0) {
                ++stats.skipped;
                continue;
            }
            const int n = static_cast<int>(shp.frame_count());
            const bool constructing = o.build_p >= 0.0f;
            const int fi = constructing ? std::min(2, n - 1)
                                        : (o.hp < 128 && n > 1 ? std::min(1, n - 1) : 0);
            // 帧 RGBA 缓存（通用 SHP 帧缓存：美术名|帧号）
            const auto get_entry = [&](int f, ObjectRenderCache::BldEntry& ent) -> bool {
                const std::string key = img_name + '|' + std::to_string(f);
                if (cache && cache->shp_frames.count(key)) {
                    ent = cache->shp_frames[key];
                    return true;
                }
                const auto& frame = shp.frame(f);
                if (frame.cx == 0 || frame.cy == 0) return false;
                const auto rgba = shp_frame_rgba(shp, f, ulut);
                if (rgba.empty()) return false;
                ent.rgba = rgba;
                ent.cx = frame.cx;
                ent.cy = frame.cy;
                ent.fx = frame.x;
                ent.fy = frame.y;
                ent.canvas_w = shp.width();
                ent.canvas_h = shp.height();
                if (cache) cache->shp_frames.emplace(key, ent);
                return true;
            };
            const float ax = ox + o.cx * 60.0f + (o.cy & 1) * 30.0f + 30.0f;
            // y 锚点 = 顶格顶顶点（格中心再上移半格 15px，原版观感实测）
            const float ay = oy + o.cy * 15.0f - o.height * kHeightLevelPx;
            // 阴影帧（3+i）垫底；精灵帧置顶；建造中按进度最近邻缩放（底心锚定）
            const auto blit_entry = [&](const ObjectRenderCache::BldEntry& ent, float scale,
                                        uint8_t alpha) {
                const int dx = static_cast<int>(ent.cx * scale);
                const int dy = static_cast<int>(ent.cy * scale);
                const int bx =
                    static_cast<int>(std::lround(ax)) + ent.fx - ent.canvas_w / 2 +
                    (ent.cx - dx) / 2; // 底心锚定：水平居中
                const int by =
                    static_cast<int>(std::lround(ay)) + ent.fy - ent.canvas_h / 2 +
                    (ent.cy - dy); // 底部对齐
                for (int y = 0; y < dy; ++y) {
                    const int sy = y * ent.cy / dy;
                    const uint8_t* s =
                        ent.rgba.data() + static_cast<size_t>(sy) * ent.cx * 4;
                    for (int x = 0; x < dx; ++x) {
                        const int sx = x * ent.cx / dx;
                        if (s[sx * 4 + 3])
                            put_px(canvas, bw, bh, bx + x, by + y, s + sx * 4, alpha);
                    }
                }
            };
            ObjectRenderCache::BldEntry shadow, sprite;
            if (n > fi + 3 && get_entry(fi + 3, shadow)) {
                const float s0 = constructing ? std::clamp(0.25f + 0.75f * o.build_p, 0.25f,
                                                          1.0f)
                                              : 1.0f;
                blit_entry(shadow, s0, o.alpha);
            }
            if (get_entry(fi, sprite)) {
                const float s0 = constructing ? std::clamp(0.25f + 0.75f * o.build_p, 0.25f,
                                                          1.0f)
                                              : 1.0f;
                blit_entry(sprite, s0, o.alpha);
            }
            ++stats.buildings;
        }
    }
    return stats;
}

ShowcaseResult run_showcase(const std::vector<std::string>& vxl_names, const FileLoader& load,
                            int scale) {
    ShowcaseResult out;
    const int cols = 12;
    const int cell_w = 160, cell_h = 130;
    const int rows = (static_cast<int>(vxl_names.size()) + cols - 1) / cols;
    out.w = cols * cell_w;
    out.h = rows * cell_h;
    out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    // 深灰背景
    for (size_t i = 0; i < out.rgba.size() / 4; ++i) {
        uint8_t* d = out.rgba.data() + i * 4;
        d[0] = 30;
        d[1] = 30;
        d[2] = 36;
        d[3] = 255;
    }
    int idx = 0;
    for (const auto& name : vxl_names) {
        const auto* raw = load(name);
        if (!raw) {
            ++idx;
            continue;
        }
        ra2r::assets::VxlFile vxl;
        std::string err;
        if (!vxl.open(raw->data(), raw->size(), &err)) {
            ++idx;
            continue;
        }
        const std::string base = name.substr(0, name.size() - 4);
        ra2r::assets::HvaFile hva;
        bool have_hva = false;
        if (const auto* hraw = load(base + ".HVA")) {
            have_hva = hva.open(hraw->data(), hraw->size());
        }
        ra2r::render::VoxelView view;
        view.scale = std::clamp(scale, 1, 3);
        float ax = 0, ay = 0;
        const auto img = ra2r::render::rasterize_voxel_section(
            vxl, have_hva ? &hva : nullptr, 0, 0, view, &ax, &ay);
        const int gx = (idx % cols) * cell_w + (cell_w - img.w) / 2;
        const int gy = (idx / cols) * cell_h + (cell_h - img.h) / 2;
        for (int y = 0; y < img.h; ++y) {
            const uint8_t* s = img.rgba.data() + static_cast<size_t>(y) * img.w * 4;
            for (int x = 0; x < img.w; ++x) {
                if (s[x * 4 + 3]) put_px(out.rgba, out.w, out.h, gx + x, gy + y, s + x * 4);
            }
        }
        ++out.count;
        ++idx;
    }
    return out;
}

} // namespace ra2r::render
