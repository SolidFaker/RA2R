// RA2R — MIX 读取器实现（格式事实来源：Chronoshift mixfile.h / ra2-mix crate / 本地实测）。
#include "ra2r/assets/mix_file.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "ra2r/core/endian.h"
#include "ra2r/crypto/bigint.h"
#include "ra2r/crypto/blowfish.h"

namespace ra2r::assets {

namespace {

// RA2 加密 MIX 内嵌 Blowfish 密钥的 RSA 公开参数（社区公开事实）：
//   指数 e = 65537；模数 n（320 位，小端字节序）：
const uint8_t kRsaModulusLE[40] = {
    21,  127, 67,  170, 61,  79,  251, 209, 230, 193, 176, 248, 106, 14,  221, 171,
    74,  176, 130, 102, 250, 84,  170, 232, 162, 63,  113, 81,  214, 96,  81,  86,
    228, 252, 57,  109, 8,   218, 188, 81,
};

// 标准 CRC-32（poly 0xEDB88320，init 0xFFFFFFFF，xorout 0xFFFFFFFF，与 zlib 一致；
// 实测锚点：crc("LOCAL MIX DATABASE.DAT" 混淆后) == 0x366E051F）
uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// RA2 文件名混淆：大写；若长度非 4 的倍数，追加 (len&3) 字节后
// 从索引 (len&~3) 处复制字符补齐到 4 的倍数。
std::string obfuscate_name(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const size_t len = name.size();
    const size_t salt = len & ~size_t(3);
    if (len & 3) {
        name.push_back(static_cast<char>(len & 3));
        for (size_t i = 0; i < 3 - (len & 3); ++i) {
            name.push_back(name[salt]);
        }
    }
    return name;
}

bool read_all(const std::filesystem::path& path, std::vector<uint8_t>& out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open file: " + path.string();
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) f.read(reinterpret_cast<char*>(out.data()), size);
    if (!f && !f.eof()) {
        if (error) *error = "read error: " + path.string();
        return false;
    }
    return true;
}

} // namespace

uint32_t MixFile::crc32_of_name(std::string name) {
    const std::string obf = obfuscate_name(std::move(name));
    return crc32(reinterpret_cast<const uint8_t*>(obf.data()), obf.size());
}

bool MixFile::open(const std::filesystem::path& path, std::string* error) {
    entries_.clear();
    names_.clear();
    flags_ = 0;
    file_count_ = 0;
    data_size_ = 0;
    body_start_ = 0;
    return read_all(path, data_, error) && parse(error);
}

bool MixFile::open(const uint8_t* data, size_t size, std::string* error) {
    entries_.clear();
    names_.clear();
    flags_ = 0;
    file_count_ = 0;
    data_size_ = 0;
    body_start_ = 0;
    data_.assign(data, data + size);
    return parse(error);
}

bool MixFile::looks_like_mix(const uint8_t* p, size_t n) {
    if (n < 12) return false;
    const uint16_t first = core::read_u16_le(p);
    if (first == 0) {
        const uint32_t flags = core::read_u32_le(p);
        if ((flags & 0xFFFF) != 0) return false;
        if ((flags & ~(kFlagHasChecksum | kFlagEncrypted)) != 0) return false;
        if (flags & kFlagEncrypted) return n >= 92; // count 在加密区，解析后校验
        const uint16_t count = core::read_u16_le(p + 4);
        return count > 0 && count < 0x8000; // 索引长度由 parse 校验
    }
    return first > 0 && first < 0x8000; // 同上
}

bool MixFile::parse(std::string* error) {
    if (data_.size() < 12) {
        if (error) *error = "file too small for a mix header";
        return false;
    }

    const uint8_t* p = data_.data();
    bool new_format = true;
    if (core::read_u16_le(p) != 0) {
        // 旧格式（无旗标）
        new_format = false;
        flags_ = 0;
        file_count_ = core::read_u16_le(p);
        data_size_ = core::read_u32_le(p + 2);
    } else {
        flags_ = core::read_u32_le(p);
        file_count_ = core::read_u16_le(p + 4);
        data_size_ = core::read_u32_le(p + 6);
    }

    const size_t index_size = 12ull * file_count_;
    uint32_t count = file_count_;
    uint32_t dsize = data_size_;
    size_t body = (new_format ? 10 : 6) + index_size;
    std::vector<uint8_t> index_buf;

    if (flags_ & kFlagEncrypted) {
        if (data_.size() < 92) {
            if (error) *error = "too small for encrypted mix";
            return false;
        }
        // 1) RSA 解封 Blowfish 密钥（4..84，两个 40 字节块）
        const crypto::BigInt modulus = crypto::BigInt::from_bytes_le(kRsaModulusLE, 40);
        std::vector<uint8_t> key;
        for (int block = 0; block < 2; ++block) {
            const crypto::BigInt c =
                crypto::BigInt::from_bytes_le(data_.data() + 4 + block * 40, 40);
            const crypto::BigInt m = crypto::BigInt::pow_mod(c, 65537u, modulus);
            uint8_t out[40];
            const int n = m.to_bytes_le(out, 40);
            key.insert(key.end(), out, out + n);
        }

        // 2) 解密第一个 8 字节块 → count / size / 索引前 2 字节
        if (key.empty()) {
            if (error) *error = "RSA key unwrap produced empty blowfish key";
            return false;
        }
        crypto::Blowfish bf;
        bf.set_key(key.data(), static_cast<int>(key.size()));
        uint8_t first[8];
        bf.decrypt_block(data_.data() + 84, first);
        count = core::read_u16_le(first);
        dsize = core::read_u32_le(first + 2);
        if (count == 0 || count > 0x8000) {
            if (error) *error = "decrypted count out of range (not an encrypted mix?)";
            return false;
        }

        // 3) 解密剩余索引（含 8 字节对齐补零）
        const size_t remaining = 12ull * count - 2;
        const size_t pad = (8 - (remaining % 8)) % 8;
        const size_t enc_rest = remaining + pad;
        if (84 + 8 + enc_rest > data_.size()) {
            if (error) *error = "encrypted index region out of range (not an encrypted mix?)";
            return false;
        }
        index_buf.resize(12ull * count);
        index_buf[0] = first[6];
        index_buf[1] = first[7];
        std::vector<uint8_t> enc(enc_rest);
        for (size_t off = 0; off < enc_rest; off += 8) {
            bf.decrypt_block(data_.data() + 84 + 8 + off, enc.data() + off);
        }
        std::copy(enc.begin(), enc.begin() + static_cast<ptrdiff_t>(remaining),
                  index_buf.begin() + 2);

        body = 90 + 12ull * count + pad; // 10(头) + 80(密钥) + 12*count(索引) + pad
    } else {
        const size_t header = new_format ? 10 : 6;
        if (count > 0x8000 || header + index_size > data_.size()) {
            if (error) *error = "index out of range";
            return false;
        }
        index_buf.assign(data_.begin() + static_cast<ptrdiff_t>(header),
                         data_.begin() + static_cast<ptrdiff_t>(header + index_size));
    }

    file_count_ = static_cast<uint16_t>(count);
    data_size_ = dsize;
    body_start_ = static_cast<uint32_t>(body);
    if (body + data_size_ > data_.size() + 20) {
        if (error) *error = "mix body out of range (corrupt or wrong key?)";
        return false;
    }

    entries_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = index_buf.data() + 12ull * i;
        entries_[i].id = core::read_u32_le(e);
        entries_[i].offset = core::read_u32_le(e + 4);
        entries_[i].size = core::read_u32_le(e + 8);
    }

    // 本地文件名数据库：id == crc32("LOCAL MIX DATABASE.DAT")
    const uint32_t db_id = crc32_of_name("local mix database.dat");
    const MixEntry* db = find(db_id);
    if (db) {
        std::vector<uint8_t> raw;
        if (read_entry(*db, raw)) {
            // 格式：52 字节 XCC 头 + 连续 NUL 结尾文件名
            size_t start = 0;
            const char kMagic[] = "XCC by Olaf van der Spek";
            if (raw.size() > 52 &&
                std::memcmp(raw.data(), kMagic, sizeof(kMagic) - 1) == 0) {
                start = 52; // XCC_HEADER_SIZE（魔数 + 类型/版本字段）
            } else {
                start = 0;
            }
            size_t i = start;
            while (i < raw.size()) {
                size_t j = i;
                while (j < raw.size() && raw[j] != 0) ++j;
                if (j > i) {
                    const std::string name(reinterpret_cast<const char*>(raw.data() + i), j - i);
                    if (name.size() >= 3 && name.find('.') != std::string::npos) {
                        names_[crc32_of_name(name)] = name;
                    }
                }
                i = j + 1;
            }
        }
    }

    return true;
}

const MixEntry* MixFile::find(uint32_t id) const {
    for (const auto& e : entries_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

const MixEntry* MixFile::find_by_name(std::string_view name) const {
    return find(crc32_of_name(std::string(name)));
}

const std::string* MixFile::name_of(uint32_t id) const {
    const auto it = names_.find(id);
    return it == names_.end() ? nullptr : &it->second;
}

bool MixFile::read_entry(const MixEntry& e, std::vector<uint8_t>& out) const {
    const uint64_t start = static_cast<uint64_t>(body_start_) + e.offset;
    const uint64_t end = start + e.size;
    if (end > data_.size()) return false;
    out.assign(data_.begin() + static_cast<ptrdiff_t>(start),
               data_.begin() + static_cast<ptrdiff_t>(end));
    return true;
}

bool MixFile::read_head(const MixEntry& e, uint8_t* buf, size_t n) const {
    const uint64_t start = static_cast<uint64_t>(body_start_) + e.offset;
    const uint64_t end = start + e.size;
    if (start >= data_.size()) return false;
    const size_t avail = static_cast<size_t>(std::min<uint64_t>(end, data_.size()) - start);
    const size_t take = std::min(n, avail);
    std::memcpy(buf, data_.data() + start, take);
    return take == std::min(n, static_cast<size_t>(e.size));
}

} // namespace ra2r::assets
