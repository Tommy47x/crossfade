#include "tunables.h"
#include "paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

namespace
{

#define TUNE(key, group, label, desc, def, lo, hi)                                 \
    {key, group, label, desc, def, lo, hi, TunableKind::Float, false}
#define TUNE_I(key, group, label, desc, def, lo, hi)                               \
    {key, group, label, desc, def, lo, hi, TunableKind::Int, false}
#define TUNE_B(key, group, label, desc, def)                                       \
    {key, group, label, desc, def, 0.0f, 1.0f, TunableKind::Bool, false}
#define TUNE_R(key, group, label, desc, def, lo, hi)                               \
    {key, group, label, desc, def, lo, hi, TunableKind::Float, true}
#define TUNE_RI(key, group, label, desc, def, lo, hi)                              \
    {key, group, label, desc, def, lo, hi, TunableKind::Int, true}

const std::vector<TunableDef> &build_defs()
{
    static const std::vector<TunableDef> defs = {
        // ---- Playback -------------------------------------------------
        TUNE("playback.crossfade_cap_fraction", "Playback", "Crossfade cap",
             "Natural crossfade never takes more than this fraction of a track.",
             0.5f, 0.1f, 1.0f),
        TUNE_I("playback.retry_max_doublings", "Playback", "Play retries",
               "Retry backoff steps after a failed start (2 s, 4 s, ...).",
               4, 0, 8),
        TUNE_I("playback.resume_ramp_ms", "Playback", "Resume ramp",
               "Volume ramp step after a pause, milliseconds.", 50, 10, 500),
        TUNE("playback.sleep_fade_sec", "Playback", "Sleep fade",
             "Seconds of volume fade before the sleep timer pauses.", 5.0f, 1.0f, 60.0f),
        TUNE_I("playback.bg_downloads", "Playback", "Background downloads",
               "Video downloads that may run at once.", 2, 1, 8),
        TUNE_I("playback.volume_wheel_step", "Playback", "Volume wheel step",
               "Volume percent per wheel notch over the bottom-bar slider.", 5, 1, 25),
        TUNE("playback.normalize_i", "Playback", "Normalize target",
             "loudnorm integrated loudness target (LUFS).", -16.0f, -30.0f, -5.0f),
        TUNE("playback.normalize_tp", "Playback", "Normalize peak",
             "loudnorm true-peak ceiling (dBTP).", -1.5f, -6.0f, -0.1f),
        TUNE("playback.normalize_lra", "Playback", "Normalize range",
             "loudnorm loudness range.", 11.0f, 1.0f, 30.0f),
        TUNE_I("playback.mpv_ready_ms", "Playback", "mpv ready wait",
               "How long to wait for the player to accept a load, milliseconds.",
               3000, 500, 15000),
        TUNE_I("playback.mpv_seek_ms", "Playback", "mpv seek wait",
               "How long to wait for a seek to settle, milliseconds.", 5000, 500, 20000),
        TUNE_I("playback.prefetch_batch", "Playback", "Prefetch batch",
               "Stream URLs resolved per prefetch round.", 12, 1, 64),
        TUNE_I("playback.prefetch_serial_gap_ms", "Playback", "Prefetch gap",
               "Pause between serial prefetch batches, milliseconds.", 120, 0, 2000),

        // ---- Search ---------------------------------------------------
        TUNE("search.debounce_sec", "Search", "Search debounce",
             "Quiet time after typing before a search runs.", 0.35f, 0.05f, 2.0f),
        TUNE("search.suggest_debounce_sec", "Search", "Typeahead debounce",
             "Quiet time before the suggestions dropdown updates.", 0.22f, 0.05f, 2.0f),
        TUNE_I("search.suggest_songs", "Search", "Typeahead songs",
               "Song suggestions shown before albums/artists.", 6, 0, 20),
        TUNE_I("search.suggest_artists", "Search", "Typeahead artists",
               "Artist suggestions added to the dropdown.", 3, 0, 20),
        TUNE_I("search.suggest_more", "Search", "Typeahead more",
               "Extra library entries added after songs/artists.", 8, 0, 30),
        TUNE_I("search.suggest_total", "Search", "Typeahead total",
               "Hard cap on dropdown rows.", 13, 1, 40),
        TUNE_I("search.suggest_height", "Search", "Typeahead height",
               "Maximum dropdown height, logical pixels.", 400, 120, 900),
        TUNE_I("search.recent_max", "Search", "Recent searches",
               "Recent queries kept on the Home page.", 10, 0, 40),
        TUNE_I("search.prefetch_count", "Search", "Search prefetch",
               "Stream URLs warmed after a search returns.", 10, 0, 50),
        TUNE_I("home.prefetch_count", "Home", "Home prefetch",
               "Stream URLs warmed after the Home feed loads.", 3, 0, 50),
        TUNE_I("home.retry_first_sec", "Home", "Home retry delay",
               "First automatic retry after the feed fails, seconds.", 10, 1, 300),
        TUNE_I("home.retry_next_sec", "Home", "Home retry interval",
               "Follow-up retry interval after the feed fails, seconds.", 30, 1, 600),

        // ---- Radio ----------------------------------------------------
        TUNE_I("radio.max_seeds", "Radio", "Radio seeds",
               "Seed tracks fed into a generated radio station.", 8, 1, 40),
        TUNE_I("radio.prefetch_count", "Radio", "Radio prefetch",
               "Stream URLs warmed for a new radio station.", 10, 0, 50),
        TUNE_I("radio.recency_days", "Radio", "Recency window",
               "Days of listening history used for radio seeds.", 30, 1, 3650),
        TUNE("radio.half_life_days", "Radio", "Recency half-life",
             "Score half-life for older listening events, days.", 7.0f, 0.5f, 365.0f),

        // ---- Video ----------------------------------------------------
        TUNE_I("video.load_fail_sec", "Video", "Load fail watchdog",
               "Seconds a stream may stall before it counts as failed.", 8, 2, 120),
        TUNE_I("video.resolve_timeout_sec", "Video", "Resolve timeout",
               "Seconds a video resolve may take before giving up.", 30, 5, 300),
        TUNE_I("video.retry_deadline_sec", "Video", "Retry deadline",
               "Budget for the background fresh re-resolve, seconds.", 20, 5, 180),
        TUNE_I("video.sync_hold_ms", "Video", "Sync hold",
               "Sync corrections paused after a seek, milliseconds.", 450, 0, 5000),
        TUNE_I("video.seek_follow_ms", "Video", "Seek follow window",
               "Ignore audio seeks newer than this when syncing, milliseconds.",
               180, 0, 2000),
        TUNE("video.drift_threshold_sec", "Video", "Drift threshold",
             "A/V drift that triggers a corrective seek, seconds.", 2.0f, 0.2f, 10.0f),
        TUNE_I("video.drift_seek_interval_sec", "Video", "Drift seek interval",
               "Minimum time between corrective seeks, seconds.", 2, 1, 30),
        TUNE_I("video.aspect_recheck_ms", "Video", "Aspect recheck",
               "How often the video aspect ratio is re-read, milliseconds.",
               500, 100, 5000),
        TUNE("video.speed_nudge", "Video", "Speed nudge",
             "Playback-rate nudge per unit of drift.", 0.25f, 0.0f, 1.0f),
        TUNE("video.speed_clamp", "Video", "Speed clamp",
             "Maximum playback-rate correction for sync.", 0.12f, 0.0f, 1.0f),
        TUNE("video.speed_deadband", "Video", "Speed deadband",
             "Drift below this is left to the sync hold, seconds.", 0.08f, 0.01f, 1.0f),
        TUNE("video.speed_apply_threshold", "Video", "Speed apply threshold",
             "Playback-rate change smaller than this is ignored.", 0.004f, 0.0001f, 0.1f),

        // ---- Interface ------------------------------------------------
        TUNE_I("ui.frame_active_ms", "Interface", "Frame: interacting",
               "Frame budget while animating or handling input, ms.", 16, 4, 100),
        TUNE_I("ui.frame_playback_ms", "Interface", "Frame: playing",
               "Frame budget during steady playback, ms.", 100, 16, 1000),
        TUNE_I("ui.frame_idle_ms", "Interface", "Frame: idle",
               "Frame budget when nothing is happening, ms.", 1000, 100, 10000),
        TUNE_I("ui.frame_safety_ms", "Interface", "Frame: safety",
               "Longest the loop may sleep, ms.", 5000, 500, 60000),
        TUNE_I("ui.frame_lp_active_ms", "Interface", "Low power: interacting",
               "Frame budget while interacting in low power mode, ms.", 33, 8, 200),
        TUNE_I("ui.frame_lp_playback_ms", "Interface", "Low power: playing",
               "Frame budget during playback in low power mode, ms.", 250, 33, 2000),
        TUNE_I("ui.frame_lp_idle_ms", "Interface", "Low power: idle",
               "Frame budget when idle in low power mode, ms.", 2000, 250, 30000),
        TUNE_I("ui.activity_grace_ms", "Interface", "Activity grace",
               "How long after input the fast frame budget stays on, ms.",
               1000, 100, 10000),
        TUNE_I("ui.render_safety_sec", "Interface", "Repaint safety",
               "Force a repaint at least this often while active, seconds.",
               5, 1, 60),
        TUNE_I("ui.toast_max", "Interface", "Toasts",
               "Maximum notification toasts on screen.", 4, 1, 12),
        TUNE("ui.toast_slide_sec", "Interface", "Toast slide-in",
             "Toast entrance animation duration, seconds.", 0.28f, 0.0f, 2.0f),
        TUNE("ui.toast_fade_sec", "Interface", "Toast fade-out",
             "Toast exit animation duration, seconds.", 0.22f, 0.0f, 2.0f),
        TUNE_I("ui.toast_swipe_px", "Interface", "Toast swipe",
               "Horizontal swipe distance that dismisses a toast, px.", 130, 20, 600),
        TUNE("ui.toast_swipe_idle_sec", "Interface", "Toast swipe idle",
             "Idle time before a toast swipe resets, seconds.", 0.22f, 0.02f, 2.0f),
        TUNE("ui.notification_timeout_sec", "Interface", "Desktop notification",
             "How long a desktop notification stays up, seconds.", 5.0f, 1.0f, 60.0f),
        TUNE("ui.anim_subtle", "Interface", "Animation factor: subtle",
             "Motion speed multiplier for the Subtle setting.", 0.70f, 0.1f, 2.0f),
        TUNE("ui.anim_playful", "Interface", "Animation factor: playful",
             "Motion speed multiplier for the Playful setting.", 1.55f, 0.1f, 4.0f),
        TUNE("lyrics.ease_rate", "Interface", "Lyrics ease",
             "Easing rate for the active lyric line.", 6.0f, 0.5f, 40.0f),
        TUNE("lyrics.morph_rate", "Interface", "Lyrics morph",
             "Morph rate when the active line changes.", 7.0f, 0.5f, 40.0f),
        TUNE("lyrics.hold_sec", "Interface", "Lyrics scroll hold",
             "How long manual lyric scroll holds before auto-centre resumes.",
             0.75f, 0.0f, 10.0f),
        TUNE("lyrics.active_scale", "Interface", "Active line scale",
             "Font scale of the active lyric line.", 1.32f, 1.0f, 2.0f),

        // ---- Scrolling ------------------------------------------------
        TUNE("scroll.pixels_per_delta", "Scrolling", "Wheel speed",
             "Pixels scrolled per wheel delta.", 70.0f, 10.0f, 500.0f),
        TUNE("scroll.chase_tau", "Scrolling", "Wheel smoothing",
             "Input-to-display settle time, seconds.", 0.05f, 0.005f, 0.5f),
        TUNE("scroll.fling_decay", "Scrolling", "Fling decay",
             "Exponential decay of wheel inertia.", 3.0f, 0.1f, 20.0f),
        TUNE("scroll.fling_min", "Scrolling", "Fling minimum",
             "Velocity below which inertia stops, px/s.", 5.0f, 1.0f, 100.0f),
        TUNE("scroll.fling_max", "Scrolling", "Fling maximum",
             "Fastest fling speed, px/s.", 5000.0f, 100.0f, 20000.0f),
        TUNE_I("scroll.release_ms", "Scrolling", "Wheel release",
               "Idle time before wheel input counts as released, ms.", 80, 10, 1000),
        TUNE_I("scroll.gesture_gap_ms", "Scrolling", "Wheel gesture gap",
               "Gap that separates two wheel gestures, ms.", 120, 10, 2000),
        TUNE("scroll.hwheel_trigger", "Scrolling", "Horizontal swipe",
             "Wheel deltas per back/forward step.", 2.0f, 0.5f, 20.0f),
        TUNE_I("scroll.hwheel_gap_ms", "Scrolling", "Horizontal swipe gap",
               "Gap that resets the horizontal gesture, ms.", 150, 10, 2000),
        TUNE("scroll.touch_tap_slop_px", "Scrolling", "Touch tap slop",
             "Movement beyond this becomes a scroll, px.", 14.0f, 2.0f, 80.0f),
        TUNE_I("scroll.touch_fling_stop_ms", "Scrolling", "Touch fling stop",
               "Still time before a lift does not fling, ms.", 120, 0, 1000),
        TUNE("scroll.touch_fling_max", "Scrolling", "Touch fling maximum",
             "Fastest touch fling speed, px/s.", 5000.0f, 100.0f, 20000.0f),
        TUNE("scroll.touch_inertia_decay", "Scrolling", "Touch inertia decay",
             "Exponential decay of touch inertia.", 3.2f, 0.1f, 20.0f),

        // ---- Layout ---------------------------------------------------
        TUNE("layout.space_1", "Layout", "Spacing 1", "Smallest layout step.",
             4.0f, 0.0f, 40.0f),
        TUNE("layout.space_2", "Layout", "Spacing 2", "Second layout step.",
             8.0f, 0.0f, 60.0f),
        TUNE("layout.space_3", "Layout", "Spacing 3", "Third layout step.",
             12.0f, 0.0f, 80.0f),
        TUNE("layout.space_4", "Layout", "Spacing 4", "Fourth layout step.",
             16.0f, 0.0f, 100.0f),
        TUNE("layout.space_5", "Layout", "Spacing 5", "Large layout step.",
             24.0f, 0.0f, 140.0f),
        TUNE("layout.space_6", "Layout", "Spacing 6", "Largest layout step.",
             32.0f, 0.0f, 200.0f),
        TUNE("layout.radius_xs", "Layout", "Radius XS", "Smallest corner radius.",
             4.0f, 0.0f, 40.0f),
        TUNE("layout.radius_sm", "Layout", "Radius S", "Small corner radius.",
             7.0f, 0.0f, 40.0f),
        TUNE("layout.radius_md", "Layout", "Radius M", "Medium corner radius.",
             10.0f, 0.0f, 40.0f),
        TUNE("layout.radius_lg", "Layout", "Radius L", "Large corner radius.",
             14.0f, 0.0f, 60.0f),
        TUNE("layout.row_art", "Layout", "Row artwork", "Track row thumbnail size.",
             48.0f, 24.0f, 160.0f),
        TUNE("layout.header_h", "Layout", "Header height", "Top bar height.",
             68.0f, 40.0f, 200.0f),
        TUNE("layout.bottom_h", "Layout", "Bottom bar height", "Player bar height.",
             92.0f, 60.0f, 260.0f),
        TUNE("layout.content_pad", "Layout", "Content padding",
             "Horizontal padding of list pages.", 24.0f, 0.0f, 120.0f),
        TUNE("layout.lyrics_min", "Layout", "Lyrics min width",
             "Narrowest docked lyrics panel.", 300.0f, 160.0f, 700.0f),
        TUNE("layout.lyrics_max", "Layout", "Lyrics max width",
             "Widest docked lyrics panel.", 520.0f, 200.0f, 1200.0f),
        TUNE("layout.list_min", "Layout", "List min width",
             "Width the list keeps before lyrics stop docking.", 260.0f, 120.0f, 800.0f),

        // ---- Thumbnails ----------------------------------------------
        TUNE_I("thumbs.max_dim", "Thumbnails", "Decode size",
               "Longest decoded thumbnail side, pixels.", 512, 64, 2048),
        TUNE_I("thumbs.pool", "Thumbnails", "Decode threads",
               "Thumbnail decode workers.", 3, 1, 16),
        TUNE_I("thumbs.queue", "Thumbnails", "Queued jobs",
               "Maximum queued thumbnail downloads.", 64, 1, 512),
        TUNE_I("thumbs.retry_backoff_sec", "Thumbnails", "Retry backoff",
               "Wait before retrying a failed thumbnail URL, seconds.", 60, 1, 3600),

        // ---- Party ----------------------------------------------------
        TUNE_I("party.max_guests", "Party", "Max guests",
               "Guests a single party may have.", 64, 2, 512),
        TUNE_I("party.max_inbox", "Party", "Max inbox",
               "Queued guest actions awaiting the host.", 64, 4, 512),
        TUNE_I("party.queue_art_max", "Party", "Queue artwork rows",
               "Rows whose artwork is embedded in each snapshot.", 40, 0, 200),
        TUNE_I("party.guest_queue_max", "Party", "Guest queue rows",
               "Queue rows sent to a guest page.", 40, 1, 200),
        TUNE_I("party.idle_evict_sec", "Party", "Guest idle eviction",
               "Idle seconds before a guest is forgotten.", 1800, 60, 86400),
        TUNE("party.rate_add", "Party", "Rate: add",
             "Sustained song adds per second per guest.", 0.2f, 0.01f, 5.0f),
        TUNE("party.rate_add_burst", "Party", "Rate: add burst",
             "Burst allowance for song adds.", 1.0f, 1.0f, 20.0f),
        TUNE("party.rate_search", "Party", "Rate: search",
             "Sustained guest searches per second.", 1.0f, 0.01f, 10.0f),
        TUNE("party.rate_search_burst", "Party", "Rate: search burst",
             "Burst allowance for searches.", 3.0f, 1.0f, 30.0f),
        TUNE("party.rate_bump", "Party", "Rate: bump",
             "Sustained play-next taps per second per guest.", 0.5f, 0.01f, 5.0f),
        TUNE("party.rate_bump_burst", "Party", "Rate: bump burst",
             "Burst allowance for play-next taps.", 1.0f, 1.0f, 20.0f),
        TUNE("party.rate_control", "Party", "Rate: controls",
             "Sustained media-control taps per second per guest.", 1.0f, 0.01f, 10.0f),
        TUNE("party.rate_control_burst", "Party", "Rate: controls burst",
             "Burst allowance for media-control taps.", 2.0f, 1.0f, 30.0f),
        TUNE_I("party.waiter_timeout_sec", "Party", "Action timeout",
               "Seconds a guest action waits for the host.", 6, 1, 60),
        TUNE_I("party.control_timeout_sec", "Party", "Control timeout",
               "Seconds a media-control action waits for the host.", 3, 1, 60),
        TUNE_I("party.tunnel_timeout_sec", "Party", "Tunnel start timeout",
               "Seconds to wait for cloudflared to report a URL.", 30, 5, 180),
        TUNE_I("party.tunnel_log_keep", "Party", "Tunnel log tail",
               "Characters of cloudflared output kept for errors.", 3000, 200, 20000),

        // ---- Diagnostics ---------------------------------------------
        TUNE_I("diag.log_lines", "Diagnostics", "Log lines",
               "App log lines kept in memory.", 2000, 100, 200000),
        TUNE_I("diag.hang_ms", "Diagnostics", "Hang watchdog",
               "Stalled milliseconds before a hang note is written.", 60000, 5000, 600000),
        TUNE_I("diag.backend_log_lines", "Diagnostics", "Backend log lines",
               "Recent backend output lines kept for diagnostics.", 400, 20, 20000),
        TUNE_I("diag.command_clip", "Diagnostics", "Command clip",
               "Characters of a backend command kept in the log.", 180, 20, 2000),
        TUNE_I("startup.loading_watchdog_sec", "Diagnostics", "Loading watchdog",
               "Seconds a start may take before it is reported as stuck.", 30, 5, 300),
        TUNE_I("power.battery_poll_sec", "Diagnostics", "Battery poll",
               "Seconds between battery-state checks.", 30, 1, 600),

        // ---- Backend (restart required) -------------------------------
        TUNE_RI("backend.stream_ttl_sec", "Backend", "Stream URL TTL",
                "How long a resolved stream URL is reused, seconds.", 7200, 60, 86400),
        TUNE_RI("backend.stream_disk_max", "Backend", "Stream cache size",
                "Resolved URLs kept in the shared disk cache.", 400, 10, 5000),
        TUNE_RI("backend.search_ttl_sec", "Backend", "Search cache TTL",
                "Search result cache lifetime, seconds.", 900, 10, 86400),
        TUNE_RI("backend.search_cache_max", "Backend", "Search cache entries",
                "Search queries cached per backend process.", 100, 5, 5000),
        TUNE_RI("backend.video_map_ttl_days", "Backend", "Video match TTL",
                "Days an official-video match is reused.", 30, 1, 365),
        TUNE_RI("backend.video_stream_ttl_sec", "Backend", "Video URL TTL",
                "How long a resolved video stream URL is reused, seconds.",
                3600, 60, 86400),
        TUNE_RI("backend.video_disk_max", "Backend", "Video cache size",
                "Resolved video URLs kept on disk.", 64, 1, 2000),
        TUNE_RI("backend.sync_ttl_days", "Backend", "A/V sync TTL",
                "Days a measured audio/video offset is reused.", 120, 1, 3650),
        TUNE_RI("backend.sync_sr", "Backend", "Sync sample rate",
                "Sample rate used by the alignment analysis.", 8000, 2000, 48000),
        TUNE_R("backend.sync_hop_sec", "Backend", "Sync hop",
              "Hop between analysis frames, seconds.", 0.05f, 0.01f, 1.0f),
        TUNE_R("backend.sync_window_sec", "Backend", "Sync window",
              "Audio/video window compared per estimate, seconds.",
              20.0f, 2.0f, 120.0f),
        TUNE_R("backend.sync_search_sec", "Backend", "Sync search span",
              "Widest offset searched during alignment, seconds.",
              90.0f, 5.0f, 600.0f),
        TUNE_RI("backend.http_timeout_sec", "Backend", "HTTP timeout",
                "Default HTTP request timeout, seconds.", 10, 1, 120),
        TUNE_RI("backend.read_timeout_sec", "Backend", "Backend read timeout",
                "Seconds to wait for a reply from the Python backend.", 45, 5, 600),
        TUNE_RI("backend.playable_timeout_sec", "Backend", "Probe timeout",
                "Timeout for the URL playable probe, seconds.", 8, 1, 60),
        TUNE_RI("backend.playable_attempts", "Backend", "Probe attempts",
                "Attempts for the URL playable probe.", 2, 1, 10),
        TUNE_RI("backend.json_max_depth", "Backend", "JSON max depth",
                "Nesting limit for JSON replies from the backend.", 256, 16, 4096),
        TUNE_RI("backend.json_max_input_kb", "Backend", "JSON max size",
                "Largest backend reply parsed, kilobytes.", 16384, 64, 262144),
        TUNE_RI("backend.capture_max_kb", "Backend", "Capture cap",
                "Largest captured subprocess output, kilobytes.", 8192, 64, 262144),
    };
    return defs;
}

#undef TUNE
#undef TUNE_I
#undef TUNE_B
#undef TUNE_R
#undef TUNE_RI

std::map<std::string, float> g_values;
bool g_loaded = false;
bool g_dirty = false;
unsigned g_version = 0;

const std::unordered_map<std::string, const TunableDef *> &index()
{
    static const std::unordered_map<std::string, const TunableDef *> idx = [] {
        std::unordered_map<std::string, const TunableDef *> out;
        for (const TunableDef &d : build_defs())
            out.emplace(d.key, &d);
        return out;
    }();
    return idx;
}

float clamp_to_def(const TunableDef &d, float v)
{
    v = std::max(d.min, std::min(d.max, v));
    if (d.kind == TunableKind::Int)
        v = std::round(v);
    else if (d.kind == TunableKind::Bool)
        v = v >= 0.5f ? 1.0f : 0.0f;
    return v;
}

std::string file_path()
{
    return p100_config_path("tunables.json");
}

void load_file()
{
    g_values.clear();
    FILE *f = std::fopen(file_path().c_str(), "rb");
    if (!f)
        return;
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    std::fclose(f);

    // Flat {"key": value, ...} object; unknown keys are ignored.
    size_t pos = 0;
    while ((pos = text.find('"', pos)) != std::string::npos)
    {
        const size_t end = text.find('"', pos + 1);
        if (end == std::string::npos)
            break;
        const std::string key = text.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        const size_t colon = text.find(':', pos);
        if (colon == std::string::npos)
            break;
        char *parse_end = nullptr;
        const double value = std::strtod(text.c_str() + colon + 1, &parse_end);
        if (parse_end != text.c_str() + colon + 1)
        {
            auto it = index().find(key);
            if (it != index().end())
                g_values[key] = clamp_to_def(*it->second, (float)value);
        }
        pos = colon + 1;
    }
}

} // namespace

const std::vector<TunableDef> &tunable_defs()
{
    return build_defs();
}

float tunable(const char *key)
{
    if (!g_loaded)
    {
        tunables_load();
        g_loaded = true;
    }
    if (key)
    {
        auto it = g_values.find(key);
        if (it != g_values.end())
            return it->second;
    }
    auto def = index().find(key ? key : "");
    return def == index().end() ? 0.0f : def->second->def;
}

int tunable_int(const char *key)
{
    return (int)std::lround(tunable(key));
}

bool tunable_bool(const char *key)
{
    return tunable(key) >= 0.5f;
}

void tunables_load()
{
    load_file();
    g_loaded = true;
    g_dirty = false;
    ++g_version;
}

void tunables_set(const char *key, float value)
{
    if (!key)
        return;
    auto it = index().find(key);
    if (it == index().end())
        return;
    const float clamped = clamp_to_def(*it->second, value);
    const float def = it->second->def;
    if (std::fabs(clamped - def) < 0.00001f)
        g_values.erase(key);
    else
        g_values[key] = clamped;
    g_dirty = true;
    ++g_version;
}

void tunables_reset(const char *key)
{
    if (!key)
        return;
    if (g_values.erase(key) > 0)
    {
        g_dirty = true;
        ++g_version;
    }
}

void tunables_reset_all()
{
    g_values.clear();
    g_dirty = true;
    ++g_version;
}

bool tunables_is_set(const char *key)
{
    return key && g_values.find(key) != g_values.end();
}

int tunables_changed_count()
{
    return (int)g_values.size();
}

bool tunables_dirty()
{
    return g_dirty;
}

unsigned tunables_version()
{
    return g_version;
}

void tunables_save_if_dirty()
{
    if (!g_dirty)
        return;
    g_dirty = false;
    if (!p100_ensure_config_dir())
        return;
    const std::string path = file_path();
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return;
    std::fprintf(f, "{\n");
    bool first = true;
    for (const TunableDef &d : tunable_defs())
    {
        auto it = g_values.find(d.key);
        if (it == g_values.end())
            continue;
        std::fprintf(f, "%s  \"%s\": %g", first ? "" : ",\n", d.key, (double)it->second);
        first = false;
    }
    std::fprintf(f, first ? "}\n" : "\n}\n");
    std::fclose(f);
    std::rename(tmp.c_str(), path.c_str());
}
