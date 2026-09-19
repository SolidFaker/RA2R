// RA2R — 游戏安装目录自动发现实现（接口见 game_dir.h）
#include "ra2r/core/game_dir.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ra2r/core/win_unicode.h"
#endif

namespace ra2r::core {

namespace {

// 文件名比较（大小写不敏感；Linux 文件系统区分大小写，游戏目录名常是
// RA2MD.MIX / ra2md.mix 混用——必须按不敏感比对）
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// 判定目录像不像游戏目录：含 ra2md.mix（YR）或 ra2.mix（RA2）。
// 大小写不敏感（Windows 天然如此；Linux 上目录内可能是 RA2MD.MIX）。
bool looks_like_game_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
        if (!de.is_regular_file()) continue;
        const std::string n = de.path().filename().string();
        if (iequals(n, "ra2md.mix") || iequals(n, "ra2.mix")) return true;
    }
    return false;
}

// 逐级把路径解析为磁盘上真实大小写（Linux 大小写敏感；找不到返回空 + ec 置位）
std::filesystem::path resolve_ci(const std::filesystem::path& p, std::error_code& ec) {
    std::filesystem::path out;
    for (const auto& part : p) {
        if (part == "/" || part == "\\" || part == ".") {
            out /= part;
            continue;
        }
        if (part == "..") {
            out /= part;
            continue;
        }
        const std::filesystem::path next = out / part;
        if (std::filesystem::is_directory(next, ec) || std::filesystem::is_regular_file(next, ec)) {
            out = next; // 该级存在（大小写已匹配）→ 继续下一级
            continue;
        }
        // 在父目录里按大小写不敏感找一个同名项
        bool found = false;
        for (const auto& de : std::filesystem::directory_iterator(
                 out.empty() ? std::filesystem::path(".") : out, ec)) {
            if (iequals(de.path().filename().string(), part.string())) {
                out /= de.path().filename();
                found = true;
                break;
            }
        }
        if (!found) {
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return {};
        }
    }
    ec.clear();
    return out;
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

    // 2) 环境变量（跨平台；Linux 上无注册表，主要靠这个与开发布局）
    if (const char* env = std::getenv("RA2R_GAME_DIR")) {
        if (looks_like_game_dir(env)) return std::filesystem::absolute(env).string();
    }

    // 3) exe 目录向上几级（开发布局：exe 在 build/tools/，游戏在仓库根 Yuri/）
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
#else
    // Linux：/proc/self/exe 解析可执行文件路径（wmain 入口在非 Windows 不存在）
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        const std::filesystem::path exe_dir = exe.parent_path();
        for (const char* rel : {"Yuri", "RA2", "Red Alert 2", "Yuri's Revenge", "../Yuri",
                                "../RA2", "../../Yuri", "../../RA2", "../Red Alert 2",
                                "../../Yuri"}) {
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
        // 路径本身不存在时（Linux 大小写敏感：候选 `Yuri` vs 实际 `yuri`），
        // 逐级做大小写不敏感解析后再判定
        const auto ci = resolve_ci(abs, ec);
        if (!ec && looks_like_game_dir(ci)) return ci.string();
    }
    return {};
}

} // namespace ra2r::core
