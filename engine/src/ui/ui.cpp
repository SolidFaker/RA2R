// RA2R — GUI 工具统一引导实现（接口见 ui.h）
#include "ra2r/ui/ui.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

#include <SDL3/SDL.h>
#include <imgui.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ra2r::ui {

void enable_dpi_awareness() {
#ifdef _WIN32
    // Per-Monitor V2（Win10 1703+）：窗口按物理像素布局，SDL 报告 content scale；
    // 动态取用 API，旧系统回退 System Aware（位图拉伸）。
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
#endif
}

bool setup_cjk_font(float pixel_size) {
    ImGuiIO& io = ImGui::GetIO();
    // 候选顺序：黑体（纯 TTF，加载最稳）→ 雅黑 → 宋体 → 等线 → 楷体。
    // CJK 字体自带拉丁字形，直接作为唯一默认字体即可。
    // Linux：Noto Sans CJK（Arch: noto-cjk / Debian: opentype/noto）→ 文泉驿。
    static const char* kFonts[] = {
        "C:\\Windows\\Fonts\\simhei.ttf",  "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\msyh.ttc",    "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\Deng.ttf",    "C:\\Windows\\Fonts\\simkai.ttf",
        "C:\\Windows\\Fonts\\msyhl.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
        "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
        "/usr/share/fonts/wqy-microhei/wqy-microhei.ttc",
    };
    for (const char* f : kFonts) {
        std::error_code ec;
        if (!std::filesystem::exists(f, ec)) continue;
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            f, pixel_size, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        if (font) {
            io.FontDefault = font;
            return true;
        }
    }
    return false;
}

float scale_window_to_dpi(SDL_Window* window, int logical_w, int logical_h) {
    float s = 1.0f;
    if (window) s = SDL_GetWindowDisplayScale(window);
    if (s > 0.0f) {
        SDL_SetWindowSize(window, static_cast<int>(logical_w * s + 0.5f),
                          static_cast<int>(logical_h * s + 0.5f));
        return s;
    }
    return 1.0f;
}

} // namespace ra2r::ui
