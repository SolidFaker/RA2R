# RA2R 测试（GoogleTest，vendored 于 third_party/googletest）
#
# 运行：cmake -B build -DRA2R_BUILD_TESTS=ON && cmake --build build --target ra2r_tests -j
#       ctest --test-dir build --output-on-failure      （或直接跑 build/tests/ra2r_tests.exe）
#
# 分层：
#   test_core.cpp     纯逻辑：INI 解析 / 等距格 / 调色板 LUT / 8 向寻路
#   test_sim.cpp      模拟层：移动·朝向·恒速·重下令·idle / 建造队列·工期·电力·
#                     修理出售·精炼采矿 / 防御建筑攻击 / 确定性哈希
#   test_skirmish.cpp 遭遇战：基地车展开 / 阵营色 ramp（纯）；科技树/开局兵力（需资产）
#   test_assets.cpp   资产解析（需游戏目录）：FileIndex/Shp/Map/RulesDB/VXL/HVA
#   test_render.cpp   渲染功能（需游戏目录）：步兵朝向块序/走序列速率/idle/
#                     Buildup 进度/体素光栅确定性
#   test_fixtures.cpp 合成夹具（tools/gen_fixtures 产出，无需游戏素材）：
#                     PAL/SHP/VXL/HVA/MIX/地图/INI 解析回归
# 需资产的用例在游戏目录缺失时自动 GTEST_SKIP（可用 RA2R_GAME_DIR 覆盖路径）。

add_executable(ra2r_tests
  test_core.cpp
  test_codecs.cpp
  test_fixtures.cpp
  test_integration.cpp
  test_engine_modules.cpp
  test_sim.cpp
  test_skirmish.cpp
  test_assets.cpp
  test_render.cpp
)
target_link_libraries(ra2r_tests PRIVATE ra2r_engine GTest::gtest GTest::gtest_main)
target_include_directories(ra2r_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MINGW)
  target_link_options(ra2r_tests PRIVATE -static)
endif()

# 每个 gtest 用例注册为独立 ctest 测试（CI 门禁逐条可见/逐条定位失败），
# 需要资产的用例在无素材环境自动 GTEST_SKIP（SKIP 不算失败）。
include(GoogleTest)
gtest_discover_tests(ra2r_tests
  PROPERTIES
    SKIP_REGULAR_EXPRESSION "SKIPPED"
  DISCOVERY_TIMEOUT 60
)
