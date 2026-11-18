#pragma once

#include <imgui.h>

#include <functional>
#include <memory>

class MpvPlayer;

// Renders video from an MpvPlayer into an offscreen GL texture that the UI
// draws with ImGui::Image (the main content area's video page).
//
// libmpv's render API is used instead of `--wid` embedding: the app already
// has a single SDL/OpenGL context and a fully immediate-mode UI, so a native
// child window would sit above the ImGui draw list and fight it for z-order.
//
// Usage per frame:
//   * the video page computes the on-screen rect (physical pixels) and calls
//     set_target_size() while building the UI;
//   * render() runs once, after the UI has been laid out and before
//     ImGui::Render(), and decodes/uploads a new frame if one is due;
//   * texture() is sampled by the ImGui draw list during RenderDrawData.
class VideoSurface
{
public:
    VideoSurface();
    ~VideoSurface();
    VideoSurface(const VideoSurface &) = delete;
    VideoSurface &operator=(const VideoSurface &) = delete;

    // Create the mpv render context for `player` (main thread, GL context
    // current). Replaces any previous attachment. Returns false when libmpv
    // or the GL driver refuses the render API.
    bool attach(MpvPlayer &player);
    // Tear down the context and its GL objects. Safe when not attached.
    void detach();
    bool attached() const;
    // True when the context is bound to this exact player handle.
    bool attached_to(const MpvPlayer &player) const;

    // Wake the event loop when mpv has a new frame ready. `cb` runs on mpv's
    // render thread, so it may only touch thread-safe state (SDL_PushEvent is
    // fine). Set it once at startup, before attach().
    void set_wake_callback(std::function<void()> cb);

    // Size of the video image in physical pixels. 0x0 skips rendering.
    void set_target_size(int w, int h);

    // Render a new frame into the texture when mpv reports one. Returns true
    // when the texture changed.
    bool render();

    ImTextureID texture() const;
    bool has_frame() const;
    // Number of frames delivered to the texture (diagnostics).
    unsigned frame_count() const;
    // Reported video aspect (width / height); 16:9 until mpv knows better.
    float aspect() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

VideoSurface &video_surface();

// Stable pixel size for the video FBO at the given aspect ratio (longest side
// up to ~1080p). A fixed size avoids re-allocating the texture when the page
// animates in, which used to cause a dropped-frame stutter at the pop-up.
void video_render_target_size(float aspect, int &w, int &h);
