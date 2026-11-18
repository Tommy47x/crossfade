#include "video_surface.h"

#include "mpvplayer.h"

#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <mpv/render_gl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

// The system GL 1.1 headers don't declare the framebuffer-object entry points
// (and Windows' GL/gl.h stops at 1.1 entirely). Resolve the handful we need
// through SDL, exactly like the backdrop shader pass does.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

namespace
{
typedef void(APIENTRY *PFN_GenFramebuffers)(GLsizei, GLuint *);
typedef void(APIENTRY *PFN_BindFramebuffer)(GLenum, GLuint);
typedef void(APIENTRY *PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY *PFN_CheckFramebufferStatus)(GLenum);
typedef void(APIENTRY *PFN_DeleteFramebuffers)(GLsizei, const GLuint *);

PFN_GenFramebuffers p_glGenFramebuffers = nullptr;
PFN_BindFramebuffer p_glBindFramebuffer = nullptr;
PFN_FramebufferTexture2D p_glFramebufferTexture2D = nullptr;
PFN_CheckFramebufferStatus p_glCheckFramebufferStatus = nullptr;
PFN_DeleteFramebuffers p_glDeleteFramebuffers = nullptr;

template <typename T>
bool load_gl(T &fn, const char *name)
{
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    return fn != nullptr;
}

bool load_gl_functions()
{
    static const bool ok = load_gl(p_glGenFramebuffers, "glGenFramebuffers") &&
                           load_gl(p_glBindFramebuffer, "glBindFramebuffer") &&
                           load_gl(p_glFramebufferTexture2D, "glFramebufferTexture2D") &&
                           load_gl(p_glCheckFramebufferStatus, "glCheckFramebufferStatus") &&
                           load_gl(p_glDeleteFramebuffers, "glDeleteFramebuffers");
    return ok;
}

void *gl_get_proc_address(void *, const char *name)
{
    return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name));
}
} // namespace

struct VideoSurface::Impl
{
    mpv_render_context *ctx = nullptr;
    mpv_handle *mpv = nullptr;

    GLuint tex = 0;
    GLuint fbo = 0;
    int fbo_w = 0;
    int fbo_h = 0;
    int target_w = 0;
    int target_h = 0;
    float aspect = 16.0f / 9.0f;

    std::atomic<bool> has_frame{false};
    std::atomic<unsigned> frame_count{0};
    std::function<void()> wake;
    std::chrono::steady_clock::time_point aspect_checked{};

    ~Impl()
    {
        // GL resources must be released while the context is current; the
        // public detach() is called from the main thread for that reason.
        // Reaching here with a live context means the process is exiting.
        if (ctx)
        {
            mpv_render_context_set_update_callback(ctx, nullptr, nullptr);
            mpv_render_context_free(ctx);
            ctx = nullptr;
        }
    }

    void release_gl()
    {
        if (fbo && p_glDeleteFramebuffers)
        {
            p_glDeleteFramebuffers(1, &fbo);
            fbo = 0;
        }
        if (tex)
        {
            glDeleteTextures(1, &tex);
            tex = 0;
        }
        fbo_w = fbo_h = 0;
        has_frame.store(false, std::memory_order_relaxed);
    }

    bool ensure_fbo(int w, int h)
    {
        if (w == fbo_w && h == fbo_h && fbo != 0 && tex != 0)
            return true;
        release_gl();
        if (!load_gl_functions())
            return false;

        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        p_glGenFramebuffers(1, &fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        const GLenum status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            fprintf(stderr, "video: video FBO incomplete (0x%x)\n", (unsigned)status);
            release_gl();
            return false;
        }
        fbo_w = w;
        fbo_h = h;
        return true;
    }

    void refresh_aspect()
    {
        if (!mpv)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now - aspect_checked < std::chrono::milliseconds(500))
            return;
        aspect_checked = now;
        double a = 0.0;
        if (mpv_get_property(mpv, "video-params/aspect", MPV_FORMAT_DOUBLE, &a) >= 0 &&
            a > 0.05 && a < 20.0)
            aspect = (float)a;
    }
};

VideoSurface::VideoSurface() : impl_(std::make_unique<Impl>()) {}

VideoSurface::~VideoSurface() = default;

bool VideoSurface::attach(MpvPlayer &player)
{
    detach();
    if (!player.handle())
        return false;
    if (!load_gl_functions())
    {
        fprintf(stderr, "video: framebuffer GL entry points unavailable\n");
        return false;
    }

    mpv_opengl_init_params gl_init{&gl_get_proc_address, nullptr};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context *ctx = nullptr;
    const int rc = mpv_render_context_create(&ctx, player.handle(), params);
    if (rc < 0 || !ctx)
    {
        fprintf(stderr, "video: mpv_render_context_create failed: %s\n", mpv_error_string(rc));
        return false;
    }
    impl_->ctx = ctx;
    impl_->mpv = player.handle();
    impl_->aspect = 16.0f / 9.0f;
    impl_->aspect_checked = {};
    impl_->has_frame.store(false, std::memory_order_relaxed);

    // mpv asks for a new frame from its own render thread. Posting an SDL
    // user event is thread-safe and wakes SDL_WaitEventTimeout() so the frame
    // is presented immediately instead of on the next idle tick.
    mpv_render_context_set_update_callback(
        ctx,
        [](void *opaque)
        {
            auto *self = static_cast<Impl *>(opaque);
            if (self->wake)
                self->wake();
        },
        impl_.get());
    return true;
}

void VideoSurface::detach()
{
    if (!impl_)
        return;
    if (impl_->ctx)
    {
        mpv_render_context_set_update_callback(impl_->ctx, nullptr, nullptr);
        mpv_render_context_free(impl_->ctx);
        impl_->ctx = nullptr;
    }
    impl_->release_gl();
    impl_->mpv = nullptr;
    impl_->target_w = impl_->target_h = 0;
    impl_->aspect = 16.0f / 9.0f;
}

bool VideoSurface::attached() const { return impl_ && impl_->ctx != nullptr; }

bool VideoSurface::attached_to(const MpvPlayer &player) const
{
    return impl_ && impl_->ctx && impl_->mpv == player.handle();
}

void VideoSurface::set_wake_callback(std::function<void()> cb) { impl_->wake = std::move(cb); }

void VideoSurface::set_target_size(int w, int h)
{
    impl_->target_w = w > 0 ? w : 0;
    impl_->target_h = h > 0 ? h : 0;
}

bool VideoSurface::render()
{
    Impl &d = *impl_;
    if (!d.ctx || d.target_w <= 0 || d.target_h <= 0)
        return false;

    const unsigned update = mpv_render_context_update(d.ctx);
    if (!(update & MPV_RENDER_UPDATE_FRAME))
        return false;

    // Only (re)create the target when a frame is actually about to be drawn.
    // Resizing eagerly reset has_frame and made the page flash "unavailable"
    // whenever the FBO size changed before the next decoded frame arrived.
    if (!d.ensure_fbo(d.target_w, d.target_h))
        return false;

    mpv_opengl_fbo fbo{(int)d.fbo, d.target_w, d.target_h, 0};
    // No Y flip: mpv renders into the FBO bottom-up, and ImGui samples V=0 at
    // the top of the quad, which cancelled out the wrong way with FLIP_Y on
    // (the picture came out upside down). Leave mpv's native orientation.
    int flip_y = 0;
    // Do not block until the frame's target presentation time: the default
    // paces render() to the video's frame rate (up to ~33 ms per call at
    // 30 fps), stalling the entire UI thread every frame. The UI's own vsync
    // paces presentation instead.
    int block_for_target_time = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    p_glBindFramebuffer(GL_FRAMEBUFFER, d.fbo);
    glViewport(0, 0, d.target_w, d.target_h);
    mpv_render_context_render(d.ctx, params);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    mpv_render_context_report_swap(d.ctx);

    d.has_frame.store(true, std::memory_order_relaxed);
    d.frame_count.fetch_add(1, std::memory_order_relaxed);
    d.refresh_aspect();
    return true;
}

ImTextureID VideoSurface::texture() const
{
    return impl_ && impl_->tex ? (ImTextureID)(intptr_t)impl_->tex : (ImTextureID)0;
}

bool VideoSurface::has_frame() const { return impl_->has_frame.load(std::memory_order_relaxed); }

unsigned VideoSurface::frame_count() const
{
    return impl_->frame_count.load(std::memory_order_relaxed);
}

float VideoSurface::aspect() const { return impl_->aspect; }

VideoSurface &video_surface()
{
    static VideoSurface surface;
    return surface;
}

void video_render_target_size(float aspect, int &w, int &h)
{
    aspect = std::clamp(aspect, 0.3f, 3.5f);
    // 720p is the sweet spot: the default video stream is 720p, and the
    // software decode + GL scaling path measured ~35 ms/frame at 1080p,
    // which capped the reveal animation at ~25 fps. At 720p it fits a
    // 60 fps budget.
    int height = 720;
    int width = (int)std::lround(height * aspect);
    if (width > 1280)
    {
        width = 1280;
        height = (int)std::lround(width / aspect);
    }
    w = std::max(2, width);
    h = std::max(2, height);
}
