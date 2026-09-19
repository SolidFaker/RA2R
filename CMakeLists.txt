# CMake 顶层：RA2R 引擎 + 工具（M0 骨架）
cmake_minimum_required(VERSION 3.21)
project(ra2r LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# SDL3 提前查找：engine 下的 ra2r_ui 模块按 SDL3_FOUND 决定是否构建
find_package(SDL3 CONFIG QUIET)

# third_party 必须先于 engine：ra2r_ui 需要 RA2R_IMGUI_DIR（在 third_party 里定义）
add_subdirectory(third_party)
add_subdirectory(engine)
add_subdirectory(tools)

# 测试（GoogleTest，vendored 于 third_party/googletest）
option(RA2R_BUILD_TESTS "Build tests" OFF)
if(RA2R_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
