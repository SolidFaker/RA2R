# RA2R M0 工具集
# MinGW：静态链接运行时（libstdc++/libgcc/libwinpthread），双击不再缺 DLL
if(MINGW)
  set(RA2R_RUNTIME_STATIC -static)
else()
  set(RA2R_RUNTIME_STATIC)
endif()

# SDL3.dll 拷贝到各 GUI 工具 exe 旁（包内无静态 SDL3）
function(ra2r_copy_sdl target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${target}>")
endfunction()

add_executable(mixdump mixdump/main.cpp)
target_link_libraries(mixdump PRIVATE ra2r_engine)
target_link_options(mixdump PRIVATE -municode ${RA2R_RUNTIME_STATIC})

find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_executable(shpview shpview/main.cpp)
  target_link_libraries(shpview PRIVATE ra2r_engine SDL3::SDL3)
  target_link_options(shpview PRIVATE -municode -mwindows ${RA2R_RUNTIME_STATIC})
  ra2r_copy_sdl(shpview)

  add_executable(vxlview vxlview/main.cpp)
  target_link_libraries(vxlview PRIVATE ra2r_engine SDL3::SDL3)
  target_link_options(vxlview PRIVATE -municode -mwindows ${RA2R_RUNTIME_STATIC})
  ra2r_copy_sdl(vxlview)

  # mixbrowser：SDL3 + ImGui（vendored）
  add_executable(mixbrowser
    mixbrowser/main.cpp
    mixbrowser/media_player.cpp
    mixbrowser/ffmpeg_bind.cpp
    ${RA2R_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${RA2R_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
  )
  target_link_libraries(mixbrowser PRIVATE ra2r_engine ra2r_ui ra2r_imgui SDL3::SDL3)
  # ffmpeg 头（仅 mixbrowser：音视频播放运行时加载 DLL，无需导入库）
  target_include_directories(mixbrowser PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mixbrowser/ffmpeg/include)
  target_include_directories(mixbrowser PRIVATE ${RA2R_IMGUI_DIR} ${RA2R_IMGUI_DIR}/backends)
  target_link_options(mixbrowser PRIVATE -municode -mwindows ${RA2R_RUNTIME_STATIC})
  ra2r_copy_sdl(mixbrowser)

  # mapview：M2 地图查看器（等距地形渲染）
  add_executable(mapview mapview/main.cpp)
  target_link_libraries(mapview PRIVATE ra2r_engine ra2r_ui SDL3::SDL3)
  target_include_directories(mapview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_options(mapview PRIVATE -municode -mwindows ${RA2R_RUNTIME_STATIC})
  ra2r_copy_sdl(mapview)

  # stage：舞台展示 GUI（地形/地图生成模式/对象放置，测试调试用）
  add_executable(stage
    stage/main.cpp
    stage/stage_app.cpp
    stage/stage_map.cpp
  )
  target_link_libraries(stage PRIVATE ra2r_engine ra2r_ui ra2r_imgui SDL3::SDL3)
  target_include_directories(stage PRIVATE ${RA2R_IMGUI_DIR} ${RA2R_IMGUI_DIR}/backends
                                           ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_options(stage PRIVATE -municode -mwindows ${RA2R_RUNTIME_STATIC})
  ra2r_copy_sdl(stage)
endif()

add_executable(smoketest smoketest/main.cpp)
target_link_libraries(smoketest PRIVATE ra2r_engine)
target_link_options(smoketest PRIVATE -municode ${RA2R_RUNTIME_STATIC})

add_executable(assetcheck assetcheck/main.cpp)
target_link_libraries(assetcheck PRIVATE ra2r_engine)
target_link_options(assetcheck PRIVATE -municode ${RA2R_RUNTIME_STATIC})

add_executable(cachebuild cachebuild/main.cpp)
target_link_libraries(cachebuild PRIVATE ra2r_engine)
target_link_options(cachebuild PRIVATE -municode ${RA2R_RUNTIME_STATIC})
