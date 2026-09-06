// mixbrowser 媒体播放器实现。
// 架构（两条通路，按扩展名分流）：
//   AUD/WAV → 引擎 AudFile 整段预解码为 S16 PCM → SDL3 设备流（无需 FFmpeg）。
//   BIK     → FFmpeg 运行时绑定（SDL_LoadFunction 免导入库）：
//             视频按墙钟逐帧解码输出 RGBA；音频包在"追帧"途中顺带解码，
//             交错灌入 SDL3 流（缓冲超上限丢包防膨胀）；EOF 冲刷解码器收尾，
//             视频读尽且音频排空后自动停止。
// 关键修复（相对旧版 main.cpp 内联实现）：
//   1. 旧版先整段预解码音频，期间读到的视频包被丢弃且文件已到 EOF，
//      视频永远无帧可解 —— 改为交错解码，视频包必达视频解码器。
//   2. 每次 av_read_frame 前不 unref 旧包导致包引用泄漏 —— 全部显式 unref。
//   3. swr_alloc_set_opts2 末参实为 void* log_ctx（旧版误写 AVDictionary**）。
//   4. 暂停/恢复用 base_sec 累计，恢复后音视频时钟不跳变。
#include "media_player.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <SDL3/SDL.h>

#include "ffmpeg_bind.h"          // FFmpeg 运行时绑定（见 ffmpeg_bind.*）
#include "ra2r/assets/aud_file.h"

namespace mixbrowser {

namespace {

// 打开默认播放设备流（S16/声道/采样率；设备初始为暂停态，play() 时恢复）
SDL_AudioStream* make_sdl_stream(int channels, int rate) {
    SDL_InitSubSystem(SDL_INIT_AUDIO);  // SDL3 需显式初始化音频子系统（SDL_INIT_VIDEO 不覆盖）
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = channels;
    spec.freq = rate;
    return SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
}

}  // namespace

// ── 内部状态 ──────────────────────────────────────────────────────────

struct MediaPlayer::Impl {
    bool active = false;
    bool playing = false;
    std::string last_error;

    // 音频输出（AUD/WAV 与 BIK 共用 SDL3 流）
    SDL_AudioStream* sdl_stream = nullptr;
    int a_channels = 1;
    int a_rate = 44100;

    // FFmpeg 对象（仅 BIK）
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vcc = nullptr;
    AVCodecContext* acc = nullptr;
    AVFrame* vframe = nullptr;
    AVFrame* aframe = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    int vstream = -1, astream = -1;  // FFmpeg 流序号（-1 = 无）
    int vw = 0, vh = 0;
    int vstride = 0;             // RGBA 行字节距（32 字节对齐；见 swscale 越界缺陷说明）
    double fps = 15.0;
    double duration = 0.0;
    double vtb = 0.0;             // 视频流 time_base（pts → 秒）
    uint64_t t0 = 0;              // 本段播放起点（SDL ticks）
    double base_sec = 0.0;        // 暂停前累计播放秒数
    double next_video_sec = 0.0;  // 下一视频帧应出现的时刻
    bool video_eof = false;       // 视频流读尽且解码器已清空
    bool eof_flushed = false;     // EOF 后已向解码器送过空包冲刷
    std::vector<uint8_t> vrgba;
    bool vframe_valid = false;
    std::filesystem::path tmp_path;

    void close_all();
    bool open_aud(const uint8_t* data, size_t size);
    bool open_bik(const uint8_t* data, size_t size);
    void play_now();
    void pause_now();
    bool tick_now();
    int decode_video_step();      // 1=出新帧；-1=视频结束
    void send_audio_packet();     // 当前 packet 是音频包：解码入流（超上限丢包）
    void drain_audio_decoder();
    void queue_audio_frame();
    double now_sec() const {
        return base_sec + (playing ? (SDL_GetTicks() - t0) / 1000.0 : 0.0);
    }
    int64_t audio_available() const {
        // 输入侧未播字节（我们的 S16 PCM 口径；Available 是设备侧转换后口径，勿混用）
        return sdl_stream ? SDL_GetAudioStreamQueued(sdl_stream) : 0;
    }
    double audio_buffered_secs() const {
        if (!sdl_stream || a_rate <= 0 || a_channels <= 0) return 0.0;
        const int64_t bytes = audio_available();
        return bytes > 0 ? static_cast<double>(bytes) / (a_rate * a_channels * 2.0) : 0.0;
    }
};

// ── 生命周期 ──────────────────────────────────────────────────────────

MediaPlayer::MediaPlayer() : impl_(std::make_unique<Impl>()) {}
MediaPlayer::~MediaPlayer() = default;
MediaPlayer::MediaPlayer(MediaPlayer&&) noexcept = default;
MediaPlayer& MediaPlayer::operator=(MediaPlayer&&) noexcept = default;

void MediaPlayer::Impl::close_all() {
    // FFmpeg 仅在确有 FFmpeg 对象时才加载/释放（AUD 路径零 FFmpeg 依赖）
    const bool need_ff = fmt || vcc || acc || sws || swr || vframe || aframe || packet;
    const FFmpegApi& ff = need_ff ? ffmpeg_api() : no_ffmpeg_api();
    if (sdl_stream) {
        SDL_PauseAudioStreamDevice(sdl_stream);
        SDL_DestroyAudioStream(sdl_stream);
    }
    if (ff.ok) {
        if (sws) ff.sws_freeContext(sws);
        if (swr) ff.swr_free(&swr);
        ff.av_frame_free(&vframe);
        ff.av_frame_free(&aframe);
        ff.av_packet_free(&packet);
        if (vcc) ff.avcodec_free_context(&vcc);
        if (acc) ff.avcodec_free_context(&acc);
        if (fmt) ff.avformat_close_input(&fmt);
    }
    if (!tmp_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
    }
    *this = Impl{};  // 全部复位（含 last_error）
}

void MediaPlayer::close() {
    if (impl_) impl_->close_all();
}

bool MediaPlayer::open(const uint8_t* data, size_t size, const std::string& ext) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->close_all();
    std::string e = ext;
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (e == ".aud" || e == ".wav") return impl_->open_aud(data, size);
    return impl_->open_bik(data, size);
}

// ── AUD/WAV：引擎解码通路 ─────────────────────────────────────────────

bool MediaPlayer::Impl::open_aud(const uint8_t* data, size_t size) {
    ra2r::assets::AudFile aud;
    std::string err;
    if (!aud.open(data, size, &err)) {
        last_error = "音频解析失败: " + err;
        return false;
    }
    std::vector<int16_t> pcm;
    if (!aud.decode_pcm16(pcm, &err)) {
        last_error = "音频解码失败: " + err;
        return false;
    }
    a_channels = std::clamp(aud.channels(), 1, 2);
    a_rate = aud.rate();
    if (pcm.empty() || a_rate == 0) {
        last_error = "音频无有效样本";
        return false;
    }
    sdl_stream = make_sdl_stream(a_channels, a_rate);
    if (!sdl_stream) {
        last_error = std::string("音频设备打开失败: ") + SDL_GetError();
        return false;
    }
    if (SDL_PutAudioStreamData(sdl_stream, pcm.data(),
                               static_cast<int>(pcm.size() * sizeof(int16_t)))) {
        SDL_FlushAudioStream(sdl_stream);  // 整段已备齐：通知流可全部转出
    }
    duration = aud.duration_seconds();
    active = true;
    return true;
}

// ── BIK：FFmpeg 通路 ──────────────────────────────────────────────────

bool MediaPlayer::Impl::open_bik(const uint8_t* data, size_t size) {
    const FFmpegApi& ff = ffmpeg_api();
    if (!ff.ok) {
        last_error = "FFmpeg 8.x 就绪失败（" + ff.missing +
                     "）：请把 avutil-60/avcodec-62/avformat-62/swscale-9/swresample-6.dll "
                     "复制到 mixbrowser.exe 同目录";
        return false;
    }
    auto fail = [&](const char* msg) {
        close_all();
        last_error = msg;
        return false;
    };
    // 提取到 %TEMP%\ra2r_mixbrowser（FFmpeg 按文件名/魔数探测）
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "ra2r_mixbrowser";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    static uint32_t seq = 0;
    const std::filesystem::path out = dir / ("clip_" + std::to_string(++seq) + ".bik");
    {
        std::ofstream f(out, std::ios::binary);
        if (!f) return fail("临时文件创建失败");
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!f) return fail("临时文件写入失败");
    }
    tmp_path = out;  // fail() 将顺带删除

    AVFormatContext* fmt = nullptr;
    if (ff.avformat_open_input(&fmt, out.string().c_str(), nullptr, nullptr) != 0 || !fmt)
        return fail("avformat_open_input 失败（格式不被识别）");
    if (ff.avformat_find_stream_info(fmt, nullptr) < 0)
        return fail("avformat_find_stream_info 失败");
    this->fmt = fmt;
    vstream = ff.av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    astream = ff.av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vstream, nullptr, 0);

    // 解码器/转换器就绪；失败仅退化该流，不致命
    auto open_codec = [&](int si, AVCodecContext** cc) -> bool {
        AVCodecContext* c = ff.avcodec_alloc_context3(nullptr);
        *cc = c;
        if (!c) return false;
        if (ff.avcodec_parameters_to_context(c, fmt->streams[si]->codecpar) < 0) return false;
        const AVCodec* dec = ff.avcodec_find_decoder(c->codec_id);
        if (!dec) return false;
        return ff.avcodec_open2(c, dec, nullptr) == 0;
    };
    if (vstream >= 0 && open_codec(vstream, &vcc)) {
        const AVStream* vs = fmt->streams[vstream];
        vw = vcc->width;
        vh = vcc->height;
        fps = av_q2d(vs->avg_frame_rate);
        if (fps <= 0 || fps > 120) fps = 15.0;
        vtb = av_q2d(vs->time_base);
        // 关键：目标行距按 32 字节对齐并预留尾量。FFmpeg 8.x 的 swscale 对
        // 宽度非 16 倍数的帧（如 140）在 bilinear 路径会按对齐宽度越界写行尾，
        // 行距不足时破坏堆（表现为 sws_freeContext 时崩溃）。加宽行距即安全。
        vstride = (vw * 4 + 31) & ~31;
        vrgba.assign(static_cast<size_t>(vstride) * vh, 0);
        sws = ff.sws_getContext(vw, vh, vcc->pix_fmt, vw, vh, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                nullptr, nullptr, nullptr);
        if (!sws || vw <= 0 || vh <= 0) vstream = -1;
    } else {
        vstream = -1;
    }
    if (astream >= 0 && open_codec(astream, &acc)) {
        const int ch = acc->ch_layout.nb_channels;
        if (ch >= 1 && ch <= 8) {
            a_channels = ch;
            a_rate = acc->sample_rate;
            AVChannelLayout out_ch;
            out_ch.order = AV_CHANNEL_ORDER_UNSPEC;  // 默认布局（与源同声道数）
            out_ch.nb_channels = ch;
            out_ch.u.mask = 0;
            if (ff.swr_alloc_set_opts2(&swr, &out_ch, AV_SAMPLE_FMT_S16, acc->sample_rate,
                                       &acc->ch_layout, acc->sample_fmt, acc->sample_rate, 0,
                                       nullptr) == 0 &&
                ff.swr_init(swr) == 0) {
                sdl_stream = make_sdl_stream(ch, acc->sample_rate);
            }
        }
        if (!swr || !sdl_stream) astream = -1;  // 音频退化：无声播放
    } else {
        astream = -1;
    }
    if (vstream < 0 && astream < 0) return fail("无可用音视频流");

    // 时长：容器 > 视频流 > 音频流，均未知则 0（UI 显示 -）
    if (fmt->duration > 0) {
        duration = fmt->duration / static_cast<double>(AV_TIME_BASE);
    } else if (vstream >= 0 && fmt->streams[vstream]->duration > 0) {
        duration = fmt->streams[vstream]->duration * vtb;
    } else if (astream >= 0 && fmt->streams[astream]->duration > 0) {
        duration = fmt->streams[astream]->duration * av_q2d(fmt->streams[astream]->time_base);
    }
    vframe = ff.av_frame_alloc();
    aframe = ff.av_frame_alloc();
    packet = ff.av_packet_alloc();
    active = true;

    // 无视频流（通用音频兜底）：整段预解码，无需按墙钟驱动
    if (vstream < 0 && astream >= 0 && sdl_stream) {
        while (ff.av_read_frame(fmt, packet) >= 0) {
            if (packet->stream_index == astream) send_audio_packet();
            ff.av_packet_unref(packet);
        }
        ff.avcodec_send_packet(acc, nullptr);
        drain_audio_decoder();
        SDL_FlushAudioStream(sdl_stream);
    }
    return true;
}

// ── 播放控制 ──────────────────────────────────────────────────────────

void MediaPlayer::play() {
    if (impl_) impl_->play_now();
}

void MediaPlayer::Impl::play_now() {
    if (!active || playing) return;
    t0 = SDL_GetTicks();
    playing = true;
    if (sdl_stream) SDL_ResumeAudioStreamDevice(sdl_stream);
}

void MediaPlayer::pause() {
    if (impl_) impl_->pause_now();
}

void MediaPlayer::Impl::pause_now() {
    if (!active || !playing) return;
    base_sec = now_sec();  // 冻结位置；恢复后音视频时钟不跳变
    playing = false;
    if (sdl_stream) SDL_PauseAudioStreamDevice(sdl_stream);
}

bool MediaPlayer::tick() {
    return impl_ ? impl_->tick_now() : false;
}

bool MediaPlayer::Impl::tick_now() {
    if (!active || !playing) return false;
    bool new_frame = false;
    if (vstream >= 0) {
        // 解码直到追平墙钟；视频读尽后继续收解码器尾帧（不再受 now 限制）
        int guard = 0;
        while (guard++ < 240) {
            if (!video_eof && next_video_sec > now_sec()) break;
            const int r = decode_video_step();
            if (r == 1) new_frame = true;
            else if (r == -1) break;
        }
    }
    // 播完自动停：视频读尽且音频排空（纯音频只看排空）
    if ((vstream < 0 || video_eof) && audio_available() <= 0) {
        base_sec = now_sec();
        playing = false;
        if (sdl_stream) SDL_PauseAudioStreamDevice(sdl_stream);
    }
    return new_frame;
}

// ── 视频解码（交错音视频）────────────────────────────────────────────

// 供包 → 解帧。产出新帧返回 1；视频流彻底结束返回 -1。
// 途中遇到的音频包顺带解码入流；其他流丢弃。
int MediaPlayer::Impl::decode_video_step() {
    const FFmpegApi& ff = ffmpeg_api();
    if (vstream < 0 || !vcc || !sws || !fmt || !vframe || !packet) {
        video_eof = true;
        return -1;
    }
    for (;;) {
        if (ff.avcodec_receive_frame(vcc, vframe) == 0) {
            uint8_t* dst[4] = {vrgba.data(), nullptr, nullptr, nullptr};
            const int dstride[4] = {vstride, 0, 0, 0};
            ff.sws_scale(sws, vframe->data, vframe->linesize, 0, vh, dst, dstride);
            const double pts = vframe->pts >= 0 ? vframe->pts * vtb
                                                : next_video_sec + 1.0 / fps;
            next_video_sec = pts;
            vframe_valid = true;
            return 1;
        }
        for (;;) {
            if (ff.av_read_frame(fmt, packet) < 0) {
                if (!eof_flushed) {
                    eof_flushed = true;
                    if (astream >= 0 && acc) ff.avcodec_send_packet(acc, nullptr);
                    drain_audio_decoder();
                    ff.avcodec_send_packet(vcc, nullptr);
                    if (sdl_stream) SDL_FlushAudioStream(sdl_stream);
                    continue;  // 回外层收解码器尾帧
                }
                video_eof = true;
                return -1;
            }
            if (packet->stream_index == vstream) {
                ff.avcodec_send_packet(vcc, packet);
                ff.av_packet_unref(packet);
                break;  // 已供入输入 → 回外层收帧
            }
            if (packet->stream_index == astream && astream >= 0) send_audio_packet();
            ff.av_packet_unref(packet);
        }
    }
}

void MediaPlayer::Impl::send_audio_packet() {
    const FFmpegApi& ff = ffmpeg_api();
    if (astream < 0 || !acc || !swr || !sdl_stream || !aframe || !packet) return;
    if (audio_buffered_secs() > 6.0) return;  // 追帧时缓冲已超前：丢包防无界膨胀
    ff.avcodec_send_packet(acc, packet);
    drain_audio_decoder();
}

void MediaPlayer::Impl::drain_audio_decoder() {
    const FFmpegApi& ff = ffmpeg_api();
    while (ff.avcodec_receive_frame(acc, aframe) == 0) queue_audio_frame();
}

// 音频帧 → S16 PCM → SDL 流（swr 统一采样格式/布局）
void MediaPlayer::Impl::queue_audio_frame() {
    const FFmpegApi& ff = ffmpeg_api();
    const int ns = aframe->nb_samples;
    if (ns <= 0) return;
    std::vector<uint8_t> buf(static_cast<size_t>(ns) * a_channels * 2);
    uint8_t* out[1] = {buf.data()};
    const uint8_t* in[8];
    for (int i = 0; i < 8; ++i) in[i] = aframe->data[i];
    const int got = ff.swr_convert(swr, out, ns, in, ns);
    const int n = std::min(got, ns);
    if (n > 0)
        SDL_PutAudioStreamData(sdl_stream, buf.data(), n * a_channels * 2);
}

// ── 查询接口 ──────────────────────────────────────────────────────────

bool MediaPlayer::is_open() const { return impl_ && impl_->active; }
bool MediaPlayer::has_video() const { return impl_ && impl_->active && impl_->vstream >= 0; }
bool MediaPlayer::has_audio() const { return impl_ && impl_->sdl_stream != nullptr; }
bool MediaPlayer::playing() const { return impl_ && impl_->playing; }
int MediaPlayer::video_w() const { return impl_ ? impl_->vw : 0; }
int MediaPlayer::video_h() const { return impl_ ? impl_->vh : 0; }
int MediaPlayer::video_stride() const { return impl_ ? impl_->vstride : 0; }
double MediaPlayer::video_fps() const { return impl_ ? impl_->fps : 0.0; }

const std::vector<uint8_t>& MediaPlayer::video_rgba() const {
    static const std::vector<uint8_t> kEmpty;
    return impl_ && impl_->vframe_valid ? impl_->vrgba : kEmpty;
}

double MediaPlayer::duration() const { return impl_ ? impl_->duration : 0.0; }

double MediaPlayer::position() const {
    if (!impl_ || !impl_->active) return 0.0;
    const double p = impl_->now_sec();
    return impl_->duration > 0.0 ? std::min(p, impl_->duration) : p;
}

double MediaPlayer::audio_buffered() const { return impl_ ? impl_->audio_buffered_secs() : 0.0; }

const std::string& MediaPlayer::last_error() const {
    static const std::string kEmpty;
    return impl_ ? impl_->last_error : kEmpty;
}

}  // namespace mixbrowser
