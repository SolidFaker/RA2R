// RA2R — GUI 工具统一引导实现（接口见 ui.h）
#include "ra2r/ui/ui.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <clocale>
#endif

namespace ra2r::ui {

void enable_dpi_awareness() {
#ifdef _WIN32
    // Per-Monitor V2（Win10 1703+）：窗口按物理像素布局，SDL 报告 content scale。
    // 动态取 API，旧系统回退 System Aware（位图拉伸）。
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32) {
        const FARPROC fp = ::GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        BOOL(WINAPI * pfn)(void*) = nullptr;
        static_assert(sizeof(pfn) == sizeof(fp), "function pointer size");
        std::memcpy(&pfn, &fp, sizeof(pfn)); // 规避 -Wcast-function-type
        if (pfn) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4
            pfn(reinterpret_cast<void*>(static_cast<INT_PTR>(-4)));
            return;
        }
    }
    ::SetProcessDPIAware();
#endif
}

void console_utf8() {
#ifdef _WIN32
    if (::GetStdHandle(STD_OUTPUT_HANDLE) != nullptr) {
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);
    }
#else
    // Linux 终端默认 UTF-8；仅显式设置 locale（ImGui/printf 均按字节输出）
    setlocale(LC_ALL, "");
#endif
}

namespace {
// 候选字体：path 为字体文件；face_index 为 TTC/OTC 集合内的字体索引
// （如 macOS 苹方 PingFang.ttc 内含 HK/MO/TC/SC 多个字面，简体为索引 3）。
struct FontCandidate {
    std::string path;
    int face_index = 0;
};

// 追加存在的字体文件到候选表
void add_font(std::vector<FontCandidate>& out, const std::string& path, int face_index = 0) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) out.push_back({path, face_index});
}

#ifdef __linux__
// fontconfig 查询（Linux 发行版字体路径各不相同：Noto/WQY/思源/文鼎…）：
// `fc-match -f %{file}` 交给 fontconfig 选一个含中文字形的字体。
// 不依赖 libfontconfig 链接（只用命令行，缺失时静默跳过）。
void add_fontconfig_candidates(std::vector<FontCandidate>& out) {
    static const char* kQueries[] = {
        "sans-serif:lang=zh-cn", "Noto Sans CJK SC", "Source Han Sans SC",
        "WenQuanYi Micro Hei",   "WenQuanYi Zen Hei", "AR PL UMing CN",
    };
    for (const char* q : kQueries) {
        std::string cmd = "fc-match -f '%{file}' \"";
        cmd += q;
        cmd += "\" 2>/dev/null";
        // NOLINTNEXTLINE(bugprone-command-processor) —— 命令与字体名均为常量表，无注入面
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) continue;
        char buf[1024] = {};
        const size_t n = fread(buf, 1, sizeof(buf) - 1, p);
        pclose(p);
        if (n == 0) continue;
        std::string path(buf, n);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
            path.pop_back();
        add_font(out, path);
    }
}

// 列出系统里所有"看起来像 CJK"的字体文件（fontconfig 不可用时的兜底扫描）
std::vector<std::string> list_cjk_font_files() {
    std::vector<std::string> out;
    static const char* kDirs[] = {"/usr/share/fonts", "/usr/local/share/fonts",
                                  "/run/host/fonts", "/var/lib/flatpak/exports/share/fonts"};
    for (const char* root : kDirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file()) continue;
            const std::string name = it->path().filename().string();
            std::string low;
            low.reserve(name.size());
            for (char c : name)
                low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            const bool ext = low.size() > 4 &&
                             (low.compare(low.size() - 4, 4, ".ttf") == 0 ||
                              low.compare(low.size() - 4, 4, ".ttc") == 0 ||
                              low.compare(low.size() - 4, 4, ".otf") == 0);
            if (!ext) continue;
            static const char* kKeys[] = {"cjk", "han",   "hei",   "ming",
                                          "song", "kai",  "wqy",   "noto",
                                          "droid", "uming", "ukai"};
            bool hit = false;
            for (const char* k : kKeys)
                if (low.find(k) != std::string::npos) {
                    hit = true;
                    break;
                }
            if (hit) out.push_back(it->path().string());
        }
    }
    return out;
}
#endif

#ifdef __APPLE__
// macOS 系统中文字体：优先苹方（PingFang SC），其次冬青黑体/黑体/宋体。
// .ttc 集合内的简体字面索引见各 add_font 注释（ImGui FontNo）。
// 新版 macOS 把苹方放进 /System/Library/AssetsV2 的哈希目录（路径不稳定），
// 固定路径缺失时扫描 MobileAsset 字体目录兜底。
void add_macos_candidates(std::vector<FontCandidate>& out) {
    const size_t before = out.size();
    add_font(out, "/System/Library/Fonts/PingFang.ttc", 3); // 苹方 SC Regular
    if (out.size() == before) {
        const std::filesystem::path assets = "/System/Library/AssetsV2";
        std::error_code ec;
        if (std::filesystem::is_directory(assets, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(
                     assets, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec) break;
                const std::string dir = entry.path().filename().string();
                if (dir.rfind("com_apple_MobileAsset_Font", 0) != 0) continue; // 仅字体资产目录
                for (auto it = std::filesystem::recursive_directory_iterator(
                         entry.path(),
                         std::filesystem::directory_options::skip_permission_denied, ec);
                     it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec) break;
                    if (it->is_regular_file() && it->path().filename() == "PingFang.ttc")
                        add_font(out, it->path().string(), 3); // 苹方 SC Regular
                }
            }
        }
    }
    add_font(out, "/System/Library/Fonts/Hiragino Sans GB.ttc", 0);    // 冬青黑体 SC W3
    add_font(out, "/System/Library/Fonts/STHeiti Medium.ttc", 1);      // 黑体 SC Medium
    add_font(out, "/System/Library/Fonts/STHeiti Light.ttc", 1);       // 黑体 SC Light
    add_font(out, "/System/Library/Fonts/Supplemental/Songti.ttc", 0); // 宋体 SC
    add_font(out, "/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
    add_font(out, "/Library/Fonts/Arial Unicode.ttf");
}
#endif
} // namespace

bool setup_cjk_font(float pixel_size) {
    ImGuiIO& io = ImGui::GetIO();
    // 候选顺序：Windows 黑体/雅黑/宋体/等线/楷体；macOS 苹方/冬青黑体/黑体/宋体；
    // Linux 常见发行版路径；fontconfig 兜底（发行版路径千差万别，交给 fontconfig 最稳）。
    // CJK 字体自带拉丁字形，直接作为唯一默认字体即可。
    std::vector<FontCandidate> fonts;
    add_font(fonts, "C:\\Windows\\Fonts\\simhei.ttf");
    add_font(fonts, "C:\\Windows\\Fonts\\msyh.ttf");
    add_font(fonts, "C:\\Windows\\Fonts\\msyh.ttc");
    add_font(fonts, "C:\\Windows\\Fonts\\simsun.ttc");
    add_font(fonts, "C:\\Windows\\Fonts\\Deng.ttf");
    add_font(fonts, "C:\\Windows\\Fonts\\simkai.ttf");
    add_font(fonts, "C:\\Windows\\Fonts\\msyhl.ttc");
#ifdef __APPLE__
    add_macos_candidates(fonts);
#endif
    // Arch（noto-cjk）/ Debian（opentype/noto）/ Fedora / 文泉驿 / 思源
    add_font(fonts, "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc");
    add_font(fonts, "/usr/share/fonts/noto-cjk/NotoSansCJKsc-Regular.otf");
    add_font(fonts, "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    add_font(fonts, "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf");
    add_font(fonts, "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc");
    add_font(fonts, "/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc");
    add_font(fonts, "/usr/share/fonts/adobe-source-han-sans/SourceHanSansSC-Regular.otf");
    add_font(fonts, "/usr/share/fonts/opentype/source-han-sans/SourceHanSansSC-Regular.otf");
    add_font(fonts, "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc");
    add_font(fonts, "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc");
    add_font(fonts, "/usr/share/fonts/wqy-microhei/wqy-microhei.ttc");
    add_font(fonts, "/usr/share/fonts/truetype/arphic/uming.ttc");
    add_font(fonts, "/usr/share/fonts/truetype/arphic/ukai.ttc");
#ifdef __linux__
    const size_t before_fc = fonts.size();
    add_fontconfig_candidates(fonts);
    if (fonts.size() == before_fc) {
        // fontconfig 也不可用（极简容器）：扫描常见字体目录里名字像 CJK 的字体
        for (const auto& f : list_cjk_font_files()) fonts.push_back({f, 0});
    }
#endif
    for (const auto& f : fonts) {
        ImFontConfig cfg;
        cfg.FontNo = static_cast<ImU32>(f.face_index); // TTC/OTC 集合内字面索引
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            f.path.c_str(), pixel_size, &cfg, io.Fonts->GetGlyphRangesChineseFull());
        if (font) {
            io.FontDefault = font;
            std::printf("[ui] CJK font: %s\n", f.path.c_str());
            return true;
        }
    }
    std::fprintf(stderr,
                 "[ui] 未找到中文字体（Windows：黑体/雅黑；Linux：noto-cjk/wqy；"
                 "macOS：系统自带苹方/冬青黑体）\n");
    return false;
}

float scale_window_to_dpi(SDL_Window* window, int logical_w, int logical_h) {
    float s = 1.0f;
    if (window) s = SDL_GetWindowDisplayScale(window);
    if (s > 0.0f) {
        SDL_SetWindowSize(window,
                          static_cast<int>(std::lround(static_cast<float>(logical_w) * s)),
                          static_cast<int>(std::lround(static_cast<float>(logical_h) * s)));
        return s;
    }
    return 1.0f;
}

} // namespace ra2r::ui
