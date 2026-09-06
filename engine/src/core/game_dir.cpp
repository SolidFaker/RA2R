// RA2R — 游戏安装目录自动发现实现（接口见 game_dir.h）
#include "ra2r/core/game_dir.h"

#include <filesystem>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ra2r/core/win_unicode.h"
#endif

namespace ra2r::core {

namespace {

// 判定目录像不像游戏目录：含 ra2md.mix（YR）或 ra2.mix（RA2）。
// Windows 文件系统大小写不敏感，小写检测即可覆盖。
bool looks_like_game_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    for (const char* n : {"ra2md.mix", "ra2.mix"}) {
        if (std::filesystem::exists(dir / n, ec)) return true;
    }
    return false;
}

// 注册表读 InstallPath（宽字符值 → UTF-8）
std::string registry_install_path(const std::vector<std::wstring>& subkeys) {
#ifdef _WIN32
    for (const auto& sub : subkeys) {
        wchar_t buf[1024] = {};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (::RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), L"InstallPath", RRF_RT_REG_SZ,
                           &type, buf, &size) == ERROR_SUCCESS && buf[0] != 0) {
            return utf8_from_wide(buf);
        }
        // 用户级（HKCU 下同一子路径）
        size = sizeof(buf);
        if (::RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), L"InstallPath", RRF_RT_REG_SZ,
                           &type, buf, &size) == ERROR_SUCCESS && buf[0] != 0) {
            return utf8_from_wide(buf);
        }
    }
#else
    (void)subkeys;
#endif
    return {};
}

} // namespace

std::string find_game_dir() {
    // 1) 注册表（原版安装器写入 32 位视图 = WOW6432Node；64 位视图一并尝试）
    {
        const std::vector<std::wstring> keys = {
            L"SOFTWARE\\WOW6432Node\\Westwood\\Yuri's Revenge",
            L"SOFTWARE\\Westwood\\Yuri's Revenge",
            L"SOFTWARE\\WOW6432Node\\Westwood\\Red Alert 2",
            L"SOFTWARE\\Westwood\\Red Alert 2",
        };
        std::string p = registry_install_path(keys);
        if (!p.empty() && looks_like_game_dir(p)) return p;
    }

    // 2) exe 目录向上几级（开发布局：exe 在 build/tools/，游戏在仓库根 Yuri/）
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        const std::filesystem::path exe_dir = std::filesystem::path(buf).parent_path();
        for (const char* rel : {"Yuri", "RA2", "Red Alert 2", "Yuri's Revenge", "../Yuri",
                                "../RA2", "../../Yuri", "../../RA2", "../Red Alert 2"}) {
            candidates.push_back(exe_dir / rel);
        }
    }
#endif
    candidates.emplace_back("Yuri");
    candidates.emplace_back(".");
    candidates.emplace_back("..");
    for (const auto& c : candidates) {
        std::error_code ec;
        const auto abs = std::filesystem::absolute(c, ec);
        if (ec) continue;
        if (looks_like_game_dir(abs)) return abs.string();
    }
    return {};
}

} // namespace ra2r::core
