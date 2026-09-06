#pragma once
// mixbrowser 媒体播放器（AUD/WAV 引擎解码；BIK 经 FFmpeg 运行时绑定解码）。
// 视频帧以 RGBA 输出，供 SDL 纹理上传；音频经 SDL3 设备流播放。
// main.cpp 只依赖本头，全部 FFmpeg 对象藏于 Impl（PIMPL）。
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mixbrowser {

class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();
    MediaPlayer(MediaPlayer&&) noexcept;
    MediaPlayer& operator=(MediaPlayer&&) noexcept;
    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    // 打开条目内容并准备播放（不自动起播）。
    // ext：".aud"/".wav" 走引擎 AudFile（无需 FFmpeg）；".bik" 走 FFmpeg。
    // 失败返回 false，原因见 last_error()。
    bool open(const uint8_t* data, size_t size, const std::string& ext);
    void close();

    bool is_open() const;
    bool has_video() const;  // 视频流就绪
    bool has_audio() const;  // 音频设备流已建好
    bool playing() const;

    void play();   // 起播/继续（音视频同一时刻开始）
    void pause();  // 暂停（保留缓冲，可继续播放）

    // 每帧调用：按墙钟推进视频解码，播完自动停。产出新视频帧时返回 true。
    bool tick();

    int video_w() const;
    int video_h() const;
    int video_stride() const;  // RGBA 行字节距（已 32 字节对齐，可能 > w*4）
    double video_fps() const;
    const std::vector<uint8_t>& video_rgba() const;  // 最近一帧 RGBA（tick 之后有效）

    double duration() const;         // 总时长（秒）；未知为 0
    double position() const;         // 当前播放位置（秒）
    double audio_buffered() const;   // 音频流未播数据折算秒数

    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mixbrowser
