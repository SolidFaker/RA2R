#pragma once
// RA2R — AUD（Westwood ADPCM/ZAP/无压缩）与 WAV（RIFF PCM 8/16 或 IMA ADPCM fmt=17）音频读取
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ra2r::assets {

class AudFile {
public:
    bool open(const uint8_t* data, size_t size, std::string* error = nullptr);
    bool open(const std::filesystem::path& path, std::string* error = nullptr);

    bool is_open() const { return !data_.empty(); }
    bool is_wav() const { return is_wav_; }
    uint16_t rate() const { return rate_; }
    uint32_t uncomp_size() const { return uncomp_size_; }
    int channels() const { return channels_; }
    int bits() const { return bits_; }
    uint8_t compression() const { return compression_; } // AUD: 0/1/99；WAV: 0(PCM)/17(IMA ADPCM)
    bool is_adpcm_wav() const { return is_wav_ && compression_ == 17; }
    double duration_seconds() const {
        // ADPCM WAV 的 uncomp_size 是压缩数据长，用块数×每块样本数计算
        if (is_adpcm_wav() && block_align_ > 0 && samples_per_block_ > 0) {
            const size_t blocks = body_size_ / block_align_;
            return static_cast<double>(blocks * samples_per_block_) / rate_;
        }
        const int frame_bytes = channels_ * (bits_ / 8);
        return (rate_ > 0 && frame_bytes > 0)
                   ? static_cast<double>(uncomp_size_) / (rate_ * frame_bytes)
                   : 0.0;
    }

    // 解码为 16 位交错 PCM（样本数 = channels × frames）
    bool decode_pcm16(std::vector<int16_t>& out, std::string* error = nullptr) const;

private:
    std::vector<uint8_t> data_;
    bool is_wav_ = false;
    uint16_t rate_ = 0;
    uint32_t uncomp_size_ = 0;
    uint8_t flags_ = 0;
    uint8_t compression_ = 0;
    int channels_ = 1;
    int bits_ = 16;
    size_t body_start_ = 0; // AUD: 头后偏移；WAV: 数据块偏移
    uint32_t body_size_ = 0;
    uint16_t block_align_ = 0;        // WAV IMA ADPCM 块对齐
    uint16_t samples_per_block_ = 0;  // WAV IMA ADPCM 每块样本数
};

} // namespace ra2r::assets
