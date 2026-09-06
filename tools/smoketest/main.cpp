// RA2R smoketest — M1/M2 开发工具：按扩展名解析并打印格式摘要
// 用法：smoketest <file> [palette]   （.tmp 时给定调色盘会渲染帧 0 为 <file>.bmp）
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ra2r/assets/aud_file.h"
#include "ra2r/assets/csf_file.h"
#include "ra2r/assets/fnt_file.h"
#include "ra2r/assets/hva_file.h"
#include "ra2r/assets/map_file.h"
#include "ra2r/core/win_unicode.h"
#include "ra2r/render/terrain_tile.h"

namespace fs = std::filesystem;

static bool read_all(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

// 简单 BMP 写出（自检用，与 mixbrowser 的 write_bmp 等价）
static bool write_bmp(const fs::path& path, int w, int h, const std::vector<uint8_t>& rgba) {
    const int stride = (w * 3 + 3) & ~3;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto w16 = [&](uint16_t v) { f.put(v & 0xFF); f.put(v >> 8); };
    auto w32 = [&](uint32_t v) {
        f.put(v & 0xFF);
        f.put((v >> 8) & 0xFF);
        f.put((v >> 16) & 0xFF);
        f.put((v >> 24) & 0xFF);
    };
    w16(0x4D42);
    w32(54 + stride * h);
    w32(0);
    w32(54);
    w32(40);
    w32(w);
    w32(h);
    w16(1);
    w16(24);
    w32(0);
    w32(stride * h);
    w32(2835);
    w32(2835);
    w32(0);
    w32(0);
    std::vector<uint8_t> row(stride, 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
            row[x * 3 + 0] = p[2];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[0];
        }
        f.write(reinterpret_cast<const char*>(row.data()), stride);
    }
    return true;
}

static int run(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: smoketest <file> [palette(.pal|.jasc|768B)]\n");
        return 1;
    }
    std::vector<uint8_t> data;
    if (!read_all(argv[1], data)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    std::string ext = fs::path(argv[1]).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".csf") {
        ra2r::assets::CsfFile f;
        std::string err;
        if (!f.open(data.data(), data.size(), &err)) {
            std::fprintf(stderr, "csf failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("CSF: %zu strings, language=%u\n", f.size(), f.language());
        size_t n = 0;
        for (const auto& [k, v] : f.all()) {
            if (n++ < 5) {
                std::printf("  %s = %s%s%s\n", k.c_str(), v.value.c_str(),
                            v.extra.empty() ? "" : "  [extra: ", v.extra.c_str(),
                            v.extra.empty() ? "" : "]");
            }
        }
    } else if (ext == ".aud" || ext == ".wav") {
        ra2r::assets::AudFile f;
        std::string err;
        if (!f.open(data.data(), data.size(), &err)) {
            std::fprintf(stderr, "aud failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("%s: %u Hz, %d ch, %d bit, comp=%u, %.2f s\n",
                    f.is_wav() ? "WAV" : "AUD", f.rate(), f.channels(), f.bits(),
                    f.compression(), f.duration_seconds());
        std::vector<int16_t> pcm;
        if (f.decode_pcm16(pcm, &err)) {
            std::printf("decode: %zu samples (first 8: ", pcm.size());
            for (size_t i = 0; i < 8 && i < pcm.size(); ++i) std::printf("%d ", pcm[i]);
            std::printf(")\n");
        } else {
            std::fprintf(stderr, "decode failed: %s\n", err.c_str());
        }
    } else if (ext == ".map" || ext == ".yrm" || ext == ".yro" || ext == ".mmx") {
        ra2r::assets::MapFile f;
        std::string err;
        if (!f.open(data.data(), data.size(), &err)) {
            std::fprintf(stderr, "map failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("MAP: %dx%d, theater=%s, present=%d, overlay=%d (pack %zuB, data %zuB)\n",
                    f.cell_w(), f.cell_h(), f.theater().c_str(), f.present_count(),
                    f.overlay_present(), f.overlays().size(), f.overlay_data().size());
        size_t shown = 0;
        for (int ry = 0; ry < 512 && shown < 8; ++ry) {
            for (int rx = 0; rx < 512; ++rx) {
                const uint8_t t = f.overlay_type(rx, ry);
                if (t != 0xFF) {
                    std::printf("  overlay (%d,%d): type=%u data=%u\n", rx, ry, t,
                                f.overlay_data_at(rx, ry));
                    if (++shown >= 8) break;
                }
            }
        }
    } else if (ext == ".fnt") {
        ra2r::assets::FntFile f;
        std::string err;
        if (!f.open(data.data(), data.size(), &err)) {
            std::fprintf(stderr, "fnt failed: %s\n", err.c_str());
            return 1;
        }
        if (f.is_unicode()) {
            std::printf("FNT(Unicode): %u glyphs, %ux%u (stride %u), ideograph %u\n",
                        f.glyph_count(), f.stride() * 8, f.lines(), f.stride(),
                        f.ideograph_width());
            for (uint32_t cp : {uint32_t('A'), 0x4E2Du, 0x3000u}) {
                const auto g = f.glyph(cp);
                std::printf("  U+%04X: width=%u %s\n", cp, g.width,
                            f.has_glyph(cp) ? "" : "(empty)");
            }
        } else {
            std::printf("FNT(TS): %u chars, height %d\n", f.ts_char_count(), f.ts_height());
            const auto g = f.ts_glyph('A');
            std::printf("  'A': width=%u lines=%d\n", g.width, g.lines);
        }
    } else if (ext == ".tmp" || ext == ".tem" || ext == ".sno" || ext == ".urb" ||
               ext == ".des" || ext == ".lun" || ext == ".ubn") {
        ra2r::render::TerrainTile f;
        std::string err;
        if (!f.open(data.data(), data.size(), &err)) {
            std::fprintf(stderr, "tmp failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("TMP: template %dx%d, tile %dx%d, %d frames\n", f.template_w(),
                    f.template_h(), f.tile_w(), f.tile_h(), f.frame_count());
        for (int i = 0; i < f.frame_count() && i < 4; ++i) {
            const auto& fr = f.frame(i);
            std::printf("  frame %d: bounds (%d,%d) %dx%d extra=%d\n", i, fr.bounds_x,
                        fr.bounds_y, fr.bounds_w, fr.bounds_h, fr.has_extra ? 1 : 0);
        }
        if (argc >= 3) {
            std::vector<uint8_t> pal;
            if (!read_all(argv[2], pal) || pal.size() < 768) {
                std::fprintf(stderr, "palette unreadable: %s\n", argv[2]);
                return 1;
            }
            // 渲染首个非空帧（模板中常有空帧哨兵）
            int frame_i = -1;
            for (int i = 0; i < f.frame_count(); ++i) {
                if (f.frame(i).bounds_w > 0 && f.frame(i).bounds_h > 0) {
                    frame_i = i;
                    break;
                }
            }
            if (frame_i < 0) {
                std::fprintf(stderr, "no non-empty frame\n");
                return 1;
            }
            const auto img = f.render_frame(frame_i, pal.data());
            const fs::path out = fs::path(argv[1]).string() + ".bmp";
            if (!write_bmp(out, img.w, img.h, img.rgba)) {
                std::fprintf(stderr, "bmp write failed: %s\n", out.string().c_str());
                return 1;
            }
            std::printf("rendered frame %d -> %s (%dx%d)\n", frame_i, out.string().c_str(),
                        img.w, img.h);
        }
    } else if (ext == ".hva") {
        ra2r::assets::HvaFile f;
        std::string err;
        if (!f.open(data.data(), data.size(), &err)) {
            std::fprintf(stderr, "hva failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("HVA: %u frames x %u sections\n", f.frame_count(), f.section_count());
        for (uint32_t i = 0; i < f.section_count(); ++i) {
            std::printf("  section %u: %s\n", i, f.section_names()[i].c_str());
        }
        if (f.frame_count() > 0 && f.section_count() > 0) {
            const float* m = f.matrix(0, 0);
            std::printf("  frame0/sec0 matrix: ");
            for (int i = 0; i < 12; ++i) std::printf("%.3f ", m[i]);
            std::printf("\n");
        }
    } else {
        std::fprintf(stderr, "unsupported extension: %s\n", ext.c_str());
        return 1;
    }
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) { return ra2r::core::run_wide(run, argc, wargv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
