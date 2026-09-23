#pragma once
// RA2R — 当前可执行文件路径（跨平台）
#include <filesystem>

namespace ra2r::core {

// 返回当前可执行文件的路径：
//   Windows  GetModuleFileNameW
//   macOS    _NSGetExecutablePath（无 /proc）
//   Linux    /proc/self/exe
// 用于基于 exe 位置发现游戏目录、XCC 名库等资源（开发布局 exe 在 build/tools/）。
// 失败返回空路径。
std::filesystem::path executable_path();

} // namespace ra2r::core
