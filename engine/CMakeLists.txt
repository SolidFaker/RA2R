# RA2R 引擎核心库
add_library(ra2r_engine STATIC
  src/core/ini_file.cpp
  src/core/crc32.cpp
  src/core/game_dir.cpp
  src/cache/cache_manager.cpp
  src/crypto/blowfish.cpp
  src/crypto/bigint.cpp
  src/assets/mix_file.cpp
  src/assets/pal_file.cpp
  src/assets/shp_file.cpp
  src/assets/shp_layout.cpp
  src/assets/vxl_file.cpp
  src/assets/lcw.cpp
  src/assets/lzo1x.cpp
  src/assets/map_file.cpp
  src/assets/name_db.cpp
  src/assets/csf_file.cpp
  src/assets/aud_file.cpp
  src/assets/hva_file.cpp
  src/assets/fnt_file.cpp
  src/assets/tileset.cpp
  src/assets/file_index.cpp
  src/assets/theater.cpp
  src/assets/rules_db.cpp
  src/assets/pcx_file.cpp
  src/render/voxel_raster.cpp
  src/render/terrain_tile.cpp
  src/render/palette_lut.cpp
  src/render/minimap.cpp
  src/render/fog.cpp
  src/render/object_layer.cpp
  src/render/overlay_layer.cpp
  src/render/map_scene.cpp
  src/sim/pathfind.cpp
  src/sim/sim_world.cpp
  src/sim/skirmish.cpp
)
target_include_directories(ra2r_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(ra2r_engine PUBLIC RA2R_ENGINE)

# GUI 工具统一引导（DPI 感知 / 中文字体 / 控制台 UTF-8 / 多后端渲染宿主，
# 见 include/ra2r/ui/*.h）。依赖 ImGui 与 SDL3（在 third_party 与根
# find_package 中定义/查找，目标引用在生成期解析）。
if(SDL3_FOUND)
  add_library(ra2r_ui STATIC src/ui/ui.cpp src/ui/backend.cpp
    ${RA2R_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${RA2R_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    ${RA2R_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(ra2r_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(ra2r_ui PUBLIC ra2r_engine ra2r_imgui SDL3::SDL3)
  if(MINGW)
    target_link_libraries(ra2r_ui PUBLIC opengl32)
  else()
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
      target_link_libraries(ra2r_ui PUBLIC OpenGL::GL)
    endif()
  endif()
endif()
