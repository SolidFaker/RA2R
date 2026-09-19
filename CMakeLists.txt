# CMake 顶层：RA2R 引擎 + 工具（M0 骨架）
cmake_minimum_required(VERSION 3.21)
project(ra2r LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

# ── 内存安全加固 ─────────────────────────────────────────────────────────────
# RA2R_SANITIZE：动态检测（GCC/Clang），如 -DRA2R_SANITIZE=address,undefined
#   注意：MinGW 的 ASan 支持不完整，建议在 Linux 上使用。
# RA2R_HARDEN（默认 ON）：
#   - libstdc++ 容器边界断言 _GLIBCXX_ASSERTIONS（ABI 兼容，区别于 _GLIBCXX_DEBUG）
#   - 栈保护 -fstack-protector-strong
#   - 优化构建加 _FORTIFY_SOURCE=2（需 -O 才有意义）
set(RA2R_SANITIZE "" CACHE STRING "启用消毒器（如 address,undefined）")
option(RA2R_HARDEN "启用编译期内存安全加固" ON)

if(RA2R_SANITIZE)
  if(MSVC)
    add_compile_options(/fsanitize=${RA2R_SANITIZE})
  else()
    add_compile_options(-fsanitize=${RA2R_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${RA2R_SANITIZE})
  endif()
endif()

if(RA2R_HARDEN AND NOT MSVC)
  add_compile_options(-fstack-protector-strong)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_definitions(_GLIBCXX_ASSERTIONS)
  endif()
  add_compile_definitions($<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:_FORTIFY_SOURCE=2>)
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
