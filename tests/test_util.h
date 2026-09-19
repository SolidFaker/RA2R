#pragma once
// 测试公共工具：游戏目录定位、资产读取与 rulesmd 缓存、SHP 帧识别
//
// 游戏素材（/Yuri）不入库；需要资产的用例在目录不可用时 GTEST_SKIP，
// 可用环境变量 RA2R_GAME_DIR 指定游戏目录。
#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "ra2r/assets/file_index.h"
#include "ra2r/assets/rules_db.h"
#include "ra2r/core/game_dir.h"

namespace ra2r::test {

// 游戏目录：环境变量 RA2R_GAME_DIR 优先，否则用引擎的自动发现
// （注册表 / exe 相对路径 / 当前目录，大小写不敏感）——跨平台同一套逻辑
inline const char* game_dir() {
    if (const char* env = std::getenv("RA2R_GAME_DIR")) return env;
    static const std::string found = core::find_game_dir();
    return found.c_str();
}

// 懒加载的 MIX 名称索引（失败返回 nullptr；只尝试一次）
inline assets::FileIndex* file_index() {
    static assets::FileIndex idx;
    static int state = 0; // 0=未试 1=成功 2=失败
    if (state == 0) state = idx.build(game_dir()) ? 1 : 2;
    return state == 1 ? &idx : nullptr;
}

// 读资产（失败返回空表）
inline std::vector<uint8_t> read_asset(const std::string& name) {
    std::vector<uint8_t> v;
    if (auto* idx = file_index()) idx->read(name, v);
    return v;
}

// 懒加载的 rulesmd/artmd（失败返回 nullptr）
inline const assets::RulesDB* rules_db() {
    static assets::RulesDB db;
    static int state = 0;
    if (state == 0) {
        const auto r = read_asset("RULESMD.INI");
        const auto a = read_asset("ARTMD.INI");
        std::string err;
        state = (!r.empty() && db.load(r.data(), r.size(), a.data(), a.size(), &err)) ? 1 : 2;
    }
    return state == 1 ? &db : nullptr;
}

#define RA2R_REQUIRE_ASSETS()                                                      \
    do {                                                                           \
        if (!ra2r::test::file_index())                                             \
            GTEST_SKIP() << "游戏目录不可用（设 RA2R_GAME_DIR 指向 RA2/YR 目录）"; \
    } while (0)

#define RA2R_REQUIRE_RULES()                                                       \
    do {                                                                           \
        if (!ra2r::test::rules_db())                                               \
            GTEST_SKIP() << "rulesmd/artmd 不可用（设 RA2R_GAME_DIR）";            \
    } while (0)

} // namespace ra2r::test
