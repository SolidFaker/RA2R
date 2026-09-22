// RA2R — 多后端渲染宿主实现（接口见 backend.h）
#include "ra2r/ui/backend.h"

#include <cstdio>
#include <cstring>

#include <SDL3/SDL_opengl.h>

#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

namespace ra2r::ui {

struct BackendHost::Impl {
    BackendKind kind = BackendKind::kOpenGL;
    SDL_Renderer* renderer = nullptr; // SDL 后端
    SDL_GLContext gl = nullptr;       // GL 后端
    SDL_Texture* tex = nullptr;       // SDL 后端画布纹理
    unsigned gl_tex = 0;              // GL 后端画布纹理
    int tex_w = 0, tex_h = 0;
    bool imgui_ready = false;
};

BackendHost::BackendHost() : impl_(std::make_unique<Impl>()) {}

BackendHost::~BackendHost() { shutdown(); }

bool BackendHost::init_window(const char* title, int logical_w, int logical_h, BackendKind kind,
                              bool fallback, std::string* error) {
    const auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    // ── OpenGL（默认）──
    if (kind == BackendKind::kOpenGL) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        window_ = SDL_CreateWindow(title, logical_w, logical_h,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
        if (window_) {
            impl_->gl = SDL_GL_CreateContext(window_);
            if (impl_->gl && SDL_GL_MakeCurrent(window_, impl_->gl)) {
                kind_ = BackendKind::kOpenGL;
                impl_->kind = kind_;
                return true;
            }
            if (impl_->gl) SDL_GL_DestroyContext(impl_->gl);
            impl_->gl = nullptr;
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
        if (!fallback) return fail("OpenGL backend init failed");
        std::fprintf(stderr, "[backend] OpenGL 失败，回退 SDLRenderer\n");
    }
    // ── SDLRenderer（回退/显式选择）──
    window_ = SDL_CreateWindow(title, logical_w, logical_h, SDL_WINDOW_RESIZABLE);
    if (!window_) return fail("SDL_CreateWindow failed");
    impl_->renderer = SDL_CreateRenderer(window_, nullptr);
    if (!impl_->renderer) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return fail("SDL_CreateRenderer failed");
    }
    kind_ = BackendKind::kSDLRenderer;
    impl_->kind = kind_;
    return true;
}

bool BackendHost::init_imgui() {
    if (!window_) return false;
    if (kind_ == BackendKind::kOpenGL) {
        if (!ImGui_ImplSDL3_InitForOpenGL(window_, impl_->gl)) return false;
        if (!ImGui_ImplOpenGL3_Init()) {
            ImGui_ImplSDL3_Shutdown();
            return false;
        }
    } else {
        if (!ImGui_ImplSDL3_InitForSDLRenderer(window_, impl_->renderer)) return false;
        if (!ImGui_ImplSDLRenderer3_Init(impl_->renderer)) {
            ImGui_ImplSDL3_Shutdown();
            return false;
        }
    }
    impl_->imgui_ready = true;
    return true;
}

void BackendHost::shutdown() {
    if (impl_->imgui_ready) {
        if (kind_ == BackendKind::kOpenGL) {
            ImGui_ImplOpenGL3_Shutdown();
        } else {
            ImGui_ImplSDLRenderer3_Shutdown();
        }
        ImGui_ImplSDL3_Shutdown();
        impl_->imgui_ready = false;
    }
    if (kind_ == BackendKind::kOpenGL) {
        if (impl_->gl_tex) {
            SDL_GL_MakeCurrent(window_, impl_->gl);
            glDeleteTextures(1, &impl_->gl_tex);
            impl_->gl_tex = 0;
        }
        if (impl_->gl) {
            SDL_GL_DestroyContext(impl_->gl);
            impl_->gl = nullptr;
        }
    } else {
        if (impl_->tex) {
            SDL_DestroyTexture(impl_->tex);
            impl_->tex = nullptr;
        }
        if (impl_->renderer) {
            SDL_DestroyRenderer(impl_->renderer);
            impl_->renderer = nullptr;
        }
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    impl_->tex_w = impl_->tex_h = 0;
}

const char* BackendHost::kind_name() const {
    return kind_ == BackendKind::kOpenGL ? "OpenGL" : "SDLRenderer";
}

void BackendHost::process_event(const SDL_Event* ev) { ImGui_ImplSDL3_ProcessEvent(ev); }

void BackendHost::new_frame() {
    // 渲染后端 NewFrame（OpenGL 在此懒创建着色器程序）+ SDL3 事件后端 NewFrame
    if (kind_ == BackendKind::kOpenGL)
        ImGui_ImplOpenGL3_NewFrame();
    else
        ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void BackendHost::begin_frame() {
    if (kind_ == BackendKind::kOpenGL) {
        int w = 0, h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.063f, 0.063f, 0.078f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        SDL_SetRenderDrawColor(impl_->renderer, 16, 16, 20, 255);
        SDL_RenderClear(impl_->renderer);
    }
}

void BackendHost::end_frame() {
    ImGui::Render();
    if (kind_ == BackendKind::kOpenGL) {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window_);
    } else {
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), impl_->renderer);
        SDL_RenderPresent(impl_->renderer);
    }
}

void BackendHost::update_texture(int w, int h, const uint8_t* rgba) {
    if (w <= 0 || h <= 0 || !rgba) return;
    if (kind_ == BackendKind::kOpenGL) {
        SDL_GL_MakeCurrent(window_, impl_->gl);
        if (!impl_->gl_tex) glGenTextures(1, &impl_->gl_tex);
        glBindTexture(GL_TEXTURE_2D, impl_->gl_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (w != impl_->tex_w || h != impl_->tex_h) {
            // 内部格式用未定尺寸 GL_RGBA：驱动常以 RGBA8 存储，读回保持逐字节一致
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         rgba);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
        impl_->tex_w = w;
        impl_->tex_h = h;
    } else {
        if (!impl_->tex || w != impl_->tex_w || h != impl_->tex_h) {
            if (impl_->tex) SDL_DestroyTexture(impl_->tex);
            impl_->tex = SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STREAMING, w, h);
            impl_->tex_w = w;
            impl_->tex_h = h;
        }
        if (impl_->tex) SDL_UpdateTexture(impl_->tex, nullptr, rgba, w * 4);
    }
}

void BackendHost::update_texture_rect(int x, int y, int w, int h, int stride, const uint8_t* src) {
    if (w <= 0 || h <= 0 || !src || stride <= 0) return;
    if (kind_ == BackendKind::kOpenGL) {
        if (!impl_->gl_tex || impl_->tex_w <= 0 || impl_->tex_h <= 0) return;
        SDL_GL_MakeCurrent(window_, impl_->gl);
        glBindTexture(GL_TEXTURE_2D, impl_->gl_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                        src + (static_cast<size_t>(y) * stride + x) * 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else {
        if (!impl_->tex) return;
        const SDL_Rect r{x, y, w, h};
        SDL_UpdateTexture(impl_->tex, &r, src + (static_cast<size_t>(y) * stride + x) * 4,
                          stride * 4);
    }
}

ImTextureID BackendHost::texture_id() const {
    if (kind_ == BackendKind::kOpenGL)
        return static_cast<ImTextureID>(static_cast<uint64_t>(impl_->gl_tex));
    return reinterpret_cast<ImTextureID>(impl_->tex);
}

bool BackendHost::read_texture(int w, int h, std::vector<uint8_t>& rgba) {
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    if (kind_ == BackendKind::kOpenGL) {
        SDL_GL_MakeCurrent(window_, impl_->gl);
        glBindTexture(GL_TEXTURE_2D, impl_->gl_tex);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // glGetTexImage 返回纹理存储顺序 = 上传时的行序（自顶向下），无需翻转
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        return true;
    }
    // SDL：经渲染目标纹理中转读回
    if (!impl_->tex) return false;
    SDL_Texture* tmp = SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_TARGET, w, h);
    if (!tmp) return false;
    SDL_SetRenderTarget(impl_->renderer, tmp);
    SDL_RenderTexture(impl_->renderer, impl_->tex, nullptr, nullptr);
    SDL_Surface* surf = SDL_RenderReadPixels(impl_->renderer, nullptr);
    SDL_SetRenderTarget(impl_->renderer, nullptr);
    SDL_DestroyTexture(tmp);
    if (!surf) return false;
    SDL_Surface* conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surf);
    if (!conv) return false;
    std::memcpy(rgba.data(), conv->pixels, rgba.size());
    SDL_DestroySurface(conv);
    return true;
}

bool BackendHost::read_back(int w, int h, std::vector<uint8_t>& rgba) {
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    if (kind_ == BackendKind::kOpenGL) {
        SDL_GL_MakeCurrent(window_, impl_->gl);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        std::vector<uint8_t> tmp(static_cast<size_t>(w) * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
        for (int y = 0; y < h; ++y)
            std::memcpy(rgba.data() + static_cast<size_t>(y) * w * 4,
                        tmp.data() + static_cast<size_t>(h - 1 - y) * w * 4,
                        static_cast<size_t>(w) * 4);
        return true;
    }
    SDL_Surface* surf = SDL_RenderReadPixels(impl_->renderer, nullptr);
    if (!surf) return false;
    SDL_Surface* conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surf);
    if (!conv) return false;
    std::memcpy(rgba.data(), conv->pixels, rgba.size());
    SDL_DestroySurface(conv);
    return true;
}

} // namespace ra2r::ui
