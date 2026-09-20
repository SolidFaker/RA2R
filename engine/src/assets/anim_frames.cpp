// RA2R — artmd 动画段 → 帧序列计划（语义说明见头文件）
#include "ra2r/assets/anim_frames.h"

#include <algorithm>
#include <string>

#include "ra2r/core/ini_file.h"

namespace ra2r::assets {

AnimFramePlan plan_anim_frames(const core::IniFile& art, const std::string& section,
                               int shp_frames, const ShpLayout& layout, bool damaged) {
    AnimFramePlan plan;
    if (shp_frames <= 0) return plan;
    // 帧区间：LoopStart 优先于 Start；LoopEnd 为开区间上界
    int first = art.get_int(section, "Start", 0);
    if (!art.get(section, "LoopStart", "").empty())
        first = art.get_int(section, "LoopStart", first);
    if (first < 0) first = 0;
    int count = 0;
    if (!art.get(section, "LoopEnd", "").empty()) {
        const int end = art.get_int(section, "LoopEnd", 0);
        if (end > first) count = end - first;
    }
    if (count <= 0) { // 无元数据（mod/非常规段）：回退内容启发式
        first = damaged && layout.has_damaged ? std::max(1, layout.seg_len) : 0;
        count = std::max(1, layout.seg_len);
    } else if (damaged && layout.has_damaged && count * 4 == shp_frames) {
        first += count; // 无显式受损段：四段布局的受损段紧随空闲段
        plan.damaged_offset = true;
    }
    if (first + count > shp_frames) count = shp_frames - first;
    if (count <= 0) return plan;
    plan.first = first;
    plan.count = count;
    // 帧时长：Rate=（毫秒/帧），15Hz 逻辑帧 66.7ms → step = round(Rate/66.7)；
    // 段内无 Rate 时回退 Image= 指向段的 Rate（如 [GAPOWR_AD] → [GAPOWR_A]）
    int ms = -1;
    if (!art.get(section, "Rate", "").empty()) {
        ms = art.get_int(section, "Rate", 0);
    } else {
        const std::string img = art.get(section, "Image", "");
        if (!img.empty() && img != section && !art.get(img, "Rate", "").empty())
            ms = art.get_int(img, "Rate", 0);
    }
    if (ms > 66) plan.step = std::max(1, (ms + 33) / 67);
    // 阴影段：n%4==0（四段布局）或 artmd Shadow=yes（两段布局）
    plan.shadow = (shp_frames % 4 == 0) || art.get_bool(section, "Shadow", false);
    return plan;
}

} // namespace ra2r::assets
