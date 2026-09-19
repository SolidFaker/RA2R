// RA2R mapview 对象层实现（接口见 object_layer.h）
#include "ra2r/render/object_layer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "ra2r/assets/hva_file.h"
#include "ra2r/assets/shp_file.h"
#include "ra2r/assets/theater.h"
#include "ra2r/assets/vxl_file.h"
#include "ra2r/core/ini_file.h"
#include "ra2r/render/palette_lut.h"
#include "ra2r/render/voxel_raster.h"

namespace ra2r::render {

namespace {

// 通用像素写入（带边界检查；alpha<255 时半透明混合——建造中建筑 + 阴影索引 1）
void put_px(std::vector<uint8_t>& canvas, int bw, int bh, int x, int y, const uint8_t* src,
            uint8_t alpha = 255) {
    if (x < 0 || y < 0 || x >= bw || y >= bh || src[3] == 0) return;
    // 源 alpha（索引 1 = 阴影 140）与调用方 alpha（建造中淡入）相乘
    const int ea = static_cast<int>(src[3]) * alpha / 255;
    if (ea == 0) return;
    uint8_t* d = canvas.data() + (static_cast<size_t>(y) * bw + x) * 4;
    if (ea >= 255) {
        d[0] = src[0];
        d[1] = src[1];
        d[2] = src[2];
        d[3] = 255;
        return;
    }
    const uint8_t ia = static_cast<uint8_t>(255 - ea);
    d[0] = static_cast<uint8_t>((src[0] * ea + d[0] * ia) / 255);
    d[1] = static_cast<uint8_t>((src[1] * ea + d[1] * ia) / 255);
    d[2] = static_cast<uint8_t>((src[2] * ea + d[2] * ia) / 255);
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

// 序列第 4 参数（定向字母，如 Idle1=56,15,0,S）→ 朝向下标 0..7；未给返回 -1。
// 字母表与引擎朝向帧序一致：N,NE,E,SE,S,SW,W,NW（原版"步兵在该动画中/之后
// 所指方向"，ModEnc Infantry Animation Sequences）
int dir_letter_index(const char* s) {
    if (!s || !s[0]) return -1;
    char c0 = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    char c1 = s[1] ? static_cast<char>(std::toupper(static_cast<unsigned char>(s[1]))) : 0;
    if (c0 == 'N') return c1 == 'E' ? 1 : (c1 == 'W' ? 7 : 0);
    if (c0 == 'E') return 2;
    if (c0 == 'S') return c1 == 'E' ? 3 : (c1 == 'W' ? 5 : 4);
    if (c0 == 'W') return 6;
    return -1;
}

} // namespace

std::string resolve_art_name(const std::string& image, const std::string& theater,
                             const FileLoader& load) {
    if (image.size() < 2 || !load) return image;
    const auto has = [&](const std::string& n) { return load(n + ".SHP") != nullptr; };
    // 先换剧场代号（类型名第 2 字母是雪 'A'，直接按原名加载会拿到雪地美术），
    // 再回退通用 'G'，最后才用原名。
    const char codes[2] = {ra2r::assets::theater_code(theater), 'G'};
    for (char c : codes) {
        std::string v = image;
        v[1] = c;
        if (v != image && has(v)) return v;
    }
    return image;
}

ObjectRenderStats render_objects(const std::vector<PlacedObject>& objs,
                                 const UnitPaletteCfg& cfg,
                                 const ra2r::render::IsometricGrid& grid, const FileLoader& load,
                                 int bw, int bh, int ox, int oy,
                                 std::vector<uint8_t>& canvas, float obj_scale,
                                 ObjectRenderCache* cache) {
    ObjectRenderStats stats;
    // artmd 覆盖表：Image=（美术名）、Foundation=（地基尺寸）、Sequence=（序列节名）；
    // 有缓存时跨帧复用。缓存必须连**解析结果本体**一起存：只缓存 art_images 会让
    // 后续帧的 art 为空 → Buildup=/Walk=/Idle1= 等 artmd 键全部查不到（动画静默
    // 失效：建筑停 make 帧、步兵永远 Guard——曾把建造动画误判为"受损帧"）。
    ra2r::core::IniFile art_local;
    bool have_art = false;
    std::map<std::string, std::string> art_images;
    if (cache && cache->art_ready && cache->art_file) {
        have_art = true;
        art_images = cache->art_images;
    } else {
        const auto* artraw = load("ARTMD.INI");
        if (artraw) {
            std::string err;
            if (art_local.parse(artraw->data(), artraw->size(), &err)) {
                have_art = true;
                for (const auto& sn : art_local.section_names()) {
                    const std::string img = art_local.get(sn, "Image", "");
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
            cache->art_file = std::make_shared<ra2r::core::IniFile>(std::move(art_local));
            cache->art_images = art_images;
            cache->art_ready = true;
        }
    }
    const ra2r::core::IniFile& art =
        (cache && cache->art_file) ? *cache->art_file : art_local;
    // 单位调色盘（剧场单位盘；建筑/步兵共用）+ 阵营色重映射 LUT（按 remap 下标缓存）
    const std::vector<uint8_t>* upal = load(cfg.unit_pal);
    const bool have_upal = upal && upal->size() >= 768;
    std::map<int, ra2r::render::PaletteLut> luts;
    const auto lut_for = [&](int remap) -> const ra2r::render::PaletteLut* {
        if (!have_upal) return nullptr;
        const auto it = luts.find(remap);
        if (it != luts.end()) return &it->second;
        const uint8_t* rm = nullptr;
        if (remap > 0 && cfg.house_ramps && remap <= static_cast<int>(cfg.house_ramps->size()))
            rm = (*cfg.house_ramps)[static_cast<size_t>(remap - 1)].rgb[0];
        ra2r::render::PaletteLut& l = luts[remap];
        l.build(upal->data(), rm);
        return &l;
    };
    const auto ramp_ptr = [&](int remap) -> const uint8_t* {
        if (remap <= 0 || !cfg.house_ramps ||
            remap > static_cast<int>(cfg.house_ramps->size()))
            return nullptr;
        return (*cfg.house_ramps)[static_cast<size_t>(remap - 1)].rgb[0];
    };
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
        int remap;        // 阵营色重映射下标（0 = 无）
        int seq;          // 原始序号（确定性决胜：同格对象稳定排序）
        int build_ticks;  // 建造已用逻辑帧（Buildup 动画时长用）
        int build_total;  // 建造总逻辑帧
        uint8_t moving;   // 步兵行进中（Walk 序列）
        uint32_t anim_clock; // 动画时钟（逻辑帧）
        uint8_t idle_kind;   // 步兵 idle 动作（1=Idle1 2=Idle2 0=无）
        uint32_t idle_start; // idle 动作触发逻辑帧
    };
    std::vector<Obj> sorted;
    for (size_t i = 0; i < objs.size(); ++i) {
        const auto& o = objs[i];
        sorted.push_back({o.cx, o.cy, o.cx + o.cy, o.kind, o.id, o.dir, o.subcell, o.height,
                          o.off_x, o.off_y, o.alpha, o.hp, o.build_p, o.remap,
                          static_cast<int>(i), o.build_ticks, o.build_total, o.moving,
                          o.anim_clock, o.idle_kind, o.idle_start});
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
            // 体素光栅缓存（按 美术名|朝向|比例16|阵营色；跨帧复用）
            const int scale16 = static_cast<int>(std::clamp(obj_scale, 0.1f, 4.0f) * 16.0f);
            const std::string vkey = img_name + '|' + std::to_string(o.dir) + '|' +
                                     std::to_string(scale16) + '|' + std::to_string(o.remap);
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
                // 炮塔/炮管是独立体素模型（原版命名：<Image>TUR.VXL、<Image>BARL.VXL，
                // 如 GTNK + GTNKTUR + GTNKBARL），三件同坐标系，必须一起深度合成，
                // 否则坦克只剩底盘、炮塔悬空缺失。
                ra2r::assets::VxlFile tvxl, bvxl;
                ra2r::assets::HvaFile thva, bhva;
                bool have_tur = false, have_barl = false, have_thva = false, have_bhva = false;
                if (const auto* traw = load(img_name + "TUR.VXL")) {
                    have_tur = tvxl.open(traw->data(), traw->size(), &err);
                    if (have_tur)
                        if (const auto* thraw = load(img_name + "TUR.HVA"))
                            have_thva = thva.open(thraw->data(), thraw->size());
                }
                if (const auto* braw = load(img_name + "BARL.VXL")) {
                    have_barl = bvxl.open(braw->data(), braw->size(), &err);
                    if (have_barl)
                        if (const auto* bhraw = load(img_name + "BARL.HVA"))
                            have_bhva = bhva.open(bhraw->data(), bhraw->size());
                }
                ra2r::render::VoxelView view;
                // 朝向：RA2 dir 0=右上(与格平行) 32=右(与水平线平行) 64=右下…，
                // 体素模型车头 = +x 轴（实测 TNKD 炮管向 +x 延伸：x+ 35.5 vs x− 21.5）
                // → yaw = dir/256·2π − π/2（dir0→270°→车头右上，8 向全表吻合）
                view.yaw = o.dir / 256.0f * 6.2831853f - 1.5707963f;
                view.pitch = 0.0f;
                view.scale = std::clamp(obj_scale, 0.1f, 4.0f);
                view.remap = ramp_ptr(o.remap);
                float ax = 0, ay = 0;
                const ra2r::render::VoxelPart parts[3] = {
                    {&vxl, have_hva ? &hva : nullptr, 0},
                    {have_tur ? &tvxl : nullptr, have_thva ? &thva : nullptr, 0},
                    {have_barl ? &bvxl : nullptr, have_bhva ? &bhva : nullptr, 0}};
                float bx = 0, by = 0;
                const auto img = ra2r::render::rasterize_voxel_parts(
                    parts, 1 + (have_tur ? 1u : 0u) + (have_barl ? 1u : 0u), view, &ax, &ay,
                    &bx, &by);
                ObjectRenderCache::VoxelEntry ent;
                ent.w = img.w;
                ent.h = img.h;
                ent.ax = static_cast<int>(ax);
                ent.ay = static_cast<int>(ay);
                ent.bx = static_cast<int>(bx);
                ent.by = static_cast<int>(by);
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
            // 动画序列（artmd Sequence= → [seq] 节；格式 Start,Length,Stride[,定向]）：
            //   Guard/Ready = 站立（Length=1 的朝向表）；Walk = 行走循环
            //   （每朝向连续 Length 帧依次播放，Stride = 朝向间帧步长）；
            //   Idle1/Idle2 = 静止动作（sim 按 IdleActionFrequency 触发；单朝向
            //   Stride=0 时第 4 参数给出动画所绘方向）。
            int walk_start = -1, walk_len = 0, walk_stride = 1;
            int idle1_start = -1, idle1_len = 0, idle1_stride = 0, idle1_dir = -1;
            int idle2_start = -1, idle2_len = 0, idle2_stride = 0, idle2_dir = -1;
            if (have_art) {
                // 序列节查找：artmd [id]，缺省回退美术名节（如 E1 → [GI]）
                const std::string art_sec = art.has_section(o.id) ? o.id : img_name;
                const std::string seq = art.get(art_sec, "Sequence", "");
                if (!seq.empty()) {
                    std::string guard = art.get(seq, "Guard", "");
                    if (guard.empty()) guard = art.get(seq, "Ready", "0,1,1");
                    int glen = 1;
                    std::sscanf(guard.c_str(), "%d,%d,%d", &seq_start, &glen, &seq_stride);
                    const std::string walk = art.get(seq, "Walk", "");
                    if (!walk.empty())
                        std::sscanf(walk.c_str(), "%d,%d,%d", &walk_start, &walk_len,
                                    &walk_stride);
                    const auto parse_idle = [&](const char* key, int& start, int& len,
                                                int& stride, int& dir) {
                        const std::string v = art.get(seq, key, "");
                        if (v.empty()) return;
                        char letter[4] = {0, 0, 0, 0};
                        std::sscanf(v.c_str(), "%d,%d,%d,%3s", &start, &len, &stride, letter);
                        dir = dir_letter_index(letter);
                    };
                    parse_idle("Idle1", idle1_start, idle1_len, idle1_stride, idle1_dir);
                    parse_idle("Idle2", idle2_start, idle2_len, idle2_stride, idle2_dir);
                }
                // 无 Sequence= 的步兵（动物/平民）默认帧 0..7 为朝向帧（stride 1）
            }
            const int facing = (o.dir + 16) / 32 % 8;
            int frame_i = seq_start + facing * seq_stride;
            if (o.moving && walk_start >= 0 && walk_len > 0) {
                // 行走相位：原版硬编码播放速率 = 每 3 逻辑帧推进 1 动画帧
                // （ModEnc Infantry Animation Sequences：Walk=3 @15fps 逻辑帧；
                // 旧实现 2/3 帧/逻辑帧 = 4.5 倍速已修）
                const uint32_t phase =
                    (o.anim_clock / 3) % static_cast<uint32_t>(walk_len);
                frame_i = walk_start + facing * walk_stride + static_cast<int>(phase);
            } else if (o.idle_kind) {
                // 静止 idle 动作（sim 触发）：Idle1/Idle2 段按同速率 3 播放；
                // 段内帧播完即回 Guard（busy 期由 sim 控制）
                const int i_start = o.idle_kind == 2 ? idle2_start : idle1_start;
                const int i_len = o.idle_kind == 2 ? idle2_len : idle1_len;
                const int i_stride = o.idle_kind == 2 ? idle2_stride : idle1_stride;
                const int i_dir = o.idle_kind == 2 ? idle2_dir : idle1_dir;
                if (i_start >= 0 && i_len > 0) {
                    const uint32_t t =
                        o.anim_clock >= o.idle_start ? o.anim_clock - o.idle_start : 0;
                    if (t < static_cast<uint32_t>(i_len) * 3) {
                        // Stride>0 = 多朝向段（罕见）：基址 = Start + 朝向·Stride，
                        // 朝向取第 4 参数字母（缺省 = 单位当前朝向）；
                        // Stride=0 = 单朝向段，帧序即 Start..Start+Length-1
                        const int base =
                            i_stride > 0
                                ? i_start + (i_dir >= 0 ? i_dir : facing) * i_stride
                                : i_start;
                        frame_i = base + static_cast<int>(t / 3);
                    }
                }
            }
            frame_i = std::clamp(frame_i, 0, static_cast<int>(shp.frame_count()) - 1);
            // 步兵帧 RGBA 缓存（按 美术名|帧号|阵营色；朝向帧跨帧复用）
            const std::string ikey =
                img_name + '|' + std::to_string(frame_i) + '|' + std::to_string(o.remap);
            ObjectRenderCache::BldEntry ient;
            bool have_ient = false;
            if (cache && cache->shp_frames.count(ikey)) {
                ient = cache->shp_frames[ikey];
                have_ient = true;
            } else {
                const auto rgba = shp_frame_rgba(shp, frame_i, *lut_for(o.remap));
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
            // 美术名经 NewTheater 回退链解析（GTGCAN → GAGCAN.SHP 等）。
            const auto& img_name = resolve_art_name(
                art_images.count(o.id) ? art_images[o.id] : o.id, cfg.theater, load);
            // 建造/展开动画（artmd Buildup=，如 GACNSTMK/YACNSTMK）：建造中优先播放
            // 该 SHP 的逐帧序列（原版"建筑拔地而起"），无 Buildup 才退回 make 帧缩放。
            const std::string art_sec = have_art && art.has_section(o.id) ? o.id : img_name;
            const std::string buildup_name =
                have_art ? art.get(art_sec, "Buildup", "") : std::string();
            const bool constructing = o.build_p >= 0.0f;
            ra2r::assets::ShpFile bshp;
            bool have_buildup = false;
            if (constructing && !buildup_name.empty()) {
                if (const auto* braw = load(buildup_name + ".SHP")) {
                    std::string berr;
                    have_buildup = bshp.open(braw->data(), braw->size(), &berr) &&
                                   bshp.frame_count() > 0;
                }
            }
            const auto* raw = load(img_name + ".SHP");
            if (!have_upal || (!raw && !have_buildup)) {
                ++stats.skipped;
                continue;
            }
            ra2r::assets::ShpFile shp;
            std::string err;
            if (raw && (!shp.open(raw->data(), raw->size(), &err) || shp.frame_count() == 0)) {
                if (!have_buildup) {
                    ++stats.skipped;
                    continue;
                }
            }
            const int n = static_cast<int>(shp.frame_count());
            const int fi = constructing ? std::min(2, n - 1)
                                        : (o.hp < 128 && n > 1 ? std::min(1, n - 1) : 0);
            // 帧 RGBA 缓存（通用 SHP 帧缓存：美术名|帧号|阵营色）
            const auto entry_of = [&](const ra2r::assets::ShpFile& s, const std::string& sname,
                                      int f, int remap, ObjectRenderCache::BldEntry& ent) -> bool {
                const std::string key =
                    sname + '|' + std::to_string(f) + '|' + std::to_string(remap);
                if (cache && cache->shp_frames.count(key)) {
                    ent = cache->shp_frames[key];
                    return true;
                }
                if (f < 0 || f >= static_cast<int>(s.frame_count())) return false;
                const auto& frame = s.frame(f);
                if (frame.cx == 0 || frame.cy == 0) return false;
                const auto rgba = shp_frame_rgba(s, f, *lut_for(remap));
                if (rgba.empty()) return false;
                ent.rgba = rgba;
                ent.cx = frame.cx;
                ent.cy = frame.cy;
                ent.fx = frame.x;
                ent.fy = frame.y;
                ent.canvas_w = s.width();
                ent.canvas_h = s.height();
                if (cache) cache->shp_frames.emplace(key, ent);
                return true;
            };
            const auto get_entry = [&](int f, ObjectRenderCache::BldEntry& ent) -> bool {
                return entry_of(shp, img_name, f, o.remap, ent);
            };
            const float ax = ox + o.cx * 60.0f + (o.cy & 1) * 30.0f + 30.0f;
            // y 锚点 = 顶格顶顶点（格中心再上移半格 15px，原版观感实测）
            const float ay = oy + o.cy * 15.0f - o.height * kHeightLevelPx;
            // 阴影帧垫底、精灵帧置顶（scale 恒 1；建造生长由 Buildup 动画承担）
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
            // ── 建造/展开动画：Buildup SHP（前半建造帧、后半同数阴影帧）──
            // 原版语义（ModEnc BuildupTime）：动画在放置后按 [General] BuildupTime
            // 平均分钟数播完一遍（YR = .06 → 3.6s = 54 逻辑帧；stage 已把该值折算
            // 为 build_total），与 Buildup 帧数无关；播完后建筑完工（无 Buildup 的
            // 建筑原版根本不进入建造状态）。旧实现帧按 build_p 摊满工期
            //（GAPOWR 25 帧动画被拉成 27s）已修；固定 45 帧的近似也已替换。
            const int b_ticks = o.build_ticks;
            const int b_total = o.build_total > 0 ? o.build_total : kBuildupTicks;
            if (have_buildup && (!raw || b_ticks < b_total)) {
                const int bn = static_cast<int>(bshp.frame_count());
                const int sh = ra2r::assets::shp_shadow_start(bshp);
                const int make_n = sh > 0 ? sh : bn; // 建造段帧数（无阴影段则全部）
                const int bf = (!raw) // 无本体 SHP（如 GADUMY）：始终末帧
                                   ? make_n - 1
                                   : std::clamp(b_ticks * make_n / b_total, 0, make_n - 1);
                ObjectRenderCache::BldEntry bshadow, bsprite;
                if (sh > 0 && sh + bf < bn &&
                    entry_of(bshp, buildup_name, sh + bf, o.remap, bshadow))
                    blit_entry(bshadow, 1.0f, o.alpha); // 阴影帧垫底
                if (entry_of(bshp, buildup_name, bf, o.remap, bsprite))
                    blit_entry(bsprite, 1.0f, o.alpha);
                ++stats.buildings;
                continue;
            }
            ObjectRenderCache::BldEntry shadow, sprite;
            // 阴影帧起点：本体后半全为阴影/空帧时 = n/2（6 帧建筑 = 3、8 帧 = 4，
            // 如 CAOILD/CAHOSP 的 8 帧本体）；不再用固定 +3——8 帧本体 +3 会取到
            // 建造（彩色脚手架）帧，画成建筑下方的彩色乱图。
            int sh_start = -1;
            if (cache) {
                const auto it = cache->body_shadow.find(img_name);
                if (it != cache->body_shadow.end()) {
                    sh_start = it->second;
                } else {
                    sh_start = ra2r::assets::shp_shadow_start(shp);
                    cache->body_shadow.emplace(img_name, sh_start);
                }
            } else {
                sh_start = ra2r::assets::shp_shadow_start(shp);
            }
            if (sh_start >= 0 && sh_start + fi < n && get_entry(sh_start + fi, shadow)) {
                blit_entry(shadow, 1.0f, o.alpha);
            }
            if (get_entry(fi, sprite)) {
                blit_entry(sprite, 1.0f, o.alpha);
            }
            // 建筑体素炮塔：由 stage 侧 draw_bld_turret_voxel 统一绘制
            // （rulesmd Turret=/TurretAnim=/TurretAnimX/Y 驱动 + HVA 动画时钟）。
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
        // 炮塔/炮管独立体素（<Image>TUR.VXL / <Image>BARL.VXL）一并合成，
        // 否则陈列里只有车体
        ra2r::assets::VxlFile tvxl, bvxl;
        ra2r::assets::HvaFile thva, bhva;
        bool have_tur = false, have_barl = false, have_thva = false, have_bhva = false;
        if (const auto* traw = load(base + "TUR.VXL")) {
            have_tur = tvxl.open(traw->data(), traw->size(), &err);
            if (have_tur)
                if (const auto* thraw = load(base + "TUR.HVA"))
                    have_thva = thva.open(thraw->data(), thraw->size());
        }
        if (const auto* braw = load(base + "BARL.VXL")) {
            have_barl = bvxl.open(braw->data(), braw->size(), &err);
            if (have_barl)
                if (const auto* bhraw = load(base + "BARL.HVA"))
                    have_bhva = bhva.open(bhraw->data(), bhraw->size());
        }
        ra2r::render::VoxelView view;
        view.scale = std::clamp(scale, 1, 3);
        float ax = 0, ay = 0;
        const ra2r::render::VoxelPart parts[3] = {
            {&vxl, have_hva ? &hva : nullptr, 0},
            {have_tur ? &tvxl : nullptr, have_thva ? &thva : nullptr, 0},
            {have_barl ? &bvxl : nullptr, have_bhva ? &bhva : nullptr, 0}};
        const auto img = ra2r::render::rasterize_voxel_parts(
            parts, 1 + (have_tur ? 1u : 0u) + (have_barl ? 1u : 0u), view, &ax, &ay);
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
