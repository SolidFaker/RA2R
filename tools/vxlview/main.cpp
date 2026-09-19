// RA2R vxlview — M0 工具：软件光栅化 VXL 体素（等距投影 + 画家排序 + 法线光照）。
// 用法：
//   vxlview <vxl> [--limb N] [--scale N] [--dump <out.bmp>] [--light x,y,z]
// 无 --dump 时打开 SDL 窗口；有 --dump 时无头渲染为 24 位 BMP 后退出。
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ra2r/assets/vxl_file.h"
#include "ra2r/core/win_unicode.h"

#include "../common/bmp_write.h"

using ra2r::assets::VxlFile;
using ra2r::assets::VxlSection;

namespace {
bool read_all(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

struct RenderResult {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// 软件光栅：体素 → 屏幕菱形，画家排序（x+y+z 升序），法线 Lambert 光照
RenderResult render_vxl(const VxlFile& vxl, int limb, int scale, float lx, float ly, float lz) {
    const VxlSection& s = vxl.sections()[limb];
    const float k = 2.0f;  // 等距投影系数（每个体素单位的屏幕位移）
    const float kh = 1.0f;
    const float kz = 2.0f;
    const float fs = static_cast<float>(scale);
    const int pitch_x = static_cast<int>(k * fs);
    const int pitch_y = static_cast<int>(kh * fs);
    const int pitch_z = static_cast<int>(kz * fs);

    const int min_x = static_cast<int>((0 - static_cast<int>(s.sy)) * pitch_x);
    const int max_x = static_cast<int>(static_cast<int>(s.sx) * pitch_x);
    const int min_y = static_cast<int>(-static_cast<int>(s.sz) * pitch_z);
    const int max_y = static_cast<int>((static_cast<int>(s.sx) + static_cast<int>(s.sy)) * pitch_y);
    const int margin = scale * 4;
    const int w = max_x - min_x + margin * 2;
    const int h = max_y - min_y + margin * 2;
    std::vector<uint8_t> img(static_cast<size_t>(w) * h * 4, 0);

    const float llen = std::sqrt(lx * lx + ly * ly + lz * lz) + 1e-6f;
    lx /= llen; ly /= llen; lz /= llen;
    const float inv_sqrt2 = 0.70710678f;

    struct Vox {
        int x, y, z;
        uint8_t color, normal;
        int sx, sy;
    };
    std::vector<Vox> voxels;
    voxels.reserve(static_cast<size_t>(s.sx) * s.sy * s.sz);
    for (uint16_t x = 0; x < s.sx; ++x) {
        for (uint16_t y = 0; y < s.sy; ++y) {
            for (uint16_t z = 0; z < s.sz; ++z) {
                const auto& v = s.at(x, y, z);
                if (v.color == 0) continue;
                Vox vo;
                vo.x = x; vo.y = y; vo.z = z;
                vo.color = v.color;
                vo.normal = v.normal;
                vo.sx = margin + (x - y) * pitch_x - min_x;
                vo.sy = margin + (x + y) * pitch_y - z * pitch_z - min_y;
                voxels.push_back(vo);
            }
        }
    }
    std::sort(voxels.begin(), voxels.end(), [](const Vox& a, const Vox& b) {
        const int ka = a.x + a.y + a.z, kb = b.x + b.y + b.z;
        if (ka != kb) return ka < kb;
        if (a.sy != b.sy) return a.sy < b.sy;
        return a.sx < b.sx;
    });

    const int hw = pitch_x / 2; // 菱形半宽
    const int hh = pitch_y / 2; // 菱形半高
    const auto& pal = vxl.palette();
    const uint8_t ntype = s.normal_type;
    for (const Vox& v : voxels) {
        // 法线：表坐标系 (front=x, right=y, up=z) → 本渲染坐标系
        const float* n = VxlFile::normal(ntype, v.normal);
        const float nx = (n[0] + n[1]) * inv_sqrt2;
        const float ny = (n[0] - n[1]) * inv_sqrt2;
        const float nz = n[2];
        float d = nx * lx + ny * ly + nz * lz;
        if (d < 0) d = 0;
        const float shade = 0.55f + 0.45f * d;
        int r = static_cast<int>(pal[v.color * 3] * 4 * shade);
        int g = static_cast<int>(pal[v.color * 3 + 1] * 4 * shade);
        int b = static_cast<int>(pal[v.color * 3 + 2] * 4 * shade);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        for (int dy = -hh; dy <= hh; ++dy) {
            const int half = hw - std::abs(dy) * hw / (hh + 1);
            const int sy = v.sy + dy;
            if (sy < 0 || sy >= h) continue;
            for (int dx = -half; dx <= half; ++dx) {
                const int sx = v.sx + dx;
                if (sx < 0 || sx >= w) continue;
                uint8_t* p = img.data() + (static_cast<size_t>(sy) * w + sx) * 4;
                p[0] = static_cast<uint8_t>(r);
                p[1] = static_cast<uint8_t>(g);
                p[2] = static_cast<uint8_t>(b);
                p[3] = 255;
            }
        }
    }
    RenderResult res;
    res.w = w;
    res.h = h;
    res.rgba = std::move(img);
    return res;
}

// ── BMP 输出（-shot 自检用；实现见 tools/common/bmp_write.h）──
bool write_bmp(const std::filesystem::path& path, int w, int h,
               const std::vector<uint8_t>& rgba) {
    return ra2r::tools::write_bmp_rgba(path, w, h, rgba);
}

} // namespace

static int run(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: vxlview <vxl> [--limb N] [--scale N] [--light x,y,z] [--dump <bmp>]\n");
        return 1;
    }
    const char* path = argv[1];
    const char* dump_path = nullptr;
    int limb = 0;
    int scale = 4;
    float lx = -0.58f, ly = -0.58f, lz = 0.57f;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--limb") == 0 && i + 1 < argc) limb = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) scale = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) dump_path = argv[++i];
        else if (std::strcmp(argv[i], "--light") == 0 && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%f,%f,%f", &lx, &ly, &lz) != 3) {
                std::fprintf(stderr, "bad --light\n");
                return 1;
            }
        }
    }
    if (scale < 1) scale = 1;

    std::vector<uint8_t> raw;
    if (!read_all(path, raw)) {
        std::fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    VxlFile vxl;
    std::string error;
    if (!vxl.open(raw.data(), raw.size(), &error)) {
        std::fprintf(stderr, "vxl open failed: %s\n", error.c_str());
        return 1;
    }
    if (limb < 0 || limb >= static_cast<int>(vxl.sections().size())) {
        std::fprintf(stderr, "limb %d out of range (0..%d)\n", limb,
                     static_cast<int>(vxl.sections().size()) - 1);
        return 1;
    }
    const VxlSection& s = vxl.sections()[limb];
    std::printf("vxl: %s (%u limbs)\n", path, static_cast<unsigned>(vxl.sections().size()));
    std::printf("limb %d '%s': %ux%ux%u normal_type=%u\n", limb, s.name.c_str(), s.sx, s.sy,
                s.sz, s.normal_type);

    const RenderResult res = render_vxl(vxl, limb, scale, lx, ly, lz);
    std::printf("canvas %dx%d, voxels drawn: alpha>0 px %zu\n", res.w, res.h,
                std::count_if(res.rgba.begin(), res.rgba.end(), [](uint8_t v) { return v > 0; }) / 4);

    if (dump_path) {
        if (!write_bmp(dump_path, res.w, res.h, res.rgba)) {
            std::fprintf(stderr, "write bmp failed\n");
            return 1;
        }
        std::printf("dumped -> %s\n", dump_path);
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("RA2R vxlview", res.w, res.h, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    SDL_Texture* texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, res.w, res.h);
    if (!window || !renderer || !texture) {
        std::fprintf(stderr, "window setup failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_UpdateTexture(texture, nullptr, res.rgba.data(), res.w * 4);
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) running = false;
        }
        SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
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
