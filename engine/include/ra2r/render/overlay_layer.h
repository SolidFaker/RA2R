#pragma once
// RA2R — 地图装饰层渲染（树木/岩石/矿石/宝石/弹坑等）
//
// 数据来源：MapFile 的 [Terrain] 对象（树木/岩石，艺术 = 名 + 剧场后缀的文件）
// 与 [OverlayPack]/[OverlayDataPack]（矿石/宝石/弹坑等；艺术 = RULESMD
// [OverlayTypes] 名 + 剧场后缀，缺则同名 SHP；帧 = OverlayData 生长/损伤档）。
// 放置约定：树木/矿石/桥面等装饰文件虽带 .TEM/.SNO… 后缀，实为 SHP 数据
// （帧 0=精灵、帧 1=阴影），以画布中心对格中心绘制；真正 TMP 格式瓦片
// （如有）与地形瓦片同公式。均画在地形之上、对象之下。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ra2r/render/isometric.h"
#include "ra2r/render/palette_lut.h"

namespace ra2r::render {

using FileLoader = std::function<const std::vector<uint8_t>*(const std::string&)>;

struct MapDecorObject {
    enum Kind { kTmpTile, kShpSprite } kind = kTmpTile;
    std::string art; // 文件名（如 "TREE24.TEM" / "GUWALL.SHP"）
    int cx = 0, cy = 0;
    int height = 0;  // 格高度级
    int frame_i = 0; // 帧（覆盖物 = 有效渲染帧；树木 = 0）
};

// 覆盖物类型名 → 装饰艺术文件名（OpenRA 序列语义）。
//   桥类：BRIDGE1/2 → BRIDGE.<剧场后缀>；
//   围墙类 → 剧场前缀墙 SHP（<首字母><剧场字母><其余>.SHP，剧场字母
//     T/N/U/D/L；如 GAWALL→GUWALL.SHP、CAFNCP→CUFNCP.SHP）；
//   其余类型直接用 名.<ext>（缺则回退同名 .SHP）。
std::string overlay_art_name(const std::string& overlay_type_name, const std::string& ext,
                             char theater_letter);

// 按覆盖物类型号解析艺术（原版硬编码表优先 —— RULESMD 名在 240-247 段有误导：
// 240/244=CAKRMW 241/245=CAFNCP 242/246=WCRATE 243/247=GAFWLL）。
std::string overlay_type_art_name(int overlay_type, const std::string& rulesmd_name,
                                  const std::string& ext, char theater_letter);

// 桥类硬编码艺术表（游戏内 OverlayType 表；rulesmd 名偏移）：
//   木桥 74-101 → LOBRDG01-28；木坡副本 122-125 → LOBRDG19/21/23/25；
//   混凝土桥 205-232 → LOBRDB01-28；混凝土坡副本 233-236 → LOBRDB19/21/23/25；
//   高架桥面 24/25 → BRIDGE；高架木桥基 237/238 → BRIDGB、239 → BRIDGE；
//   其余返回空串。
std::string bridge_art_name(int overlay_type);

// 剧场名 → 墙 SHP 剧场字母（TEMPERATE=T, SNOW=N, URBAN/NEWURBAN=U, DESERT=D, LUNAR=L）
char wall_theater_letter(const std::string& theater);

// 是否为资源精灵（矿石/宝石）：原版用固定 TEMPERAT.PAL 绘制（索引 16-31 =
// 矿石金色阶），不随剧场地形盘变化。
bool is_resource_art(const std::string& art);

// 是否为单位盘覆盖物（墙/围栏类：artmd 无 Theater=yes 只有 NewTheater=yes，
// 原版用单位盘绘制而非地形盘）。
bool is_unit_palette_art(const std::string& art);

// 渲染装饰元素到画布（调用方保证 canvas 为全图 RGBA）。
// terrain_lut = 剧场地形盘；resource_lut = TEMPERAT.PAL（矿石/宝石用）；
// unit_lut = 剧场单位盘（墙/围栏 SHP 用）。
// only_row >= 0 时只画该行（cy == only_row）的装饰：供"统一画家序"逐行
// 把装饰插进地形与对象之间（树木/围墙同样按行压住后方单位）。
void render_map_decor(const std::vector<MapDecorObject>& objs, const PaletteLut& terrain_lut,
                      const PaletteLut& resource_lut, const PaletteLut& unit_lut,
                      const IsometricGrid& grid, const FileLoader& load, int bw, int bh, int ox,
                      int oy, std::vector<uint8_t>& canvas, int only_row = -1);

} // namespace ra2r::render
