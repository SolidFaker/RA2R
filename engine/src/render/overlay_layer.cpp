// RA2R — 地图装饰层渲染实现（接口见 overlay_layer.h）
#include "ra2r/render/overlay_layer.h"

#include <algorithm>
#include <cstring>

#include "ra2r/assets/shp_file.h"
#include "ra2r/assets/theater.h"
#include "ra2r/render/terrain_tile.h"

namespace ra2r::render {

char wall_theater_letter(const std::string& theater) {
    // 围墙/沙袋/栅栏的前缀字母与 NewTheater 同一张表
    // （GAWALL=雪、GTWALL=温和、GUSAND=城市、GDSAND=沙漠、GLSAND=月球、GNSAND=新城市）
    return ra2r::assets::theater_code(theater);
}

std::string overlay_art_name(const std::string& overlay_type_name, const std::string& ext,
                             char theater_letter) {
    if (overlay_type_name == "BRIDGE1" || overlay_type_name == "BRIDGE2")
        return "BRIDGE." + ext;
    // 围墙类 → 剧场前缀墙 SHP（OpenRA UseTilesetCode 语义；缺前缀变体时回退基名）
    if (overlay_type_name == "GAWALL") return std::string("G") + theater_letter + "WALL.SHP";
    if (overlay_type_name == "NAWALL") return std::string("N") + theater_letter + "WALL.SHP";
    if (overlay_type_name == "GASAND") return std::string("G") + theater_letter + "SAND.SHP";
    if (overlay_type_name == "CAFNCB") return std::string("C") + theater_letter + "FNCB.SHP";
    if (overlay_type_name == "CAFNCW") return std::string("C") + theater_letter + "FNCW.SHP";
    if (overlay_type_name == "CAFNCP") return std::string("C") + theater_letter + "FNCP.SHP";
    if (overlay_type_name == "CAKRMW") return std::string("C") + theater_letter + "KRMW.SHP";
    if (overlay_type_name == "GAFWLL") return "GAFWLL.SHP";
    if (overlay_type_name == "YAWALL") return "YAWALL.SHP";
    return overlay_type_name + "." + ext;
}

// 桥类硬编码艺术表（原版游戏内 OverlayType 表；rulesmd [OverlayTypes] 名是编辑器
// 标签，与绘制艺术偏移——木桥 -3、混凝土桥 -4）：
//   木桥 74-101 → LOBRDG01-28（74=桥身1 … 101=死桥占位）
//   木坡副本 122-125 → LOBRDG19/21/23/25（SE/NW/NE/SW）
//   混凝土桥 205-232 → LOBRDB01-28（205-208="/"桥身四变体、209-211="/"损毁、
//     212-213="/"断桥坡、214-217="\"桥身四变体、218-220="\"损毁、221-222="\"断桥坡、
//     223-224=SE坡、225-226=NW坡、227-228=NE坡、229-230=SW坡、231-232=死桥占位）
//   混凝土坡副本 233-236 → LOBRDB19/21/23/25
//   高架桥面 24/25 → BRIDGE（帧=OverlayData 0-17，0-8="/"、9-17="\"）
//   高架木桥基 237/238 → BRIDGB（帧=OverlayData）、239 → BRIDGE
std::string bridge_art_name(int overlay_type) {
    if (overlay_type >= 74 && overlay_type <= 101) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "LOBRDG%02d", overlay_type - 73);
        return buf;
    }
    if (overlay_type >= 122 && overlay_type <= 125) {
        static const int kRamps[4] = {19, 21, 23, 25};
        char buf[16];
        std::snprintf(buf, sizeof buf, "LOBRDG%02d", kRamps[overlay_type - 122]);
        return buf;
    }
    if (overlay_type >= 205 && overlay_type <= 232) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "LOBRDB%02d", overlay_type - 204);
        return buf;
    }
    if (overlay_type >= 233 && overlay_type <= 236) {
        static const int kRamps[4] = {19, 21, 23, 25};
        char buf[16];
        std::snprintf(buf, sizeof buf, "LOBRDB%02d", kRamps[overlay_type - 233]);
        return buf;
    }
    if (overlay_type >= 237 && overlay_type <= 239)
        return overlay_type <= 238 ? "BRIDGB" : "BRIDGE";
    // 高架桥面 24/25（rulesmd 名为 DUMMY15/BRIDGE1，实际都是 BRIDGE 条带；
    // 26 = 苏军围墙 NAWALL，见上方硬编码表）
    if (overlay_type >= 24 && overlay_type <= 25) return "BRIDGE";
    return "";
}

std::string overlay_type_art_name(int overlay_type, const std::string& rulesmd_name,
                                  const std::string& ext, char theater_letter) {
    // 原版硬编码覆盖物表（YR 类型号；RULESMD 名与绘制艺术不符——
    // 实测 atwar 战俘笼 = 241 画围栏艺术，242 = 水木箱，243 = 月球碎石无艺术）
    switch (overlay_type) {
        case 0: return std::string("G") + theater_letter + "SAND.SHP";    // 沙袋围墙（rulesmd 无此表项）
        case 2: return std::string("G") + theater_letter + "WALL.SHP";    // 盟军围墙（rulesmd 名 CYCL 是标签）
        case 26: return std::string("N") + theater_letter + "WALL.SHP";   // 苏军围墙（rulesmd 名 BRIDGE2 是标签）
        case 203: return std::string("C") + theater_letter + "FNCB.SHP";  // 黑/绿栅栏（rulesmd 名 FENCE20）
        case 204: return std::string("C") + theater_letter + "FNCW.SHP";  // 白栅栏（rulesmd 名 FENCE21）
        case 240: return std::string("C") + theater_letter + "KRMW.SHP";  // 克里姆林宫围墙（rulesmd 名 LOBRDGB4）
        case 241:
        case 245: return std::string("C") + theater_letter + "FNCP.SHP"; // 战俘笼/监狱围栏
        case 242:
        case 246: return "WCRATE." + ext;                                // 水木箱
        case 243: return "YAWALL.SHP";                                   // 尤里围墙（rulesmd 名 RUBBLE_OVERLAY）
        case 244: return std::string("C") + theater_letter + "KRMW.SHP"; // 克里姆林墙
        case 247: return "GAFWLL.SHP";                                   // 德军围墙
        default: break;
    }
    // 桥类：原版按游戏内表取艺术（rulesmd 名偏移，实测验证——
    // test1.mpr "\" 三片 227/215/229 → LOBRDB23/11/25；
    // test2.yrm "/" 三片 225/206/223 → LOBRDB21/02/19）
    if (const std::string bridge = bridge_art_name(overlay_type); !bridge.empty())
        return bridge + "." + ext;
    return overlay_art_name(rulesmd_name, ext, theater_letter);
}

bool is_resource_art(const std::string& art) {
    // 矿石（TIB01..20 及其剧场变体 TIB2_*/TIB3_*）与宝石（GEM01..12）；
    // TIBTRE*（泰伯利亚树）与普通树木同走剧场地形盘。
    if (art.rfind("TIB", 0) == 0) return art.rfind("TIBTRE", 0) != 0;
    return art.rfind("GEM", 0) == 0;
}

bool is_unit_palette_art(const std::string& art) {
    // 墙/围栏类 SHP：artmd 无 Theater=yes（仅 NewTheater=yes），原版用单位盘
    static const char* kSuffixes[] = {"WALL.SHP", "FNCB.SHP", "FNCW.SHP", "FNCP.SHP",
                                      "KRMW.SHP", "GAFWLL.SHP", "YAWALL.SHP", "SAND.SHP"};
    for (const char* s : kSuffixes) {
        const size_t l = std::strlen(s);
        if (art.size() >= l && art.compare(art.size() - l, l, s) == 0) return true;
    }
    return false;
}

namespace {

// 通用像素写入（带边界检查；src alpha < 255 时按 alpha 混合——索引 1 = 阴影）
void put_px(std::vector<uint8_t>& canvas, int bw, int bh, int x, int y, const uint8_t* src) {
    if (x < 0 || y < 0 || x >= bw || y >= bh || src[3] == 0) return;
    uint8_t* d = canvas.data() + (static_cast<size_t>(y) * bw + x) * 4;
    if (src[3] >= 255) {
        d[0] = src[0];
        d[1] = src[1];
        d[2] = src[2];
        d[3] = 255;
        return;
    }
    const uint8_t a = src[3];
    const uint8_t ia = static_cast<uint8_t>(255 - a);
    d[0] = static_cast<uint8_t>((src[0] * a + d[0] * ia) / 255);
    d[1] = static_cast<uint8_t>((src[1] * a + d[1] * ia) / 255);
    d[2] = static_cast<uint8_t>((src[2] * a + d[2] * ia) / 255);
    d[3] = 255;
}

// TEM 瓦片帧 → RGBA（剧场地形盘）
std::vector<uint8_t> tmp_frame_rgba(const ra2r::render::TerrainTile& t, int frame_i,
                                    const PaletteLut& lut) {
    const auto& f = t.frame(frame_i);
    std::vector<uint8_t> rgba(static_cast<size_t>(f.bounds_w) * f.bounds_h * 4, 0);
    for (size_t p = 0; p < f.pixels.size(); ++p) {
        const uint8_t idx = f.pixels[p];
        if (idx == 0) continue;
        uint8_t r, g, b, a;
        lut.rgba(idx, 0, r, g, b, a);
        uint8_t* d = rgba.data() + p * 4;
        d[0] = r;
        d[1] = g;
        d[2] = b;
        d[3] = 255;
    }
    return rgba;
}

} // namespace

void render_map_decor(const std::vector<MapDecorObject>& objs, const PaletteLut& terrain_lut,
                      const PaletteLut& resource_lut, const PaletteLut& unit_lut,
                      const IsometricGrid& grid, const FileLoader& load, int bw, int bh, int ox,
                      int oy, std::vector<uint8_t>& canvas) {
    // 按 (cy, cx) 行序绘制（与对象层一致，后行盖前行）
    std::vector<const MapDecorObject*> sorted;
    sorted.reserve(objs.size());
    for (const auto& o : objs) sorted.push_back(&o);
    std::sort(sorted.begin(), sorted.end(), [](const MapDecorObject* a, const MapDecorObject* b) {
        if (a->cy != b->cy) return a->cy < b->cy;
        return a->cx < b->cx;
    });
    for (const MapDecorObject* o : sorted) {
        const auto* raw = load(o->art);
        MapDecorObject eff = *o;
        if (!raw && o->kind == MapDecorObject::kTmpTile) {
            // TEM 缺失时回退同名 SHP（如弹坑 CRAT01.SHP）
            const std::string base = o->art.substr(0, o->art.rfind('.'));
            if (!base.empty()) {
                eff.art = base + ".SHP";
                eff.kind = MapDecorObject::kShpSprite;
                raw = load(eff.art);
                // 墙 SHP 剧场前缀变体缺失时回退基名（GUWALL → GAWALL）
                if (!raw && base.size() >= 2 && base[1] >= 'T' && base[1] <= 'Z' &&
                    (base[1] == 'T' || base[1] == 'N' || base[1] == 'U' || base[1] == 'D' ||
                     base[1] == 'L')) {
                    eff.art = base.substr(0, 1) + "A" + base.substr(2) + ".SHP";
                    raw = load(eff.art);
                }
                // 矿石剧场变体（TIB2_*/TIB3_*）无独立艺术：原版统一用
                // TIB01.<剧场后缀> + 生长帧（实测 TIB3_19.LUN 不存在而 TIB01.LUN 存在）
                if (!raw && base.size() > 3 && base.rfind("TIB", 0) == 0 &&
                    base.rfind("TIBTRE", 0) != 0 && base != "TIB01") {
                    const size_t dot = o->art.rfind('.');
                    eff.art = "TIB01" + (dot == std::string::npos ? "" : o->art.substr(dot));
                    eff.kind = MapDecorObject::kTmpTile;
                    raw = load(eff.art);
                }
            }
        }
        if (!raw) continue;
        int px, py;
        grid.cell_to_pixel(eff.cx, eff.cy, px, py);
        if (eff.kind == MapDecorObject::kTmpTile) {
            // 先按 TMP 瓦片解析；失败则按 SHP 处理 —— 树木/矿石/桥面等装饰的
            // .TEM/.SNO/.URB… 文件实为 SHP 数据（帧 0=精灵，帧 1=阴影），
            // 扩展名只是剧场后缀（实测 TREE12.TEM 头 = 101×101×2 帧 SHP）。
            ra2r::render::TerrainTile t;
            std::string err;
            if (t.open(raw->data(), raw->size(), &err)) {
                const int fi = std::min<int>(eff.frame_i, t.frame_count() - 1);
                if (fi < 0) continue;
                const auto& f = t.frame(fi);
                if (f.bounds_w <= 0 || f.bounds_h <= 0) continue;
                const auto rgba = tmp_frame_rgba(t, fi, terrain_lut);
                // 与地形瓦片同公式：帧原点对齐格包围盒左上
                const int top_x = ox + px + f.bounds_x;
                const int top_y = oy + py + f.bounds_y - eff.height * kHeightLevelPx;
                for (int fy = 0; fy < f.bounds_h; ++fy) {
                    const uint8_t* s = rgba.data() + static_cast<size_t>(fy) * f.bounds_w * 4;
                    for (int fx = 0; fx < f.bounds_w; ++fx) {
                        if (s[fx * 4 + 3])
                            put_px(canvas, bw, bh, top_x + fx, top_y + fy, s + fx * 4);
                    }
                }
                continue;
            }
            ra2r::assets::ShpFile probe;
            std::string e2;
            if (!probe.open(raw->data(), raw->size(), &e2) || probe.frame_count() == 0)
                continue;
            eff.kind = MapDecorObject::kShpSprite;
        }
        // SHP 路径（显式 SHP 或 TEM→SHP 判定）
        {
            ra2r::assets::ShpFile shp;
            std::string err;
            if (!shp.open(raw->data(), raw->size(), &err) || shp.frame_count() == 0) continue;
            // 帧选择：先按覆盖物生长/损伤档（矿石等）；桥面类 SHP 每文件仅
            // 一个真实帧（如 LOBRDG26 仅帧 1 有内容），档位命中空帧时回退到
            // 第一个非空帧；全部为空（死桥标记 LOBRDG27/28）则跳过。
            int fi = std::min<int>(eff.frame_i, shp.frame_count() - 1);
            if (fi < 0) continue;
            if (shp.frame(fi).cx == 0 || shp.frame(fi).cy == 0) {
                fi = -1;
                for (int k = 0; k < shp.frame_count(); ++k) {
                    if (shp.frame(k).cx > 0 && shp.frame(k).cy > 0 && shp.frame(k).offset > 0) {
                        fi = k;
                        break;
                    }
                }
                if (fi < 0) continue; // 无内容帧（死桥等）
            }
            const auto& frame = shp.frame(fi);
            std::vector<uint8_t> idx;
            if (!shp.decode_frame(fi, idx, &err)) continue;
            // 调色盘：矿石/宝石 → TEMPERAT.PAL；墙/围栏 SHP → 单位盘；
            // 其余（树/岩石等 Theater=yes 艺术）→ 剧场地形盘
            const PaletteLut& lut = is_resource_art(eff.art) ? resource_lut
                                    : is_unit_palette_art(eff.art) ? unit_lut
                                                                  : terrain_lut;
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
            // SHP 装饰：精灵画布中心对格中心（OpenRA SHP 偏移语义 ——
            // 帧在画布内的 (x,y) 位置参与定位；低桥多格件的"绘制格"已由
            // build_scene_decor 移到件中格（data=1），桥面顶角即落在件顶角）
            const int bx = ox + px + grid.tile_w / 2 + static_cast<int>(frame.x) -
                           static_cast<int>(shp.width()) / 2;
            const int by = oy + py + grid.tile_h / 2 + static_cast<int>(frame.y) -
                           static_cast<int>(shp.height()) / 2 - eff.height * kHeightLevelPx;
            for (int y = 0; y < static_cast<int>(frame.cy); ++y) {
                const uint8_t* s = rgba.data() + static_cast<size_t>(y) * frame.cx * 4;
                for (int x = 0; x < static_cast<int>(frame.cx); ++x) {
                    if (s[x * 4 + 3]) put_px(canvas, bw, bh, bx + x, by + y, s + x * 4);
                }
            }
        }
    }
}

} // namespace ra2r::render
