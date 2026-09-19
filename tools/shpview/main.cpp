// RA2R shpview — M0 工具：查看/导出 TS/RA2 SHP 动画帧。
// 用法：
//   shpview <shp> [--pal <pal>] [--frame N] [--scale N] [--dump <out.bmp>]
// 无 --dump 时打开 SDL 窗口循环播放；有 --dump 时无头渲染单帧为 24 位 BMP 后退出。
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ra2r/assets/pal_file.h"
#include "ra2r/assets/shp_file.h"
#include "ra2r/core/win_unicode.h"

#include "../common/bmp_write.h"

using ra2r::assets::Palette;
using ra2r::assets::ShpFile;

namespace {
bool read_all(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

// 调色板索引帧 → RGBA
std::vector<uint8_t> to_rgba(const std::vector<uint8_t>& idx, const Palette& pal) {
    std::vector<uint8_t> out(idx.size() * 4);
    for (size_t i = 0; i < idx.size(); ++i) {
        pal.to_rgba(idx[i], out[i * 4], out[i * 4 + 1], out[i * 4 + 2], out[i * 4 + 3]);
    }
    return out;
}

// ── BMP 输出（-shot 自检用；实现见 tools/common/bmp_write.h）──
bool write_bmp(const std::filesystem::path& path, int w, int h,
               const std::vector<uint8_t>& rgba) {
    return ra2r::tools::write_bmp_rgba(path, w, h, rgba);
}

} // namespace

static int run(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: shpview <shp> [--pal <pal>] [--frame N] [--scale N] [--dump <bmp>]\n");
        return 1;
    }
    const char* shp_path = argv[1];
    const char* pal_path = nullptr;
    const char* dump_path = nullptr;
    int frame_no = 0;
    int scale = 3;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pal") == 0 && i + 1 < argc) pal_path = argv[++i];
        else if (std::strcmp(argv[i], "--frame") == 0 && i + 1 < argc) frame_no = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) scale = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) dump_path = argv[++i];
    }
    if (scale < 1) scale = 1;

    std::vector<uint8_t> raw;
    if (!read_all(shp_path, raw)) {
        std::fprintf(stderr, "cannot read %s\n", shp_path);
        return 1;
    }
    ShpFile shp;
    std::string error;
    if (!shp.open(raw.data(), raw.size(), &error)) {
        std::fprintf(stderr, "shp open failed: %s\n", error.c_str());
        return 1;
    }

    Palette pal;
    if (pal_path) {
        if (!pal.load(pal_path, &error)) {
            std::fprintf(stderr, "palette load failed: %s\n", error.c_str());
            return 1;
        }
    } else {
        // 灰度后备调色板（索引 0 透明）
        for (int i = 0; i < 256; ++i) {
            pal.rgb[i * 3 + 0] = pal.rgb[i * 3 + 1] = pal.rgb[i * 3 + 2] = static_cast<uint8_t>(i);
        }
        pal.loaded = true;
    }

    std::printf("shp: %s (%ux%u, %u frames)\n", shp_path, shp.width(), shp.height(),
                shp.frame_count());

    if (dump_path) {
        if (frame_no < 0 || frame_no >= shp.frame_count()) {
            std::fprintf(stderr, "frame %d out of range (0..%d)\n", frame_no,
                         shp.frame_count() - 1);
            return 1;
        }
        const auto& fh = shp.frame(frame_no);
        std::vector<uint8_t> idx;
        if (!shp.decode_frame(frame_no, idx, &error)) {
            std::fprintf(stderr, "decode failed: %s\n", error.c_str());
            return 1;
        }
        const auto rgba = to_rgba(idx, pal);
        if (!write_bmp(dump_path, fh.cx, fh.cy, rgba)) {
            std::fprintf(stderr, "write bmp failed\n");
            return 1;
        }
        std::printf("dumped frame %d (%ux%u) -> %s\n", frame_no, fh.cx, fh.cy, dump_path);
        return 0;
    }

    // 窗口模式
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window =
        SDL_CreateWindow("RA2R shpview", shp.width() * scale, shp.height() * scale, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!window || !renderer) {
        std::fprintf(stderr, "window/renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STREAMING, shp.width(), shp.height());
    if (!texture) {
        std::fprintf(stderr, "texture failed: %s\n", SDL_GetError());
        return 1;
    }

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) running = false;
        }
        const int n = static_cast<int>(SDL_GetTicks() / 120) % shp.frame_count();
        const auto& fh = shp.frame(n);
        std::vector<uint8_t> idx;
        shp.decode_frame(n, idx, &error);
        const auto rgba = to_rgba(idx, pal);
        std::vector<uint8_t> canvas(static_cast<size_t>(shp.width()) * shp.height() * 4, 0);
        for (uint16_t y = 0; y < fh.cy; ++y) {
            std::memcpy(canvas.data() + (static_cast<size_t>(fh.y + y) * shp.width() + fh.x) * 4,
                        rgba.data() + static_cast<size_t>(y) * fh.cx * 4,
                        static_cast<size_t>(fh.cx) * 4);
        }
        SDL_UpdateTexture(texture, nullptr, canvas.data(), shp.width() * 4);
        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
        SDL_RenderClear(renderer);
        SDL_FRect dst{0, 0, static_cast<float>(shp.width() * scale),
                      static_cast<float>(shp.height() * scale)};
        SDL_RenderTexture(renderer, texture, nullptr, &dst);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) { return ra2r::core::run_wide(run, argc, wargv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
