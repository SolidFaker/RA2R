#pragma once
// RA2R — 多后端渲染宿主（架构升级：呈现后端统一抽象）
//
// 画布渲染管线不变（CPU 侧 RGBA 整幅），宿主负责：窗口/呈现上下文/
// ImGui 后端绑定、画布纹理上传、帧循环（清屏/交换）、帧读回。
//
// 后端（--backend 选择，默认 OpenGL）：
//   kOpenGL       SDL_GL 上下文 + imgui_impl_opengl3，画布经 GL 纹理呈现
//   kSDLRenderer  SDL_Renderer + imgui_impl_sdlrenderer3（旧路径保留）
// OpenGL 初始化失败自动回退 SDLRenderer（fallback=true 时）。
//
// 用法约定：
//   host.init_window(title, w, h, kind, true, &err);  // 建窗 + 上下文
//   float s = ra2r::ui::scale_window_to_dpi(host.window(), w, h);
//   ImGui::CreateContext(); ... 字体/样式 ...
//   host.init_imgui();                                 // 按后端绑 ImGui
//   循环: while(host.process_event(&ev)) { host.new_frame(); host.begin_frame();
//         ...ImGui UI...; host.end_frame(); }
//   host.shutdown();
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "imgui.h"

namespace ra2r::ui {

enum class BackendKind { kOpenGL, kSDLRenderer };

class BackendHost {
public:
    BackendHost();
    ~BackendHost();
    BackendHost(const BackendHost&) = delete;
    BackendHost& operator=(const BackendHost&) = delete;

    // 创建窗口与呈现上下文（不含 ImGui；ImGui 上下文须在 init_imgui 前就绪）。
    // kind=kOpenGL 失败且 fallback=true 时回退 SDLRenderer。
    bool init_window(const char* title, int logical_w, int logical_h, BackendKind kind,
                     bool fallback, std::string* error);
    // 按当前后端绑定 ImGui（CreateContext 之后调用一次）
    bool init_imgui();
    void shutdown();

    BackendKind kind() const { return kind_; }
    const char* kind_name() const;
    SDL_Window* window() const { return window_; }

    // 事件/帧循环：process_event 转发给 ImGui SDL3 后端；
    // new_frame = ImGui_ImplSDL3_NewFrame + ImGui::NewFrame；
    // begin_frame 清屏；end_frame = ImGui::Render + 绘制 + 交换。
    void process_event(const SDL_Event* ev);
    void new_frame();
    void begin_frame();
    void end_frame();

    // 画布纹理：整幅 RGBA（w×h×4，行主序自顶向下）；尺寸变化自动重建
    void update_texture(int w, int h, const uint8_t* rgba);
    // 当前画布纹理句柄（ImGui::Image 用）
    ImTextureID texture_id() const;

    // 读回画布纹理内容（自顶向下 RGBA；无头自检/截图用）
    bool read_texture(int w, int h, std::vector<uint8_t>& rgba);
    // 读回当前呈现帧（窗口截图；自顶向下 RGBA；须在 end_frame 前调用）
    bool read_back(int w, int h, std::vector<uint8_t>& rgba);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    BackendKind kind_ = BackendKind::kOpenGL;
    SDL_Window* window_ = nullptr;
};

} // namespace ra2r::ui
