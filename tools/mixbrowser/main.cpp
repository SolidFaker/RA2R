// RA2R mixbrowser — M0 GUI 工具：MIX 资源浏览器
// 功能：打开 MIX → 条目列表（排序/查找/类型识别/右键导出）→ 选中预览：
//   嵌套 MIX（下钻）/ SHP（帧动画+调色板）/ VXL（软件光栅）/ PAL（色板）/ PCX（位图）
//   / 文本 INI / 十六进制 / AUD / BIK；耗时操作（调色盘搜索/地图扫描/缩略图）
//   后台线程执行，底部状态栏展示进度。
// 用法：
//   mixbrowser [<mix 路径>] [--shot <out.bmp>] [--shot-frame N]
// 中文路径支持：wmain + 系统中文字体（Microsoft YaHei / SimHei）。
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"

#include "ra2r/assets/aud_file.h"
#include "ra2r/assets/csf_file.h"
#include "ra2r/assets/file_index.h"
#include "ra2r/assets/hva_file.h"
#include "ra2r/assets/map_file.h"
#include "ra2r/assets/mix_file.h"
#include "ra2r/assets/name_db.h"
#include "ra2r/assets/pal_file.h"
#include "ra2r/assets/pcx_file.h"
#include "ra2r/assets/shp_file.h"
#include "ra2r/assets/theater.h"
#include "ra2r/assets/tileset.h"
#include "ra2r/assets/vxl_file.h"
#include "ra2r/core/game_dir.h"
#include "ra2r/core/win_unicode.h"
#include "ra2r/ui/ui.h"

#include "media_player.h"  // AUD/WAV/BIK 播放（FFmpeg 运行时绑定封装在此模块内）
#include "ra2r/render/minimap.h"
#include "ra2r/render/terrain_tile.h"
#include "ra2r/render/voxel_raster.h"

namespace fs = std::filesystem;
using ra2r::assets::AudFile;
using ra2r::assets::CsfFile;
using ra2r::assets::HvaFile;
using ra2r::assets::MixEntry;
using ra2r::assets::MixFile;

// ── 崩溃定位（临时）：未处理异常过滤器打印故障地址 ────────────────
#ifdef _WIN32
#include <windows.h>
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    const HMODULE base = GetModuleHandleW(nullptr);
    std::fprintf(stderr,
                 "CRASH code=%08lX addr=%p base=%p offset=%p\n",
                 static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                 ep->ExceptionRecord->ExceptionAddress, base,
                 reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) -
                                         reinterpret_cast<uintptr_t>(base)));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
using ra2r::assets::Palette;
using ra2r::assets::ShpFile;
using ra2r::assets::VxlFile;
using ra2r::assets::VxlSection;

namespace {

// ── 通用工具 ────────────────────────────────────────────────────────

enum class Kind {
    Unknown, Mix, Shp, Vxl, Pal, Text, Hex, Aud, Csf, Hva, Map, Bik, Pcx, Tmp
};

// 判定是否为地形瓦片 TMP（RA2：瓦片 60×30、模板 ≤64×64、帧偏移表自洽）
bool looks_like_tmp(const uint8_t* d, size_t size) {
    if (size < 20) return false;
    auto u32 = [&](size_t o) {
        return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) |
               (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
    };
    auto i32 = [&](size_t o) { return static_cast<int32_t>(u32(o)); };
    const uint32_t tw = u32(0), th = u32(4);
    if (tw == 0 || tw > 64 || th == 0 || th > 64) return false;
    if (i32(8) != 60 || i32(12) != 30) return false;  // RA2 瓦片恒 60×30
    if (16ull + static_cast<uint64_t>(tw) * th * 4 > size) return false;
    return true;
}

Kind detect_kind(const std::vector<uint8_t>& d) {
    if (MixFile::looks_like_mix(d.data(), d.size())) {
        MixFile probe;
        std::string err;
        if (probe.open(d.data(), d.size(), &err) && probe.structure_valid()) return Kind::Mix;
    }
    if (d.size() >= 4 && d[0] == 0 && d[1] == 0) return Kind::Shp; // 近似：进一步解析验证
    if (d.size() >= 16 && std::memcmp(d.data(), "Voxel Animation", 16) == 0) return Kind::Vxl;
    if (d.size() >= 12 && std::memcmp(d.data(), "BIK", 3) == 0 &&
        (d[3] == 'b' || d[3] == 'i' || d[3] == 'f' || d[3] == 'g' || d[3] == 'k'))
        return Kind::Bik;
    // PCX（ZSoft PC Paintbrush）：魔数 0x0A + 版本 5 + RLE 编码（RA2/YR UI 美术）
    if (d.size() >= 128 && d[0] == 0x0A && d[1] >= 2 && d[1] <= 5 && d[2] == 1)
        return Kind::Pcx;
    // 地形瓦片 TMP（60×30 菱形瓦片，模板 N×M 帧矩阵）
    if (looks_like_tmp(d.data(), d.size())) return Kind::Tmp;
    if (d.size() >= 4 && std::memcmp(d.data(), "JASC", 4) == 0) return Kind::Pal;
    if (d.size() >= 4 && d[0] == ' ' && d[1] == 'F' && d[2] == 'S' && d[3] == 'C') return Kind::Csf;
    if (d.size() == 768 || d.size() == 1024) return Kind::Pal;
    if (!d.empty()) {
        const size_t n = std::min<size_t>(d.size(), 4096);
        size_t printable = 0;
        bool has_nul = false;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t c = d[i];
            if (c == 0) { has_nul = true; break; }
            if (c == 9 || c == 10 || c == 13 || c >= 32) ++printable;
        }
        if (!has_nul && printable * 100 / n >= 95) return Kind::Text;
    }
    return Kind::Hex;
}

std::string fmt_size(uint32_t v) {
    char buf[32];
    if (v >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", v / 1048576.0);
    else if (v >= 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", v / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%u B", v);
    return buf;
}

// RGBA → SDL 纹理（尺寸感知：与已有纹理不符时重建，杜绝尺寸失配的越界写）
// stride：行字节距（媒体帧为 32 字节对齐，可能 > w*4）
void upload_texture(SDL_Texture*& tex, SDL_Renderer* renderer, int w, int h, int stride,
                    const std::vector<uint8_t>& rgba) {
    static int tex_w = 0, tex_h = 0;
    if (!tex || tex_w != w || tex_h != h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w;
        tex_h = h;
    }
    if (tex && rgba.size() >= static_cast<size_t>(stride) * h) {
        const SDL_Rect r{0, 0, w, h};
        SDL_UpdateTexture(tex, &r, rgba.data(), stride);
    }
}

// ── SHP 渲染 ────────────────────────────────────────────────────────

std::vector<uint8_t> render_shp(const ShpFile& shp, int frame, const Palette& pal,
                                bool grayscale) {
    std::vector<uint8_t> idx;
    std::string err;
    if (!shp.decode_frame(frame, idx, &err)) return {};
    Palette gray;
    if (grayscale) {
        for (int i = 0; i < 256; ++i) {
            gray.rgb[i * 3] = gray.rgb[i * 3 + 1] = gray.rgb[i * 3 + 2] =
                static_cast<uint8_t>(i);
        }
    }
    const Palette& p = grayscale ? gray : pal;
    std::vector<uint8_t> rgba(idx.size() * 4);
    for (size_t i = 0; i < idx.size(); ++i) {
        p.to_rgba(idx[i], rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]);
    }
    return rgba;
}

// ── VXL 软件光栅（M2 起统一走引擎渲染模块 ra2r::render::voxel_raster） ──

// ── BMP 输出（--shot 自检用） ────────────────────────────────────────

bool write_bmp(const fs::path& path, int w, int h, const std::vector<uint8_t>& rgba) {
    const int stride = (w * 3 + 3) & ~3;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto w16 = [&](uint16_t v) { f.put(v & 0xFF); f.put(v >> 8); };
    auto w32 = [&](uint32_t v) {
        f.put(v & 0xFF); f.put((v >> 8) & 0xFF); f.put((v >> 16) & 0xFF); f.put((v >> 24) & 0xFF);
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
    w32(2835); w32(2835);
    w32(0); w32(0);
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

// ── 应用状态 ────────────────────────────────────────────────────────

struct Level {
    std::string label;
    MixFile mix;
};

// MIX 树内发现的地图条目（递归扫描结果）
struct MapRef {
    std::string path;   // "lv0/lv1/..."（MIX 树路径，用于展示）
    std::string name;   // 条目名（未知为空）
    uint32_t id = 0, size = 0;
    std::vector<int> level_path; // 逐层条目索引（点击时按路径重读）
    int entry_idx = -1;
};

// ── 后台任务（耗时操作卸载到工作线程）────────────────────────────────
// 安全约定：worker 只持有启动前主线程做的快照（MixFile/MapFile 深拷贝），
// 绝不触碰 App/Session——会话关闭/替换无悬垂风险；完成标志与进度经原子量
// 共享，主线程每帧 poll_bg_jobs() 收结果并入 UI 状态。
struct BgJob {
    enum class Kind { Pool, MapScan, MapThumb, Export };
    Kind kind = Kind::Pool;
    std::thread thread;
    std::atomic<bool> done{false};
    std::shared_ptr<std::atomic<int>> progress = std::make_shared<std::atomic<int>>(-1);
    std::shared_ptr<std::atomic<int>> total = std::make_shared<std::atomic<int>>(-1);
    std::shared_ptr<std::atomic<int>> stage = std::make_shared<std::atomic<int>>(0);
    // 结果（worker 填；主线程在 done 后取出）
    std::map<std::string, std::vector<uint8_t>> pool;   // 调色盘池
    std::vector<MapRef> maps;                            // 地图扫描
    ra2r::render::RasterImage img;                      // 地图缩略图
    std::string img_src;
    ra2r::assets::TerrainTileset tileset;               // 新建瓦片集（回填缓存）
    bool tileset_ok = false;
    std::string theater;
    size_t export_bytes = 0;
    std::string export_path;
    bool export_ok = false;
    // 指纹：合并结果时校验（会话索引/层标签/条目号），过期丢弃
    int session_idx = -1;
    std::vector<std::string> level_labels;
    int entry_idx = -1;
    uint64_t thumb_gen = 0;   // 地图缩略图代次（会话内单调递增）
};

// 一个标签页 = 一个独立浏览会话（浏览栈 + 选中项 + 预览状态）
struct Session {
    std::vector<Level> stack;        // stack[0] = 根 mix
    std::string root_path;           // 根 mix 的完整路径（调色盘池兄弟扫描用）
    int selected = -1;               // 当前层选中条目（-1 = 无）
    std::vector<uint8_t> sel_data;
    Kind sel_kind = Kind::Unknown;
    ShpFile shp;
    VxlFile vxl;
    Palette pal;
    AudFile aud;
    CsfFile csf;
    HvaFile hva;
    bool shp_ok = false, vxl_ok = false;
    bool aud_ok = false, csf_ok = false, hva_ok = false;
    int shp_frame = 0, vxl_limb = 0, vxl_scale = 3, shp_scale = 2;
    // VXL 3D 视图状态（附加需求 2：可交互旋转）
    float vxl_yaw = 0.0f;                        // 绕 z 轴（默认 0 = 经典等距朝向）
    float vxl_pitch = 35.264f * 3.14159265f / 180.0f; // 绕 x 轴俯仰（默认 ≈ 等距俯角）
    int vxl_hva_frame = 0;
    bool vxl_view_dirty = true;
    bool hva_play = false;           // HVA 动画自动播放
    uint64_t hva_t0 = 0;             // 播放起点（SDL ticks）
    bool hva_vxl_linked = false;     // 独立 HVA 条目已联动同名 VXL（可 3D 预览）
    bool shp_play = false;
    uint64_t shp_t0 = 0;
    // 地图条目（.map/.yrm/.yro/.mmx 或含 [IsoMapPack5] 的 INI 壳）
    ra2r::assets::MapFile mapf;
    bool map_ok = false;
    // PCX 位图条目（UI 美术：载入画面/战役界面等）
    ra2r::assets::PcxFile pcx;
    bool pcx_ok = false;
    std::vector<uint8_t> pcx_rgba;
    // 地形瓦片条目（.tem/.urb/.sno/.des/.lun/.ubn，TMP 格式）
    ra2r::render::TerrainTile tmp;
    bool tmp_ok = false;
    int tmp_frame = 0;
    int tmp_scale = 2;
    // 音视频播放器（AUD/WAV 引擎解码；BIK 经 FFmpeg 运行时绑定，见 media_player.*）
    mixbrowser::MediaPlayer media;
    std::vector<MapRef> found_maps; // 本会话 MIX 树递归扫描出的全部地图
    bool map_scan_done = false;
    // 地图缩略图（后台构建结果；thumb_gen 变化即视为过期）
    ra2r::render::RasterImage map_thumb;
    std::string map_thumb_src;
    bool map_thumb_ready = false;
    bool map_thumb_uploaded = false;
    uint64_t thumb_gen = 0;      // 选择代次（丢弃过期后台结果）
    int pal_choice = -1;             // -1 = 灰度
    std::vector<std::string> pal_names;
    std::vector<std::vector<uint8_t>> pal_datas;
    std::vector<std::string> pal_keys;  // 每项的规范名（大写，未知为空），供自动识别匹配
    int sort_col = 0;
    bool sort_desc = false;
    std::vector<int> order;
    char find_buf[64] = {};
    char open_buf[1024] = {};
    // 预览重上传脏标记（组合内容版本号）
    uint64_t preview_dirty_ver = 0;
    uint64_t last_upload_ver = ~0ull;
};

struct App {
    SDL_Window* window = nullptr;
    std::vector<Session> sessions;   // 每个标签页一个会话
    int active = -1;                 // 当前标签索引（-1 = 无会话）
    std::string last_dir;
    std::map<uint32_t, std::string> global_names; // 跨 MIX 累积的名称库
    // 调色盘全局池（从所有会话根 MIX 递归收集，见 docs/formats/palettes.md）
    std::map<std::string, std::vector<uint8_t>> palette_pool;
    bool palette_pool_built = false;
    std::string auto_pal_hint;       // 预览面板显示用：最近一次自动识别说明
    SDL_Texture* preview_tex = nullptr;  // 活动标签的预览纹理
    int ptex_w = 0, ptex_h = 0;
    bool ptex_dirty = false;
    ImVec2 combo_min{}, combo_max{};
    bool combo_rect_valid = false;
    ImVec2 openbtn_min{}, openbtn_max{};
    bool openbtn_valid = false;
    std::vector<ImVec2> tab_btn_min, tab_btn_max;  // 标签按钮矩形（自测用）
    bool click_phase1 = false;   // 已合成"点击下拉框"事件
    bool click_phase2 = false;   // 已合成"点击弹出项"事件
    // ── 地图预览（瓦片集由后台缩略图任务构建并回填缓存）──
    std::map<std::string, ra2r::assets::TerrainTileset> tileset_cache; // 剧场 → 瓦片集
    // ── 后台任务（worker 不触碰 App；见 BgJob）──
    std::unique_ptr<BgJob> pool_job;      // 全局调色盘池搜索
    std::unique_ptr<BgJob> map_scan_job;  // 地图递归扫描
    std::unique_ptr<BgJob> map_thumb_job; // 地图缩略图构建
    std::unique_ptr<BgJob> export_job;    // 条目导出
    std::string status_msg;               // 状态栏一次性消息（导出结果等）
    uint64_t status_msg_t0 = 0;
    int ctx_entry = -1;                   // 右键菜单目标条目（列表内序号）
    std::vector<uint8_t> pending_export;  // 右键导出待写数据（保存对话框期间暂存）
};

Session& cur(App& a) { return a.sessions[a.active]; }
bool has_session(App& a) {
    return a.active >= 0 && a.active < static_cast<int>(a.sessions.size());
}

// 条目名：本级名称库 → 全局累积名称库
const std::string* entry_name(App& a, uint32_t id) {
    if (!has_session(a)) return nullptr;
    const std::string* n = cur(a).stack.back().mix.name_of(id);
    if (n) return n;
    const auto it = a.global_names.find(id);
    return it == a.global_names.end() ? nullptr : &it->second;
}

void merge_global_names(App& a, const MixFile& mix) {
    for (const auto& [id, name] : mix.names()) {
        a.global_names.emplace(id, name);
    }
}

void sort_entries(App& a) {
    Session& s = cur(a);
    Level& lv = s.stack.back();
    const auto& es = lv.mix.entries();
    s.order.resize(es.size());
    for (size_t i = 0; i < es.size(); ++i) s.order[i] = static_cast<int>(i);
    const int col = s.sort_col;
    const bool desc = s.sort_desc;
    std::sort(s.order.begin(), s.order.end(), [&](int x, int y) {
        const MixEntry& e1 = es[x];
        const MixEntry& e2 = es[y];
        bool less;
        switch (col) {
            case 1: less = e1.size != e2.size ? e1.size < e2.size : e1.id < e2.id; break;
            case 2: {
                const std::string* n1 = entry_name(a, e1.id);
                const std::string* n2 = entry_name(a, e2.id);
                less = (n1 ? *n1 : "") < (n2 ? *n2 : "");
                break;
            }
            default: less = e1.id < e2.id; break;
        }
        return desc ? !less : less;
    });
}

void collect_palettes(App& a) {
    Session& s = cur(a);
    s.pal_names.clear();
    s.pal_datas.clear();
    s.pal_keys.clear();
    Level& lv = s.stack.back();
    for (const auto& e : lv.mix.entries()) {
        if (e.size != 768 && e.size != 1024) continue;
        std::vector<uint8_t> d;
        if (!lv.mix.read_entry(e, d)) continue;
        bool ok = d.size() == 768;
        if (!ok && d.size() >= 4) ok = std::memcmp(d.data(), "JASC", 4) == 0;
        if (!ok) continue;
        char label[64];
        const std::string* nm = entry_name(a, e.id);
        std::snprintf(label, sizeof(label), "%08X %s (%uB)", e.id, nm ? nm->c_str() : "",
                      e.size);
        s.pal_names.emplace_back(label);
        s.pal_datas.push_back(std::move(d));
        std::string key;
        if (nm) {
            key = *nm;
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        }
        s.pal_keys.push_back(std::move(key));
    }
}

// ── SHP 调色盘自动识别（附加需求 1）──────────────────────────────────
// RA2/YR 已知调色盘全集（实测自 ra2.mix / ra2md.mix，见 docs/formats/palettes.md）：
//   地形 iso: ISOTEM/ISOSNO/ISOURB + YR: ISOLUN/ISODES
//   单位:     UNITTEM/UNITSNO/UNITURB + YR: UNITLUN/UNITDES
//   动画 ANIM；图标 CAMEO；菜单 PALETTE；剧场 TEMPERAT/SNOW + YR: URBANN/LUNAR/DESERT
// 识别规则（按优先级）：
//   1. SHP 名或 MIX 路径含 CAMEO/MOUSE → CAMEO.PAL / MOUSE.PAL
//   2. 路径含剧场 token：SNO>URB>LUN>DES，默认 TEM
//   3. 路径含 ISO → ISO<剧场>.PAL（地形）
//   4. 路径含 CACHE/ANIM/CONQ → ANIM.PAL（动画）
//   5. 其余（ra2/ra2md/local/localmd 等）→ UNIT<剧场>.PAL（单位）
//   6. 池中无匹配 → 灰度
const char* kKnownPalettes[] = {
    "ISOTEM.PAL", "ISOSNO.PAL", "ISOURB.PAL", "ISOLUN.PAL", "ISODES.PAL",
    "UNITTEM.PAL", "UNITSNO.PAL", "UNITURB.PAL", "UNITLUN.PAL", "UNITDES.PAL",
    "ANIM.PAL", "CAMEO.PAL", "PALETTE.PAL", "MOUSE.PAL",
    "TEMPERAT.PAL", "SNOW.PAL", "URBANN.PAL", "LUNAR.PAL", "DESERT.PAL"};

// 在 mix 树中递归按名查找（复用 mixdump 的 find 策略：先本级、再嵌套）
bool find_recursive(const MixFile& mix, const std::string& name, std::vector<uint8_t>& out,
                    int depth = 0) {
    if (depth > 3) return false;
    const MixEntry* e = mix.find_by_name(name);
    if (e && mix.read_entry(*e, out)) return true;
    for (const auto& ent : mix.entries()) {
        if (ent.size < 12) continue;
        std::vector<uint8_t> d;
        if (!mix.read_entry(ent, d)) continue;
        if (!MixFile::looks_like_mix(d.data(), d.size())) continue;
        MixFile probe;
        std::string err;
        if (probe.open(d.data(), d.size(), &err) && probe.structure_valid() &&
            find_recursive(probe, name, out, depth + 1)) {
            return true;
        }
    }
    return false;
}

std::string upper_str(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return v;
}

// 池中条目并入会话下拉框（本级已有的不重复）
void merge_pool_palettes(App& a) {
    Session& s = cur(a);
    for (const auto& [name, data] : a.palette_pool) {
        bool dup = false;
        for (const auto& k : s.pal_keys) {
            if (k == name) { dup = true; break; }
        }
        if (dup) continue;
        s.pal_names.emplace_back("全局: " + name);
        s.pal_datas.push_back(data);
        s.pal_keys.push_back(name);
    }
}

// 启发式：返回目标调色盘规范名（池中不存在时返回空串 → 灰度兜底）
std::string auto_palette_name(const std::string& shp_name, const std::string& mix_ctx) {
    const std::string u = upper_str(shp_name), c = upper_str(mix_ctx);
    auto has = [&c](const char* t) { return c.find(t) != std::string::npos; };
    if (u.find("CAMEO") != std::string::npos || has("CAMEO")) return "CAMEO.PAL";
    if (u.find("MOUSE") != std::string::npos || has("MOUSE")) return "MOUSE.PAL";
    std::string theater = "TEM";
    if (has("SNO")) theater = "SNO";
    else if (has("URB")) theater = "URB";
    else if (has("LUN")) theater = "LUN";
    else if (has("DES")) theater = "DES";
    if (has("ISO")) return "ISO" + theater + ".PAL";
    if (has("CACHE") || has("ANIM") || has("CONQ")) return "ANIM.PAL";
    return "UNIT" + theater + ".PAL";
}

// 自动选择调色板：返回 pal_choice（-1 = 灰度）
int auto_pick_palette(App& a, const std::string& shp_name, const std::string& mix_ctx) {
    Session& s = cur(a);
    const std::string target = auto_palette_name(shp_name, mix_ctx);
    for (size_t i = 0; i < s.pal_keys.size(); ++i) {
        if (s.pal_keys[i] == target) {
            a.auto_pal_hint = "自动识别: " + target;
            return static_cast<int>(i) + 1;
        }
    }
    if (target.empty()) {
        a.auto_pal_hint = "自动识别: 无候选调色盘";
    } else {
        a.auto_pal_hint = "自动识别: " + target + "（池中缺失，回退灰度）";
    }
    return -1;
}

// ── 后台任务（实现见"地图预览辅助"之后）──
void start_pool_job(App& a);
void start_map_scan_job(App& a);
void start_map_thumb_job(App& a);
void start_export_job(App& a, std::vector<uint8_t> data, std::string path);
void poll_bg_jobs(App& a);

// 确保调色盘可用（本级收集 + 后台池搜索 + 合并），并按 ctx 自动选；
// name 用于 CAMEO/MOUSE 特判（无则传空串）。池后台完成后的重识别由 poll_bg_jobs 补做。
void ensure_palette_picked(App& a, const std::string& name, const std::string& ctx) {
    Session& s = cur(a);
    collect_palettes(a);
    if (!a.palette_pool_built && !a.pool_job) start_pool_job(a);
    if (a.palette_pool_built) merge_pool_palettes(a);
    s.pal_choice = auto_pick_palette(a, name, ctx);
}

void select_entry(App& a, int idx) {
    Session& s = cur(a);
    s.selected = idx;
    s.shp_ok = false;
    s.vxl_ok = false;
    s.map_ok = false;
    s.pcx_ok = false;
    s.tmp_ok = false;
    s.tmp_frame = 0;
    s.hva_vxl_linked = false;
    s.media.close();
    s.sel_data.clear();
    s.sel_kind = Kind::Unknown;
    s.pal_choice = -1;
    s.map_thumb_ready = false;
    s.map_thumb_uploaded = false;
    ++s.thumb_gen;  // 新选择：此前的地图缩略图后台结果一律过期
    a.ptex_dirty = true;
    s.last_upload_ver = ~0ull; // 强制新选中项重新上传
    if (a.preview_tex) { SDL_DestroyTexture(a.preview_tex); a.preview_tex = nullptr; }
    a.ptex_w = a.ptex_h = 0;
    if (idx < 0) return;
    Level& lv = s.stack.back();
    const MixEntry& e = lv.mix.entries()[idx];
    if (!lv.mix.read_entry(e, s.sel_data)) return;
    s.sel_kind = detect_kind(s.sel_data);
    // 无魔数格式按名称后缀分发
    const std::string* nm = entry_name(a, e.id);
    if (nm) {
        std::string ext;
        const size_t dot = nm->rfind('.');
        if (dot != std::string::npos) {
            ext = nm->substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        }
        if (ext == ".AUD" || ext == ".WAV") {
            std::string err;
            s.aud_ok = s.aud.open(s.sel_data.data(), s.sel_data.size(), &err);
            s.sel_kind = s.aud_ok ? Kind::Aud : Kind::Hex;
        } else if (ext == ".CSF") {
            std::string err;
            s.csf_ok = s.csf.open(s.sel_data.data(), s.sel_data.size(), &err);
            s.sel_kind = s.csf_ok ? Kind::Csf : Kind::Hex;
        } else if (ext == ".HVA") {
            std::string err;
            s.hva_ok = s.hva.open(s.sel_data.data(), s.sel_data.size(), &err);
            s.sel_kind = s.hva_ok ? Kind::Hva : Kind::Hex;
        } else if (ext == ".MAP" || ext == ".YRM" || ext == ".YRO" || ext == ".MMX" ||
                   ext == ".MPR" || ext == ".PKR") {
            std::string err;
            s.map_ok = s.mapf.open(s.sel_data.data(), s.sel_data.size(), &err);
            s.sel_kind = s.map_ok ? Kind::Map : Kind::Text;
        } else if (ext == ".TEM" || ext == ".URB" || ext == ".SNO" || ext == ".DES" ||
                   ext == ".LUN" || ext == ".UBN") {
            // 地形瓦片（TMP 格式，扩展名 = 剧场后缀）
            std::string err;
            s.tmp_ok = s.tmp.open(s.sel_data.data(), s.sel_data.size(), &err);
            s.sel_kind = s.tmp_ok ? Kind::Tmp : Kind::Hex;
            s.tmp_frame = 0;
        }
    }
    // 无扩展名/未知名的地图（如 mapsmd03.mix 内十六进制 id 条目）：文本壳含
    // [IsoMapPack5] 即地图（文本头无 NUL，Kind::Text 会先命中，这里改判）
    if (s.sel_kind == Kind::Text && s.sel_data.size() < (16ull << 20)) {
        static const std::string kMapMarker = "[IsoMapPack5";
        if (std::search(s.sel_data.begin(), s.sel_data.end(), kMapMarker.begin(),
                        kMapMarker.end()) != s.sel_data.end()) {
            std::string err;
            s.map_ok = s.mapf.open(s.sel_data.data(), s.sel_data.size(), &err);
            if (s.map_ok) s.sel_kind = Kind::Map;
        }
    }
    if (s.sel_kind == Kind::Shp) {
        std::string err;
        s.shp_ok = s.shp.open(s.sel_data.data(), s.sel_data.size(), &err);
        if (!s.shp_ok) s.sel_kind = Kind::Hex;
        else {
            // 调色盘池搜索（递归读嵌套 MIX）较耗时 → 后台线程；完成后自动补合并
            std::string ctx = s.stack[0].label;
            for (size_t i = 1; i < s.stack.size(); ++i) ctx += "/" + s.stack[i].label;
            ensure_palette_picked(a, nm ? *nm : "", ctx);
            std::fprintf(stderr, "[auto-pal] %s (ctx=%s)\n", a.auto_pal_hint.c_str(),
                         ctx.c_str());
        }
    } else if (s.sel_kind == Kind::Vxl) {
        std::string err;
        s.vxl_ok = s.vxl.open(s.sel_data.data(), s.sel_data.size(), &err);
        if (!s.vxl_ok) s.sel_kind = Kind::Hex;
        else {
            // 尝试加载同名 HVA
            s.hva_ok = false;
            if (nm) {
                const std::string hva_name = nm->substr(0, nm->rfind('.')) + ".HVA";
                const MixEntry* he = lv.mix.find_by_name(hva_name);
                if (he) {
                    std::vector<uint8_t> hd;
                    if (lv.mix.read_entry(*he, hd)) {
                        s.hva_ok = s.hva.open(hd.data(), hd.size());
                    }
                }
            }
        }
    } else if (s.sel_kind == Kind::Hva) {
        // 独立 HVA 条目：联动同级同名 VXL，可直接 3D 动画预览
        if (nm) {
            const std::string vxl_name = nm->substr(0, nm->rfind('.')) + ".VXL";
            const MixEntry* ve = lv.mix.find_by_name(vxl_name);
            if (ve) {
                std::vector<uint8_t> vd;
                std::string err;
                if (lv.mix.read_entry(*ve, vd) && s.vxl.open(vd.data(), vd.size(), &err)) {
                    s.vxl_ok = true;
                    s.hva_vxl_linked = true;
                    s.vxl_limb = 0;
                    s.vxl_hva_frame = 0;
                }
            }
        }
    } else if (s.sel_kind == Kind::Pal) {
        std::string err;
        s.pal.load(s.sel_data.data(), s.sel_data.size(), &err);
    } else if (s.sel_kind == Kind::Pcx) {
        std::string err;
        if (!s.pcx.open(s.sel_data.data(), s.sel_data.size(), &err)) {
            s.sel_kind = Kind::Hex;
            std::fprintf(stderr, "[pcx] 打开失败: %s\n", err.c_str());
        } else if (!s.pcx.decode_rgba(s.pcx_rgba, &err)) {
            s.sel_kind = Kind::Hex;
            std::fprintf(stderr, "[pcx] 解码失败: %s\n", err.c_str());
        } else {
            s.pcx_ok = true;
            std::fprintf(stderr, "[pcx] 解码成功 %dx%d（%d 平面 × %d 位）\n", s.pcx.width(),
                         s.pcx.height(), s.pcx.planes(), s.pcx.bits_per_pixel());
        }
    } else if (s.sel_kind == Kind::Tmp) {
        // 地形瓦片：调色盘按 MIX 栈剧场 token 自动选（ISOTEM.MIX→ISOTEM.PAL 等）
        std::string err;
        if (!s.tmp.open(s.sel_data.data(), s.sel_data.size(), &err)) {
            s.sel_kind = Kind::Hex;
            std::fprintf(stderr, "[tmp] 打开失败: %s\n", err.c_str());
        } else {
            s.tmp_ok = true;
            s.tmp_frame = 0;
            std::string ctx = s.stack[0].label;
            for (size_t i = 1; i < s.stack.size(); ++i) ctx += "/" + s.stack[i].label;
            ensure_palette_picked(a, "", ctx);
            std::fprintf(stderr, "[tmp] 瓦片 %dx%d 模板 %dx%d 帧 %d (ctx=%s, %s)\n",
                         s.tmp.tile_w(), s.tmp.tile_h(), s.tmp.template_w(),
                         s.tmp.template_h(), s.tmp.frame_count(), ctx.c_str(),
                         a.auto_pal_hint.c_str());
        }
    }
}

void enter_mix(App& a, int idx) {
    Session& s = cur(a);
    Level& lv = s.stack.back();
    const MixEntry& e = lv.mix.entries()[idx];
    std::vector<uint8_t> d;
    if (!lv.mix.read_entry(e, d)) return;
    Level next;
    const std::string* nm = entry_name(a, e.id);
    char label[64];
    std::snprintf(label, sizeof(label), "%s%s%08X", nm ? nm->c_str() : "", nm ? " / " : "",
                  e.id);
    next.label = label;
    std::string err;
    if (!next.mix.open(d.data(), d.size(), &err) || !next.mix.structure_valid()) return;
    ra2r::assets::enrich_names(next.mix);
    merge_global_names(a, next.mix);
    s.stack.push_back(std::move(next));
    s.selected = -1;
    sort_entries(a);
}

// 打开一个 MIX 到会话（不含预览纹理等 App 级资源）
bool open_session(App& a, Session& s, const char* path) {
    Level root;
    std::string err;
    if (!root.mix.open(path, &err)) {
        std::fprintf(stderr, "mix open failed: %s\n", err.c_str());
        return false;
    }
    a.palette_pool_built = false; // 新会话根 → 调色盘池需重建
    ra2r::assets::enrich_names(root.mix);
    std::fprintf(stderr, "[open] %s: %u 条目, %zu 名字\n", path, root.mix.file_count(),
                 root.mix.names().size());
    merge_global_names(a, root.mix);
    root.label = fs::path(path).filename().string();
    s = Session{};
    s.root_path = path;
    s.stack.push_back(std::move(root));
    const auto& es = s.stack.back().mix.entries();
    s.order.resize(es.size());
    for (size_t i = 0; i < es.size(); ++i) s.order[i] = static_cast<int>(i);
    return true;
}

// 扫描目录下所有 .mix 并加载为标签页
void open_folder_at(App& a, const char* dir);

// 打开指定路径（文件→替换活动会话；目录→批量加载为标签页）
void open_path_at(App& a, const char* path) {
    std::error_code ec;
    if (fs::is_directory(fs::path(path), ec)) {
        open_folder_at(a, path);
        return;
    }
    if (!has_session(a)) {
        Session fresh;
        if (!open_session(a, fresh, path)) return;
        a.sessions.push_back(std::move(fresh));
        a.active = static_cast<int>(a.sessions.size()) - 1;
    } else {
        Session fresh;
        if (!open_session(a, fresh, path)) return;
        if (a.preview_tex) { SDL_DestroyTexture(a.preview_tex); a.preview_tex = nullptr; }
        a.ptex_w = a.ptex_h = 0;
        a.sessions[a.active] = std::move(fresh);
        std::snprintf(cur(a).open_buf, sizeof(cur(a).open_buf), "%s", path);
    }
    a.last_dir = fs::path(path).parent_path().string();
}

// 扫描目录下所有 .mix 并加载为标签页
void open_folder_at(App& a, const char* dir) {
    std::vector<fs::path> mixes;
    std::error_code ec;
    fs::directory_iterator it(fs::path(dir), ec);
    if (ec) {
        std::fprintf(stderr, "[dir] 无法读取目录: %s\n", dir);
        return;
    }
    for (const auto& e : it) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2)) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".mix") mixes.push_back(e.path());
    }
    std::sort(mixes.begin(), mixes.end(), [](const fs::path& x, const fs::path& y) {
        return x.filename().string() < y.filename().string();
    });
    int added = 0;
    for (const auto& p : mixes) {
        Session s;
        if (open_session(a, s, p.string().c_str())) {
            a.sessions.push_back(std::move(s));
            ++added;
        }
    }
    if (added > 0) {
        a.active = static_cast<int>(a.sessions.size()) - added;
        a.last_dir = dir;
        std::fprintf(stderr, "[dir] 目录 %s: 加载 %d 个 mix（共 %zu 个标签）\n", dir, added,
                     a.sessions.size());
    } else {
        std::fprintf(stderr, "[dir] 目录下未找到 .mix: %s\n", dir);
    }
}

// 激活标签页（销毁旧预览纹理，强制新标签重传预览）
void activate_session(App& a, int i) {
    if (i == a.active) return;
    // 切走前暂停当前标签的媒体播放（避免后台继续出声）
    if (has_session(a)) a.sessions[a.active].media.pause();
    if (a.preview_tex) { SDL_DestroyTexture(a.preview_tex); a.preview_tex = nullptr; }
    a.ptex_w = a.ptex_h = 0;
    a.active = i;
    if (has_session(a)) a.sessions[i].last_upload_ver = ~0ull;
}

// 关闭标签页
void close_session(App& a, int i) {
    if (i < 0 || i >= static_cast<int>(a.sessions.size())) return;
    if (i == a.active) {
        if (a.preview_tex) { SDL_DestroyTexture(a.preview_tex); a.preview_tex = nullptr; }
        a.ptex_w = a.ptex_h = 0;
    }
    a.sessions.erase(a.sessions.begin() + i);
    if (a.sessions.empty()) {
        a.active = -1;
    } else if (i < a.active) {
        --a.active;
    } else if (i == a.active) {
        a.active = std::min(a.active, static_cast<int>(a.sessions.size()) - 1);
    }
}

// SDL3 原生文件对话框回调（主线程事件泵内触发）
static void open_dialog_cb(void* userdata, const char* const* filelist, int) {
    App* a = static_cast<App*>(userdata);
    std::fprintf(stderr, "[dialog] callback: filelist=%p first=%s\n",
                 static_cast<const void*>(filelist),
                 (filelist && filelist[0]) ? filelist[0] : "(null/取消)");
    if (filelist && filelist[0]) open_path_at(*a, filelist[0]);
}

// SDL3 原生目录对话框回调
static void open_folder_dialog_cb(void* userdata, const char* const* filelist, int) {
    App* a = static_cast<App*>(userdata);
    if (filelist && filelist[0]) open_folder_at(*a, filelist[0]);
}

// ── 合成点击（自测用）：直接驱动 ImGui IO 事件队列 ──────────────────
// 说明：ImGui SDL3 后端的 NewFrame 每帧从 SDL 重新同步真实鼠标位置与按键，
// 因此 SDL 事件注入会被覆盖；必须在后端 NewFrame 之后、ImGui::NewFrame
// 之前写入 io.MousePos / AddMouseButtonEvent，且按下与抬起分属两帧。
static bool g_inject_armed = false;
static int g_inject_frame = -1;
static ImVec2 g_inject_pos{};

static void inject_click_at(int frame, ImVec2 pos) {
    g_inject_armed = true;
    g_inject_frame = frame;
    g_inject_pos = pos;
}

// 在 ImGui_ImplSDL3_NewFrame() 之后调用
static void inject_process(int frame) {
    if (!g_inject_armed) return;
    if (frame == g_inject_frame) {
        ImGui::GetIO().MousePos = g_inject_pos;
        ImGui::GetIO().AddMouseButtonEvent(0, true);
    } else if (frame == g_inject_frame + 1) {
        ImGui::GetIO().MousePos = g_inject_pos;
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        g_inject_armed = false;
    } else {
        ImGui::GetIO().MousePos = g_inject_pos;
    }
}

// 回归测试钩子：在 draw_list_pane 帧内（按钮区之后）触发一次 open_mix_at，
// 精确复现"中帧替换浏览栈"的悬垂引用路径
static bool g_reopen_hook = false;
static const char* g_reopen_hook_path = nullptr;
static int g_reopen_hook_frame = -1;

void do_find(App& a) {
    Session& s = cur(a);
    Level& lv = s.stack.back();
    const uint32_t id = MixFile::crc32_of_name(s.find_buf);
    const MixEntry* e = lv.mix.find(id);
    if (e) {
        const auto& es = lv.mix.entries();
        for (size_t i = 0; i < es.size(); ++i) {
            if (&es[i] == e) { select_entry(a, static_cast<int>(i)); return; }
        }
    }
}

// ── 右键导出 ─────────────────────────────────────────────────────────

// 保存对话框回调：拿暂存的数据启动后台导出任务
static void export_save_cb(void* userdata, const char* const* filelist, int) {
    App* a = static_cast<App*>(userdata);
    if (filelist && filelist[0]) {
        start_export_job(*a, std::move(a->pending_export), filelist[0]);
        std::fprintf(stderr, "[export] 开始导出 → %s\n", filelist[0]);
    }
    a->pending_export.clear();
    a->pending_export.shrink_to_fit();
}

// 右键"导出到文件…"：主线程读条目数据（内存 memcpy）→ 弹保存对话框
static void export_entry_dialog(App& a, int oi) {
    if (!has_session(a)) return;
    Level& lv = cur(a).stack.back();
    const auto& es = lv.mix.entries();
    if (oi < 0 || oi >= static_cast<int>(es.size())) return;
    const std::string* nm = entry_name(a, es[oi].id);
    std::string defname;
    if (nm && !nm->empty()) {
        defname = fs::path(*nm).filename().string();  // 名称可能含路径，取末段
        if (defname.size() > 120) defname = defname.substr(0, 120);
    } else {
        char idbuf[16];
        std::snprintf(idbuf, sizeof(idbuf), "%08X.bin", es[oi].id);
        defname = idbuf;
    }
    std::string loc;
    if (!a.last_dir.empty() && fs::exists(a.last_dir)) loc = a.last_dir + "\\" + defname;
    else loc = defname;
    if (!lv.mix.read_entry(es[oi], a.pending_export)) {
        a.status_msg = "读取条目失败";
        a.status_msg_t0 = SDL_GetTicks();
        return;
    }
    SDL_ShowSaveFileDialog(export_save_cb, &a, a.window, nullptr, 0, loc.c_str());
}

// 状态栏预留高度（列表/预览面板底部让位）
static float g_status_h = 0.0f;

// ── UI ─────────────────────────────────────────────────────────────

void draw_list_pane(App& a, int frame) {
    ImGui::BeginChild("list", ImVec2(430, -g_status_h), ImGuiChildFlags_Borders);
    // ── 按钮区：用会话指针；"打开路径"会替换会话对象，用后立即重绑 ──
    {
        Session* s = &cur(a);
        std::string crumb;
        for (const auto& l : s->stack) {
            if (!crumb.empty()) crumb += " > ";
            crumb += l.label;
        }
        ImGui::TextUnformatted(crumb.c_str());
        if (s->stack.size() > 1 && ImGui::Button("< 上一级")) {
            s->stack.pop_back();
            s->selected = -1;
            s->shp_ok = s->vxl_ok = false;
            sort_entries(a);
        }
        ImGui::SameLine();
        if (ImGui::Button("打开...")) {
            static const SDL_DialogFileFilter kFilters[] = {{"MIX 文件", "mix"}};
            const std::string loc =
                !a.last_dir.empty() && fs::exists(a.last_dir) ? a.last_dir : std::string();
            SDL_ShowOpenFileDialog(open_dialog_cb, &a, a.window, kFilters, 1,
                                   loc.empty() ? nullptr : loc.c_str(), false);
        }
        ImGui::SameLine();
        if (ImGui::Button("打开目录...")) {
            const std::string loc =
                !a.last_dir.empty() && fs::exists(a.last_dir) ? a.last_dir : std::string();
            SDL_ShowOpenFolderDialog(open_folder_dialog_cb, &a, a.window,
                                     loc.empty() ? nullptr : loc.c_str(), false);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##openpath", s->open_buf, sizeof(s->open_buf));
        ImGui::SameLine();
        if (ImGui::Button("打开路径")) {
            if (s->open_buf[0]) {
                open_path_at(a, s->open_buf);
                s = &cur(a); // 会话可能已被替换，重绑
            }
        }
        a.openbtn_min = ImGui::GetItemRectMin();
        a.openbtn_max = ImGui::GetItemRectMax();
        a.openbtn_valid = true;
        // 名称查找
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("##find", "名称查找 (如 RULESMD.INI)", s->find_buf,
                                 sizeof(s->find_buf));
        ImGui::SameLine();
        if (ImGui::Button("查找")) do_find(a);
    }
    // ── 回归测试：帧内触发一次 MIX 重开（模拟"打开路径"按钮的中帧替换）──
    if (g_reopen_hook && frame == g_reopen_hook_frame) {
        g_reopen_hook = false;
        std::fprintf(stderr, "[test-reopen] 帧内触发 open_path_at(%s)\n", g_reopen_hook_path);
        open_path_at(a, g_reopen_hook_path);
    }
    // ── 表格区：在按钮之后重新获取当前层（避免悬垂引用）──
    Session& s = cur(a);
    Level& lv = s.stack.back();
    ImGui::Text("条目: %u（右键条目可导出 / 复制 ID）", lv.mix.file_count());

    if (ImGui::BeginTable("entries", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("名称");
        ImGui::TableSetupColumn("大小");
        ImGui::TableHeadersRow();
        const auto& es = lv.mix.entries();
        for (int oi : s.order) {
            const MixEntry& e = es[oi];
            char idstr[16];
            std::snprintf(idstr, sizeof(idstr), "%08X", e.id);
            const std::string* nm = entry_name(a, e.id);
            ImGui::PushID(oi);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(idstr, s.selected == oi, ImGuiSelectableFlags_SpanAllColumns)) {
                select_entry(a, oi);
            }
            // 右键：导出/复制上下文菜单
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                a.ctx_entry = oi;
                ImGui::OpenPopup("entry_ctx");
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(nm ? nm->c_str() : "");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(fmt_size(e.size).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // ── 条目右键菜单 ──
    if (ImGui::BeginPopup("entry_ctx")) {
        const int oi = a.ctx_entry;
        const bool valid = oi >= 0 && oi < static_cast<int>(lv.mix.entries().size());
        if (valid) {
            const MixEntry& ce = lv.mix.entries()[oi];
            char cid[16];
            std::snprintf(cid, sizeof(cid), "%08X", ce.id);
            const std::string* cnm = entry_name(a, ce.id);
            if (ImGui::MenuItem("导出到文件…")) export_entry_dialog(a, oi);
            if (ImGui::MenuItem("复制 ID")) SDL_SetClipboardText(cid);
            ImGui::Separator();
            ImGui::TextDisabled("%s（%s）", cnm ? cnm->c_str() : cid,
                                fmt_size(ce.size).c_str());
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

// ── 地图预览辅助 ────────────────────────────────────────────────────

// 无游戏资源时的回退小地图：按高度着色的格点图（黑=空格，绿→亮=高度级）
ra2r::render::RasterImage map_height_preview(const ra2r::assets::MapFile& map) {
    ra2r::render::RasterImage img;
    img.w = map.cell_w();
    img.h = map.cell_h();
    img.rgba.assign(static_cast<size_t>(img.w) * img.h * 4, 0);
    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            const auto& c = map.cell(x, y);
            if (!c.present) continue;
            uint8_t* p = img.rgba.data() + (static_cast<size_t>(y) * img.w + x) * 4;
            const int h = std::min(15, static_cast<int>(c.height));
            p[0] = static_cast<uint8_t>(30 + h * 8);
            p[1] = static_cast<uint8_t>(70 + h * 11);
            p[2] = static_cast<uint8_t>(30 + h * 6);
            p[3] = 255;
        }
    }
    return img;
}

// 递归扫描 MIX 树内的全部地图（扩展名或 [IsoMapPack5] 内容判定）；
// progress 非空时每发现一张地图 +1（供状态栏展示后台扫描进度）
void scan_maps_rec(MixFile& mix, const std::string& path, const std::vector<int>& level_path,
                   int depth, std::vector<MapRef>& out, std::atomic<int>* progress = nullptr) {
    if (depth > 6) return;
    static const std::string kMarker = "[IsoMapPack5";
    const auto& es = mix.entries();
    for (int i = 0; i < static_cast<int>(es.size()); ++i) {
        const MixEntry& e = es[i];
        if (e.size == 0 || e.size > (8u << 20)) continue; // 地图不会超过 8MB
        std::vector<uint8_t> d;
        if (!mix.read_entry(e, d)) continue;
        const std::string* nm = mix.name_of(e.id);
        std::string label = nm ? *nm : "";
        if (label.empty()) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08X", e.id);
            label = buf;
        }
        bool is_map = false;
        if (nm) {
            const size_t dot = nm->rfind('.');
            if (dot != std::string::npos) {
                std::string ext = nm->substr(dot);
                for (auto& ch : ext) ch = static_cast<char>(std::toupper(ch));
                is_map = ext == ".MAP" || ext == ".YRM" || ext == ".YRO" || ext == ".MMX" ||
                         ext == ".MPR" || ext == ".PKR";
            }
        }
        if (!is_map && d.size() > 64 &&
            std::search(d.begin(), d.end(), kMarker.begin(), kMarker.end()) != d.end())
            is_map = true;
        if (is_map) {
            MapRef r;
            r.path = path.empty() ? label : path + "/" + label;
            r.name = nm ? *nm : "";
            r.id = e.id;
            r.size = e.size;
            r.level_path = level_path;
            r.entry_idx = i;
            out.push_back(std::move(r));
            if (progress) progress->fetch_add(1);
            continue;
        }
        if (MixFile::looks_like_mix(d.data(), d.size())) {
            MixFile sub;
            std::string err;
            if (sub.open(d.data(), d.size(), &err) && sub.structure_valid()) {
                ra2r::assets::enrich_names(sub);
                auto lp = level_path;
                lp.push_back(i);
                scan_maps_rec(sub, path.empty() ? label : path + "/" + label, lp, depth + 1,
                              out, progress);
            }
        }
    }
}

// 点击扫描结果：按 level_path 逐层重读地图字节并载入预览（不改变浏览栈）
bool load_map_ref(App& a, const MapRef& r) {
    Session& s = cur(a);
    if (r.level_path.empty()) return false;
    std::vector<uint8_t> d;
    MixFile* mix = &s.stack[0].mix;
    std::unique_ptr<MixFile> sub;
    for (size_t k = 0; k < r.level_path.size(); ++k) {
        const int idx = r.level_path[k];
        const auto& es = mix->entries();
        if (idx < 0 || idx >= static_cast<int>(es.size())) return false;
        if (!mix->read_entry(es[idx], d)) return false;
        if (k + 1 == r.level_path.size()) break; // 最后一层 = 地图本体
        auto next = std::make_unique<MixFile>();
        std::string err;
        if (!next->open(d.data(), d.size(), &err) || !next->structure_valid()) return false;
        ra2r::assets::enrich_names(*next);
        mix = next.get();
        sub = std::move(next);
    }
    // 复用 select_entry 的预览态清理（不改变 selected 索引）
    s.selected = -1;
    s.sel_data = std::move(d);
    s.sel_kind = Kind::Map;
    s.pal_choice = -1;
    s.map_thumb_ready = false;
    s.map_thumb_uploaded = false;
    ++s.thumb_gen;
    a.ptex_dirty = true;
    s.last_upload_ver = ~0ull;
    if (a.preview_tex) {
        SDL_DestroyTexture(a.preview_tex);
        a.preview_tex = nullptr;
    }
    a.ptex_w = a.ptex_h = 0;
    std::string err;
    s.map_ok = s.mapf.open(s.sel_data.data(), s.sel_data.size(), &err);
    return s.map_ok;
}

// ── 后台任务实现（调色盘池搜索 / 地图扫描 / 地图缩略图 / 导出）────────

// 调色盘池搜索 worker：在根 MIX 快照上递归查找；缺失时扫描同目录兄弟 MIX
static void pool_worker(BgJob* j, std::vector<std::pair<MixFile, std::string>> roots) {
    j->progress->store(0);
    j->total->store(static_cast<int>(sizeof(kKnownPalettes) / sizeof(kKnownPalettes[0])));
    std::map<std::string, std::vector<uint8_t>> out;
    auto collect_from = [&](const MixFile& m, const char* from) {
        for (const char* n : kKnownPalettes) {
            if (out.count(n)) continue;
            std::vector<uint8_t> d;
            if (find_recursive(m, n, d) && (d.size() == 768 || d.size() == 1024)) {
                out[n] = std::move(d);
                j->progress->store(static_cast<int>(out.size()));
                std::fprintf(stderr, "[palette-pool] %s (%zuB) from %s\n", n, out[n].size(),
                             from);
            }
        }
    };
    for (auto& [m, p] : roots) collect_from(m, p.c_str());
    // 兄弟 MIX 回退（仅调色盘缺失时打开，且只打开 RA2 系）
    bool missing = false;
    for (const char* n : kKnownPalettes) {
        if (!out.count(n)) { missing = true; break; }
    }
    if (missing) {
        std::set<std::string> scanned;
        for (const auto& [m, root_path] : roots) {
            if (root_path.empty()) continue;
            const fs::path dir = fs::path(root_path).parent_path();
            std::error_code ec;
            for (auto& de : fs::directory_iterator(dir, ec)) {
                if (!de.is_regular_file()) continue;
                const std::string fn = upper_str(de.path().filename().string());
                if (fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".MIX") != 0) continue;
                if (fn.find("RA2") == std::string::npos &&
                    fn.find("EXPAND") == std::string::npos &&
                    fn.find("LOCAL") == std::string::npos)
                    continue;
                if (scanned.count(fn)) continue;
                scanned.insert(fn);
                MixFile m2;
                std::string err;
                if (!m2.open(de.path(), &err)) continue;
                collect_from(m2, fn.c_str());
            }
            if (ec) break;
        }
    }
    j->pool = std::move(out);
    j->done = true;
}

void start_pool_job(App& a) {
    if (a.pool_job || a.palette_pool_built) return;
    auto j = std::make_unique<BgJob>();
    j->kind = BgJob::Kind::Pool;
    // 快照：根 MIX 深拷贝（已在内存中，一次 memcpy 换取线程零共享）
    std::vector<std::pair<MixFile, std::string>> roots;
    for (const Session& s : a.sessions) {
        if (s.stack.empty()) continue;
        roots.emplace_back(s.stack[0].mix, s.root_path);
    }
    if (roots.empty()) return;
    j->thread = std::thread([j = j.get(), roots = std::move(roots)]() mutable {
        pool_worker(j, std::move(roots));
    });
    a.pool_job = std::move(j);
}

void start_map_scan_job(App& a) {
    if (a.map_scan_job || !has_session(a)) return;
    Session& s = cur(a);
    if (s.stack.empty()) return;
    auto j = std::make_unique<BgJob>();
    j->kind = BgJob::Kind::MapScan;
    j->session_idx = a.active;
    j->level_labels = {s.stack[0].label};
    MixFile root = s.stack[0].mix;  // 快照
    const std::string label = s.stack[0].label;
    std::shared_ptr<std::atomic<int>> prog = j->progress;
    j->thread = std::thread([j = j.get(), root = std::move(root), label, prog]() mutable {
        std::vector<MapRef> out;
        scan_maps_rec(root, label, {}, 0, out, prog.get());
        j->maps = std::move(out);
        j->done = true;
    });
    a.map_scan_job = std::move(j);
}

void start_map_thumb_job(App& a) {
    if (a.map_thumb_job || !has_session(a)) return;
    Session& s = cur(a);
    if (!s.map_ok) return;
    std::fprintf(stderr, "[bg] 启动地图缩略图任务 (gen=%llu theater=%s)\n",
                 static_cast<unsigned long long>(s.thumb_gen), s.mapf.theater().c_str());
    auto j = std::make_unique<BgJob>();
    j->kind = BgJob::Kind::MapThumb;
    j->session_idx = a.active;
    j->entry_idx = s.selected;
    j->thumb_gen = s.thumb_gen;
    j->theater = s.mapf.theater();
    ra2r::assets::MapFile map = s.mapf;   // 快照
    const std::string theater = s.mapf.theater();
    const std::string gdir = ra2r::core::find_game_dir();
    // 已缓存的瓦片集一并快照（shared_ptr 深拷贝，避免 worker 重复构建）
    std::shared_ptr<ra2r::assets::TerrainTileset> ts;
    const auto it = a.tileset_cache.find(theater);
    if (it != a.tileset_cache.end())
        ts = std::make_shared<ra2r::assets::TerrainTileset>(it->second);
    j->thread = std::thread([j = j.get(), map = std::move(map), theater, gdir,
                             ts]() mutable {
        ra2r::render::RasterImage raw;
        std::string src;
        if (ts) {
            raw = ra2r::render::render_minimap(map, *ts);
            src = "雷达色小地图（剧场资源，已转置）";
        } else {
            j->stage->store(1);  // 构建瓦片集
            ra2r::assets::TerrainTileset built;
            ra2r::assets::FileIndex idx;
            std::vector<uint8_t> ini;
            bool ok = false;
            const auto cfg = ra2r::assets::theater_config(theater);
            if (!gdir.empty() && idx.build(gdir)) ok = idx.read(cfg.ini, ini);
            if (ok) ok = built.build(ini.data(), ini.size(), cfg.ext, nullptr);
            j->stage->store(2);  // 渲染
            if (ok) {
                raw = ra2r::render::render_minimap(map, built);
                src = "雷达色小地图（剧场资源，已转置）";
                j->tileset = std::move(built);
                j->tileset_ok = true;
            } else {
                raw = map_height_preview(map);
                src = "高度图回退（未找到游戏目录，无法取剧场资源，已转置）";
            }
        }
        // 转置：out[j][i] = in[i][j]（展示方向与 FinalAlert2 一致）
        ra2r::render::RasterImage img;
        img.w = raw.h;
        img.h = raw.w;
        img.rgba.assign(static_cast<size_t>(img.w) * img.h * 4, 0);
        for (int y = 0; y < raw.h; ++y)
            for (int x = 0; x < raw.w; ++x)
                std::memcpy(&img.rgba[(static_cast<size_t>(x) * img.w + y) * 4],
                            &raw.rgba[(static_cast<size_t>(y) * raw.w + x) * 4], 4);
        // 小地图黑色透明化，避免整块黑底
        for (size_t i = 0; i < img.rgba.size() / 4; ++i) {
            if (img.rgba[i * 4] == 0 && img.rgba[i * 4 + 1] == 0 && img.rgba[i * 4 + 2] == 0)
                img.rgba[i * 4 + 3] = 0;
        }
        j->img = std::move(img);
        j->img_src = std::move(src);
        j->done = true;
    });
    a.map_thumb_job = std::move(j);
}

void start_export_job(App& a, std::vector<uint8_t> data, std::string path) {
    if (a.export_job) return;
    auto j = std::make_unique<BgJob>();
    j->kind = BgJob::Kind::Export;
    j->export_path = path;
    j->thread = std::thread([j = j.get(), data = std::move(data), path = std::move(path)]() mutable {
        std::ofstream f(path, std::ios::binary);
        bool ok = static_cast<bool>(f);
        if (ok) {
            f.write(reinterpret_cast<const char*>(data.data()),
                    static_cast<std::streamsize>(data.size()));
            ok = static_cast<bool>(f);
        }
        j->export_bytes = data.size();
        j->export_ok = ok;
        j->export_path = path;
        j->done = true;
    });
    a.export_job = std::move(j);
}

// 每帧调用：收已完成的后台任务，合并结果（过期指纹丢弃），并保证当前
// 选中地图的缩略图任务在跑
void poll_bg_jobs(App& a) {
    // 取出 + join + 读取（避免 reset 顺序问题）
    if (a.pool_job && a.pool_job->done) {
        std::unique_ptr<BgJob> j = std::move(a.pool_job);
        if (j->thread.joinable()) j->thread.join();
        for (auto& [n, d] : j->pool)
            if (!a.palette_pool.count(n)) a.palette_pool[n] = std::move(d);
        a.palette_pool_built = true;
        // 当前选中 SHP → 补做合并与自动识别（后台结果到达后预览刷新）
        if (has_session(a)) {
            Session& s = cur(a);
            if (s.sel_kind == Kind::Shp && s.shp_ok) {
                merge_pool_palettes(a);
                const std::string* nm = nullptr;
                Level& lv = s.stack.back();
                if (s.selected >= 0 && s.selected < static_cast<int>(lv.mix.entries().size()))
                    nm = entry_name(a, lv.mix.entries()[s.selected].id);
                std::string ctx = s.stack[0].label;
                for (size_t i = 1; i < s.stack.size(); ++i) ctx += "/" + s.stack[i].label;
                s.pal_choice = auto_pick_palette(a, nm ? *nm : "", ctx);
                s.last_upload_ver = ~0ull;  // 强制重渲染
                std::fprintf(stderr, "[auto-pal] 池后台完成后重识别: %s\n",
                             a.auto_pal_hint.c_str());
            }
        }
    }
    if (a.map_scan_job && a.map_scan_job->done) {
        std::unique_ptr<BgJob> j = std::move(a.map_scan_job);
        if (j->thread.joinable()) j->thread.join();
        const int si = j->session_idx;
        const std::string lbl = j->level_labels.empty() ? std::string() : j->level_labels[0];
        if (si >= 0 && si < static_cast<int>(a.sessions.size()) &&
            !a.sessions[si].stack.empty() && a.sessions[si].stack[0].label == lbl) {
            a.sessions[si].found_maps = std::move(j->maps);
            a.sessions[si].map_scan_done = true;
        }
    }
    if (a.map_thumb_job && a.map_thumb_job->done) {
        std::unique_ptr<BgJob> j = std::move(a.map_thumb_job);
        if (j->thread.joinable()) j->thread.join();
        std::fprintf(stderr, "[bg] 缩略图任务完成 %dx%d src=%s\n", j->img.w, j->img.h,
                     j->img_src.c_str());
        if (j->tileset_ok) a.tileset_cache[j->theater] = std::move(j->tileset);
        const int si = j->session_idx;
        if (has_session(a) && a.active == si) {
            Session& s = cur(a);
            if (s.map_ok && s.selected == j->entry_idx && !s.map_thumb_ready &&
                s.thumb_gen == j->thumb_gen) {
                s.map_thumb = std::move(j->img);
                s.map_thumb_src = j->img_src;
                s.map_thumb_ready = true;
                s.map_thumb_uploaded = false;
            }
        }
    }
    if (a.export_job && a.export_job->done) {
        std::unique_ptr<BgJob> j = std::move(a.export_job);
        if (j->thread.joinable()) j->thread.join();
        if (j->export_ok) {
            a.status_msg = "已导出 " + fmt_size(static_cast<uint32_t>(j->export_bytes)) + " → " +
                           j->export_path;
        } else {
            a.status_msg = "导出失败: " + j->export_path;
        }
        a.status_msg_t0 = SDL_GetTicks();
    }
    // 当前选中地图且无缩略图任务 → 接续启动（含旧任务过期被丢弃的情形）
    if (has_session(a) && !a.map_thumb_job) {
        Session& s = cur(a);
        if (s.map_ok && !s.map_thumb_ready) start_map_thumb_job(a);
    }
}

// 状态栏：展示后台任务进度 + 会话概览 + 一次性消息（导出结果等）
void draw_status_bar(App& a) {
    ImGui::BeginChild("statusbar", ImVec2(0, ImGui::GetFrameHeight() + 8),
                      ImGuiChildFlags_Borders);
    std::string line;
    bool busy = false;
    auto add = [&line, &busy](const std::string& s, bool is_busy) {
        if (busy) line += "   |   ";
        if (is_busy) {
            line += "\xE2\x8F\xB3 ";  // ⏳
            busy = true;
        }
        line += s;
    };
    if (a.pool_job) {
        const int f = a.pool_job->progress->load();
        const int t = a.pool_job->total->load();
        add("正在搜索调色盘池… " + std::to_string(f) + "/" + std::to_string(t), true);
    }
    if (a.map_scan_job) {
        const int f = a.map_scan_job->progress->load();
        add(f >= 0 ? "正在扫描地图… 已发现 " + std::to_string(f) + " 张"
                   : "正在扫描地图…",
            true);
    }
    if (a.map_thumb_job) {
        const int st = a.map_thumb_job->stage->load();
        add(st >= 2 ? "正在渲染小地图…" : "正在构建剧场瓦片集…", true);
    }
    if (a.export_job) add("正在导出…", true);
    if (has_session(a)) {
        Session& s = cur(a);
        Level& lv = s.stack.back();
        add("条目 " + std::to_string(lv.mix.file_count()), false);
    }
    if (!a.status_msg.empty() && SDL_GetTicks() - a.status_msg_t0 < 5000) add(a.status_msg, false);
    if (line.empty()) line = "就绪";
    ImGui::TextUnformatted(line.c_str());
    ImGui::EndChild();
}

// ── 体素 3D 动画视图（Kind::Vxl 与联动了 VXL 的 Kind::Hva 共用）──
// HVA 自动播放：vxl_hva_frame 随时间推进；脏版本含帧号，每帧自动重光栅
void draw_voxel_anim_view(App& a, SDL_Renderer* renderer) {
    Session& s = cur(a);
    if (s.vxl_limb < 0) s.vxl_limb = 0;
    if (s.vxl_limb >= static_cast<int>(s.vxl.sections().size())) s.vxl_limb = 0;
    ImGui::Text("VXL %u limbs", static_cast<unsigned>(s.vxl.sections().size()));
    if (s.hva_ok && s.hva.frame_count() > 0) {
        ImGui::Text("HVA: %u 帧 × %u sections", s.hva.frame_count(), s.hva.section_count());
        if (s.vxl_hva_frame >= static_cast<int>(s.hva.frame_count())) s.vxl_hva_frame = 0;
        if (s.hva_play) {
            if (s.hva_t0 == 0) s.hva_t0 = SDL_GetTicks();
            s.vxl_hva_frame = static_cast<int>((SDL_GetTicks() - s.hva_t0) / 100) %
                              static_cast<int>(s.hva.frame_count());
        } else {
            s.hva_t0 = 0;
        }
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("##hvaframe", &s.vxl_hva_frame, 0,
                             static_cast<int>(s.hva.frame_count()) - 1)) {
            s.hva_play = false;
            s.vxl_view_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("播放动画", &s.hva_play)) {
            s.hva_t0 = 0;
            s.vxl_view_dirty = true;
        }
    }
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("##limb", &s.vxl_limb, 0,
                         static_cast<int>(s.vxl.sections().size()) - 1)) {
        s.vxl_view_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("重置视角")) {
        s.vxl_yaw = 0.0f;
        s.vxl_pitch = 35.264f * 3.14159265f / 180.0f;
        s.vxl_scale = 3;
        s.vxl_view_dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("拖拽旋转 · 滚轮缩放");
    // 视图脏版本：量化浮点角度避免浮点比较抖动
    const uint64_t ver = (static_cast<uint64_t>(s.selected) << 40) |
                         (static_cast<uint64_t>(s.vxl_limb) << 30) |
                         (static_cast<uint64_t>(s.vxl_hva_frame) << 20) |
                         (static_cast<uint64_t>(s.vxl_scale) << 12) |
                         (static_cast<uint64_t>(static_cast<int>(s.vxl_yaw * 60.0f)) << 6) |
                         static_cast<uint64_t>(static_cast<int>(s.vxl_pitch * 60.0f));
    if (ver != s.last_upload_ver || s.vxl_view_dirty) {
        s.last_upload_ver = ver;
        s.vxl_view_dirty = false;
        // 体素光栅统一走引擎渲染模块（ra2r::render::voxel_raster）
        const ra2r::render::RasterImage r = ra2r::render::rasterize_voxel_section(
            s.vxl, s.hva_ok ? &s.hva : nullptr, s.vxl_limb, s.vxl_hva_frame,
            ra2r::render::VoxelView{s.vxl_yaw, s.vxl_pitch,
                                    static_cast<float>(s.vxl_scale)});
        if (!r.empty()) {
            a.ptex_w = r.w;
            a.ptex_h = r.h;
            upload_texture(a.preview_tex, renderer, r.w, r.h, r.w * 4, r.rgba);
        }
    }
    // 3D 视图：先画纹理，再覆盖 InvisibleButton 捕获拖拽/滚轮
    if (a.preview_tex) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(a.preview_tex),
                     ImVec2(static_cast<float>(a.ptex_w), static_cast<float>(a.ptex_h)));
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("vxl3d",
                               ImVec2(static_cast<float>(a.ptex_w),
                                      static_cast<float>(a.ptex_h)),
                               ImGuiButtonFlags_MouseButtonLeft);
        if (ImGui::IsItemActive()) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            s.vxl_yaw -= d.x * 0.012f;
            s.vxl_pitch += d.y * 0.012f;
            if (s.vxl_pitch > 1.52f) s.vxl_pitch = 1.52f;
            if (s.vxl_pitch < -1.52f) s.vxl_pitch = -1.52f;
        }
        if (ImGui::IsItemHovered()) {
            const float wh = ImGui::GetIO().MouseWheel;
            if (wh != 0.0f) {
                const int ns = s.vxl_scale + (wh > 0 ? 1 : -1);
                s.vxl_scale = std::max(1, std::min(8, ns));
            }
        }
    }
}

void draw_preview_pane(App& a, SDL_Renderer* renderer) {
    // 填满剩余宽度（(0,0) 是按内容自适应，之前预览面板只有几百像素宽）；
    // 底部为状态栏预留高度
    const float pane_w = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("preview", ImVec2(pane_w, -g_status_h), ImGuiChildFlags_Borders);
    Session& s = cur(a);
    if (s.sel_data.empty()) { // 直载路径（地图扫描/HVA 测试）selected=-1 但 sel_data 非空
        ImGui::TextUnformatted("选中左侧条目以预览");
        ImGui::Separator();
        // ── 地图总览：后台递归扫描本 MIX 树内的全部地图，点击直接预览 ──
        const bool scanning_this =
            a.map_scan_job && a.map_scan_job->session_idx == a.active;
        if (ImGui::Button(s.map_scan_done ? "重新扫描地图" : "扫描本 MIX 全部地图")) {
            if (a.map_scan_job) {
                a.status_msg = "已有扫描任务进行中";
                a.status_msg_t0 = SDL_GetTicks();
            } else {
                s.found_maps.clear();
                s.map_scan_done = false;
                start_map_scan_job(a);
            }
        }
        if (scanning_this) {
            const int f = a.map_scan_job->progress->load();
            ImGui::Text(f >= 0 ? "正在扫描地图… 已发现 %d 张" : "正在扫描地图…", f);
        }
        if (s.map_scan_done) {
            ImGui::Text("发现 %zu 张地图（点击预览；扫描需读取条目，大 MIX 首次较慢）",
                        s.found_maps.size());
            if (ImGui::BeginTable("maps", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                  ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("路径");
                ImGui::TableSetupColumn("id");
                ImGui::TableSetupColumn("大小");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < s.found_maps.size(); ++i) {
                    const MapRef& r = s.found_maps[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(r.path.c_str())) load_map_ref(a, r);
                    ImGui::TableNextColumn();
                    ImGui::Text("%08X", r.id);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(fmt_size(r.size).c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        return;
    }
    Level& lv = s.stack.back();
    if (s.selected >= 0 && s.selected < static_cast<int>(lv.mix.entries().size())) {
        const MixEntry& e = lv.mix.entries()[s.selected];
        const std::string* nm = entry_name(a, e.id);
        ImGui::Text("id=%08X  大小=%s  类型=%s", e.id, fmt_size(e.size).c_str(),
                    nm ? nm->c_str() : "");
        ImGui::SameLine();
        if (ImGui::Button("导出到文件…")) export_entry_dialog(a, s.selected);
        ImGui::Separator();
    }

    switch (s.sel_kind) {
        case Kind::Mix: {
            MixFile probe;
            std::string err;
            if (probe.open(s.sel_data.data(), s.sel_data.size(), &err)) {
                ImGui::Text("嵌套 MIX：%u 个条目", probe.file_count());
                if (ImGui::Button("进入此 MIX")) enter_mix(a, s.selected);
            }
            break;
        }
        case Kind::Shp: {
            if (s.shp.frame_count() == 0) {
                ImGui::TextUnformatted("SHP 帧数为 0");
                break;
            }
            if (s.shp_play && s.shp_t0 == 0) s.shp_t0 = SDL_GetTicks();
            if (s.shp_play) {
                const int n = s.shp.frame_count();
                s.shp_frame = static_cast<int>((SDL_GetTicks() - s.shp_t0) / 120) % n;
            } else {
                s.shp_t0 = 0;
            }
            if (s.shp_frame < 0) s.shp_frame = 0;
            if (s.shp_frame >= s.shp.frame_count()) s.shp_frame = s.shp.frame_count() - 1;
            ImGui::Text("SHP %ux%u, %u 帧", s.shp.width(), s.shp.height(),
                        s.shp.frame_count());
            ImGui::SliderInt("帧", &s.shp_frame, 0, s.shp.frame_count() - 1);
            ImGui::SameLine();
            ImGui::Checkbox("播放", &s.shp_play);
            ImGui::SliderInt("缩放", &s.shp_scale, 1, 8);
            // 调色板选择
            std::vector<const char*> pal_opts;
            pal_opts.push_back("灰度");
            for (const auto& n : s.pal_names) pal_opts.push_back(n.c_str());
            if (s.pal_choice >= static_cast<int>(pal_opts.size())) s.pal_choice = -1;
            ImGui::SetNextItemWidth(420);
            ImGui::Combo("调色板", &s.pal_choice, pal_opts.data(),
                         static_cast<int>(pal_opts.size()));
            a.combo_min = ImGui::GetItemRectMin();
            a.combo_max = ImGui::GetItemRectMax();
            a.combo_rect_valid = true;
            if (!a.auto_pal_hint.empty()) {
                ImGui::TextDisabled("%s", a.auto_pal_hint.c_str());
            }
            Palette* p = nullptr;
            if (s.pal_choice > 0 &&
                s.pal_choice - 1 < static_cast<int>(s.pal_datas.size())) {
                s.pal.load(s.pal_datas[s.pal_choice - 1].data(),
                           s.pal_datas[s.pal_choice - 1].size());
                p = &s.pal;
            }
            // 内容版本：选中项 + 帧 + 调色板 + 缩放（播放时每帧都变）
            s.preview_dirty_ver = (static_cast<uint64_t>(s.selected) << 40) |
                                  (static_cast<uint64_t>(s.shp_frame) << 20) |
                                  (static_cast<uint64_t>(s.pal_choice + 2) << 10) |
                                  static_cast<uint64_t>(s.shp_scale);
            if (s.shp_play) s.preview_dirty_ver ^= SDL_GetTicks();
            if (s.preview_dirty_ver != s.last_upload_ver) {
                s.last_upload_ver = s.preview_dirty_ver;
                const auto rgba = render_shp(s.shp, s.shp_frame, p ? *p : s.pal,
                                             s.pal_choice <= 0);
                if (!rgba.empty()) {
                    const auto& fh = s.shp.frame(s.shp_frame);
                    a.ptex_w = fh.cx;
                    a.ptex_h = fh.cy;
                    upload_texture(a.preview_tex, renderer, a.ptex_w, a.ptex_h, a.ptex_w * 4,
                                   rgba);
                }
            }
            if (a.preview_tex) {
                const auto& fh = s.shp.frame(s.shp_frame);
                ImGui::Image(reinterpret_cast<ImTextureID>(a.preview_tex),
                             ImVec2(fh.cx * s.shp_scale, fh.cy * s.shp_scale));
            }
            break;
        }
        case Kind::Vxl: {
            draw_voxel_anim_view(a, renderer);
            break;
        }
        case Kind::Map: {
            if (!s.map_ok) {
                ImGui::TextUnformatted("地图解析失败");
                break;
            }
            const std::string mname = s.mapf.ini().get("Basic", "Name", "");
            const int players = 1 + std::max<int>(s.mapf.buildings().size() > 8 ? 2 : 1,
                                                  s.mapf.units().size() > 6 ? 2 : 1);
            ImGui::Text("地图: %s", mname.c_str());
            ImGui::Text("剧场 %s · %dx%d 格 · 对象: 建筑%zu / 单位%zu / 步兵%zu",
                        s.mapf.theater().c_str(), s.mapf.cell_w(), s.mapf.cell_h(),
                        s.mapf.buildings().size(), s.mapf.units().size(),
                        s.mapf.infantry().size());
            ImGui::Separator();
            // 小地图：后台线程构建（剧场瓦片集 + 渲染 + 转置），完成后上纹理；
            // 展示方向与 FinalAlert2 一致：行列转置（用户实测对照，仅本工具展示层）
            if (!s.map_thumb_ready) {
                std::string prog = "正在构建小地图…";
                if (a.map_thumb_job && a.map_thumb_job->session_idx == a.active) {
                    const int st = a.map_thumb_job->stage->load();
                    if (st == 1) prog = "正在构建剧场瓦片集…";
                    else if (st >= 2) prog = "正在渲染小地图…";
                }
                ImGui::TextUnformatted(prog.c_str());
                break;
            }
            const ra2r::render::RasterImage& img = s.map_thumb;
            if (!s.map_thumb_uploaded && img.w > 0 && img.h > 0 && !img.rgba.empty()) {
                upload_texture(a.preview_tex, renderer, img.w, img.h, img.w * 4, img.rgba);
                a.ptex_w = img.w;
                a.ptex_h = img.h;
                s.map_thumb_uploaded = true;
            }
            if (s.map_thumb_uploaded) {
                const float scale = std::max(1.0f, std::min(6.0f, 420.0f / img.w));
                ImGui::Text("%s（%dx%d × %.1f）", s.map_thumb_src.c_str(), img.w, img.h,
                            scale);
                ImGui::Image(reinterpret_cast<ImTextureID>(a.preview_tex),
                             ImVec2(img.w * scale, img.h * scale));
            }
            break;
        }
        case Kind::Pal: {
            std::vector<uint8_t> rgba(256 * 4);
            for (int i = 0; i < 256; ++i) {
                s.pal.to_rgba(static_cast<uint8_t>(i), rgba[i * 4], rgba[i * 4 + 1],
                              rgba[i * 4 + 2], rgba[i * 4 + 3]);
            }
            a.ptex_w = 256;
            a.ptex_h = 1;
            upload_texture(a.preview_tex, renderer, 256, 1, 256 * 4, rgba);
            ImGui::TextUnformatted("PAL (256 色)");
            ImGui::Image(reinterpret_cast<ImTextureID>(a.preview_tex), ImVec2(512, 24));
            break;
        }
        case Kind::Pcx: {
            if (!s.pcx_ok || s.pcx_rgba.empty()) {
                ImGui::TextUnformatted("PCX 解码失败");
                break;
            }
            const int w = s.pcx.width();
            const int h = s.pcx.height();
            if (!a.preview_tex || a.ptex_w != w || a.ptex_h != h) {
                upload_texture(a.preview_tex, renderer, w, h, w * 4, s.pcx_rgba);
                a.ptex_w = w;
                a.ptex_h = h;
            }
            ImGui::Text("PCX %dx%d（%d 平面 × %d 位%s）", w, h, s.pcx.planes(),
                        s.pcx.bits_per_pixel(),
                        s.pcx.has_palette() ? "，256 色调色板" : "，24 位真彩");
            const float scale = std::max(0.5f, std::min(2.0f, 640.0f / w));
            ImGui::Image(reinterpret_cast<ImTextureID>(a.preview_tex),
                         ImVec2(w * scale, h * scale));
            break;
        }
        case Kind::Tmp: {
            if (!s.tmp_ok || s.tmp.frame_count() == 0) {
                ImGui::TextUnformatted("地形瓦片解析失败");
                break;
            }
            if (s.tmp_frame < 0 || s.tmp_frame >= s.tmp.frame_count()) s.tmp_frame = 0;
            ImGui::Text("地形瓦片 %dx%d · 模板 %dx%d · %d 帧", s.tmp.tile_w(), s.tmp.tile_h(),
                        s.tmp.template_w(), s.tmp.template_h(), s.tmp.frame_count());
            if (s.tmp.frame_count() > 1) {
                ImGui::SliderInt("帧", &s.tmp_frame, 0, s.tmp.frame_count() - 1);
            } else {
                ImGui::TextDisabled("单帧瓦片");
            }
            ImGui::SameLine();
            ImGui::SliderInt("缩放", &s.tmp_scale, 1, 8);
            // 调色板选择（与 SHP 同一套：灰度 / 本级 PAL / 全局池）
            std::vector<const char*> pal_opts;
            pal_opts.push_back("灰度");
            for (const auto& n : s.pal_names) pal_opts.push_back(n.c_str());
            if (s.pal_choice >= static_cast<int>(pal_opts.size())) s.pal_choice = -1;
            ImGui::SetNextItemWidth(420);
            ImGui::Combo("调色板", &s.pal_choice, pal_opts.data(),
                         static_cast<int>(pal_opts.size()));
            if (!a.auto_pal_hint.empty()) ImGui::TextDisabled("%s", a.auto_pal_hint.c_str());
            static const std::array<uint8_t, 768> kGrayRamp = [] {
                std::array<uint8_t, 768> g{};
                for (int i = 0; i < 256; ++i)
                    g[i * 3] = g[i * 3 + 1] = g[i * 3 + 2] = static_cast<uint8_t>(i);
                return g;
            }();
            const uint8_t* pal = kGrayRamp.data();
            if (s.pal_choice > 0 &&
                s.pal_choice - 1 < static_cast<int>(s.pal_datas.size())) {
                s.pal.load(s.pal_datas[s.pal_choice - 1].data(),
                           s.pal_datas[s.pal_choice - 1].size());
                pal = s.pal.rgb.data();
            }
            // 内容版本：选中项 + 帧 + 调色板 + 缩放
            const uint64_t ver = (static_cast<uint64_t>(s.selected) << 40) |
                                 (static_cast<uint64_t>(s.tmp_frame) << 20) |
                                 (static_cast<uint64_t>(s.pal_choice + 2) << 10) |
                                 static_cast<uint64_t>(s.tmp_scale);
            if (ver != s.last_upload_ver) {
                s.last_upload_ver = ver;
                const ra2r::render::RasterImage img = s.tmp.render_frame(s.tmp_frame, pal);
                if (!img.rgba.empty()) {
                    a.ptex_w = img.w;
                    a.ptex_h = img.h;
                    upload_texture(a.preview_tex, renderer, img.w, img.h, img.w * 4, img.rgba);
                }
            }
            if (a.preview_tex) {
                ImGui::Image(reinterpret_cast<ImTextureID>(a.preview_tex),
                             ImVec2(a.ptex_w * static_cast<float>(s.tmp_scale),
                                    a.ptex_h * static_cast<float>(s.tmp_scale)));
            }
            break;
        }
        case Kind::Text: {
            const size_t cap = 200 * 1024;
            const size_t n = std::min(s.sel_data.size(), cap);
            ImGui::TextUnformatted("文本 (显示前 200KB)");
            ImGui::BeginChild("textview", ImVec2(0, 0), ImGuiChildFlags_Borders);
            // 分块渲染，避免单次超长字符串
            size_t pos = 0;
            std::string line;
            while (pos < n) {
                line.clear();
                const size_t start = pos;
                while (pos < n && s.sel_data[pos] != '\n' && pos - start < 512) ++pos;
                if (pos < n && s.sel_data[pos] == '\n') ++pos;
                line.assign(reinterpret_cast<const char*>(s.sel_data.data() + start), pos - start);
                ImGui::TextUnformatted(line.c_str());
            }
            ImGui::EndChild();
            break;
        }
        case Kind::Aud: {
            ImGui::Text("%s: %u Hz, %d 声道, %d bit, 压缩=%u, 时长 %.2f 秒",
                        s.aud.is_wav() ? "WAV" : "AUD", s.aud.rate(), s.aud.channels(),
                        s.aud.bits(), s.aud.compression(), s.aud.duration_seconds());
            if (ImGui::Button("解码统计")) {
                std::vector<int16_t> pcm;
                std::string err;
                if (s.aud.decode_pcm16(pcm, &err)) {
                    ImGui::Text("解码成功: %zu 个 16bit 样本", pcm.size());
                } else {
                    ImGui::Text("解码失败: %s", err.c_str());
                }
            }
            ImGui::Separator();
            // 引擎解码播放（AUD 的 Westwood ADPCM / WAV 均支持，无需 FFmpeg）
            if (!s.media.is_open()) {
                if (ImGui::Button("▶ 播放（引擎解码）")) {
                    s.media.open(s.sel_data.data(), s.sel_data.size(),
                                 s.aud.is_wav() ? ".wav" : ".aud");
                }
            } else {
                if (ImGui::Button(s.media.playing() ? "‖ 暂停" : "▶ 播放")) {
                    s.media.playing() ? s.media.pause() : s.media.play();
                }
                ImGui::SameLine();
                if (ImGui::Button("■ 关闭")) s.media.close();
                if (s.media.has_audio())
                    ImGui::Text("音频缓冲剩余 %.1f 秒", s.media.audio_buffered());
                if (s.media.duration() > 0)
                    ImGui::Text("%.1f/%.1f 秒", s.media.position(), s.media.duration());
            }
            if (!s.media.last_error().empty())
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s",
                                   s.media.last_error().c_str());
            break;
        }
        case Kind::Bik: {
            const uint32_t frames =
                s.sel_data.size() >= 12
                    ? static_cast<uint32_t>(s.sel_data[8]) |
                          (static_cast<uint32_t>(s.sel_data[9]) << 8) |
                          (static_cast<uint32_t>(s.sel_data[10]) << 16) |
                          (static_cast<uint32_t>(s.sel_data[11]) << 24)
                    : 0;
            ImGui::Text("Bink 视频 (BIK%c): %u 帧, %s", s.sel_data.size() > 3 ? s.sel_data[3]
                                                                              : '?',
                        frames, fmt_size(static_cast<uint32_t>(s.sel_data.size())).c_str());
            ImGui::Separator();
            if (!s.media.is_open()) {
                if (ImGui::Button("▶ 播放（FFmpeg 解码）")) {
                    s.media.open(s.sel_data.data(), s.sel_data.size(), ".bik");
                }
            } else {
                if (ImGui::Button(s.media.playing() ? "‖ 暂停" : "▶ 播放")) {
                    s.media.playing() ? s.media.pause() : s.media.play();
                }
                ImGui::SameLine();
                if (ImGui::Button("■ 关闭")) s.media.close();
            }
            if (!s.media.last_error().empty())
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s",
                                   s.media.last_error().c_str());
            if (s.media.has_video() && a.preview_tex && a.ptex_w > 0) {
                ImGui::Image(reinterpret_cast<ImTextureID>(a.preview_tex),
                             ImVec2(static_cast<float>(a.ptex_w), static_cast<float>(a.ptex_h)));
                if (s.media.duration() > 0)
                    ImGui::Text("%dx%d @ %.1ffps  %.1f/%.1f 秒", s.media.video_w(),
                                s.media.video_h(), s.media.video_fps(), s.media.position(),
                                s.media.duration());
            }
            break;
        }
        case Kind::Csf: {
            ImGui::Text("CSF: %zu 条字串, 语言=%u", s.csf.size(), s.csf.language());
            ImGui::BeginChild("csfview", ImVec2(0, 0), ImGuiChildFlags_Borders);
            size_t shown = 0;
            for (const auto& [k, v] : s.csf.all()) {
                if (shown++ >= 2000) {
                    ImGui::TextUnformatted("...(截断)");
                    break;
                }
                if (v.extra.empty()) {
                    ImGui::Text("%s = %s", k.c_str(), v.value.c_str());
                } else {
                    ImGui::Text("%s = %s  [音效: %s]", k.c_str(), v.value.c_str(),
                                v.extra.c_str());
                }
            }
            ImGui::EndChild();
            break;
        }
        case Kind::Hva: {
            ImGui::Text("HVA: %u 帧 × %u sections", s.hva.frame_count(),
                        s.hva.section_count());
            if (s.hva_vxl_linked && s.vxl_ok) {
                ImGui::TextUnformatted("已联动同名 VXL — 3D 动画预览：");
                draw_voxel_anim_view(a, renderer);
                break;
            }
            ImGui::TextUnformatted("（未找到同名 VXL，仅显示节列表）");
            for (uint32_t i = 0; i < s.hva.section_count() && i < 16; ++i) {
                ImGui::Text("  section %u: %s", i, s.hva.section_names()[i].c_str());
            }
            break;
        }
        case Kind::Hex:
        default: {
            ImGui::TextUnformatted("十六进制 (前 8KB)");
            ImGui::BeginChild("hexview", ImVec2(0, 0), ImGuiChildFlags_Borders);
            const size_t n = std::min<size_t>(s.sel_data.size(), 8192);
            char row[128];
            for (size_t base = 0; base < n; base += 16) {
                int off = std::snprintf(row, sizeof(row), "%06zX  ", base);
                for (size_t i = base; i < base + 16 && i < n; ++i) {
                    off += std::snprintf(row + off, sizeof(row) - off, "%02X ", s.sel_data[i]);
                }
                row[off] = 0;
                ImGui::TextUnformatted(row);
            }
            if (s.sel_data.size() > n) ImGui::TextUnformatted("...(截断)");
            ImGui::EndChild();
            break;
        }
    }
    ImGui::EndChild();
}

} // namespace

// ── UI 缩放（HiDPI + 用户放大） ────────────────────────────────────

static float g_ui_scale = 1.0f;
static SDL_Window* g_window = nullptr;
static SDL_Renderer* g_renderer = nullptr;

void rebuild_ui_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    // 中文字体走引擎统一引导（Windows 系统字体 + Linux 发行版路径 + fontconfig
    // 兜底）；字号 16×缩放，字形范围用全量中文（原简化常用集缺字）。
    const bool ok = ra2r::ui::setup_cjk_font(16.0f * scale);
    if (!ok) {
        io.Fonts->AddFontDefault(); // 无 CJK 字体：退回内置字体（中文变占位符）
    }
    io.Fonts->Build();
    // 重置样式再统一缩放（避免累积）
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
}

void apply_ui_scale(float scale) {
    g_ui_scale = scale;
    if (g_ui_scale < 0.9f) g_ui_scale = 0.9f;
    if (g_ui_scale > 3.0f) g_ui_scale = 3.0f;
    rebuild_ui_fonts(g_ui_scale);
    SDL_DisplayID disp = SDL_GetDisplayForWindow(g_window);
    SDL_Rect bounds{};
    if (disp) SDL_GetDisplayUsableBounds(disp, &bounds);
    int w = static_cast<int>(1360 * g_ui_scale);
    int h = static_cast<int>(800 * g_ui_scale);
    if (bounds.w > 0) {
        if (w > bounds.w - 40) w = bounds.w - 40;
        if (h > bounds.h - 60) h = bounds.h - 60;
    }
    SDL_SetWindowSize(g_window, w, h);
}

static int run(int argc, char** argv) {
    const char* mix_path = nullptr;
    const char* shot_path = nullptr;
    int shot_frames = 40;
    bool test_combo = false;
    bool test_maps = false; // 自动扫描地图并预览首张（无头验证）
    bool test_export = false; // 选中首条目后台导出到指定路径（无头验证）
    const char* export_path = nullptr;
    bool test_hva = false;  // 选中首个可联动 VXL 的 HVA 并播放（无头验证）
    bool test_aud = false;  // 找首个音频条目引擎解码播放（无头验证）
    bool test_video = false; // 找首个 BIK 视频条目 FFmpeg 播放（无头验证）
    bool test_dialog = false;
    bool test_tabs = false;
    const char* reopen_path = nullptr;
    uint32_t test_entry = 0;
    bool has_test_entry = false;
    bool test_nested = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot_path = argv[++i];
        else if (std::strcmp(argv[i], "--shot-frame") == 0 && i + 1 < argc)
            shot_frames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--test-combo") == 0) test_combo = true;
        else if (std::strcmp(argv[i], "--test-maps") == 0) test_maps = true;
        else if (std::strcmp(argv[i], "--test-export") == 0 && i + 1 < argc) {
            test_export = true;
            export_path = argv[++i];
        } else if (std::strcmp(argv[i], "--test-hva") == 0) test_hva = true;
        else if (std::strcmp(argv[i], "--test-aud") == 0) test_aud = true;
        else if (std::strcmp(argv[i], "--test-video") == 0) test_video = true;
        else if (std::strcmp(argv[i], "--test-dialog") == 0) test_dialog = true;
        else if (std::strcmp(argv[i], "--test-tabs") == 0) test_tabs = true;
        else if (std::strcmp(argv[i], "--test-nested") == 0) test_nested = true;
        else if (std::strcmp(argv[i], "--test-reopen") == 0 && i + 1 < argc)
            reopen_path = argv[++i];
        else if (std::strcmp(argv[i], "--test-entry") == 0 && i + 1 < argc) {
            test_entry = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
            has_test_entry = true;
        } else if (!mix_path) mix_path = argv[i];
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#endif
    SDL_Window* window = SDL_CreateWindow("RA2R mixbrowser", 1360, 800,
                                          SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!window || !renderer) {
        std::fprintf(stderr, "window failed: %s\n", SDL_GetError());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    g_window = window;
    g_renderer = renderer;

    // DPI 缩放：取系统内容缩放，低于 1.25 时按 1.25（用户要求放大）
    {
        float content_scale = 1.0f;
        const SDL_DisplayID disp = SDL_GetDisplayForWindow(window);
        if (disp) content_scale = SDL_GetDisplayContentScale(disp);
        apply_ui_scale(std::max(1.25f, content_scale));
    }

    App app;
    app.window = window;
    // XCC 官方名库（可选）：大幅提升条目名覆盖率（获取见 tools/fetch_references.ps1）
    {
        MixFile tmp;
        std::string from;
        if (ra2r::assets::try_load_xcc_database(tmp, &from)) {
            merge_global_names(app, tmp);
            std::fprintf(stderr, "[names] XCC 名库 %s: %zu 名\n", from.c_str(),
                         tmp.names().size());
        }
    }
    if (mix_path) {
        open_path_at(app, mix_path); // 文件 → 单标签；目录 → 批量加载
    }
    if (has_session(app)) {
        std::snprintf(cur(app).open_buf, sizeof(cur(app).open_buf), "%s", mix_path ? mix_path : "");
    }

    int frame = 0;
    bool running = true;
    uint64_t frame_t0 = SDL_GetTicks();
    const uint64_t test_t0 = frame_t0;
    while (running && !(shot_path && frame >= shot_frames)) {
        // ── 帧率上限（~120fps）+ 主循环让出 ──────────────────────────────
        // 实测：主循环无让出、高频 D3D11 呈现时会饿死 SDL3/WASAPI 音频设备
        // 线程（音频播放速度骤降为 1/60~1/4）；每帧补足 8ms 并 SDL_Delay
        // 让出 CPU 后恢复正常。此上限同时避免 GUI 空转烧 CPU。
        // ── 合成真实鼠标点击（在事件泵之前入队，走 ImGui 完整交互路径）──
        // ── 新功能无头验证：地图扫描预览 / HVA 动画联动 ──
        if (has_session(app)) {
            // 音视频播放推进；产出新视频帧时上传预览纹理
            mixbrowser::MediaPlayer& mp = cur(app).media;
            if (mp.tick() && mp.has_video() && !mp.video_rgba().empty()) {
                upload_texture(app.preview_tex, renderer, mp.video_w(), mp.video_h(),
                               mp.video_stride(), mp.video_rgba());
                app.ptex_w = mp.video_w();
                app.ptex_h = mp.video_h();
            }
        }
        poll_bg_jobs(app);  // 后台任务收尾 + 接续调度

        if ((test_aud || test_video) && has_session(app) && frame == 5) {
            // 在当前层找第一个音频（AUD/WAV）或视频（BIK）条目，装载并开始播放
            Session& s = cur(app);
            const char* want = test_video ? ".BIK" : ".AUD";
            const char* want2 = test_video ? ".BIK" : ".WAV";
            std::fprintf(stderr, "[test-media] entries=%zu names:", s.stack.back().mix.entries().size());
            for (size_t i = 0; i < s.stack.back().mix.entries().size() && i < 12; ++i) {
                const std::string* nm =
                    s.stack.back().mix.name_of(s.stack.back().mix.entries()[i].id);
                std::fprintf(stderr, " %s", nm ? nm->c_str() : "?");
            }
            std::fprintf(stderr, "\n");
            for (size_t i = 0; i < s.stack.back().mix.entries().size(); ++i) {
                const std::string* nm =
                    s.stack.back().mix.name_of(s.stack.back().mix.entries()[i].id);
                if (nm) {
                    std::string up;
                    for (char ch : *nm) up += static_cast<char>(std::toupper(ch));
                    if (up.size() < 5 ||
                        (up.compare(up.size() - 4, 4, want) != 0 &&
                         up.compare(up.size() - 4, 4, want2) != 0))
                        continue;
                }
                select_entry(app, static_cast<int>(i));
                if (s.sel_kind != Kind::Aud && s.sel_kind != Kind::Bik) continue;
                const std::string ext = s.sel_kind == Kind::Bik
                                            ? ".bik"
                                            : (s.aud.is_wav() ? ".wav" : ".aud");
                const bool ok =
                    s.media.open(s.sel_data.data(), s.sel_data.size(), ext);
                std::fprintf(stderr, "[test-media] %s kind=%d open=%d",
                             nm ? nm->c_str() : "?", static_cast<int>(s.sel_kind), ok ? 1 : 0);
                if (ok) {
                    s.media.play();
                    std::fprintf(stderr, " v=%dx%d@%.1f a=%s dur=%.1fs", s.media.video_w(),
                                 s.media.video_h(), s.media.video_fps(),
                                 s.media.has_audio() ? "yes" : "no", s.media.duration());
                } else {
                    std::fprintf(stderr, " err=%s", s.media.last_error().c_str());
                }
                std::fprintf(stderr, "\n");
                break;
            }
        }
        if ((test_aud || test_video) && has_session(app) &&
            (frame == 40 || frame == 120 || frame == 200 || frame == 300)) {
            Session& s = cur(app);
            std::fprintf(stderr, "[test-media] frame%d t=%llu playing=%d tex=%dx%d buf=%.2fs\n",
                         frame, static_cast<unsigned long long>(SDL_GetTicks() - test_t0),
                         s.media.playing() ? 1 : 0,
                         s.media.has_video() ? s.media.video_w() : 0,
                         s.media.has_video() ? s.media.video_h() : 0,
                         s.media.audio_buffered());
        }

        if (test_maps && has_session(app) && frame == 5) {
            // 后台扫描启动（worker 快照根 MIX 递归）；结果由 poll_bg_jobs 收
            Session& s = cur(app);
            s.found_maps.clear();
            s.map_scan_done = false;
            start_map_scan_job(app);
            std::fprintf(stderr, "[test-maps] 扫描任务已启动\n");
        }
        if (test_maps && has_session(app) && frame == 40) {
            Session& s = cur(app);
            std::fprintf(stderr, "[test-maps] frame40 done=%d found=%zu\n",
                         s.map_scan_done ? 1 : 0, s.found_maps.size());
            if (s.map_scan_done && !s.found_maps.empty() && !s.map_ok) {
                load_map_ref(app, s.found_maps.front());
                std::fprintf(stderr, "[test-maps] preview %s map_ok=%d %dx%d\n",
                             s.found_maps.front().path.c_str(), s.map_ok ? 1 : 0,
                             s.mapf.cell_w(), s.mapf.cell_h());
            }
        }
        if (test_maps && has_session(app) &&
            (frame == 120 || frame == 300 || frame == 500)) {
            Session& s = cur(app);
            std::fprintf(stderr, "[test-maps] frame%d thumb_ready=%d %dx%d job=%d\n", frame,
                         s.map_thumb_ready ? 1 : 0, s.map_thumb_ready ? s.map_thumb.w : 0,
                         s.map_thumb_ready ? s.map_thumb.h : 0, app.map_thumb_job ? 1 : 0);
        }
        if (test_export && has_session(app) && frame == 5) {
            // 无头导出验证：选中首条目 → 直接启动后台导出任务（绕过保存对话框）
            Session& s = cur(app);
            if (!s.order.empty()) {
                select_entry(app, s.order[0]);
                const auto& es = s.stack.back().mix.entries();
                if (s.selected >= 0 && s.selected < static_cast<int>(es.size())) {
                    std::vector<uint8_t> d;
                    if (s.stack.back().mix.read_entry(es[s.selected], d)) {
                        start_export_job(app, std::move(d), export_path);
                        std::fprintf(stderr, "[test-export] 导出任务已启动 → %s\n",
                                     export_path);
                    }
                }
            }
        }
        if (test_export && has_session(app) && frame == 40) {
            std::fprintf(stderr, "[test-export] done=%d msg=%s\n",
                         app.export_job ? 0 : 1, app.status_msg.c_str());
        }
        if (test_hva && has_session(app) && frame == 5) {
            // 递归找第一个带同名 VXL 的 .HVA，直接读字节装载预览态（不经 UI 导航）
            Session& s = cur(app);
            struct Found {
                std::vector<uint8_t> hva, vxl;
                std::string name;
            };
            std::function<bool(MixFile&, int, Found&)> walk = [&](MixFile& mix, int depth,
                                                                  Found& out) -> bool {
                if (depth > 4) return false;
                for (const auto& e : mix.entries()) {
                    const std::string* nm = mix.name_of(e.id);
                    if (!nm || nm->size() < 5 || nm->compare(nm->size() - 4, 4, ".HVA") != 0)
                        continue;
                    const MixEntry* ve = mix.find_by_name(nm->substr(0, nm->rfind('.')) + ".VXL");
                    if (!ve) continue;
                    if (!mix.read_entry(e, out.hva) || !mix.read_entry(*ve, out.vxl)) continue;
                    out.name = *nm;
                    return true;
                }
                for (const auto& e : mix.entries()) {
                    if (e.size == 0 || e.size > (64u << 20)) continue;
                    std::vector<uint8_t> d;
                    if (!mix.read_entry(e, d)) continue;
                    if (!MixFile::looks_like_mix(d.data(), d.size())) continue;
                    MixFile sub;
                    std::string err;
                    if (!sub.open(d.data(), d.size(), &err) || !sub.structure_valid())
                        continue;
                    ra2r::assets::enrich_names(sub);
                    if (walk(sub, depth + 1, out)) return true;
                }
                return false;
            };
            Found f;
            if (walk(s.stack.back().mix, 0, f)) {
                std::string err;
                s.selected = -1;
                s.sel_data = std::move(f.hva);
                s.pal_choice = -1;
                if (app.preview_tex) {
                    SDL_DestroyTexture(app.preview_tex);
                    app.preview_tex = nullptr;
                }
                app.ptex_w = app.ptex_h = 0;
                s.last_upload_ver = ~0ull;
                s.hva_ok = s.hva.open(s.sel_data.data(), s.sel_data.size(), &err);
                s.vxl_ok = s.vxl.open(f.vxl.data(), f.vxl.size(), &err);
                s.hva_vxl_linked = s.hva_ok && s.vxl_ok;
                s.sel_kind = s.hva_ok ? Kind::Hva : Kind::Hex;
                s.vxl_limb = 0;
                s.vxl_hva_frame = 0;
                if (s.hva_vxl_linked) s.hva_play = true;
                std::fprintf(stderr, "[test-hva] %s linked=%d frames=%u\n", f.name.c_str(),
                             s.hva_vxl_linked ? 1 : 0, s.hva.frame_count());
            } else {
                std::fprintf(stderr, "[test-hva] no HVA+VXL pair found\n");
            }
        }
        if (test_combo && has_session(app) && cur(app).sel_kind == Kind::Shp) {
            if (!app.click_phase1 && app.combo_rect_valid && frame >= 12) {
                app.click_phase1 = true;
                const float cx = (app.combo_min.x + app.combo_max.x) * 0.5f;
                const float cy = (app.combo_min.y + app.combo_max.y) * 0.5f;
                std::fprintf(stderr, "[test-combo] 注入点击下拉框 (%.0f, %.0f)\n", cx, cy);
                inject_click_at(frame + 1, ImVec2(cx, cy));
            } else if (app.click_phase1 && !app.click_phase2 && frame >= 18) {
                app.click_phase2 = true;
                // 弹出项在组合框正下方
                const float cx = app.combo_min.x + 30;
                const float cy = app.combo_max.y + 14;
                std::fprintf(stderr, "[test-combo] 注入点击弹出项 (%.0f, %.0f)\n", cx, cy);
                inject_click_at(frame + 1, ImVec2(cx, cy));
            }
        }
        // ── 重开 MIX 回归：第 8 帧在 draw_list_pane 帧内触发 open_path_at ──
        if (reopen_path && frame == 6) {
            g_reopen_hook = true;
            g_reopen_hook_path = reopen_path;
            g_reopen_hook_frame = 8;
            if (has_session(app)) {
                std::snprintf(cur(app).open_buf, sizeof(cur(app).open_buf), "%s", reopen_path);
            }
            std::fprintf(stderr, "[test-reopen] open_buf 设为 %s\n", reopen_path);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) running = false;
        }
        // Ctrl+滚轮：UI 缩放微调
        {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl && io.MouseWheel != 0.0f) {
                apply_ui_scale(g_ui_scale + io.MouseWheel * 0.15f);
                io.MouseWheel = 0.0f;
            }
        }
        if (test_tabs && (frame == 26 || frame == 27)) {
            ImGuiIO& dbo = ImGui::GetIO();
            std::fprintf(stderr, "[dbg] f%d MouseDown=%d Clicked=%d\n", frame, dbo.MouseDown[0],
                         ImGui::IsMouseClicked(0));
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        inject_process(frame); // 后端同步真实鼠标之后注入合成点击
        ImGui::NewFrame();

        // 全屏主窗口：此前 UI 直接画在隐式根窗口上，隐式窗口按内容自动收缩到
        // ~430px，预览面板被挤压不可见（见 docs/DEBUGGING.md 3.4）
        {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::Begin("main", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoSavedSettings);
        }

        if (has_session(app)) {
            // ── 自绘标签行（普通按钮，状态完全由应用掌控；避免 ImGui 标签栏
            //    的选中滞后与 IsItemClicked 不可靠问题）──
            ImGui::BeginChild("tabrow", ImVec2(0, ImGui::GetFrameHeight() + 10),
                              ImGuiChildFlags_Borders);
            app.tab_btn_min.assign(app.sessions.size(), ImVec2{});
            app.tab_btn_max.assign(app.sessions.size(), ImVec2{});
            for (int i = 0; i < static_cast<int>(app.sessions.size()); ++i) {
                std::string name = app.sessions[i].stack.empty()
                                       ? "?"
                                       : app.sessions[i].stack[0].label;
                if (name.size() > 28) name = name.substr(0, 28) + "...";
                char lbl[256];
                std::snprintf(lbl, sizeof(lbl), "%s##tab%d", name.c_str(), i);
                const bool is_active = (i == app.active);
                if (is_active) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.44f, 0.74f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(0.30f, 0.50f, 0.80f, 1.0f));
                }
                if (ImGui::SmallButton(lbl)) {
                    std::fprintf(stderr, "[tabs] 按钮 %d 被点击\n", i);
                    activate_session(app, i);
                }
                if (is_active) ImGui::PopStyleColor(2);
                app.tab_btn_min[i] = ImGui::GetItemRectMin();
                app.tab_btn_max[i] = ImGui::GetItemRectMax();
                ImGui::SameLine();
                char xl[32];
                std::snprintf(xl, sizeof(xl), "×##tabx%d", i);
                if (ImGui::SmallButton(xl)) close_session(app, i);
                if (i + 1 < static_cast<int>(app.sessions.size())) ImGui::SameLine();
            }
            ImGui::EndChild();
            if (has_session(app)) {
                const float status_h = ImGui::GetFrameHeight() + 10.0f;
                g_status_h = status_h;  // 列表/预览面板底部让位给状态栏
                draw_list_pane(app, frame);
                ImGui::SameLine();
                draw_preview_pane(app, renderer);
                g_status_h = 0.0f;
                draw_status_bar(app);
            } else {
                ImGui::TextUnformatted("所有标签已关闭");
                draw_status_bar(app);
            }
        } else {
            ImGui::Begin("打开");
            ImGui::TextUnformatted("打开一个 mix 文件或 YR 游戏目录（自动加载目录下全部 .mix）");
            if (ImGui::Button("打开文件...")) {
                static const SDL_DialogFileFilter kFilters[] = {{"MIX 文件", "mix"}};
                SDL_ShowOpenFileDialog(open_dialog_cb, &app, app.window, kFilters, 1, nullptr,
                                       false);
            }
            ImGui::SameLine();
            if (ImGui::Button("打开目录...")) {
                SDL_ShowOpenFolderDialog(open_folder_dialog_cb, &app, app.window, nullptr, false);
            }
            ImGui::End();
        }
        ImGui::End(); // main 窗口

        // ── 自测模式（必须在帧作用域内，即 ImGui::Render() 之前）──
        if (test_combo && has_session(app)) {
            Session& ts = cur(app);
            if (frame == 5) {
                std::fprintf(stderr, "[test-combo] frame5 block entered (stack=%zu order=%zu)\n",
                             ts.stack.size(), ts.order.size());
                Level& lv = ts.stack.back();
                if (has_test_entry) {
                    bool found = false;
                    for (int oi : ts.order) {
                        if (lv.mix.entries()[oi].id == test_entry) {
                            select_entry(app, oi);
                            std::fprintf(stderr, "[test-combo] selected entry %d id=%08X\n", oi,
                                         test_entry);
                            found = true;
                            break;
                        }
                    }
                    // 本级没有 → 在第一个包含该 id 的嵌套 MIX 里找
                    if (!found && test_nested) {
                        for (int oi : ts.order) {
                            const MixEntry& e = lv.mix.entries()[oi];
                            std::vector<uint8_t> d;
                            if (!lv.mix.read_entry(e, d)) continue;
                            MixFile probe;
                            std::string err;
                            if (!probe.open(d.data(), d.size(), &err) ||
                                !probe.structure_valid()) {
                                continue;
                            }
                            if (!probe.find(test_entry)) continue;
                            enter_mix(app, oi);
                            std::fprintf(stderr,
                                         "[test-nested] entered mix entry %d id=%08X for %08X\n",
                                         oi, e.id, test_entry);
                            found = true;
                            break;
                        }
                    }
                    if (found && ts.stack.size() == 2) {
                        for (int oi : ts.order) {
                            if (ts.stack.back().mix.entries()[oi].id == test_entry) {
                                select_entry(app, oi);
                                std::fprintf(stderr,
                                             "[test-nested] selected entry %d id=%08X\n", oi,
                                             test_entry);
                                break;
                            }
                        }
                    }
                } else {
                    bool found = false;
                    for (int oi : ts.order) {
                        const MixEntry& e = lv.mix.entries()[oi];
                        std::vector<uint8_t> d;
                        if (!lv.mix.read_entry(e, d)) continue;
                        if (detect_kind(d) == Kind::Shp) {
                            select_entry(app, oi);
                            std::fprintf(stderr,
                                         "[test-combo] selected SHP entry %d id=%08X\n", oi,
                                         e.id);
                            found = true;
                            break;
                        }
                    }
                    // --test-nested：本级无 SHP → 进入第一个嵌套 MIX，再选其中第一个 SHP
                    if (!found && test_nested) {
                        if (ts.stack.size() == 1) {
                            for (int oi : ts.order) {
                                const MixEntry& e = lv.mix.entries()[oi];
                                std::vector<uint8_t> d;
                                if (!lv.mix.read_entry(e, d)) continue;
                                if (!MixFile::looks_like_mix(d.data(), d.size())) continue;
                                MixFile probe;
                                std::string err;
                                if (!probe.open(d.data(), d.size(), &err) ||
                                    !probe.structure_valid()) {
                                    continue;
                                }
                                enter_mix(app, oi);
                                std::fprintf(stderr,
                                             "[test-nested] entered mix entry %d id=%08X\n", oi,
                                             e.id);
                                break;
                            }
                        }
                        if (ts.stack.size() == 2) {
                            Level& lv2 = ts.stack.back();
                            for (int oi : ts.order) {
                                if (oi >= static_cast<int>(lv2.mix.entries().size())) continue;
                                const MixEntry& e = lv2.mix.entries()[oi];
                                std::vector<uint8_t> d;
                                if (!lv2.mix.read_entry(e, d)) continue;
                                if (detect_kind(d) == Kind::Shp) {
                                    select_entry(app, oi);
                                    std::fprintf(stderr,
                                                 "[test-nested] selected SHP %d id=%08X\n", oi,
                                                 e.id);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if (frame == 8 && ts.sel_kind == Kind::Shp) {
                ImGui::Begin("##testctx");
                ImGui::OpenPopup(ImGui::GetID("调色板"));
                ImGui::End();
                std::fprintf(stderr, "[test-combo] popup open requested\n");
            }
            if (frame >= 10 && frame % 20 == 0 && ts.sel_kind == Kind::Shp &&
                !ts.pal_names.empty()) {
                const int total = static_cast<int>(ts.pal_names.size()) + 1;
                ts.pal_choice = ((frame / 20) % total) - 1;
                std::fprintf(stderr, "[test-combo] frame %d pal_choice=%d (of %d)\n", frame,
                             ts.pal_choice, total);
            }
            if (frame == 30 && ts.sel_kind == Kind::Shp) {
                ts.shp_play = true; // 覆盖每帧纹理上传的高压路径
            }
            // VXL 3D 旋转自动化（帧 20–100 持续转 yaw 至 229°；帧 40+ 切 HVA 帧 1）
            if (ts.sel_kind == Kind::Vxl && frame >= 20 && frame < 100) {
                ts.vxl_yaw = static_cast<float>(frame - 20) * 0.05f;
                if (ts.hva_ok && frame >= 40 && ts.hva.frame_count() > 1) ts.vxl_hva_frame = 1;
            }
            if (frame % 10 == 0) {
                std::fprintf(stderr, "[test-combo] frame %d alive sel=%d kind=%d\n", frame,
                             ts.selected, static_cast<int>(ts.sel_kind));
            }
        }

        // ── 对话框诊断（帧 10 触发一次，观察回调/错误） ──
        if (test_dialog && frame == 10) {
            static const SDL_DialogFileFilter kFilters[] = {{"MIX 文件", "mix"}};
            SDL_ClearError();
            SDL_ShowOpenFileDialog(open_dialog_cb, &app, app.window, kFilters, 1, nullptr, false);
            std::fprintf(stderr, "[test-dialog] ShowOpenFileDialog 调用完成, SDL_GetError=%s\n",
                         SDL_GetError() ? SDL_GetError() : "(空)");
        }
        // ── 标签轮换自测：每 20 帧激活下一个标签；第 25 帧注入真实点击标签 1 ──
        if (test_tabs && has_session(app) && app.sessions.size() > 1) {
            if (frame == 25 && app.tab_btn_min.size() > 1) {
                const float cx = (app.tab_btn_min[1].x + app.tab_btn_max[1].x) * 0.5f;
                const float cy = (app.tab_btn_min[1].y + app.tab_btn_max[1].y) * 0.5f;
                std::fprintf(stderr, "[test-tabs] 注入点击标签 1 (%.0f, %.0f)\n", cx, cy);
                inject_click_at(26, ImVec2(cx, cy));
            }
            if (frame == 35) {
                std::fprintf(stderr, "[test-tabs] 点击后 active=%d\n", app.active);
            }
            if (frame >= 40 && frame % 20 == 0) {
                const int next = (app.active + 1) % static_cast<int>(app.sessions.size());
                activate_session(app, next);
                std::fprintf(stderr, "[test-tabs] frame %d 激活标签 %d/%zu\n", frame, next,
                             app.sessions.size());
            }
        }
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 36, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        // 帧率上限：补足到 16ms/帧（~60fps）。实测：主循环无让出、短帧高频
        // D3D11 呈现时会饿死 SDL3/WASAPI 音频设备线程（播放骤降为 1/60 速）；
        // 每帧让出后恢复正常。此上限同时避免 GUI 空转烧 CPU。
        {
            const uint64_t now_ticks = SDL_GetTicks();
            const uint64_t elapsed = now_ticks - frame_t0;
            frame_t0 = now_ticks;
            if (elapsed < 16) SDL_Delay(static_cast<Uint32>(16 - elapsed));
        }
        ++frame;

        // 截图模式：第 5 帧自动选中首条目以覆盖预览路径（test_combo 自选，勿覆盖）
        if (shot_path && frame == 5 && has_session(app) && !test_combo) {
            Session& ts = cur(app);
            if (!ts.order.empty()) select_entry(app, ts.order[0]);
        }
        if (shot_path && frame >= shot_frames) {
            SDL_Surface* surf = SDL_RenderReadPixels(renderer, nullptr);
            if (surf) {
                SDL_Surface* conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
                SDL_DestroySurface(surf);
                if (conv) {
                    std::vector<uint8_t> buf(static_cast<size_t>(conv->w) * conv->h * 4);
                    std::memcpy(buf.data(), conv->pixels, buf.size());
                    write_bmp(shot_path, conv->w, conv->h, buf);
                    std::printf("shot saved -> %s (%dx%d)\n", shot_path, conv->w, conv->h);
                    SDL_DestroySurface(conv);
                }
            }
        }
    }

    // 停掉媒体播放（释放音频设备与临时文件；其余会话随析构释放）
    if (has_session(app)) cur(app).media.close();
    // 等待后台任务收尾（快照作业自行完成，无取消协议，join 等待结束）
    for (auto* j : {&app.pool_job, &app.map_scan_job, &app.map_thumb_job, &app.export_job}) {
        if (*j && (*j)->thread.joinable()) (*j)->thread.join();
        j->reset();
    }
    if (app.preview_tex) SDL_DestroyTexture(app.preview_tex);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
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
