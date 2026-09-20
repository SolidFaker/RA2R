#pragma once
// RA2R — artmd 动画段 → 帧序列计划
//
// 原版建筑配件动画（ActiveAnim/IdleAnim/ProductionAnim 及各自 Damaged 变体）
// 的帧区间与帧率全部来自 artmd 段元数据，本文件把它翻译成绘制端的帧计划：
//   Start/LoopStart/LoopEnd：循环区间 = [LoopStart, LoopEnd)（**LoopEnd 为开
//     区间上界**：NACNST_B LoopEnd=21 共 21 帧、GACNST_A LoopEnd=3 共 3 帧、
//     NAYARD_A LoopEnd=15 共 15 帧，实测与原版一致）；LoopStart 缺省时用 Start；
//     受损变体在 artmd 里是**独立段**（如 [NACNST_CD] Image=NACNST_C
//     LoopStart=1 LoopEnd=2 → 只播帧 1），段名与 SHP 名可不同（Image= 解析）。
//   Rate：毫秒/帧 → 15Hz 逻辑帧下 step = round(Rate/66.7)（至少 1；Rate=200
//     → 3 逻辑帧/动画帧）；段内无 Rate 时回退 Image= 指向段的 Rate。
//   Shadow=yes 或总帧数 n%4==0（四段布局）→ 阴影帧 = 动画帧 + n/2；两段布局
//     且无 Shadow=yes（如 NACNST_A 22 帧后半是受损灯）不得按阴影处理。
// 段元数据缺失（mod/非常规段）时回退 SHP 内容分段布局（ShpLayout）启发式。
#include <string>

#include "ra2r/assets/shp_layout.h"

namespace ra2r::core {
class IniFile;
}

namespace ra2r::assets {

struct AnimFramePlan {
    int first = 0;       // 段首帧（LoopStart 优先，其次 Start）
    int count = 0;       // 循环帧数（>=1；0 = 无可用帧，调用方应跳过绘制）
    int step = 1;        // 逻辑帧/动画帧（Rate= 换算）
    bool shadow = false; // 阴影帧号 = 动画帧号 + 总帧数/2
    bool damaged_offset = false; // 已按四段布局偏移到第 2 段（受损）
};

// 依据 artmd 段元数据 + SHP 分段布局计算帧序列计划。
//   section    artmd 段名（如 NACNST_B / NACNST_BD）
//   shp_frames 该段实际加载 SHP 的总帧数
//   layout     SHP 内容分段布局（元数据缺失时的回退判据）
//   damaged    当前为受损状态（无显式受损段时用四段布局的第 2 段）
AnimFramePlan plan_anim_frames(const core::IniFile& art, const std::string& section,
                               int shp_frames, const ShpLayout& layout, bool damaged);

} // namespace ra2r::assets
