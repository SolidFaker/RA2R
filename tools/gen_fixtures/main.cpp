// RA2R — 测试夹具生成器（tools/gen_fixtures）
//
// 按 docs/formats/*.md 的规格，合成一组**合法但最小**的示例文件，
// 供单元测试（tests/test_fixtures.cpp）与 CI 使用——测试不再依赖游戏素材。
//
// 产出（默认写到 tests/fixtures/）：
//   sample.mix    扩展 MIX v2（明文，无校验/加密）：2 个条目（INI + PAL）
//   sample.pal    768 字节调色盘（6 位 RGB 渐变 + 指定索引标记色）
//   sample.shp    TS/RA2 SHP：4 帧 8×6（0 未压缩 / 1 RLE 压缩 / 2 空帧 / 3 全透明）
//   sample.vxl    Voxel Animation：1 section 2×2×2 立方体（span 编码）
//   sample.hva    HVA：1 帧 1 section 平移 (16,32,48)/16 = (1,2,3) 体素
//   sample.map    最小 .map（INI 文本）：[Map] + [IsoMapPack5]（未压缩块）+ 对象
//   sample.ini    rulesmd 风格片段：General/BuildingTypes/Colors/Countries 等
//
// 用法：gen_fixtures [输出目录]（缺省 tests/fixtures）
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    put_u16(v, static_cast<uint16_t>(x & 0xFFFF));
    put_u16(v, static_cast<uint16_t>(x >> 16));
}
void put_i32(std::vector<uint8_t>& v, int32_t x) { put_u32(v, static_cast<uint32_t>(x)); }
void put_f32(std::vector<uint8_t>& v, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put_u32(v, bits);
}
void put_bytes(std::vector<uint8_t>& v, const char* s) {
    while (*s) v.push_back(static_cast<uint8_t>(*s++));
}
// 定长 ASCIIZ（NUL 填充）
void put_fixed(std::vector<uint8_t>& v, const char* s, size_t n) {
    const size_t len = std::strlen(s);
    for (size_t i = 0; i < n; ++i) v.push_back(i < len ? static_cast<uint8_t>(s[i]) : 0);
}

bool write_file(const std::filesystem::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

// 文件名 → MIX id（RA2/YR：大写 + '/'→'\' + 4 字节对齐填充 + 标准 CRC32；
// 与 MixFile::crc32_of_name / obfuscate_name 同规）
uint32_t crc32_name(std::string name) {
    for (char& c : name) {
        if (c == '/') c = '\\';
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    const size_t len = name.size();
    const size_t salt = len & ~size_t(3);
    if (len & 3) {
        name.push_back(static_cast<char>(len & 3));
        for (size_t i = 0; i < 3 - (len & 3); ++i) name.push_back(name[salt]);
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : name) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

// ── PAL：768 字节 6 位调色盘（索引 0 黑、1 阴影蓝、其余渐变；几个标记色）──
std::vector<uint8_t> make_pal() {
    std::vector<uint8_t> pal(768, 0);
    auto set = [&](int i, int r, int g, int b) {
        pal[i * 3] = static_cast<uint8_t>(r);
        pal[i * 3 + 1] = static_cast<uint8_t>(g);
        pal[i * 3 + 2] = static_cast<uint8_t>(b);
    };
    set(1, 0, 0, 12);      // 阴影索引（原版深蓝）
    set(2, 63, 0, 0);      // 纯红（测试用）
    set(3, 0, 63, 0);      // 纯绿
    set(4, 0, 0, 63);      // 纯蓝
    set(5, 63, 63, 63);    // 白
    for (int i = 16; i < 32; ++i) set(i, 63, 0, 63); // Remap 段（测试重映射）
    for (int i = 32; i < 256; ++i) set(i, i % 64, (i * 3) % 64, (i * 7) % 64);
    return pal;
}

// ── SHP：4 帧 8×6（0 未压缩 / 1 RLE / 2 空帧 / 3 全透明 RLE）──
std::vector<uint8_t> make_shp() {
    const uint16_t W = 8, H = 6;
    struct Frame {
        uint16_t x, y, cx, cy;
        uint8_t flags;
        std::vector<uint8_t> data; // 未压缩像素或 RLE 载荷
    };
    std::vector<Frame> frames;
    // 帧 0：未压缩（flags bit1 = 0）
    {
        Frame f{0, 0, W, H, 0, {}};
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                f.data.push_back(static_cast<uint8_t>((x + y) % 6 + 1));
        frames.push_back(std::move(f));
    }
    // 帧 1：RLE 压缩（每行 [u16 line_len][0x00 n 透明游程 / 字面像素]）
    {
        Frame f{0, 0, W, H, 0x02, {}};
        for (int y = 0; y < H; ++y) {
            std::vector<uint8_t> row;
            row.push_back(0);
            row.push_back(2); // 前 2 像素透明
            for (int x = 2; x < W; ++x) row.push_back(static_cast<uint8_t>(x + 1));
            const uint16_t line_len = static_cast<uint16_t>(row.size() + 2);
            put_u16(f.data, line_len);
            f.data.insert(f.data.end(), row.begin(), row.end());
        }
        frames.push_back(std::move(f));
    }
    // 帧 2：空帧（offset = 0）
    {
        Frame f{0, 0, W, H, 0, {}};
        frames.push_back(std::move(f));
    }
    // 帧 3：RLE 全透明（每行一条 8 像素透明游程）
    {
        Frame f{0, 0, W, H, 0x02, {}};
        for (int y = 0; y < H; ++y) {
            put_u16(f.data, 4); // line_len = 4（含自身 2 字节）
            f.data.push_back(0);
            f.data.push_back(W);
        }
        frames.push_back(std::move(f));
    }
    std::vector<uint8_t> out;
    put_u16(out, 0);
    put_u16(out, W);
    put_u16(out, H);
    put_u16(out, static_cast<uint16_t>(frames.size()));
    const uint32_t data_start =
        8 + static_cast<uint32_t>(frames.size()) * 24;
    uint32_t cursor = data_start;
    for (const auto& f : frames) {
        put_u16(out, f.x);
        put_u16(out, f.y);
        put_u16(out, f.cx);
        put_u16(out, f.cy);
        out.push_back(f.flags);
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        put_u32(out, 0); // color（未用）
        put_u32(out, 0); // reserved
        const bool empty = f.data.empty();
        put_u32(out, empty ? 0 : cursor);
        if (!empty) cursor += static_cast<uint32_t>(f.data.size());
    }
    for (const auto& f : frames) out.insert(out.end(), f.data.begin(), f.data.end());
    return out;
}

// ── VXL：1 section，2×2×2 立方体（span：每列 1 run：SKIP=0 COUNT=2 两体素 + 重复 COUNT）──
std::vector<uint8_t> make_vxl() {
    const uint32_t sx = 2, sy = 2, sz = 2;
    // Body：span_start/size_x*size_y（int32） + span_end + 数据
    const uint32_t cols = sx * sy;
    std::vector<std::vector<uint8_t>> col_data(cols);
    for (uint32_t j = 0; j < cols; ++j) {
        auto& c = col_data[j];
        c.push_back(0); // SKIP
        c.push_back(static_cast<uint8_t>(sz)); // COUNT
        for (uint32_t z = 0; z < sz; ++z) {
            c.push_back(static_cast<uint8_t>(2 + j)); // color
            c.push_back(0);                           // normal（RA2 244 法线表 idx0）
        }
        c.push_back(static_cast<uint8_t>(sz)); // 重复 COUNT
    }
    std::vector<uint8_t> body;
    uint32_t data_cursor = 0;
    std::vector<uint32_t> starts(cols), ends(cols);
    for (uint32_t j = 0; j < cols; ++j) {
        starts[j] = data_cursor;
        ends[j] = data_cursor + static_cast<uint32_t>(col_data[j].size()) - 1;
        data_cursor += static_cast<uint32_t>(col_data[j].size());
    }
    for (uint32_t j = 0; j < cols; ++j) put_i32(body, static_cast<int32_t>(starts[j]));
    for (uint32_t j = 0; j < cols; ++j) put_i32(body, static_cast<int32_t>(ends[j]));
    for (uint32_t j = 0; j < cols; ++j)
        body.insert(body.end(), col_data[j].begin(), col_data[j].end());

    std::vector<uint8_t> out;
    put_fixed(out, "Voxel Animation", 16); // 魔数
    put_u32(out, 1);                       // unknown（恒 1）
    put_u32(out, 1);                       // n_limbs
    put_u32(out, 1);                       // n_limbs2
    put_u32(out, static_cast<uint32_t>(body.size()));
    put_u16(out, 0x1F10); // unknown2（版本魔数）
    {
        const auto pal = make_pal();
        out.insert(out.end(), pal.begin(), pal.end());
    }
    // Section 头（28 字节）
    put_fixed(out, "SEC_BODY", 16);
    put_i32(out, 0); // section_i
    put_i32(out, 1); // one
    put_i32(out, 0); // zero
    out.insert(out.end(), body.begin(), body.end());
    // Tailer（92 字节）：[0..3] span_start 偏移 [4..7] span_end [8..11] span_data
    // [0x0C] det [0x10..0x3F] 旋转 3×3 [0x40..0x4B] min [0x4C..0x57] max
    // [0x58] sx [0x59] sy [0x5A] sz [0x5B] normal_type
    put_i32(out, 0);                                                       // span_start
    put_i32(out, static_cast<int32_t>(cols) * 4);                          // span_end
    put_i32(out, static_cast<int32_t>(cols) * 8);                          // span_data
    put_f32(out, 1.0f);                                                    // det
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) put_f32(out, r == c ? 1.0f : 0.0f);    // 旋转（0x10..0x33）
    for (int i = 0; i < 12; ++i) out.push_back(0);                         // 0x34..0x3F 未用
    put_f32(out, 0.0f);                                                    // min（0x40..0x4B）
    put_f32(out, 0.0f);
    put_f32(out, 0.0f);
    put_f32(out, static_cast<float>(sx));                                  // max（0x4C..0x57）
    put_f32(out, static_cast<float>(sy));
    put_f32(out, static_cast<float>(sz));
    out.push_back(static_cast<uint8_t>(sx));                               // 0x58
    out.push_back(static_cast<uint8_t>(sy));                               // 0x59
    out.push_back(static_cast<uint8_t>(sz));                               // 0x5A
    out.push_back(4); // 0x5B normal_type = RA2（244 法线表）
    return out;      // tailer 恰 92 字节
}

// ── HVA：1 帧 1 section，平移 (16,32,48) = (1,2,3) 体素 ──
std::vector<uint8_t> make_hva() {
    std::vector<uint8_t> out;
    put_fixed(out, "Voxel Animation", 16); // id（同 VXL 魔数）
    put_u32(out, 1);                       // n_frames
    put_u32(out, 1);                       // n_sections
    put_fixed(out, "SEC_BODY", 16);
    // 列主序：c0=(1,0,0, tx) c1=(0,1,0, ty) c2=(0,0,1, tz)
    put_f32(out, 1.0f);
    put_f32(out, 0.0f);
    put_f32(out, 0.0f);
    put_f32(out, 16.0f);
    put_f32(out, 0.0f);
    put_f32(out, 1.0f);
    put_f32(out, 0.0f);
    put_f32(out, 32.0f);
    put_f32(out, 0.0f);
    put_f32(out, 0.0f);
    put_f32(out, 1.0f);
    put_f32(out, 48.0f);
    return out;
}

// ── MIX：扩展 MIX v2（明文无校验）：2 条目 ──
std::vector<uint8_t> make_mix(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& items) {
    struct Ent {
        uint32_t id, offset, size;
    };
    std::vector<Ent> ents;
    std::vector<uint8_t> body;
    for (const auto& [name, data] : items) {
        Ent e{crc32_name(name), static_cast<uint32_t>(body.size()),
              static_cast<uint32_t>(data.size())};
        ents.push_back(e);
        body.insert(body.end(), data.begin(), data.end());
    }
    // 索引按 id 升序
    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.id < b.id; });
    std::vector<uint8_t> out;
    put_u16(out, 0); // marker（扩展 MIX）
    put_u16(out, 0); // flags（无校验/无加密）
    put_u16(out, static_cast<uint16_t>(ents.size()));
    put_u32(out, static_cast<uint32_t>(body.size()));
    for (const auto& e : ents) {
        put_u32(out, e.id);
        put_u32(out, e.offset);
        put_u32(out, e.size);
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// ── 地图：最小 .map（4×4 可玩区 + IsoMapPack5 未压缩块 + 单位/建筑/步兵）──
std::string make_map_ini() {
    // IsoMapPack5：16×16 反对角线格（rx 0..15, ry 0..15）→ 256 条 × 11 字节
    // 映射后 (rx−ry−min_d)/2 ∈ [0,15]（列）、rx+ry−min_s ∈ [0,30]（行）；
    // [Map] Size 宽需 ≥ 16 才能让全部记录落进网格
    std::vector<uint8_t> tiles;
    for (int rx = 0; rx < 16; ++rx)
        for (int ry = 0; ry < 16; ++ry) {
            put_u16(tiles, static_cast<uint16_t>(rx));
            put_u16(tiles, static_cast<uint16_t>(ry));
            put_u16(tiles, 0); // tile id 0（Clear）
            put_u16(tiles, 0);
            tiles.push_back(0);              // subtile
            tiles.push_back(static_cast<uint8_t>((rx + ry) % 3)); // height
            tiles.push_back(0);
        }
    // [u16 comp][u16 uncomp] + 数据（comp == uncomp = 未压缩）
    std::vector<uint8_t> pack;
    put_u16(pack, static_cast<uint16_t>(tiles.size()));
    put_u16(pack, static_cast<uint16_t>(tiles.size()));
    pack.insert(pack.end(), tiles.begin(), tiles.end());
    put_u16(pack, 0);
    put_u16(pack, 0); // 结束标记

    static const char* kB64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (size_t i = 0; i < pack.size(); i += 3) {
        const uint32_t b0 = pack[i];
        const uint32_t b1 = i + 1 < pack.size() ? pack[i + 1] : 0;
        const uint32_t b2 = i + 2 < pack.size() ? pack[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        b64.push_back(kB64[(v >> 18) & 63]);
        b64.push_back(kB64[(v >> 12) & 63]);
        b64.push_back(i + 1 < pack.size() ? kB64[(v >> 6) & 63] : '=');
        b64.push_back(i + 2 < pack.size() ? kB64[v & 63] : '=');
    }
    // 按 70 字符一行折行（键 1,2,3…）
    std::string out =
        "[Basic]\n"
        "Name=Fixture Map\n"
        "NewINIFormat=5\n"
        "\n"
        "[Map]\n"
        "Size=0,0,16,16\n"
        "Theater=TEMPERAT\n"
        "LocalSize=0,0,16,16\n"
        "\n"
        "[IsoMapPack5]\n";
    int key = 1;
    for (size_t i = 0; i < b64.size(); i += 70) {
        out += std::to_string(key++) + "=" + b64.substr(i, 70) + "\n";
    }
    out +=
        "\n"
        "[Structures]\n"
        "0=Americans,GAPOWR,256,5,5,0\n"
        "\n"
        "[Units]\n"
        "0=Americans,MTNK,256,6,6,64\n"
        "\n"
        "[Infantry]\n"
        "0=Americans,E1,256,7,7,0,Guard,128\n"
        "\n"
        "[Terrain]\n"
        "6008=TREE01\n"
        "\n"
        "[Waypoints]\n"
        "0=1020\n"
        "1=3050\n"
        "\n"
        "[Digest]\n"
        "1=AAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
    return out;
}

// ── INI：rulesmd 风格片段 ──
std::string make_rules_ini() {
    return
        "[General]\n"
        "BuildupTime=.06\n"
        "IdleActionFrequency=.15\n"
        "BuildSpeed=.7\n"
        "RefundPercent=50\n"
        "PrerequisitePower=GAPOWR,NAPOWR\n"
        "\n"
        "[Colors]\n"
        "DarkBlue=25,60,255\n"
        "Red=255,0,0\n"
        "\n"
        "[Countries]\n"
        "0=Americans\n"
        "1=Russians\n"
        "\n"
        "[Americans]\n"
        "Name=America\n"
        "Side=GDI\n"
        "Color=DarkBlue\n"
        "Multiplay=yes\n"
        "\n"
        "[Russians]\n"
        "Name=Russia\n"
        "Side=Nod\n"
        "Color=Red\n"
        "Multiplay=yes\n"
        "\n"
        "[Sides]\n"
        "GDI=Americans\n"
        "Nod=Russians\n"
        "\n"
        "[BuildingTypes]\n"
        "0=GAPOWR\n"
        "1=GACNST\n"
        "2=GAPILL\n"
        "\n"
        "[GAPOWR]\n"
        "Image=GGPOWR\n"
        "Foundation=2x2\n"
        "Cost=800\n"
        "Power=200\n"
        "Strength=750\n"
        "Owner=Americans\n"
        "TechLevel=1\n"
        "Prerequisite=POWER\n"
        "BuildCat=Power\n"
        "\n"
        "[GACNST]\n"
        "Foundation=4x4\n"
        "Cost=2500\n"
        "Strength=1000\n"
        "Owner=Americans\n"
        "TechLevel=-1\n"
        "ConstructionYard=yes\n"
        "\n"
        "[GAPILL]\n"
        "Image=GGPILL\n"
        "Foundation=1x1\n"
        "Cost=600\n"
        "Strength=400\n"
        "Owner=Americans\n"
        "TechLevel=2\n"
        "Prerequisite=POWER\n"
        "Primary=PillboxWeapon\n"
        "Turret=yes\n"
        "TurretAnim=GGPILLTUR\n"
        "\n"
        "[InfantryTypes]\n"
        "0=E1\n"
        "\n"
        "[E1]\n"
        "Image=GI\n"
        "Cost=100\n"
        "Strength=125\n"
        "Primary=M60\n"
        "Owner=Americans\n"
        "\n"
        "[WeaponTypes]\n"
        "0=M60\n"
        "1=PillboxWeapon\n"
        "\n"
        "[M60]\n"
        "Damage=15\n"
        "ROF=20\n"
        "Range=4\n"
        "\n"
        "[PillboxWeapon]\n"
        "Damage=25\n"
        "ROF=15\n"
        "Range=5\n";
}

// ── artmd 风格片段 ──
std::string make_art_ini() {
    return
        "[GAPOWR]\n"
        "Image=GGPOWR\n"
        "Height=4\n"
        "Buildup=GAPOWRMK\n"
        "ActiveAnim=GAPOWR_A\n"
        "ActiveAnimDamaged=GAPOWR_AD\n"
        "ActiveAnimTwo=GAPOWR_B\n"
        "ActiveAnimTwoDamaged=GAPOWR_BD\n"
        "IdleAnim=GAPOWR_I\n"
        "IdleAnimDamaged=GAPOWR_ID\n"
        "IdleAnimTwo=GAPOWR_J\n"
        "ProductionAnim=GAPOWR_P\n"
        "ProductionAnimDamaged=GAPOWR_PD\n"
        "\n"
        "[E1]\n"
        "Image=GI\n"
        "Sequence=GISequence\n"
        "\n"
        "[GISequence]\n"
        "Guard=0,1,1\n"
        "Walk=8,6,6\n"
        "Idle1=56,15,0,S\n"
        "Idle2=71,15,0,E\n"
        "\n"
        "[GAPILL]\n"
        "Image=GGPILL\n"
        "Turret=yes\n"
        "TurretAnim=GGPILLTUR\n";
}

} // namespace

// ── PCX：8 位索引色 4×2（RLE）+ 末尾 769 字节调色板 ──
std::vector<uint8_t> make_pcx() {
    const int W = 4, H = 2;
    std::vector<uint8_t> out(128, 0);
    out[0] = 0x0A; // manufacturer
    out[1] = 5;    // version
    out[2] = 1;    // encoding = RLE
    out[3] = 8;    // bpp
    auto put16 = [&](size_t off, uint16_t v) {
        out[off] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    put16(0x08, static_cast<uint16_t>(W - 1)); // xmax
    put16(0x0A, static_cast<uint16_t>(H - 1)); // ymax
    put16(0x0C, 72);
    put16(0x0E, 72);
    out[0x41] = 1;    // planes = 1
    put16(0x42, W);   // bytes per line
    put16(0x44, 1);   // palette info
    // 行 0：像素 1,2,2,3 → 用 RLE：字面 1、游程 2×2、字面 3
    out.push_back(1);
    out.push_back(0xC2); // RLE：重复 2 次
    out.push_back(2);
    out.push_back(3);
    // 行 1：全 4（RLE 4×）
    out.push_back(0xC4);
    out.push_back(4);
    // 调色板：0x0C + 256×RGB（索引 2 纯红）
    out.push_back(0x0C);
    for (int i = 0; i < 256; ++i) {
        out.push_back(static_cast<uint8_t>(i % 64 * 4));
        out.push_back(static_cast<uint8_t>((i * 3) % 64 * 4));
        out.push_back(static_cast<uint8_t>((i * 7) % 64 * 4));
    }
    out[out.size() - 769 + 1 + 2 * 3 + 0] = 255; // 索引 2 R = 255
    out[out.size() - 769 + 1 + 2 * 3 + 1] = 0;
    out[out.size() - 769 + 1 + 2 * 3 + 2] = 0;
    return out;
}

// ── CSF：2 条目（'RTS ' 普通 + 'WRTS' 带音效引用）──
std::vector<uint8_t> make_csf() {
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) { put_u32(out, v); };
    put_bytes(out, " FSC");
    put32(3); // version
    put32(2); // count
    put32(2);
    put32(0);
    put32(0); // language = 英文
    // 条目 1：Name:Americans = "America"（RTS，正文逐字节取反 UTF-16LE）
    {
        put_bytes(out, " LBL");
        put32(1); // 值数量
        const char* key = "Name:Americans";
        put32(static_cast<uint32_t>(std::strlen(key))); // 名称长度不含 NUL
        out.insert(out.end(), key, key + std::strlen(key));
        const std::u16string val = u"America";
        put_bytes(out, " RTS"); // 真实格式：'RTS ' 的 LE 存储为 " RTS"
        put32(static_cast<uint32_t>(val.size())); // 长度按 UTF-16 单元数
        for (char16_t c : val) {
            const uint16_t inv = static_cast<uint16_t>(~static_cast<uint16_t>(c));
            put_u16(out, inv);
        }
    }
    // 条目 2：DESC:E31 = "Demo Map"（WRTS + extra 音效名）
    {
        put_bytes(out, " LBL");
        put32(1);
        const char* key = "DESC:E31";
        put32(static_cast<uint32_t>(std::strlen(key))); // 名称长度不含 NUL
        out.insert(out.end(), key, key + std::strlen(key));
        const std::u16string val = u"Demo Map";
        put_bytes(out, "WRTS");
        put32(static_cast<uint32_t>(val.size())); // 单元数
        for (char16_t c : val) {
            const uint16_t inv = static_cast<uint16_t>(~static_cast<uint16_t>(c));
            put_u16(out, inv);
        }
        const std::u16string snd = u"SND_DEMO";
        put32(static_cast<uint32_t>(snd.size() * 2));
        for (char16_t c : snd) {
            const uint16_t inv = static_cast<uint16_t>(~static_cast<uint16_t>(c));
            put_u16(out, inv);
        }
    }
    return out;
}

// ── FNT（Unicode 'fonT'）：1 字形（'A' = U+0041），3×2 位图 ──
std::vector<uint8_t> make_fnt() {
    const uint32_t stride = 1, lines = 2, count = 1;
    const uint32_t symbol_data_size = 1 + stride * lines;
    std::vector<uint8_t> out(0x1C + 65536 * 2, 0);
    std::memcpy(out.data(), "fonT", 4);
    auto put32_at = [&](size_t off, uint32_t v) {
        out[off] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    put32_at(0x04, 20); // ideograph width
    put32_at(0x08, stride);
    put32_at(0x0C, lines);
    put32_at(0x10, 3); // font height
    put32_at(0x14, count);
    put32_at(0x18, symbol_data_size);
    // UnicodeTable：'A' → 1（字形 0）
    out[0x1C + 0x41 * 2] = 1;
    out[0x1C + 0x41 * 2 + 1] = 0;
    // 字形 0：width=3，位图 2 行（0b11100000 / 0b10100000）
    out.push_back(3);
    out.push_back(0xE0);
    out.push_back(0xA0);
    return out;
}

// ── AUD：无压缩（compression=0）16 位单声道，2 块 ──
std::vector<uint8_t> make_aud() {
    std::vector<uint8_t> body;
    // 块：fsize=dsize（magic 非 DEAF → 原样 16 位 PCM 拷贝）
    auto block = [&](const std::initializer_list<int16_t>& samples) {
        const uint16_t n = static_cast<uint16_t>(samples.size() * 2);
        put_u16(body, n);
        put_u16(body, n);
        put_u32(body, 0); // magic = 0 → 原样
        for (int16_t s : samples) put_u16(body, static_cast<uint16_t>(s));
    };
    block({1, 2, 3, 4});
    block({5, 6});
    std::vector<uint8_t> out;
    put_u16(out, 22050);                               // rate
    put_u32(out, static_cast<uint32_t>(body.size()));  // size
    put_u32(out, static_cast<uint32_t>(body.size()));  // uncompressed size
    out.push_back(2);                                  // flags：16 位单声道
    out.push_back(0);                                  // compression = 无压缩
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

static int run_fixtures(const std::vector<std::string>& args) {
    const std::filesystem::path out_dir =
        !args.empty() ? std::filesystem::path(args[0]) : std::filesystem::path("tests/fixtures");
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s: %s\n", out_dir.string().c_str(),
                     ec.message().c_str());
        return 1;
    }
    const auto pal = make_pal();
    const auto shp = make_shp();
    const auto vxl = make_vxl();
    const auto hva = make_hva();
    const auto ini = make_rules_ini();
    const auto art = make_art_ini();
    const auto map_ini = make_map_ini();
    const std::vector<std::pair<std::string, std::vector<uint8_t>>> mix_items = {
        {"SAMPLE.INI", std::vector<uint8_t>(ini.begin(), ini.end())},
        {"SAMPLE.PAL", pal},
    };
    const auto mix = make_mix(mix_items);
    const auto pcx = make_pcx();
    const auto csf = make_csf();
    const auto fnt = make_fnt();
    const auto aud = make_aud();

    struct Out {
        const char* name;
        const std::vector<uint8_t>* data;
    };
    const std::vector<uint8_t> map_bytes(map_ini.begin(), map_ini.end());
    const std::vector<uint8_t> art_bytes(art.begin(), art.end());
    const Out files[] = {
        {"sample.pal", &pal},   {"sample.shp", &shp}, {"sample.vxl", &vxl},
        {"sample.hva", &hva},   {"sample.map", &map_bytes},
        {"sample.ini", &art_bytes}, {"sample.mix", &mix},
        {"sample.pcx", &pcx},   {"sample.csf", &csf}, {"sample.fnt", &fnt},
        {"sample.aud", &aud},
    };
    int ok = 0;
    for (const auto& f : files) {
        const auto p = out_dir / f.name;
        if (write_file(p, *f.data)) {
            std::printf("wrote %-12s %zu bytes\n", f.name, f.data->size());
            ++ok;
        } else {
            std::fprintf(stderr, "FAILED %s\n", p.string().c_str());
        }
    }
    // rulesmd 片段（sample.ini 是 artmd；rules 另存）
    {
        const std::vector<uint8_t> rules(ini.begin(), ini.end());
        const auto p = out_dir / "sample_rules.ini";
        if (write_file(p, rules)) {
            std::printf("wrote %-12s %zu bytes\n", "sample_rules.ini", rules.size());
            ++ok;
        }
    }
    std::printf("fixtures: %d files -> %s\n", ok, out_dir.string().c_str());
    return ok == 12 ? 0 : 1;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::wstring w(wargv[i]);
        args.emplace_back(w.begin(), w.end());
    }
    return run_fixtures(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return run_fixtures(args);
}
#endif
