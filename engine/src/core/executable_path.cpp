// RA2R — 当前可执行文件路径实现（接口见 executable_path.h）
#include "ra2r/core/executable_path.h"

#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace ra2r::core {

std::filesystem::path executable_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // 首次调用仅取所需长度
    if (size == 0) return {};
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    // 返回路径可能含 . / .. / 符号链接，尽量规范化为绝对路径
    std::error_code ec;
    const auto canon = std::filesystem::weakly_canonical(std::filesystem::path(buf.data()), ec);
    return ec ? std::filesystem::path(buf.data()) : canon;
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

} // namespace ra2r::core
