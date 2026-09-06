#pragma once
// RA2R — Windows 命令行 Unicode 支持：wmain 脚手架（MinGW -municode）
#include <string>
#include <vector>

namespace ra2r::core {

inline std::string utf8_from_wide(std::wstring_view w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) {
        const uint32_t cp = static_cast<uint32_t>(c);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// 在工具 main 中使用：
//   static int run(int argc, char** argv);
//   #ifdef _WIN32
//   int wmain(int argc, wchar_t** wargv) { return ra2r::core::run_wide(run, argc, wargv); }
//   #else
//   int main(int argc, char** argv) { return run(argc, argv); }
//   #endif
inline int run_wide(int (*run)(int, char**), int argc, wchar_t** wargv) {
    std::vector<std::string> storage(argc);
    std::vector<char*> argv(argc);
    for (int i = 0; i < argc; ++i) {
        storage[i] = utf8_from_wide(wargv[i]);
        argv[i] = storage[i].data();
    }
    return run(argc, argv.data());
}

} // namespace ra2r::core
