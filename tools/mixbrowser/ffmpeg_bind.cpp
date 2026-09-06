// FFmpeg 8.x 运行时绑定实现：SDL_LoadFunction 解析 DLL 导出，免导入库。
// 关键点（实测踩坑）：
//   1. av_packet_alloc/free/unref 在 libavcodec，不在 libavutil；
//   2. swr_alloc_set_opts2 末参为 void* log_ctx（不是 AVDictionary**）；
//   3. 加载顺序必须按依赖（avutil → swresample/swscale → avcodec → avformat）：
//      Windows 加载器按"已载入模块名"满足依赖，后加载的库依赖先加载的。
#include "ffmpeg_bind.h"

#include <cstdio>

namespace mixbrowser {

// 空 API（未用 FFmpeg 时避免加载其 DLL）
const FFmpegApi& no_ffmpeg_api() {
    static FFmpegApi api;
    return api;
}

// 一次性加载：exe 同目录优先，PATH 兜底；任一 DLL 缺失即视为未安装
const FFmpegApi& ffmpeg_api() {
    static FFmpegApi api;
    static bool tried = false;
    if (tried) return api;
    tried = true;
    const char* base = SDL_GetBasePath();
    const std::string dir = base ? base : "";
    auto open = [&](const char* name) -> SDL_SharedObject* {
        if (SDL_SharedObject* m = SDL_LoadObject((dir + name).c_str())) return m;
        return SDL_LoadObject(name);
    };
    auto need = [&](const char* name) -> SDL_SharedObject* {
        SDL_SharedObject* m = open(name);
        if (!m && api.missing.empty()) api.missing = name;
        return m;
    };
    api.avutil = need("avutil-60.dll");
    api.swresample = need("swresample-6.dll");  // 依赖 avutil
    api.swscale = need("swscale-9.dll");        // 依赖 avutil
    api.avcodec = need("avcodec-62.dll");       // 依赖 avutil/swresample
    api.avformat = need("avformat-62.dll");     // 依赖 avcodec/avutil
    if (!api.avutil || !api.avcodec || !api.avformat || !api.swscale || !api.swresample)
        return api;
#define BIND(lib, fn)                                                             \
    api.fn = reinterpret_cast<decltype(api.fn)>(                                   \
        reinterpret_cast<void*>(SDL_LoadFunction(api.lib, #fn)))
    BIND(avutil, av_frame_alloc);
    BIND(avutil, av_frame_free);
    BIND(avutil, av_log_set_level);
    BIND(avcodec, av_packet_alloc);
    BIND(avcodec, av_packet_free);
    BIND(avcodec, av_packet_unref);
    BIND(avcodec, avcodec_alloc_context3);
    BIND(avcodec, avcodec_free_context);
    BIND(avcodec, avcodec_find_decoder);
    BIND(avcodec, avcodec_parameters_to_context);
    BIND(avcodec, avcodec_open2);
    BIND(avcodec, avcodec_send_packet);
    BIND(avcodec, avcodec_receive_frame);
    BIND(avformat, avformat_open_input);
    BIND(avformat, avformat_find_stream_info);
    BIND(avformat, avformat_close_input);
    BIND(avformat, av_find_best_stream);
    BIND(avformat, av_read_frame);
    BIND(swscale, sws_getContext);
    BIND(swscale, sws_scale);
    BIND(swscale, sws_freeContext);
    BIND(swresample, swr_convert);
    BIND(swresample, swr_alloc_set_opts2);
    BIND(swresample, swr_init);
    BIND(swresample, swr_free);
#undef BIND
    // 任一关键符号缺失即视为不可用；记下名字便于诊断
    auto chk = [&](bool ok2, const char* name) {
        if (!ok2 && api.missing.empty()) api.missing = name;
        return ok2;
    };
    api.ok = chk(api.av_frame_alloc != nullptr, "av_frame_alloc") &&
             chk(api.av_packet_unref != nullptr, "av_packet_unref") &&
             chk(api.avcodec_send_packet != nullptr, "avcodec_send_packet") &&
             chk(api.av_read_frame != nullptr, "av_read_frame") &&
             chk(api.sws_scale != nullptr, "sws_scale") &&
             chk(api.swr_convert != nullptr, "swr_convert");
    if (api.ok && api.av_log_set_level) api.av_log_set_level(16);  // AV_LOG_ERROR：静默
    return api;
}

}  // namespace mixbrowser
