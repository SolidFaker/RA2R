// RA2R 动画帧序列测试：artmd 段元数据 → AnimFramePlan（无需游戏素材）
//
// 用例值全部取自原版 ARTMD.INI 实测段（并在注释中标注），锁死以下语义：
//   - LoopEnd 为**开区间上界**（count = LoopEnd - LoopStart）；
//   - 受损变体是独立段（自带 Start/LoopStart/LoopEnd，Image= 可指向同一 SHP）；
//   - Rate= 毫秒/帧 → 15Hz 逻辑帧 step = round(Rate/66.7)，段内缺失回退 Image= 段；
//   - Shadow=yes 或总帧数 n%4==0 → 阴影帧 = 动画帧 + n/2（NACNST_A 这类
//     22 帧两段"受损灯"不得按阴影处理）。
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "ra2r/assets/anim_frames.h"
#include "ra2r/assets/shp_layout.h"
#include "ra2r/core/ini_file.h"

using namespace ra2r;

namespace {

core::IniFile art_from(const char* text) {
    core::IniFile ini;
    std::string err;
    EXPECT_TRUE(ini.parse(reinterpret_cast<const uint8_t*>(text), std::strlen(text), &err))
        << err;
    return ini;
}

assets::ShpLayout layout_of(int frames, int seg_len, bool has_damaged) {
    assets::ShpLayout l;
    l.frames = frames;
    l.seg_len = seg_len;
    l.has_damaged = has_damaged;
    return l;
}

// 绘制端帧号公式（stage draw_bld_anim_frame 同款）
int anim_frame(const assets::AnimFramePlan& p, int clock) {
    return p.first + ((clock / p.step) % p.count + p.count) % p.count;
}

} // namespace

// artmd [NACNST_C]：Image=NACNST_C LoopStart=0 LoopEnd=1 Shadow=yes（6 帧=臂 3+影 3）
TEST(AnimFrames, IdleConyardArmIsStaticFrameZero) {
    const auto art = art_from("[NACNST_C]\nImage=NACNST_C\nLoopStart=0\nLoopEnd=1\nShadow=yes\n");
    const auto p = assets::plan_anim_frames(art, "NACNST_C", 6, layout_of(6, 3, true), false);
    EXPECT_EQ(p.first, 0);
    EXPECT_EQ(p.count, 1); // LoopEnd 开区间：0..0 共 1 帧
    EXPECT_EQ(p.step, 1);
    EXPECT_TRUE(p.shadow);
    // 任意逻辑帧都画同一帧 → "静态机械臂"（用户报告"动画有问题"的根因判据）
    EXPECT_EQ(anim_frame(p, 0), 0);
    EXPECT_EQ(anim_frame(p, 333), 0);
    // 阴影帧 = 动画帧 + n/2（stage 绘制端规则）：臂阴影在 NACNST_C.SHP 帧 3
    EXPECT_TRUE(6 / 2 > 0 && anim_frame(p, 0) + 6 / 2 < 6);
    EXPECT_EQ(anim_frame(p, 0) + 6 / 2, 3);
}

// artmd [NACNST_CD]：Image=NACNST_C LoopStart=1 LoopEnd=2 Shadow=yes
TEST(AnimFrames, DamagedIdleUsesSecondFrame) {
    const auto art = art_from(
        "[NACNST_C]\nImage=NACNST_C\nLoopStart=0\nLoopEnd=1\nShadow=yes\n"
        "[NACNST_CD]\nImage=NACNST_C\nLoopStart=1\nLoopEnd=2\nShadow=yes\n");
    const auto p = assets::plan_anim_frames(art, "NACNST_CD", 6, layout_of(6, 3, true), false);
    EXPECT_EQ(p.first, 1);
    EXPECT_EQ(p.count, 1);
    EXPECT_TRUE(p.shadow); // 显式 Shadow=yes（6 帧 n%4!=0）
    EXPECT_EQ(anim_frame(p, 99), 1);
}

// artmd [NACNST_B]：Image=NACNST_B LoopStart=0 LoopEnd=21 Rate=200 Shadow=yes（84 帧）
TEST(AnimFrames, ProductionCraneLoopEndIsExclusiveAndRateDrivesClock) {
    const auto art = art_from(
        "[NACNST_B]\nImage=NACNST_B\nNormalized=yes\nLoopStart=0\nLoopEnd=21\nLoopCount=1\n"
        "Rate=200\nShadow=yes\n");
    const auto p = assets::plan_anim_frames(art, "NACNST_B", 84, layout_of(84, 21, true), false);
    EXPECT_EQ(p.first, 0);
    EXPECT_EQ(p.count, 21); // LoopEnd - LoopStart（开区间）
    EXPECT_EQ(p.step, 3);   // Rate=200ms → round(200/66.7) = 3 逻辑帧/动画帧
    EXPECT_TRUE(p.shadow);
    // 生产 120 帧 → (120/3)%21 = 19；130 帧 → 43%21 = 1（机械臂推进）
    EXPECT_EQ(anim_frame(p, 120), 19);
    EXPECT_EQ(anim_frame(p, 130), 1);
    EXPECT_NE(anim_frame(p, 120), anim_frame(p, 130));
}

// artmd [NACNST_BD]：Start=22 LoopStart=22 LoopEnd=42 Rate=200 Image=NACNST_B
TEST(AnimFrames, DamagedProductionUsesOwnStartAndRange) {
    const auto art = art_from(
        "[NACNST_BD]\nImage=NACNST_B\nStart=22\nLoopStart=22\nLoopEnd=42\nRate=200\n"
        "Shadow=yes\n");
    const auto p = assets::plan_anim_frames(art, "NACNST_BD", 84, layout_of(84, 21, true), false);
    EXPECT_EQ(p.first, 22);
    EXPECT_EQ(p.count, 20);
    EXPECT_EQ(p.step, 3);
    EXPECT_EQ(anim_frame(p, 120), 22); // (120/3)%20 = 0 → 段首帧
    EXPECT_EQ(anim_frame(p, 130), 25); // (130/3)%20 = 3
}

// artmd [GACNST_A]/[GACNST_B]：盟军建造厂（雷达盘 3 帧 / 吊臂 20 帧，Rate=200）
TEST(AnimFrames, AlliedConyardRangesMatchArtmd) {
    const auto art = art_from(
        "[GACNST_A]\nStart=0\nLoopStart=0\nLoopEnd=3\nRate=200\n"
        "[GACNST_B]\nStart=0\nLoopStart=0\nLoopEnd=20\nRate=200\nShadow=yes\n");
    const auto a = assets::plan_anim_frames(art, "GACNST_A", 12, layout_of(12, 3, true), false);
    EXPECT_EQ(a.count, 3);
    EXPECT_EQ(a.step, 3);
    EXPECT_TRUE(a.shadow); // 12%4==0 → 阴影段在 +6
    const auto b = assets::plan_anim_frames(art, "GACNST_B", 80, layout_of(80, 20, true), false);
    EXPECT_EQ(b.count, 20);
    EXPECT_EQ(b.step, 3);
    EXPECT_EQ(anim_frame(b, 120), 0); // (120/3)%20 = 0
}

// Rate= 换算：>66ms 才起效；round 到最近逻辑帧（66.7ms/帧）
TEST(AnimFrames, RateRoundsToLogicTicks) {
    const auto art = art_from(
        "[A]\nLoopStart=0\nLoopEnd=10\nRate=200\n"
        "[B]\nLoopStart=0\nLoopEnd=10\nRate=133\n"
        "[C]\nLoopStart=0\nLoopEnd=10\nRate=66\n"
        "[D]\nLoopStart=0\nLoopEnd=10\n"
        "[E]\nLoopStart=0\nLoopEnd=10\nRate=67\n");
    EXPECT_EQ(assets::plan_anim_frames(art, "A", 10, {}, false).step, 3); // 200/66.7≈3.0
    EXPECT_EQ(assets::plan_anim_frames(art, "B", 10, {}, false).step, 2); // 133/66.7≈2.0
    EXPECT_EQ(assets::plan_anim_frames(art, "C", 10, {}, false).step, 1); // ≤66ms → 1
    EXPECT_EQ(assets::plan_anim_frames(art, "D", 10, {}, false).step, 1); // 无 Rate → 1
    EXPECT_EQ(assets::plan_anim_frames(art, "E", 10, {}, false).step, 1); // 67→(67+33)/67=1
}

// 段内无 Rate 时回退 Image= 指向段（[GAPOWR_AD] → [GAPOWR_A] Rate=200）
TEST(AnimFrames, RateFallsBackToImageSection) {
    const auto art = art_from(
        "[GAPOWR_A]\nRate=200\nLoopStart=0\nLoopEnd=8\n"
        "[GAPOWR_AD]\nImage=GAPOWR_A\nStart=8\nLoopStart=8\nLoopEnd=16\n");
    const auto p = assets::plan_anim_frames(art, "GAPOWR_AD", 32, layout_of(32, 8, true), false);
    EXPECT_EQ(p.step, 3);
    EXPECT_EQ(p.first, 8);
    EXPECT_EQ(p.count, 8);
    EXPECT_TRUE(p.shadow); // 32%4==0 → 阴影在 +16
}

// 无显式受损段 + 四段布局（count*4 == n）：受损状态偏移到第 2 段
TEST(AnimFrames, DamagedFourSegmentLayoutOffsetsSecondSegment) {
    const auto art = art_from("[GAPOWR_A]\nLoopStart=0\nLoopEnd=8\n");
    const auto layout = layout_of(32, 8, true);
    const auto ok = assets::plan_anim_frames(art, "GAPOWR_A", 32, layout, false);
    EXPECT_EQ(ok.first, 0);
    EXPECT_FALSE(ok.damaged_offset);
    const auto dmg = assets::plan_anim_frames(art, "GAPOWR_A", 32, layout, true);
    EXPECT_EQ(dmg.first, 8);
    EXPECT_EQ(dmg.count, 8);
    EXPECT_TRUE(dmg.damaged_offset);
}

// 元数据缺失（mod/非常规段）：回退 ShpLayout 分段（NACNST_A 22 帧 → 段长 11）
TEST(AnimFrames, MissingMetadataFallsBackToLayoutSegments) {
    const auto art = art_from("[MYANIM]\nLayer=ground\n");
    const auto layout = layout_of(22, 11, true);
    const auto idle = assets::plan_anim_frames(art, "MYANIM", 22, layout, false);
    EXPECT_EQ(idle.first, 0);
    EXPECT_EQ(idle.count, 11);
    EXPECT_FALSE(idle.shadow); // 22%4!=0 且无 Shadow=yes → 后半是受损帧，不是阴影
    const auto dmg = assets::plan_anim_frames(art, "MYANIM", 22, layout, true);
    EXPECT_EQ(dmg.first, 11);
    EXPECT_EQ(dmg.count, 11);
}

// 区间越界钳制：LoopEnd 超出实际帧数 / 起点之后无帧 → count=0（调用方跳过）
TEST(AnimFrames, ClampsToAvailableFrames) {
    const auto art = art_from(
        "[A]\nLoopStart=0\nLoopEnd=100\n"
        "[B]\nLoopStart=8\nLoopEnd=50\n"
        "[C]\nLoopStart=40\nLoopEnd=50\n"
        "[D]\nLoopStart=-3\nLoopEnd=4\n");
    EXPECT_EQ(assets::plan_anim_frames(art, "A", 12, {}, false).count, 12);
    EXPECT_EQ(assets::plan_anim_frames(art, "B", 12, {}, false).count, 4);
    const auto c = assets::plan_anim_frames(art, "C", 12, {}, false);
    EXPECT_EQ(c.count, 0); // 起点越界 → 无可用帧
    const auto d = assets::plan_anim_frames(art, "D", 12, {}, false);
    EXPECT_EQ(d.first, 0); // 负起点钳到 0，count 保留 4
    EXPECT_EQ(d.count, 4);
}
