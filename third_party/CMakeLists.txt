# RA2R 第三方库（vendored）
# ImGui（MIT, https://github.com/ocornut/imgui）：核心 + SDL3 渲染后端
set(RA2R_IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui CACHE INTERNAL "imgui source dir")

add_library(ra2r_imgui STATIC
  ${RA2R_IMGUI_DIR}/imgui.cpp
  ${RA2R_IMGUI_DIR}/imgui_demo.cpp
  ${RA2R_IMGUI_DIR}/imgui_draw.cpp
  ${RA2R_IMGUI_DIR}/imgui_tables.cpp
  ${RA2R_IMGUI_DIR}/imgui_widgets.cpp
)
target_include_directories(ra2r_imgui SYSTEM PUBLIC ${RA2R_IMGUI_DIR} ${RA2R_IMGUI_DIR}/backends)
# vendored 第三方：关闭告警（imgui_internal.h 的位域转换在 -Wconversion 下必告警）
if(NOT MSVC)
  target_compile_options(ra2r_imgui PRIVATE -w)
else()
  target_compile_options(ra2r_imgui PRIVATE /w)
endif()

# GoogleTest（BSD-3, https://github.com/google/googletest v1.15.2）：
# 测试框架；仅 RA2R_BUILD_TESTS=ON 时编译（离线可用，随仓库 vendored）
if(RA2R_BUILD_TESTS)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/googletest
                   ${CMAKE_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)
endif()

