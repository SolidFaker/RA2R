// RA2R — AUD/WAV 实现
// AUD 头: [u16 rate][u32 size][u32 uncomp][u8 flags][u8 compression] 共 12 字节
//   （size 从偏移 12 起算；TD 文档的 14 字节头不适用 RA2——实测 mod 提取的
//    ra_kaboom15.aud/ra_tank5.aud 均为 12 字节头）
//   flags bit0=立体声, bit1=16bit; compression 0=无 1=ZAP 99=ADPCM
//   块: [u16 fsize][u16 dsize][u32 magic(0xDEAF=压缩)]，fsize==dsize 时原样拷贝
//   WW-ADPCM: 每 4bit 一个样本（低半字节先），IMA 步长/索引表
// WAV: 标准 RIFF，支持 PCM 8/16bit 与 IMA ADPCM（fmt=17，RA2 语言包实测格式）
#include "ra2r/assets/aud_file.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "ra2r/core/endian.h"

namespace ra2r::assets {

namespace {
constexpr int16_t kStepTab[89] = {
    7,    8,    9,    10,   11,   12,   13,   14,   16,   17,   19,   21,   23,   25,
    28,   31,   34,   37,   41,   45,   50,   55,   60,   66,   73,   80,   88,   97,
    107,  118,  130,  143,  157,  173,  190,  209,  230,  253,  279,  307,  337,  371,
    408,  449,  494,  544,  598,  658,  724,  796,  876,  963,  1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767};
constexpr int8_t kIndexTab[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct AdpcmState {
    int index = 0;
    int predicted = 0;
};

// 解一个通道：输入为压缩字节流（步进 in_stride），输出交错写（步进 out_stride 样本）
// 输入按 in_bytes 上限截断：损坏块（fsize 小于 dsize 所需）时宁少解不出界。
void adpcm_channel(const uint8_t* in, int in_bytes, int16_t* out, int sample_count,
                   int in_stride, int out_stride, AdpcmState& st) {
    int step = kStepTab[st.index];
    const int half = std::min((sample_count + 1) / 2, in_bytes);
    for (int i = 0; i < half; ++i) {
        const uint8_t byte = in[static_cast<size_t>(i) * in_stride];
        for (int nib = 0; nib < 2; ++nib) {
            if (i * 2 + nib >= sample_count) break;
            const int n = nib == 0 ? (byte & 0xF) : (byte >> 4);
            int diff = step >> 3;
            if (n & 4) diff += step;
            if (n & 2) diff += step >> 1;
            if (n & 1) diff += step >> 2;
            if (n & 8) diff = -diff;
            int sample = st.predicted + diff;
            if (sample < -32768) sample = -32768;
            if (sample > 32767) sample = 32767;
            st.predicted = sample;
            out[(static_cast<size_t>(i) * 2 + nib) * out_stride] =
                static_cast<int16_t>(sample);
            st.index += kIndexTab[n & 7];
            if (st.index < 0) st.index = 0;
            if (st.index > 88) st.index = 88;
            step = kStepTab[st.index];
        }
    }
}
} // namespace

bool AudFile::open(const uint8_t* data, size_t size, std::string* error) {
    data_.clear();
    is_wav_ = false;
    if (size >= 4 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F') {
        // ── WAV（RIFF，仅 PCM 8/16bit）──
        is_wav_ = true;
        compression_ = 0;
        size_t pos = 12;
        bool have_fmt = false;
        while (pos + 8 <= size) {
            const std::string tag(reinterpret_cast<const char*>(data + pos), 4);
            const uint32_t len = core::read_u32_le(data + pos + 4);
            pos += 8;
            if (tag == "fmt ") {
                if (pos + 16 > size || len < 16) break;
                const uint16_t fmt = core::read_u16_le(data + pos);
                channels_ = core::read_u16_le(data + pos + 2);
                rate_ = static_cast<uint16_t>(core::read_u32_le(data + pos + 4));
                block_align_ = core::read_u16_le(data + pos + 12);
                bits_ = core::read_u16_le(data + pos + 14);
                if (fmt == 17) {
                    // IMA ADPCM WAV：fmt 扩展含 cbSize + 每块样本数
                    compression_ = 17;
                    bits_ = 16; // 解码输出 16bit
                    if (len >= 20 && pos + 20 <= size) {
                        samples_per_block_ = core::read_u16_le(data + pos + 18);
                    }
                } else if (fmt == 1) {
                    compression_ = 0;
                    if (bits_ != 8 && bits_ != 16) {
                        if (error) *error = "unsupported WAV bits (PCM 8/16 only)";
                        return false;
                    }
                } else {
                    if (error) *error = "unsupported WAV format (PCM/IMA ADPCM only)";
                    return false;
                }
                have_fmt = true;
            } else if (tag == "data") {
                if (!have_fmt) break;
                body_start_ = pos;
                body_size_ = len;
                uncomp_size_ = len;
                break;
            }
            pos += len + (len & 1);
        }
        if (!have_fmt || body_size_ == 0) {
            if (error) *error = "WAV missing fmt/data";
            return false;
        }
        data_.assign(data, data + size);
        return true;
    }
    // ── AUD ──
    if (size < 12) {
        if (error) *error = "AUD too small";
        return false;
    }
    rate_ = core::read_u16_le(data);
    body_size_ = core::read_u32_le(data + 2);      // 压缩后数据区大小（从偏移 12 起算）
    uncomp_size_ = core::read_u32_le(data + 6);    // 未压缩大小
    flags_ = data[10];
    compression_ = data[11];
    channels_ = (flags_ & 1) ? 2 : 1;
    bits_ = (flags_ & 2) ? 16 : 8;
    body_start_ = 12;
    if (body_start_ + body_size_ > size) {
        if (error) *error = "AUD body out of range";
        return false;
    }
    data_.assign(data, data + size);
    return true;
}

bool AudFile::open(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open audio: " + path.string();
        return false;
    }
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return open(reinterpret_cast<const uint8_t*>(data.data()), data.size(), error);
}

bool AudFile::decode_pcm16(std::vector<int16_t>& out, std::string* error) const {
    out.clear();
    if (data_.empty()) return false;
    const uint8_t* body = data_.data() + body_start_;
    const size_t body_len = body_size_;
    const size_t total_samples = static_cast<size_t>(uncomp_size_) / (bits_ / 8);
    out.reserve(total_samples);

    if (is_wav_) {
        if (compression_ == 17) {
            // IMA ADPCM WAV：按块解码（每块: i16 predictor + u8 index + u8 保留 + 半字节流）
            if (block_align_ == 0 || samples_per_block_ == 0) {
                if (error) *error = "WAV ADPCM missing block params";
                return false;
            }
            const size_t block_count = body_len / block_align_;
            out.reserve(static_cast<size_t>(block_count) * samples_per_block_ * channels_);
            for (size_t b = 0; b < block_count; ++b) {
                const uint8_t* blk = body + b * block_align_;
                for (int c = 0; c < channels_; ++c) {
                    const uint8_t* bp = blk + c * 4; // 每通道 4 字节头
                    int pred = static_cast<int16_t>(core::read_u16_le(bp));
                    int index = bp[2];
                    if (index < 0) index = 0;
                    if (index > 88) index = 88;
                    int step = kStepTab[index];
                    out.push_back(static_cast<int16_t>(pred));
                    const uint8_t* nibs = blk + 4 * channels_;
                    const int per_ch = samples_per_block_ - 1; // 每通道半字节数
                    for (int i = 0; i < per_ch; ++i) {
                        const size_t nib_pos = static_cast<size_t>(c) * per_ch + i;
                        const uint8_t byte = nibs[nib_pos / 2];
                        const int n = (nib_pos & 1) ? (byte >> 4) : (byte & 0xF);
                        int diff = step >> 3;
                        if (n & 4) diff += step;
                        if (n & 2) diff += step >> 1;
                        if (n & 1) diff += step >> 2;
                        if (n & 8) diff = -diff;
                        int sample = pred + diff;
                        if (sample < -32768) sample = -32768;
                        if (sample > 32767) sample = 32767;
                        pred = sample;
                        out.push_back(static_cast<int16_t>(sample));
                        index += kIndexTab[n & 7];
                        if (index < 0) index = 0;
                        if (index > 88) index = 88;
                        step = kStepTab[index];
                    }
                }
            }
            return !out.empty();
        }
        // 原样 PCM → 16 位
        if (bits_ == 16) {
            for (size_t i = 0; i + 1 < body_len; i += 2) {
                out.push_back(static_cast<int16_t>(core::read_u16_le(body + i)));
            }
        } else {
            for (size_t i = 0; i < body_len; ++i) {
                out.push_back(static_cast<int16_t>((body[i] - 128) << 8));
            }
        }
        return true;
    }

    // AUD 块循环
    size_t pos = 0;
    while (pos + 8 <= body_len) {
        const uint16_t fsize = core::read_u16_le(body + pos);
        const uint16_t dsize = core::read_u16_le(body + pos + 2);
        const uint32_t magic = core::read_u32_le(body + pos + 4);
        pos += 8;
        if (fsize == 0 || dsize == 0) break;
        if (pos + fsize > body_len) {
            if (error) *error = "AUD chunk overrun";
            return false;
        }
        const uint8_t* chunk = body + pos;
        if (magic != 0x0000DEAF) {
            // 原样拷贝（或 ZAP——先按原始拷贝处理，ZAP 极少见）
            if (fsize == dsize) {
                for (size_t i = 0; i + 1 < fsize && out.size() < total_samples; i += 2) {
                    out.push_back(static_cast<int16_t>(core::read_u16_le(chunk + i)));
                }
            }
        } else {
            // WW-ADPCM
            const int ch = channels_;
            const int sample_count = dsize / (bits_ / 8); // 每块输出样本数
            std::vector<int16_t> tmp(static_cast<size_t>(sample_count) * ch);
            if (bits_ == 16) {
                const int in_bytes_per_ch = fsize / ch; // 每通道压缩字节
                for (int c = 0; c < ch; ++c) {
                    AdpcmState st;
                    adpcm_channel(chunk + c, in_bytes_per_ch, tmp.data() + c, sample_count,
                                  ch, ch, st);
                }
                out.insert(out.end(), tmp.begin(), tmp.end());
            } else {
                // 8 位输出：每样本 1 字节（^0x80 转有符号）
                const int in_bytes_per_ch = fsize / ch;
                std::vector<uint8_t> tmp8(static_cast<size_t>(sample_count) * ch);
                for (int c = 0; c < ch; ++c) {
                    AdpcmState st;
                    std::vector<int16_t> ch16(sample_count);
                    adpcm_channel(chunk + c, in_bytes_per_ch, ch16.data(), sample_count, ch, 1,
                                  st);
                    for (int i = 0; i < sample_count; ++i) {
                        tmp8[static_cast<size_t>(i) * ch + c] =
                            static_cast<uint8_t>(((ch16[i] & 0xFF00) >> 8) ^ 0x80);
                    }
                }
                for (uint8_t v : tmp8) {
                    out.push_back(static_cast<int16_t>((v - 128) << 8));
                }
            }
        }
        pos += fsize;
        if (out.size() >= total_samples) break;
    }
    return !out.empty();
}

} // namespace ra2r::assets
