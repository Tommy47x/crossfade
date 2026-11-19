#include "app_internal.h"

#include "ui.h"
#include "tunables.h"
#include "ui_notify.h"

#include <algorithm>
#include <cmath>

// Touchscreen: SDL's synthetic mouse events are disabled, so taps and
// one-finger drags are translated here. A tap becomes a real click across
// two frames; a long press becomes a right-click.
struct TouchGesture
{
    bool active = false;
    SDL_FingerID id = 0;
    SDL_TouchID touch_id = 0;
    float start_x = 0.0f, start_y = 0.0f;
    float last_x = 0.0f, last_y = 0.0f;
    Uint32 start_ticks = 0;
    Uint32 last_move_ticks = 0;
    bool scrolling = false;
    bool long_fired = false;
    bool widget_drag = false; // touch started on a direct-manipulation control
    float velocity = 0.0f; // pixels/second in scroll direction
};

static TouchGesture g_touch;
static float g_touch_fling_velocity = 0.0f;
static bool g_touch_fling_active = false;
static Uint32 g_touch_fling_last_ms = 0;

// Wheel/touchpad scrolling — browser model. A virtual scroll target
// integrates wheel deltas 1:1 (plus post-release kinetic fling); the rendered
// position chases the target with a critically-damped filter every frame, so
// unevenly-spaced touchpad packets become continuous sub-pixel motion and
// even tiny flicks glide. Children report edge clamping back via
// scroll_clamp_feedback() so the target stays in the reachable range.
static float g_scroll_target = 0.0f;        // virtual scroll position, px
static float g_scroll_pos = 0.0f;           // chased render position, px
static float g_wheel_velocity = 0.0f;      // active fling speed, px/s
static float g_wheel_input_velocity = 0.0f; // recent event speed, px/s
static bool g_wheel_inertia_active = false;
static Uint32 g_wheel_last_event_ms = 0;
static Uint32 g_wheel_last_frame_ms = 0;
static int g_wheel_event_count = 0;

// All of these live in Settings -> Tunables ("Scrolling"); defaults are the
// values this code shipped with.
static float wheel_pixels_per_delta() { return tunable("scroll.pixels_per_delta"); }
static float wheel_chase_tau() { return std::max(0.005f, tunable("scroll.chase_tau")); }
static float wheel_fling_decay() { return tunable("scroll.fling_decay"); }
static float wheel_fling_min() { return tunable("scroll.fling_min"); }
static float wheel_fling_max() { return tunable("scroll.fling_max"); }
static Uint32 wheel_release_ms() { return (Uint32)tunable_int("scroll.release_ms"); }
static Uint32 wheel_gesture_gap_ms() { return (Uint32)tunable_int("scroll.gesture_gap_ms"); }

// Horizontal two-finger swipe (back/forward navigation). See the
// SDL_MOUSEWHEEL handler for the threshold semantics.
static float g_hwheel_accum = 0.0f;
static Uint32 g_hwheel_last_ms = 0;
static float hwheel_trigger() { return tunable("scroll.hwheel_trigger"); } // deltas per step
static Uint32 hwheel_gap_ms() { return (Uint32)tunable_int("scroll.hwheel_gap_ms"); }

void reset_wheel_scroll()
{
    g_scroll_target = 0.0f;
    g_scroll_pos = 0.0f;
    g_wheel_velocity = 0.0f;
    g_wheel_input_velocity = 0.0f;
    g_wheel_inertia_active = false;
    g_wheel_last_event_ms = 0;
    g_wheel_event_count = 0;
}

enum class TouchClickPhase
{
    None,
    Down,
    Up
};

static TouchClickPhase g_touch_click_phase = TouchClickPhase::None;
static int g_touch_click_button = 0;
static ImVec2 g_touch_click_pos;
// ImGui::GetTime() of the last tap we synthesized. Track rows treat a recent
// tap as the "play" gesture (the double-click idiom has no touch analogue);
// see ui_touch_tap_recent().
double g_touch_click_time = -1.0e6;

// Ends a direct-manipulation drag: releases the synthetic mouse button once
// and clears the gesture. Safe to call when no drag is active.
static void end_widget_drag()
{
    if (!g_touch.widget_drag)
        return;
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(g_touch.last_x, g_touch.last_y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    g_touch.widget_drag = false;
    g_touch.active = false;
}

static void reset_touch_gesture()
{
    if (g_touch.widget_drag)
        end_widget_drag();
    g_touch.active = false;
    g_touch.widget_drag = false;
}
static void queue_touch_click(float x, float y, int button)
{
    if (g_touch_click_phase != TouchClickPhase::None)
        return;
    g_touch_click_time = ImGui::GetTime();
    g_touch_click_phase = TouchClickPhase::Down;
    g_touch_click_button = button;
    g_touch_click_pos = ImVec2(x, y);
}

// Feed one half of a synthetic click per frame. Sending down and up in the
// same NewFrame would leave ImGui with no pressed transition, so the down
// lands now and the up lands on the next loop iteration.
void process_touch_click()
{
    ImGuiIO &io = ImGui::GetIO();
    if (g_touch_click_phase == TouchClickPhase::Down)
    {
        io.AddMousePosEvent(g_touch_click_pos.x, g_touch_click_pos.y);
        io.AddMouseButtonEvent(g_touch_click_button, true);
        g_touch_click_phase = TouchClickPhase::Up;
    }
    else if (g_touch_click_phase == TouchClickPhase::Up)
    {
        io.AddMouseButtonEvent(g_touch_click_button, false);
        g_touch_click_phase = TouchClickPhase::None;
    }
}

void handle_touch_event(const SDL_Event &event, SDL_Window *window)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    if (ww <= 0 || wh <= 0)
        return;
    auto px = [&](float nx) { return nx * (float)ww; };
    auto py = [&](float ny) { return ny * (float)wh; };

    if (event.type == SDL_FINGERDOWN)
    {
        if (g_touch.active)
        {
            // A FINGERUP that never arrived (driver quirk, finger lifted off
            // the window edge) must not poison every later gesture. Drop the
            // stale one and release its synthetic button.
            DLOG("touch: replacing stale gesture (finger %lld -> %lld)",
                 (long long)g_touch.id, (long long)event.tfinger.fingerId);
            reset_touch_gesture();
        }
        g_touch.active = true;
        g_touch.id = event.tfinger.fingerId;
        g_touch.touch_id = event.tfinger.touchId;
        g_touch.start_x = g_touch.last_x = px(event.tfinger.x);
        g_touch.start_y = g_touch.last_y = py(event.tfinger.y);
        g_touch.start_ticks = SDL_GetTicks();
        g_touch.last_move_ticks = g_touch.start_ticks;
        g_touch.scrolling = false;
        g_touch.long_fired = false;
        g_touch.velocity = 0.0f;
        g_touch_fling_active = false;
        g_touch_fling_velocity = 0.0f;
        reset_wheel_scroll();
        // A slider (volume) opts into direct manipulation: press and hold with
        // a real mouse-button-down so the widget tracks the finger. Without
        // this a one-finger drag was always classified as a scroll and the
        // slider only ever reacted to the tap synthesised on finger-up.
        g_touch.widget_drag = ui_touch_drag_zone_contains(g_touch.start_x, g_touch.start_y);
        ImGuiIO &io = ImGui::GetIO();
        io.AddMousePosEvent(g_touch.last_x, g_touch.last_y);
        if (g_touch.widget_drag)
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        return;
    }

    if (!g_touch.active)
        return;
    // Motion must belong to the tracked finger. A release from the same touch
    // device still ends the gesture when the ids disagree: some drivers rewrite
    // finger ids, and a missed up is what latched the slider after one drag.
    const bool same_finger = event.tfinger.fingerId == g_touch.id;
    const bool same_touch =
        g_touch.touch_id != 0 && event.tfinger.touchId == g_touch.touch_id;
    if (event.type != SDL_FINGERUP && !same_finger)
        return;
    if (event.type == SDL_FINGERUP && !same_finger && !same_touch)
        return;

    if (event.type == SDL_FINGERMOTION)
    {
        const float x = px(event.tfinger.x);
        const float y = py(event.tfinger.y);
        if (g_touch.widget_drag)
        {
            // Feed the drag straight to ImGui; no scrolling, no fling.
            g_touch.last_move_ticks = SDL_GetTicks();
            g_touch.last_x = x;
            g_touch.last_y = y;
            ImGui::GetIO().AddMousePosEvent(x, y);
            return;
        }
        const float dx = x - g_touch.start_x;
        const float dy = y - g_touch.start_y;
        const float tap_slop = tunable("scroll.touch_tap_slop_px");
        if (!g_touch.scrolling && dx * dx + dy * dy > tap_slop * tap_slop)
            g_touch.scrolling = true;
        const Uint32 now_ms = SDL_GetTicks();
        const float pixel_dy = y - g_touch.last_y;
        if (g_touch.scrolling)
        {
            const float dt = (now_ms - g_touch.last_move_ticks) / 1000.0f;
            if (dt > 0.001f && dt < 0.2f)
            {
                const float instant = -pixel_dy / dt;
                // Low-pass the flick velocity so one jittery sample doesn't
                // dominate the momentum after the finger lifts.
                g_touch.velocity = g_touch.velocity * 0.65f + instant * 0.35f;
            }
            else if (dt >= 0.2f)
            {
                g_touch.velocity = -pixel_dy / std::max(dt, 0.001f);
            }
            // Direct pixel scroll: content follows the finger exactly.
            add_scroll_pixels(-pixel_dy);
        }
        g_touch.last_move_ticks = now_ms;
        g_touch.last_x = x;
        g_touch.last_y = y;
        ImGui::GetIO().AddMousePosEvent(x, y);
        return;
    }

    if (event.type == SDL_FINGERUP)
    {
        const float x = px(event.tfinger.x);
        const float y = py(event.tfinger.y);
        if (g_touch.widget_drag)
        {
            g_touch.last_x = x;
            g_touch.last_y = y;
            end_widget_drag();
            return;
        }
        const bool tap = !g_touch.scrolling && !g_touch.long_fired;
        const Uint32 now_ms = SDL_GetTicks();
        float velocity = g_touch.velocity;
        // Holding still before lifting should not fling.
        if (now_ms - g_touch.last_move_ticks > (Uint32)tunable_int("scroll.touch_fling_stop_ms"))
            velocity = 0.0f;
        const float touch_fling_max = tunable("scroll.touch_fling_max");
        velocity = std::max(-touch_fling_max, std::min(velocity, touch_fling_max));
        g_touch.active = false;
        if (g_touch.scrolling && !g_touch.long_fired && std::fabs(velocity) >= 80.0f)
        {
            g_touch_fling_velocity = velocity;
            g_touch_fling_active = true;
            g_touch_fling_last_ms = now_ms;
        }
        if (tap)
            queue_touch_click(x, y, ImGuiMouseButton_Left);
        return;
    }
}

void update_touch_long_press()
{
    if (g_touch.widget_drag)
    {
        // Reconcile with ImGui once the press has been through a frame. If
        // ImGui no longer holds the button, the gesture is over even when the
        // FINGERUP never reached us; leaving it latched made every later touch
        // a duplicate press that ImGui filtered out.
        if (!ImGui::GetIO().MouseDown[ImGuiMouseButton_Left] &&
            SDL_GetTicks() - g_touch.start_ticks > 250)
            reset_touch_gesture();
        return;
    }
    if (!g_touch.active || g_touch.scrolling || g_touch.long_fired)
        return;
    if (SDL_GetTicks() - g_touch.start_ticks < 550)
        return;
    g_touch.long_fired = true;
    queue_touch_click(g_touch.last_x, g_touch.last_y, ImGuiMouseButton_Right);
}

// Kinetic touch scrolling: keep moving after the finger lifts, with an
// exponential decay similar to a browser's fling.
bool update_touch_inertia()
{
    if (!g_touch_fling_active)
        return false;
    if (g_touch.active)
    {
        g_touch_fling_active = false;
        g_touch_fling_velocity = 0.0f;
        return false;
    }
    const Uint32 now_ms = SDL_GetTicks();
    float dt = (now_ms - g_touch_fling_last_ms) / 1000.0f;
    g_touch_fling_last_ms = now_ms;
    if (dt <= 0.0f)
        return true;
    if (dt > 0.05f)
        dt = 0.05f;
    add_scroll_pixels(g_touch_fling_velocity * dt);
    g_touch_fling_velocity *= std::exp(-tunable("scroll.touch_inertia_decay") * dt);
    if (std::fabs(g_touch_fling_velocity) < 20.0f)
    {
        g_touch_fling_velocity = 0.0f;
        g_touch_fling_active = false;
        return false;
    }
    return true;
}

// Wheel/touchpad scrolling. Input is drained with a short time constant while
// events are arriving; kinetic inertia is only started after the pad goes
// quiet, using the speed measured from the events themselves.
bool update_wheel_inertia()
{
    const Uint32 now_ms = SDL_GetTicks();
    const bool input_active = (now_ms - g_wheel_last_event_ms) <= wheel_release_ms();
    if (!input_active && !g_wheel_inertia_active &&
        std::fabs(g_scroll_target - g_scroll_pos) < 0.05f)
        return false;

    float dt = (now_ms - g_wheel_last_frame_ms) / 1000.0f;
    g_wheel_last_frame_ms = now_ms;
    if (dt <= 0.0f)
        return true;
    if (dt > 0.05f)
        dt = 0.05f;

    // Edge feedback from the last frame: the hovered child clamped the
    // scroll, so pull the target back into the reachable range and stop any
    // fling pushing into the edge.
    const float unapplied = take_scroll_clamp_feedback();
    if (unapplied != 0.0f)
    {
        g_scroll_target += unapplied;
        if (g_wheel_inertia_active)
        {
            g_wheel_velocity = 0.0f;
            g_wheel_inertia_active = false;
        }
    }

    if (input_active)
    {
        g_wheel_inertia_active = false;
        g_wheel_velocity = 0.0f;
    }
    else if (!g_wheel_inertia_active)
    {
        const float fling = std::max(-wheel_fling_max(),
                                     std::min(g_wheel_input_velocity, wheel_fling_max()));
        if (std::fabs(fling) >= wheel_fling_min())
        {
            g_wheel_velocity = fling;
            g_wheel_inertia_active = true;
        }
        g_wheel_input_velocity = 0.0f;
        g_wheel_event_count = 0;
    }

    if (g_wheel_inertia_active)
    {
        g_scroll_target += g_wheel_velocity * dt;
        g_wheel_velocity *= std::exp(-wheel_fling_decay() * dt);
        if (std::fabs(g_wheel_velocity) < wheel_fling_min())
        {
            g_wheel_velocity = 0.0f;
            g_wheel_inertia_active = false;
        }
    }

    // Critically-damped chase: the rendered position follows the target,
    // turning discrete wheel packets into continuous sub-pixel motion.
    const float chase = 1.0f - std::exp(-dt / wheel_chase_tau());
    const float new_pos = g_scroll_pos + (g_scroll_target - g_scroll_pos) * chase;
    add_scroll_pixels(new_pos - g_scroll_pos);
    g_scroll_pos = new_pos;

    if (!input_active && !g_wheel_inertia_active &&
        std::fabs(g_scroll_target - g_scroll_pos) < 0.05f)
    {
        reset_wheel_scroll();
        return false;
    }
    return true;
}


static void stop_touch_fling()
{
    g_touch_fling_active = false;
    g_touch_fling_velocity = 0.0f;
}

void reset_pointer_gestures()
{
    reset_wheel_scroll();
    g_hwheel_accum = 0.0f;
    stop_touch_fling();
}

void handle_wheel_event(const SDL_Event &event)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    const float delta = event.wheel.preciseY;
#else
    const float delta = (float)event.wheel.y;
#endif
    if (delta != 0.0f)
    {
        // Wheel-up is negative scroll. Deltas integrate into the virtual
        // target 1:1; the chase filter in update_wheel_inertia() renders
        // them continuously.
        g_touch_fling_active = false;
        g_touch_fling_velocity = 0.0f;

        const Uint32 now_ms = SDL_GetTicks();
        const Uint32 gap_ms = now_ms - g_wheel_last_event_ms;
        const float pixels = -delta * wheel_pixels_per_delta();

        if (gap_ms <= wheel_gesture_gap_ms())
        {
            if (gap_ms > 0)
            {
                const float instant = pixels / std::max(gap_ms / 1000.0f, 0.001f);
                g_wheel_input_velocity =
                    g_wheel_input_velocity * 0.65f + instant * 0.35f;
            }
            ++g_wheel_event_count;
        }
        else
        {
            g_wheel_input_velocity = 0.0f;
            g_wheel_last_frame_ms = now_ms;
            g_wheel_event_count = 1;
        }

        g_scroll_target += pixels;
        g_wheel_last_event_ms = now_ms;
        g_wheel_inertia_active = false;
        g_wheel_velocity = 0.0f;
    }

    // Horizontal touchpad swipe: two-finger navigation, like a browser.
    // Deltas accumulate during one swipe (events arriving closer than
    // hwheel_gap_ms()); crossing the threshold fires once and re-arms, so a long
    // swipe can navigate several steps. A rightward finger swipe sends
    // wheel.x > 0 (Forward); wheel.x < 0 is Back.
#if SDL_VERSION_ATLEAST(2, 0, 18)
    const float xdelta = event.wheel.preciseX;
#else
    const float xdelta = (float)event.wheel.x;
#endif
    if (xdelta == 0.0f)
        return;

    // A toast under the cursor owns the horizontal gesture: it tracks the
    // fingers (swipe-to-dismiss) instead of triggering navigation.
    if (ui_notify_swipe_feed(xdelta * wheel_pixels_per_delta()))
    {
        g_state.repaint_requested = true;
        return;
    }

    const Uint32 now_ms = SDL_GetTicks();
    if (now_ms - g_hwheel_last_ms > hwheel_gap_ms() ||
        (g_hwheel_accum != 0.0f && g_hwheel_accum * xdelta < 0.0f))
    {
        // New swipe: enough time passed since the last event, or the
        // direction flipped.
        g_hwheel_accum = 0.0f;
    }
    g_hwheel_accum += xdelta;
    g_hwheel_last_ms = now_ms;
    while (std::abs(g_hwheel_accum) >= hwheel_trigger())
    {
        const bool back = g_hwheel_accum < 0.0f;
        g_hwheel_accum -= back ? -hwheel_trigger() : hwheel_trigger();
        if (back)
            ui_go_back(g_state);
        else
            ui_go_forward(g_state);
        g_state.repaint_requested = true;
    }
}
