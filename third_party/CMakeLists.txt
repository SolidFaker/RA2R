# RA2R 第三方库（vendored）
# ImGui（MIT, https://github.com/ocornut/imgui）— 核心 + SDL3 渲染后端
set(RA2R_IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui CACHE INTERNAL "imgui source dir")

add_library(ra2r_imgui STATIC
  ${RA2R_IMGUI_DIR}/imgui.cpp
  ${RA2R_IMGUI_DIR}/imgui_demo.cpp
  ${RA2R_IMGUI_DIR}/imgui_draw.cpp
  ${RA2R_IMGUI_DIR}/imgui_tables.cpp
  ${RA2R_IMGUI_DIR}/imgui_widgets.cpp
)
target_include_directories(ra2r_imgui PUBLIC ${RA2R_IMGUI_DIR} ${RA2R_IMGUI_DIR}/backends)
