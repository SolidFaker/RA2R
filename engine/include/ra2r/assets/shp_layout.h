#pragma once
// RA2R — SHP 分段布局自判（不依赖外部表，只用游戏自身 SHP 帧内容）
//
// 原版建筑本体 / 配件动画 SHP 的帧段约定（由帧内容实测归纳）：
//   本体：  [空闲][受损][建造/临界…] + [同数阴影段]（阴影段 = 后半，帧像素仅索引 1）
//   动画四段（n = 4L）：[空闲 L][受损 L][空闲阴影 L][受损阴影 L]
//   动画两段（n = 2L）：[动画 L][阴影 L]
//   动画单段（n）：     [动画 n]（无阴影段）
// 判据：后半全为"纯索引 1（阴影色）或空帧"才存在阴影段；再用循环接缝
// （末帧 vs 首帧的画布差异率）区分四段/两段——空闲段自闭环时 L=n/4 接缝更小。
//
// 索引 1 是原版约定的阴影索引（OpenRA PaletteFromFile ShadowIndex=1 →
// ARGB(140,0,0,0)）；PaletteLut 已把它映射为半透明黑，绘制端需按 alpha 混合。
#include <cstdint>

#include "ra2r/assets/shp_file.h"

namespace ra2r::assets {

struct ShpLayout {
    int frames = 0;           // 总帧数
    int seg_len = 0;          // 段长 L
    bool has_damaged = false; // 第 2 段为受损变体（四段布局）
    int shadow_start = -1;    // 阴影段起点（-1 = 无阴影段）
    int shadow_len = 0;       // 阴影段帧数
};

// 帧类别：'C' 彩色 / 'S' 纯索引 1（阴影帧）/ 'E' 空帧 / '?' 解码失败
char shp_frame_kind(const ShpFile& shp, int frame);

// 动画 SHP 分段布局（只解码判定所需的少数帧）
ShpLayout shp_layout(const ShpFile& shp);

// 建筑本体阴影段起点：后半全为阴影/空帧时返回 n/2，否则 -1
int shp_shadow_start(const ShpFile& shp);

} // namespace ra2r::assets
