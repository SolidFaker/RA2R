// RA2R — 地图解析实现
#include "ra2r/assets/map_file.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

#include "ra2r/assets/lcw.h"
#include "ra2r/assets/lzo1x.h"
#include "ra2r/assets/mix_file.h"
#include "ra2r/core/endian.h"

namespace ra2r::assets {

namespace {

constexpr int kCellBytes = 11;

bool contains_bytes(const std::vector<uint8_t>& d, const char* needle) {
    const size_t n = std::strlen(needle);
    if (d.size() < n) return false;
    for (size_t i = 0; i + n <= d.size(); ++i) {
        if (std::memcmp(d.data() + i, needle, n) == 0) return true;
    }
    return false;
}

// 标准 Base64 解码（跳过空白；'=' 填充）
bool base64_decode(const std::string& in, std::vector<uint8_t>& out) {
    static const char* kTab =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int table[256];
    for (int i = 0; i < 256; ++i) table[i] = -1;
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(kTab[i])] = i;
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c == '=') break;
        const int v = table[static_cast<unsigned char>(c)];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
            acc &= (1u << bits) - 1;
        }
    }
    return true;
}

} // namespace

bool MapFile::open(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open map: " + path.string();
        return false;
    }
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return open(reinterpret_cast<const uint8_t*>(data.data()), data.size(), error);
}

bool MapFile::open(const uint8_t* data, size_t size, std::string* error) {
    cells_.clear();
    present_ = 0;
    // .yro/.mmx：加密 MIX 壳（实测 CrctBrd.yro：旗标 0x00030000，body_start=116 即
    // "116 字节头"），内含 [MultiMaps] 索引条目 + 地图 INI 条目（含 IsoMapPack5）。
    // structure_valid 的精确等式判别可避免把纯 INI（[Basic] 开头）误判为旧式 MIX。
    if (MixFile::looks_like_mix(data, size)) {
        MixFile mix;
        std::string err;
        if (mix.open(data, size, &err) && mix.structure_valid()) {
            const MixEntry* best = nullptr;
            std::vector<uint8_t> best_data;
            for (const auto& e : mix.entries()) {
                if (e.size < 64 || e.size > (64u << 20)) continue;
                std::vector<uint8_t> d;
                if (!mix.read_entry(e, d)) continue;
                // 优先级：含 IsoMapPack5 > 含 [Map] 节 > 最大文本条目
                if (contains_bytes(d, "IsoMapPack5")) {
                    best = &e;
                    best_data = std::move(d);
                    break;
                }
                if (!best && contains_bytes(d, "[Map]")) {
                    best = &e;
                    best_data = std::move(d);
                } else if (!best && d.size() > best_data.size() &&
                           contains_bytes(d, "[")) {
                    best = &e;
                    best_data = std::move(d);
                }
            }
            if (best && !best_data.empty()) {
                if (!ini_.parse(best_data.data(), best_data.size(), error)) return false;
                return parse(error);
            }
        }
    }
    if (!ini_.parse(data, size, error)) return false;
    return parse(error);
}

bool MapFile::parse(std::string* error) {
    // [Map]
    if (!ini_.has_section("Map")) {
        if (error) *error = "no [Map] section";
        return false;
    }
    if (std::sscanf(ini_.get("Map", "Size", "0,0,0,0").c_str(), "%d,%d,%d,%d", &left_, &top_,
                    &right_, &bottom_) != 4) {
        if (error) *error = "bad [Map] Size";
        return false;
    }
    theater_ = ini_.get("Map", "Theater", "TEMPERAT");
    // 可玩区域网格 = (right-left) × (bottom-top)；内部统一用 0 基相对坐标
    right_ = right_ - left_;
    bottom_ = bottom_ - top_;
    left_ = 0;
    top_ = 0;
    if (cell_w() <= 0 || cell_h() <= 0 || cell_w() > 4096 || cell_h() > 4096) {
        if (error) *error = "map size out of range";
        return false;
    }
    cells_.resize(static_cast<size_t>(cell_w()) * cell_h());

    // [IsoMapPack5]：把整节所有值拼接为一条标准 Base64 流一次解码
    // （不能逐行解码——填充跨行）；随后为 [u16 压缩长][u16 解压长] 块序列，
    // 每个块用 LZO1X 解压，最后以 [0][0] 头结束。解压后每瓦片 11 字节。
    std::string b64;
    for (const auto& [key, value] : ini_.section("IsoMapPack5")) {
        (void)key;
        b64 += value;
    }
    std::vector<uint8_t> pack;
    if (!base64_decode(b64, pack)) {
        if (error) *error = "base64 decode failed in IsoMapPack5";
        return false;
    }
    std::vector<uint8_t> decomp;
    std::vector<uint8_t> all;
    size_t pos = 0;
    // 可玩区域网格宽（Size 宽度；行数 = 反对角线数，见下 grid_h）
    const int W = cell_w();
    while (pos + 4 <= pack.size()) {
        const uint16_t comp = core::read_u16_le(pack.data() + pos);
        const uint16_t uncomp = core::read_u16_le(pack.data() + pos + 2);
        pos += 4;
        if (comp == 0 || uncomp == 0) break; // [0][0] 结束头
        if (pos + comp > pack.size() || uncomp > 8u * 1024 * 1024) {
            if (error) *error = "map chunk overruns pack";
            return false;
        }
        decomp.resize(uncomp);
        int n;
        if (comp == uncomp) {
            std::copy(pack.begin() + pos, pack.begin() + pos + comp, decomp.begin());
            n = comp;
        } else {
            n = lzo1x_decompress(pack.data() + pos, comp, decomp.data(), uncomp);
            if (n < 0) {
                if (error) *error = "map chunk lzo1x decompress failed";
                return false;
            }
        }
        pos += comp;
        // 记录流跨块连续：先拼接再统一解析
        all.insert(all.end(), decomp.begin(), decomp.begin() + n);
    }
    // 每 11 字节一条 lattice 记录：rx,ry,tilenum,zero1,subtile,z,zero2
    // 映射（OpenRA 语义）：dx = rx-ry+W-1, dy = rx+ry-W-1, mx = dx/2, my = dy
    // IsoMapPack5 点阵 → 砖墙网格（实测 egypt：19,434 条记录全为真实瓦片）：
    //   行 = 反对角线 (rx+ry−min_s)·15 —— 跨度 ≈ 2H（Size 的 H 是"逻辑格"数，
    //     每逻辑行 = 2 条反对角线；此前按 H 裁剪丢掉下半图 / 按对折丢掉半数行）；
    //   列 = 沿反对角线 (rx−ry−min_d)/2，奇偶行的 30px 相位由 px=(rx−ry−min_d)·30
    //     天然给出（行内步进 60px，行间错位 30px）。
    const size_t count = all.size() / kCellBytes;
    int min_d = INT_MAX, min_s = INT_MAX, max_s = INT_MIN;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* c = all.data() + i * kCellBytes;
        const int rx = core::read_u16_le(c);
        const int ry = core::read_u16_le(c + 2);
        min_d = std::min(min_d, rx - ry);
        min_s = std::min(min_s, rx + ry);
        max_s = std::max(max_s, rx + ry);
    }
    if (min_d == INT_MAX) return true; // 空地图
    min_d_ = min_d;
    min_s_ = min_s;
    const int grid_h = max_s - min_s + 1; // 反对角线行数（≈2H）
    bottom_ = grid_h; // cell_h() 暴露点阵行数（渲染/小地图/迷雾都以点阵行计）
    cells_.assign(static_cast<size_t>(W) * grid_h, MapCell{});
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* c = all.data() + i * kCellBytes;
        const int rx = core::read_u16_le(c);
        const int ry = core::read_u16_le(c + 2);
        const int row = rx + ry - min_s;
        const int col = (rx - ry - min_d) / 2;
        if (row < 0 || row >= grid_h || col < 0 || col >= W) continue;
        MapCell& cell = cells_[static_cast<size_t>(row) * W + col];
        cell.present = true;
        cell.x = static_cast<uint16_t>(rx);
        cell.y = static_cast<uint16_t>(ry);
        cell.tile_id = core::read_u16_le(c + 4);
        // FA2/原版把"默认空地（Clear01）"存为 0xFFFF；游戏按 Clear01 渲染，
        // 这里统一归一为瓦片 0（实测 test10.yrm 全图 0xFFFF = 草地）。
        if (cell.tile_id == 0xFFFF) cell.tile_id = 0;
        cell.extra = core::read_u16_le(c + 6);
        cell.subtile = c[8];
        cell.height = c[9];
        cell.extra2 = c[10];
    }
    present_ = 0;
    for (const auto& c : cells_) {
        if (c.present) ++present_;
    }
    // ── [OverlayPack]/[OverlayDataPack]：整段 Base64 → LCW 分块流 ──
    // 每块 = [u16 压缩长][u16 解压长] + LCW 数据，最后以 [0][0] 头结束
    // （与 IsoMapPack5 的 LZO 同款分块框架，OpenRA UnpackLCW 语义）。
    // 实测 atwar.yrm 共 32 块、解压 262144 字节：此前按单流解码只拿到
    // 第一块 8198 字节，格栅 ry 偏大的覆盖物（桥/矿石/宝石）全部丢失。
    // 解压后为按格栅线性数组：每格 1 字节（type；0xFF=无），索引 = rx + 512*ry。
    overlay_.clear();
    overlay_data_.clear();
    auto decode_lcw_section = [&](const char* section, std::vector<uint8_t>& out) -> bool {
        std::string b64;
        for (const auto& [key, value] : ini_.section(section)) {
            (void)key;
            b64 += value;
        }
        if (b64.empty()) return true; // 无此节（如无覆盖物的地图）不视为错误
        std::vector<uint8_t> raw;
        if (!base64_decode(b64, raw)) {
            if (error) *error = std::string("base64 decode failed in [") + section + "]";
            return false;
        }
        std::vector<uint8_t> buf;
        buf.reserve(1u << 18);
        std::vector<uint8_t> scratch(1u << 18);
        size_t pos = 0;
        while (pos + 4 <= raw.size()) {
            const uint16_t comp = core::read_u16_le(raw.data() + pos);
            const uint16_t uncomp = core::read_u16_le(raw.data() + pos + 2);
            pos += 4;
            if (comp == 0 || uncomp == 0) break; // [0][0] 结束头
            if (pos + comp > raw.size() || uncomp > (1u << 18)) {
                if (error) *error = std::string("map chunk overruns pack in [") + section + "]";
                return false;
            }
            const int n = lcw_decompress(raw.data() + pos, comp, scratch.data(), uncomp);
            if (n < 0) {
                if (error) *error = std::string("lcw decompress failed in [") + section + "]";
                return false;
            }
            buf.insert(buf.end(), scratch.begin(), scratch.begin() + n);
            pos += comp;
        }
        out = std::move(buf);
        return true;
    };
    if (!decode_lcw_section("OverlayPack", overlay_)) return false;
    if (!decode_lcw_section("OverlayDataPack", overlay_data_)) return false;
    overlay_present_ = 0;
    for (uint8_t v : overlay_) {
        if (v != 0xFF) ++overlay_present_;
    }
    // ── [Units]/[Structures]/[Infantry] ──
    // 单位/建筑: index=House,ID,health,cell,dir,...（cell 为格栅序号 rx + ry·512）
    // 步兵: index=House,ID,health,cell,dir,subcell,...（实测字段序）
    units_.clear();
    buildings_.clear();
    infantry_.clear();
    auto split_csv = [](const std::string& value) {
        std::vector<std::string> f;
        size_t start = 0;
        while (start <= value.size()) {
            const size_t comma = value.find(',', start);
            if (comma == std::string::npos) {
                f.push_back(value.substr(start));
                break;
            }
            f.push_back(value.substr(start, comma - start));
            start = comma + 1;
        }
        return f;
    };
    // 对象存储的 X,Y = 格栅坐标 (rx,ry)（与 IsoMapPack5 记录同坐标系），
    // 经与 cells_ 相同的反对角线映射换算到 (col,row)（否则对象全部挤在地图顶部）
    auto to_cell = [&](const std::string& rx_s, const std::string& ry_s, int& cx, int& cy) {
        const int rx = std::atoi(rx_s.c_str());
        const int ry = std::atoi(ry_s.c_str());
        const int row = rx + ry - min_s_;
        const int col = (rx - ry - min_d_) / 2;
        cx = col;
        cy = row;
        return col >= 0 && row >= 0 && col < cell_w() && row < cell_h();
    };
    for (const auto& [key, value] : ini_.section("Units")) {
        (void)key;
        const auto f = split_csv(value);
        if (f.size() < 6) continue; // House,ID,health,rx,ry,dir,...
        MapUnit u;
        u.owner = f[0];
        u.id = f[1];
        u.health = std::atoi(f[2].c_str());
        if (!to_cell(f[3], f[4], u.cx, u.cy)) continue;
        u.dir = static_cast<uint8_t>(std::atoi(f[5].c_str()));
        units_.push_back(std::move(u));
    }
    for (const auto& [key, value] : ini_.section("Structures")) {
        (void)key;
        const auto f = split_csv(value);
        if (f.size() < 6) continue; // House,ID,health,rx,ry,dir,...
        MapBuilding b;
        b.owner = f[0];
        b.id = f[1];
        b.health = std::atoi(f[2].c_str());
        b.rx = std::atoi(f[3].c_str());
        b.ry = std::atoi(f[4].c_str());
        if (!to_cell(f[3], f[4], b.cx, b.cy)) continue;
        b.dir = static_cast<uint8_t>(std::atoi(f[5].c_str()));
        buildings_.push_back(std::move(b));
    }
    for (const auto& [key, value] : ini_.section("Infantry")) {
        (void)key;
        const auto f = split_csv(value);
        if (f.size() < 8) continue; // House,ID,health,rx,ry,subcell,mission,dir,...
        MapInfantry n;
        n.owner = f[0];
        n.id = f[1];
        n.health = std::atoi(f[2].c_str());
        if (!to_cell(f[3], f[4], n.cx, n.cy)) continue;
        n.subcell = static_cast<uint8_t>(std::atoi(f[5].c_str()));
        n.dir = static_cast<uint8_t>(std::atoi(f[7].c_str()));
        infantry_.push_back(std::move(n));
    }
    // ── [Terrain]（树木/岩石）：key = rx + ry·1000（OpenRA ReadTerrainActors 语义）──
    terrain_.clear();
    for (const auto& [key, value] : ini_.section("Terrain")) {
        const int pos = std::atoi(key.c_str());
        const int rx = pos % 1000;
        const int ry = pos / 1000;
        int cx = -1, cy = -1;
        if (!to_cell(std::to_string(rx), std::to_string(ry), cx, cy)) continue;
        if (value.empty()) continue;
        MapTerrain t;
        t.name = value;
        t.cx = cx;
        t.cy = cy;
        terrain_.push_back(std::move(t));
    }
    // ── [Shroud]（可选）：index=状态(1..5)，key = 格栅序号 rx + 512·ry ──
    shroud_.assign(static_cast<size_t>(cell_w()) * cell_h(), 1);
    for (const auto& [key, value] : ini_.section("Shroud")) {
        const int cell = std::atoi(key.c_str());
        const int rx = cell % 512;
        const int ry = cell / 512;
        const int row = rx + ry - min_s_;
        const int col = (rx - ry - min_d_) / 2;
        if (col < 0 || row < 0 || col >= cell_w() || row >= cell_h()) continue;
        const int idx = row * cell_w() + col;
        const int state = std::atoi(value.c_str());
        if (state >= 1 && state <= 5) shroud_[idx] = static_cast<uint8_t>(state);
    }
    return true;
}

namespace {
// 低桥多格件（木 74-101/122-125、混凝土 205-236）：件 = 3 格宽（横跨桥向），
// OverlayData 0/1/2 = 件内位置；桥面只在件原点（data=0）触发绘制，实际绘制格
// = 件中格（data=1，见 overlay_draw_cell）——画布中心对中格时桥面顶角恰好落在
// 件顶角、底鼻落在件底角（lobrdb23/11/21 像素掩码 + test1/test2 实测）。
bool is_low_bridge_piece(uint8_t t) {
    return (t >= 74 && t <= 101) || (t >= 122 && t <= 125) || (t >= 205 && t <= 236);
}
// ── 实测 test.yrm 桥件布局（点阵 rx,ry + data）──
// 混凝土桥（下左向 6 节）：每节 3 格沿 rx（下右）：(62..64,ry) data 0/1/2，
//   节内类型 227/216/215/214/229（LOBRDB19/08/07/06/21，桥面艺术主轴 -0.4 = 下左向 ✓）；
// 木桥（下左向）：(62..64,73..78) 类型 96/85/86/83/98 同构；
// 下右向坡件：94/92/225/223 为 3 格沿 ry（下左）(rx,62..64) data 0/1/2，
//   艺术主轴 +0.45 = 下右向 ✓。⇒ 桥面艺术方向 = 桥长方向，与件宽轴垂直。
// 高架桥面（BRIDGE1/2 = 25/26）：每格一段条带，帧 = OverlayData 件号
//   （BRIDGE.ubn 帧 0-8 = 下左条带、9-17 = 下右条带；实测桥面格存 9/2/6/8/10/11/12）。
// 桥基（LOBRDGB1-4 = 237-240）：每格独立一片（艺术仅帧 1 有内容）。
int bridge_family(uint8_t t) {
    if (t >= 77 && t <= 85) return 1;
    if (t >= 86 && t <= 94) return 2;
    if (t >= 95 && t <= 96) return 3;
    if (t >= 97 && t <= 98) return 4;
    if (t >= 99 && t <= 100) return 5;
    if (t >= 101 && t <= 102) return 6;
    if (t == 125) return 3; // LOBRDGE1-4 = 木坡 se/nw/ne/sw
    if (t == 126) return 4;
    if (t == 127) return 5;
    if (t == 128) return 6;
    if (t >= 209 && t <= 217) return 7;
    if (t >= 218 && t <= 226) return 8;
    if (t >= 227 && t <= 228) return 9;
    if (t >= 229 && t <= 230) return 10;
    if (t >= 231 && t <= 232) return 11;
    if (t >= 233 && t <= 234) return 12;
    if (t >= 237 && t <= 238) return 9; // LOBRDGB1-4 = 混凝土坡/桥基
    if (t >= 239 && t <= 240) return 11;
    return 0;
}

bool is_bridge_overlay(uint8_t t) {
    return bridge_family(t) != 0 || t == 25 || t == 26;
}
} // namespace

uint8_t MapFile::overlay_render_frame(int cx, int cy) const {
    if (cx < 0 || cy < 0 || cx >= cell_w() || cy >= cell_h()) return 0;
    const MapCell& c = cells_[static_cast<size_t>(cy) * cell_w() + cx];
    if (!c.present) return 0;
    const uint8_t t = overlay_type(c.x, c.y);
    if (t == 0xFF) return 0;
    const uint8_t d = overlay_data_at(c.x, c.y);
    // 高架桥面（24/25 = BRIDGE1/BRIDGE2 条带）：每格一段，帧 = OverlayData 件号
    // （0..17，BRIDGE.ubn 帧 0-8="/"、9-17="\"；实测 test5.yrm "\"=25×3 data=9、
    //   test6.yrm "/"=24×3 data=0；26=苏军围墙见艺术表）
    if (t >= 24 && t <= 25) return d < 18 ? d : 0;
    // 低桥多格件（木 74-101/122-125、混凝土 205-236）：3 格件，data 0/1/2 = 件内位置；
    // 桥面只在件原点（data=0）触发，绘制格经 overlay_draw_cell 移到件中格，
    // 延续格跳过。实测 test1.mpr："\" 三片 227/215/229；test2.yrm："/" 三片 225/206/223。
    if (is_low_bridge_piece(t)) return d == 0 ? 1 : 0xFF;
    // 高架木桥基（237/238 = BRIDGB 条带；239 = BRIDGE 条带）：每格一段，
    // 帧 = OverlayData 件号（0..17；240=克里姆林宫围墙见艺术表）
    if (t >= 237 && t <= 239) return d < 18 ? d : 0;
    // FENCE01-19（DEMO 装饰围栏，2 帧精灵=本体/阴影）：恒用帧 0，data 非帧号
    // （203/204 = 黑/白栅栏 CAFNCB/CAFNCW 见艺术表）
    if (t >= 184 && t <= 202) return 0;
    return d;
}

void MapFile::overlay_anchor_offset(uint8_t type, int& dx, int& dy) const {
    // 低桥件已由 overlay_draw_cell 处理（绘制格 = 件中格），此处无额外格偏移
    (void)type;
    dx = dy = 0;
}

void MapFile::overlay_draw_cell(int cx, int cy, int& dx_, int& dy_) const {
    // 低桥多格件：绘制格 = 件中格（data=1 的同类型邻格）——桥面艺术画布中心
    // 对中格设计，顶角落件顶角、底鼻落件底角（建筑顶格锚点同语义）。
    // 其余覆盖物：绘制格 = 本格。
    dx_ = cx;
    dy_ = cy;
    if (cx < 0 || cy < 0 || cx >= cell_w() || cy >= cell_h()) return;
    const MapCell& c = cells_[static_cast<size_t>(cy) * cell_w() + cx];
    if (!c.present) return;
    const uint8_t t = overlay_type(c.x, c.y);
    if (!is_low_bridge_piece(t) || overlay_data_at(c.x, c.y) != 0) return;
    for (int ddy = -1; ddy <= 1; ++ddy) {
        for (int ddx = -1; ddx <= 1; ++ddx) {
            if (!ddx && !ddy) continue;
            const int nx = cx + ddx, ny = cy + ddy;
            if (nx < 0 || ny < 0 || nx >= cell_w() || ny >= cell_h()) continue;
            const MapCell& nc = cells_[static_cast<size_t>(ny) * cell_w() + nx];
            if (!nc.present) continue;
            if (overlay_type(nc.x, nc.y) == t && overlay_data_at(nc.x, nc.y) == 1) {
                dx_ = nx;
                dy_ = ny;
                return;
            }
        }
    }
}

} // namespace ra2r::assets
