#pragma once
// FFmpeg 8.x 运行时绑定（SDL_LoadFunction，免导入库）。
// vendored 头为 8.x（avutil-60/avcodec-62/avformat-62/swscale-9/swresample-6），
// 结构体布局与 DLL 必须同主版本，故只绑定这一套，避免混版本 ABI 崩溃。
#include <string>

#include <SDL3/SDL.h>

extern "C" {
#include "ffmpeg/include/libavformat/avformat.h"
#include "ffmpeg/include/libavcodec/avcodec.h"
#include "ffmpeg/include/libswscale/swscale.h"
#include "ffmpeg/include/libswresample/swresample.h"
}

namespace mixbrowser {

struct FFmpegApi {
    bool ok = false;
    std::string missing;  // 第一个加载/绑定失败的名字（诊断用）
    SDL_SharedObject* avutil = nullptr;
    SDL_SharedObject* avcodec = nullptr;
    SDL_SharedObject* avformat = nullptr;
    SDL_SharedObject* swscale = nullptr;
    SDL_SharedObject* swresample = nullptr;
    // avutil
    AVFrame* (*av_frame_alloc)();
    void (*av_frame_free)(AVFrame**);
    void (*av_log_set_level)(int);
    // avcodec（av_packet_* 在 libavcodec，勿误绑到 avutil）
    AVPacket* (*av_packet_alloc)();
    void (*av_packet_free)(AVPacket**);
    void (*av_packet_unref)(AVPacket*);
    AVCodecContext* (*avcodec_alloc_context3)(const AVCodec*);
    void (*avcodec_free_context)(AVCodecContext**);
    const AVCodec* (*avcodec_find_decoder)(enum AVCodecID);
    int (*avcodec_parameters_to_context)(AVCodecContext*, const AVCodecParameters*);
    int (*avcodec_open2)(AVCodecContext*, const AVCodec*, AVDictionary**);
    int (*avcodec_send_packet)(AVCodecContext*, const AVPacket*);
    int (*avcodec_receive_frame)(AVCodecContext*, AVFrame*);
    // avformat
    int (*avformat_open_input)(AVFormatContext**, const char*, const AVInputFormat*,
                               AVDictionary**);
    int (*avformat_find_stream_info)(AVFormatContext*, AVDictionary**);
    void (*avformat_close_input)(AVFormatContext**);
    int (*av_find_best_stream)(AVFormatContext*, enum AVMediaType, int, int, const AVCodec**,
                               int);
    int (*av_read_frame)(AVFormatContext*, AVPacket*);
    // swscale
    SwsContext* (*sws_getContext)(int, int, enum AVPixelFormat, int, int, enum AVPixelFormat,
                                  int, SwsFilter*, SwsFilter*, const double*);
    int (*sws_scale)(SwsContext*, const uint8_t* const*, const int*, int, int,
                     uint8_t* const*, const int*);
    void (*sws_freeContext)(SwsContext*);
    // swresample（swr_alloc_set_opts2 末参是 void* log_ctx）
    int (*swr_convert)(SwrContext*, uint8_t**, int, const uint8_t**, int);
    int (*swr_alloc_set_opts2)(SwrContext**, const AVChannelLayout*, enum AVSampleFormat, int,
                               const AVChannelLayout*, enum AVSampleFormat, int, int, void*);
    int (*swr_init)(SwrContext*);
    void (*swr_free)(SwrContext**);
};

// 一次性加载绑定（exe 同目录优先，PATH 兜底）；失败时 ok=false
const FFmpegApi& ffmpeg_api();
// 空 API（未用 FFmpeg 时避免加载其 DLL）
const FFmpegApi& no_ffmpeg_api();

}  // namespace mixbrowser
