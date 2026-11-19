#include "ui.h"
#include "imgui_internal.h"
#include "ui_backdrop.h"
#include "ui_notify.h"
#include "ui_theme.h"
#include "ui_icons.h"
#include "library.h"
#include "party.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Open a URL in the user's browser (song pages, links copied from a track).
bool ui_open_external_url(const std::string &url)
{
    if (url.empty())
        return false;
    return SDL_OpenURL(url.c_str()) == 0;
}

void ui_copy_to_clipboard(const std::string &text)
{
    if (text.empty())
        return;
    SDL_SetClipboardText(text.c_str());
}

// ----------------------------------------------------------------------
// Queue operations
// ----------------------------------------------------------------------

void queue_add_next(AppState &state, const Track &track)
{
    if (track.id.empty())
        return;
    Track pinned = track;
    pinned.from_queue_source = false;
    state.play_queue.insert(state.play_queue.begin(), pinned);
    state.queue_dirty = true;
    state.repaint_requested = true;
}

void queue_add_end(AppState &state, const Track &track)
{
    if (track.id.empty())
        return;
    Track added = track;
    added.from_queue_source = false;
    state.play_queue.push_back(added);
    state.queue_dirty = true;
    state.repaint_requested = true;
}

void queue_remove_at(AppState &state, int index)
{
    if (index < 0 || index >= (int)state.play_queue.size())
        return;
    state.play_queue.erase(state.play_queue.begin() + index);
    state.queue_dirty = true;
    state.repaint_requested = true;
}

void queue_move(AppState &state, int from, int to)
{
    if (from < 0 || from >= (int)state.play_queue.size())
        return;
    to = std::max(0, std::min((int)state.play_queue.size() - 1, to));
    if (from == to)
        return;
    Track t = state.play_queue[from];
    state.play_queue.erase(state.play_queue.begin() + from);
    state.play_queue.insert(state.play_queue.begin() + to, std::move(t));
    state.queue_dirty = true;
    state.repaint_requested = true;
}

static std::vector<Track> g_queue_undo;
static AppState *g_queue_undo_state = nullptr;

static void undo_queue_clear()
{
    if (!g_queue_undo_state || g_queue_undo.empty())
        return;
    g_queue_undo_state->play_queue = g_queue_undo;
    g_queue_undo_state->queue_dirty = true;
    g_queue_undo_state->repaint_requested = true;
    g_queue_undo.clear();
    ui_notify(UiNotifyType::Info, "Queue restored", "Your previous Up Next is back.", 4.0f);
}

void queue_clear(AppState &state)
{
    if (state.play_queue.empty())
        return;
    g_queue_undo = state.play_queue;
    g_queue_undo_state = &state;
    state.play_queue.clear();
    state.queue_dirty = true;
    state.repaint_requested = true;
    ui_notify(UiNotifyType::Info, "Queue cleared", "Up Next is empty.", 6.0f, "Undo",
              &undo_queue_clear);
}

void ui_invalidate_forward(AppState &state)
{
    state.nav_forward_valid = false;
}

void ui_go_back(AppState &state)
{
    // artist_browse_id is published by a worker; take the same lock for a
    // consistent snapshot of the current artist view.
    std::lock_guard<std::recursive_mutex> artist_lock(state.artist_mutex);
    // Remember where we are so ui_go_forward() can bring the user back.
    state.nav_forward.tab = state.current_tab;
    state.nav_forward.source = state.current_playlist_source;
    state.nav_forward.playlist_index = state.selected_playlist_index;
    if (state.current_tab == Tab::Album)
        state.nav_forward.album_id = state.album_browse_id;
    else
        state.nav_forward.album_id.clear();
    if (state.current_tab == Tab::Artist)
        state.nav_forward.artist_id = state.artist_browse_id;
    else
        state.nav_forward.artist_id.clear();
    state.nav_forward_valid = true;

    if (state.current_tab == Tab::Album)
    {
        state.current_tab = state.album_return_tab;
        state.current_playlist_source = state.album_return_source;
        if (state.current_tab == Tab::Playlists)
            state.selected_playlist_index = state.album_return_playlist_index;
    }
    else if (state.current_tab == Tab::Artist)
    {
        state.current_tab = state.artist_return_tab;
        state.current_playlist_source = state.artist_return_source;
        if (state.current_tab == Tab::Playlists)
            state.selected_playlist_index = state.artist_return_playlist_index;
    }
    else if (state.current_tab == Tab::Queue)
    {
        state.current_tab = state.pre_queue_tab;
    }
    else if (state.current_tab == Tab::Podcasts && !state.podcast_selected.browse_id.empty())
    {
        close_podcast_show(state);
    }
    else if (state.current_tab == Tab::Settings)
    {
        state.current_tab = Tab::Home;
        state.current_playlist_source = PlaylistSource::HOME;
    }
}

void ui_go_forward(AppState &state)
{
    std::lock_guard<std::recursive_mutex> artist_lock(state.artist_mutex);
    if (!state.nav_forward_valid)
        return;
    const AppState::NavSnapshot fwd = state.nav_forward;
    state.nav_forward_valid = false;

    switch (fwd.tab)
    {
    case Tab::Album:
        // The album detail fields may have been repurposed by a newer view;
        // reopen (cache-backed, so it is instant) when they no longer match.
        if (fwd.album_id.empty() || fwd.album_id != state.album_browse_id)
            open_album_view(state, fwd.album_id);
        else
            state.current_tab = Tab::Album;
        break;
    case Tab::Artist:
        if (fwd.artist_id.empty() || fwd.artist_id != state.artist_browse_id)
            open_artist_view(state, fwd.artist_id);
        else
            state.current_tab = Tab::Artist;
        break;
    case Tab::Queue:
        state.pre_queue_tab = state.current_tab;
        state.current_tab = Tab::Queue;
        break;
    default:
        state.current_tab = fwd.tab;
        break;
    }
    state.current_playlist_source = fwd.source;
    if (fwd.source == PlaylistSource::PLAYLIST)
        state.selected_playlist_index = fwd.playlist_index;
}

// ----------------------------------------------------------------------
// Shared UI helpers
// ----------------------------------------------------------------------

void ensure_local_tracks_loaded(AppState &state)
{
    if (state.local_tracks_loaded)
        return;
    state.local_tracks = library::load_local_tracks();
    state.local_tracks_loaded = true;
}

std::string local_track_url(const AppState &state, const std::string &id)
{
    for (const Track &t : state.local_tracks)
        if (t.id == id)
            return t.url;
    return "";
}

void build_smart_view(AppState &state)
{
    ensure_local_tracks_loaded(state);
    // Track metadata comes from the play log (title/artist/thumbnail per
    // play event); play counts only know ids. Newest entry per id wins.
    std::unordered_map<std::string, const PlayEvent *> meta;
    meta.reserve(state.play_log.size());
    for (const PlayEvent &ev : state.play_log)
        meta.emplace(ev.id, &ev); // log is newest-first: first hit is newest

    std::vector<Track> out;
    if (state.smart_mode == 0)
    {
        // On Repeat: most-played tracks, all time, top 50.
        std::vector<std::pair<int, const PlayEvent *>> ranked;
        ranked.reserve(meta.size());
        for (const auto &[id, ev] : meta)
        {
            int plays = 1;
            auto it = state.play_counts.find(id);
            if (it != state.play_counts.end() && it->second > 0)
                plays = it->second;
            ranked.emplace_back(plays, ev);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        for (const auto &[plays, ev] : ranked)
        {
            if ((int)out.size() >= 50)
                break;
            Track t;
            t.id = ev->id;
            t.title = ev->title;
            t.artist = ev->artist;
            t.thumbnail_url = ev->thumbnail_url;
            if (ev->id.rfind("local:", 0) == 0)
            {
                t.url = local_track_url(state, ev->id);
                if (t.url.empty())
                    continue; // local file no longer indexed
            }
            else
                t.url = "https://www.youtube.com/watch?v=" + ev->id;
            out.push_back(std::move(t));
        }
    }
    else
    {
        // Recently Played: most recent distinct tracks, top 100.
        std::unordered_set<std::string> seen;
        for (const PlayEvent &ev : state.play_log)
        {
            if ((int)out.size() >= 100)
                break;
            if (!seen.insert(ev.id).second)
                continue;
            Track t;
            t.id = ev.id;
            t.title = ev.title;
            t.artist = ev.artist;
            t.thumbnail_url = ev.thumbnail_url;
            if (ev.id.rfind("local:", 0) == 0)
            {
                t.url = local_track_url(state, ev.id);
                if (t.url.empty())
                    continue; // local file no longer indexed
            }
            else
                t.url = "https://www.youtube.com/watch?v=" + ev.id;
            out.push_back(std::move(t));
        }
    }
    state.smart_view = std::make_shared<const std::vector<Track>>(std::move(out));
}

// ----------------------------------------------------------------------
// Playlist source helper. Returns an immutable snapshot; network-fed
// lists are shared_ptrs swapped atomically by worker threads, and
// main-thread vectors are wrapped with a non-owning alias.
// ----------------------------------------------------------------------
TrackListPtr alias_of(const std::vector<Track> &v)
{
    return TrackListPtr(&v, [](const std::vector<Track> *) {});
}

PlaylistSource playlist_source_for_tab(Tab tab)
{
    switch (tab)
    {
    case Tab::Home: return PlaylistSource::HOME;
    case Tab::Downloads: return PlaylistSource::DOWNLOADS;
    case Tab::Radio: return PlaylistSource::RADIO;
    case Tab::Liked: return PlaylistSource::LIKED;
    case Tab::Playlists: return PlaylistSource::PLAYLIST;
    case Tab::Albums: return PlaylistSource::HOME;
    case Tab::Artists: return PlaylistSource::HOME;
    case Tab::History: return PlaylistSource::HISTORY;
    case Tab::Stats: return PlaylistSource::STATS;
    case Tab::Queue: return PlaylistSource::HOME;
    case Tab::Album: return PlaylistSource::ALBUM;
    case Tab::Artist: return PlaylistSource::ARTIST;
    case Tab::Podcasts: return PlaylistSource::PODCAST;
    case Tab::Settings: return PlaylistSource::HOME;
    case Tab::YouTube: return PlaylistSource::YOUTUBE_PLAYLIST;
    case Tab::Smart: return PlaylistSource::SMART;
    case Tab::Party: return PlaylistSource::HOME;
    case Tab::Studio: return PlaylistSource::STUDIO;
    }
    return PlaylistSource::HOME;
}

TrackListPtr get_playlist(const AppState &state)
{
    switch (state.current_playlist_source)
    {
    case PlaylistSource::HOME:
        return std::atomic_load(&state.recommendations);
    case PlaylistSource::SEARCH:
        return std::atomic_load(&state.search_results);
    case PlaylistSource::DOWNLOADS:
        return alias_of(state.downloaded_tracks);
    case PlaylistSource::RADIO:
        return std::atomic_load(&state.radio_results);
    case PlaylistSource::LIKED:
        return alias_of(state.liked_tracks);
    case PlaylistSource::PLAYLIST:
        if (state.selected_playlist_index >= 0 && state.selected_playlist_index < (int)state.playlists.size())
            return alias_of(state.playlists[state.selected_playlist_index].tracks);
        return nullptr;
    case PlaylistSource::ALBUM:
        return alias_of(state.album_tracks);
    case PlaylistSource::ARTIST:
    {
        // A worker publishes artist_songs under artist_mutex; return a
        // snapshot so playback can iterate it while a fetch completes.
        std::lock_guard<std::recursive_mutex> lock(state.artist_mutex);
        return std::make_shared<const std::vector<Track>>(state.artist_songs);
    }
    case PlaylistSource::PODCAST:
        return alias_of(state.podcast_tracks);
    case PlaylistSource::YOUTUBE_PLAYLIST:
        return std::atomic_load(&state.youtube_playlist_tracks);
    case PlaylistSource::HISTORY:
        if (state.history_view && !state.history_view->empty())
            return state.history_view;
        return alias_of(state.playback_history);
    case PlaylistSource::STATS:
        return alias_of(state.stats.tracks);
    case PlaylistSource::SMART:
        if (state.smart_view)
            return state.smart_view;
        return alias_of(state.playback_history);
    default:
        return nullptr;
    }
}

void resolve_current_track_links(AppState &state)
{
    if (state.current_track_id.empty() ||
        (!state.current_album_id.empty() && !state.current_artist_id.empty()))
        return;

    const std::string &id = state.current_track_id;
    auto scan = [&](const std::vector<Track> &tracks) -> bool
    {
        for (const auto &t : tracks)
        {
            if (t.id != id)
                continue;
            if (state.current_album_id.empty())
                state.current_album_id = t.album_id;
            if (state.current_artist_id.empty())
                state.current_artist_id = t.artist_id;
            return !state.current_album_id.empty() && !state.current_artist_id.empty();
        }
        return false;
    };

    auto recommendations = std::atomic_load(&state.recommendations);
    if (recommendations && scan(*recommendations))
        return;
    auto search_results = std::atomic_load(&state.search_results);
    if (search_results && scan(*search_results))
        return;
    auto radio_results = std::atomic_load(&state.radio_results);
    if (radio_results && scan(*radio_results))
        return;
    if (scan(state.downloaded_tracks) || scan(state.liked_tracks) ||
        scan(state.playback_history) || scan(state.album_tracks))
        return;
    {
        std::lock_guard<std::recursive_mutex> lock(state.artist_mutex);
        if (scan(state.artist_songs))
            return;
    }
    if (scan(state.local_search_results))
        return;
    for (const auto &pl : state.playlists)
        if (scan(pl.tracks))
            return;
}

// ----------------------------------------------------------------------
// Smooth scrolling — wheel/touch input as pixels
// ----------------------------------------------------------------------
// main.cpp turns wheel events and touch drags into pixel deltas. Each frame it
// adds them here; these pixels are applied directly to the hovered child.
// ImGui's native wheel path is disabled on those children so there is exactly
// one scroll path and no target/easing lag.
static float g_scroll_pixel_accum = 0.0f;
static float g_frame_scroll_pixels = 0.0f;
static bool g_frame_scroll_consumed = false;
// Set for the frame while a back-to-top jump is driving the scroll.
static bool g_scroll_top_animating = false;
static float g_scroll_clamp_unapplied = 0.0f;

// Absolute sub-pixel scroll accumulator. ImGui rounds every scroll position
// to whole pixels (ImRound64 in CalcNextScrollFromScrollTargetAndClamp), so a
// per-frame delta fed on top of GetScrollY() loses its fraction whenever it
// is under 0.5px -- slow scrolling would freeze and then step. Accumulating
// the absolute value here and feeding SetScrollY(absolute) makes the rounding
// happen on the cumulative position instead, so the fractions carry over and
// motion stays one-to-one at any speed.
static float g_scroll_applied = 0.0f;
static bool g_scroll_applied_valid = false;
static std::string g_scroll_applied_window;
static int g_scroll_applied_frame = -1;

void add_scroll_pixels(float delta_pixels)
{
    g_scroll_pixel_accum += delta_pixels;
}

// Reported by smooth_scroll_child when the hovered child hit its scroll limit
// and could not apply all requested pixels; consumed by update_wheel_inertia()
// to keep the virtual scroll target inside the reachable range.
void scroll_clamp_feedback(float unapplied)
{
    g_scroll_clamp_unapplied += unapplied;
}

float take_scroll_clamp_feedback()
{
    const float unapplied = g_scroll_clamp_unapplied;
    g_scroll_clamp_unapplied = 0.0f;
    return unapplied;
}

// Called before ImGui::NewFrame() each frame.
void smooth_scroll_begin_frame()
{
    g_frame_scroll_pixels = g_scroll_pixel_accum;
    g_scroll_pixel_accum = 0.0f;
    g_frame_scroll_consumed = false;
    g_scroll_top_animating = false;
}

void smooth_scroll_child()
{
    if (g_frame_scroll_consumed || g_frame_scroll_pixels == 0.0f)
        return;
    if (!ImGui::IsWindowHovered())
        return;
    g_frame_scroll_consumed = true;

    // Continuous consumption: same child received pixels on the previous
    // consuming frame, so keep accumulating sub-pixel. Any discontinuity --
    // a new gesture, or the hover moving to a different child -- re-bases on
    // the child's actual scroll so external changes (scrollbar drag, tab
    // switch) are honoured.
    const char *name = ImGui::GetCurrentWindow()->Name;
    const int frame = ImGui::GetFrameCount();
    const bool continuous = g_scroll_applied_valid && name &&
                            g_scroll_applied_window == name &&
                            frame == g_scroll_applied_frame + 1;
    g_scroll_applied_window = name ? name : "";
    g_scroll_applied_valid = true;
    g_scroll_applied_frame = frame;

    const float base = continuous ? g_scroll_applied : ImGui::GetScrollY();
    const float max_scroll = ImGui::GetScrollMaxY();
    const float requested = base + g_frame_scroll_pixels;
    const float applied = std::max(0.0f, std::min(requested, max_scroll));
    g_scroll_applied = applied;
    ImGui::SetScrollY(applied);
    if (applied != requested)
        scroll_clamp_feedback(applied - requested);
}

// Lyrics panel animation flag (see ui.h). Set by draw_lyrics_panel while its
// auto-centre scroll or fade-in is in motion; consumed by the main loop to
// hold 60 FPS. Consuming (rather than just reading) means a panel that stops
// drawing -- e.g. the user switched to a non-list tab -- can't leave the loop
// pinned at the fast cadence.
static bool g_lyrics_animating = false;

void set_lyrics_animating(bool active)
{
    g_lyrics_animating = active;
}

bool is_lyrics_animating()
{
    bool active = g_lyrics_animating;
    g_lyrics_animating = false;
    return active;
}

// ----------------------------------------------------------------------
// Motion helpers
// ----------------------------------------------------------------------
int g_ui_animation_level = 2;
static bool g_ui_animating = false;

void ui_anim_request()
{
    g_ui_animating = true;
}

bool is_ui_animating()
{
    bool active = g_ui_animating;
    g_ui_animating = false;
    return active;
}

// Page-epoch: bumped whenever the visible page changes (tab switch, search
// reset). Panel entrance animations use it to decide "replay the intro",
// which used to be inferred from gaps in the ImGui frame counter. That proxy
// breaks now that the frame loop skips renders while idle (a safety frame
// would replay every intro on screen), while a page change always coincides
// with real input, so it is always observed on a live frame.
static uint64_t g_page_epoch = 0;
uint64_t ui_page_epoch()
{
    return g_page_epoch;
}
void ui_bump_page_epoch()
{
    ++g_page_epoch;
}

float ui_anim_ease_out(float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

float ui_anim_clamp01f(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float ui_anim_ease_out_back(float t)
{
    t = ui_anim_clamp01f(t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static float anim_factor()
{
    switch (g_ui_animation_level)
    {
    case 1: return 0.70f;
    case 3: return 1.55f;
    default: return 1.0f;
    }
}

float ui_anim_duration(float base_seconds)
{
    return g_ui_animation_level > 0 ? base_seconds * anim_factor() : 0.0f;
}

float ui_anim_delay(float base_seconds)
{
    return ui_anim_duration(base_seconds);
}

float ui_anim_offset(float base_px)
{
    return g_ui_animation_level > 0 ? base_px * anim_factor() : 0.0f;
}

float ui_anim_approach(float current, float target, float speed)
{
    if (!ui_animations_enabled())
        return target;
    float dt = ImGui::GetIO().DeltaTime;
    if (!(dt > 0.0f) || dt > 0.1f)
        dt = 1.0f / 60.0f;
    const float t = 1.0f - expf(-speed * dt);
    return current + (target - current) * t;
}

namespace
{
struct WidthAnim
{
    bool init = false;
    float from = 0.0f;
    float to = 0.0f;
    float current = 0.0f;
    float last_target = -1.0f;
    double start = 0.0;
};

float anim_width(WidthAnim &st, float target, float base_duration)
{
    const double now = ImGui::GetTime();
    if (!st.init)
    {
        st.init = true;
        st.from = st.to = st.current = target;
        st.last_target = target;
        st.start = now;
        return st.current;
    }
    if (fabsf(target - st.last_target) > 0.01f)
    {
        st.from = st.current;
        st.to = target;
        st.last_target = target;
        st.start = now;
    }
    const float dur = ui_anim_duration(base_duration);
    float t = dur > 0.0f ? (float)((now - st.start) / dur) : 1.0f;
    st.current = st.from + (st.to - st.from) * ui_anim_ease_out(t);
    if (t < 1.0f)
        ui_anim_request();
    return st.current;
}
} // namespace

float ui_anim_sidebar_width(float target_width)
{
    static WidthAnim st;
    return anim_width(st, target_width, 0.26f);
}

float ui_anim_lyrics_width(float target_width)
{
    static WidthAnim st;
    return anim_width(st, target_width, 0.28f);
}

struct ListRevealState
{
    uint64_t token = 0;
    size_t count = 0;
    std::string first;
    std::string last;
    double start = -1.0;
    uint64_t epoch = (uint64_t)-1; // page epoch the list was last drawn under
};

// (last_frame removed: with idle frame-skipping, frame-number gaps no longer
// mean "not drawn recently"; page changes are tracked via ui_page_epoch.)

static std::unordered_map<std::string, ListRevealState> g_list_reveals;

void ui_anim_reset_list_reveals()
{
    g_list_reveals.clear();
}

struct ScrollRevealState
{
    int last_frame = -1;
    uint64_t epoch = (uint64_t)-1;
    double start = -1.0;
    int order = 0;
};

static std::unordered_map<std::string, ScrollRevealState> g_scroll_reveals;
// Per-scope (frame, next order) counter so rows that become visible together
// cascade instead of popping in as a block.
static std::unordered_map<int, std::pair<int, int>> g_scroll_reveal_batches;

double ui_anim_scroll_reveal(int scope, int index, const std::string &id, int *out_order)
{
    if (!ui_animations_enabled())
    {
        if (out_order)
            *out_order = 0;
        return -1.0;
    }

    std::string key = std::to_string(scope);
    key += '#';
    key += id.empty() ? std::to_string(index) : id;

    const int frame = ImGui::GetFrameCount();
    const uint64_t page_epoch = ui_page_epoch();
    ScrollRevealState &st = g_scroll_reveals[key];
    if (st.start < 0.0 || st.epoch != page_epoch)
    {
        st.start = ImGui::GetTime();
        auto &batch = g_scroll_reveal_batches[scope];
        if (batch.first != frame)
        {
            batch.first = frame;
            batch.second = 0;
        }
        st.order = batch.second++;
        ui_anim_request();
    }
    st.epoch = page_epoch;
    st.last_frame = frame;
    if (out_order)
        *out_order = st.order;
    return st.start;
}

double ui_anim_list_reveal(const char *key, uint64_t token, size_t count,
                           const std::string &first_id, const std::string &last_id)
{
    if (!ui_animations_enabled() || count == 0 || !key || !*key)
        return -1.0;

    const uint64_t page_epoch = ui_page_epoch();
    ListRevealState &st = g_list_reveals[key];
    // Restart the reveal whenever the page changed since the list was last
    // drawn (tab/view switch, returning from lyrics-only, etc.) so the
    // slide-down plays every time the list is shown, not just on the very
    // first load.
    if (st.start < 0.0 || st.epoch != page_epoch ||
        st.token != token || st.count != count ||
        st.first != first_id || st.last != last_id)
    {
        st.token = token;
        st.count = count;
        st.first = first_id;
        st.last = last_id;
        st.start = ImGui::GetTime();
    }
    st.epoch = page_epoch;

    const double elapsed = ImGui::GetTime() - st.start;
    const double total = (double)ui_anim_duration(0.22f) +
                         (double)ui_anim_delay(0.018f * 10.0f) + 0.03;
    if (elapsed < total)
        ui_anim_request();
    return st.start;
}

double ui_anim_track_list_reveal(const char *key, const std::vector<Track> &tracks,
                                 uint64_t token)
{
    if (tracks.empty())
        return -1.0;
    return ui_anim_list_reveal(key, token, tracks.size(),
                               tracks.front().id, tracks.back().id);
}

static std::unordered_map<std::string, double> g_texture_reveals;

float ui_anim_texture_alpha(const std::string &url)
{
    if (!ui_animations_enabled() || url.empty())
        return 1.0f;

    const double now = ImGui::GetTime();
    auto it = g_texture_reveals.find(url);
    if (it == g_texture_reveals.end())
    {
        it = g_texture_reveals.emplace(url, now).first;
    }
    const float dur = ui_anim_duration(0.18f);
    float t = dur > 0.0f ? (float)((now - it->second) / dur) : 1.0f;
    if (t < 1.0f)
        ui_anim_request();
    return ui_anim_ease_out(t);
}

// ----------------------------------------------------------------------
// Scroll edge fades
//
// The list dissolves into the ambient backdrop by painting the backdrop's own
// visible colour over the top and bottom of the scroll viewport with an alpha
// ramp. The colour is sampled from ui_backdrop_sample(), so it follows the
// wash, the blooms, the shader/primitive path, the track colour crossfade and
// the backdrop's visibility exactly; rows keep full opacity and only the
// overlay fades.
//
// Sampling runs on a small lattice whose colours are memoised per window. The
// backdrop colour is a pure function of screen position and backdrop state, so
// a still backdrop reuses the lattice across frames (scrolling only changes
// which rows are covered, not the colour underneath them).
// ----------------------------------------------------------------------
namespace
{
struct FadeLattice
{
    bool valid = false;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    int cols = 0, rows = 0;
    BackdropSampleInputs bg;
    std::vector<ImVec4> colours;
};

struct WindowFadeCache
{
    FadeLattice top;
    FadeLattice bottom;

    // Back-to-top button. The hit target is claimed in ui_scroll_top_begin()
    // (before the rows) and painted here, so a row can never receive the
    // click as well.
    float top_alpha = 0.0f;
    float top_hover = 0.0f;
    bool top_hovered = false;
    bool top_submitted = false;
    ImVec2 top_min;
    ImVec2 top_max;
    // Eased scroll-to-top jump.
    bool top_animating = false;
    float top_anim_from = 0.0f;
    double top_anim_start = 0.0;
    double top_anim_duration = 0.0;
};

std::unordered_map<ImGuiID, WindowFadeCache> g_fade_cache;

bool same_backdrop(const BackdropSampleInputs &a, const BackdropSampleInputs &b)
{
    return a.tint.x == b.tint.x && a.tint.y == b.tint.y && a.tint.z == b.tint.z &&
           a.visibility == b.visibility && a.strength == b.strength &&
           a.pulse == b.pulse && a.spin == b.spin && a.theme == b.theme &&
           a.accent_fallback == b.accent_fallback && a.shader == b.shader &&
           a.display_w == b.display_w && a.display_h == b.display_h;
}

void build_lattice(FadeLattice &c)
{
    const int stride = c.cols + 1;
    c.colours.resize((size_t)stride * (size_t)(c.rows + 1));
    const float sx = (c.x1 - c.x0) / (float)c.cols;
    const float sy = (c.y1 - c.y0) / (float)c.rows;
    for (int r = 0; r <= c.rows; ++r)
    {
        for (int k = 0; k <= c.cols; ++k)
        {
            ImVec4 s = ui_backdrop_sample(ImVec2(c.x0 + sx * (float)k, c.y0 + sy * (float)r));
            s.w = 1.0f;
            c.colours[(size_t)r * (size_t)stride + (size_t)k] = s;
        }
    }
}

// Alpha of the overlay at y: 1 at the outer screen edge (content fully
// replaced by backdrop), 0 at the inner edge. Smoothstep removes the visible
// crease a linear ramp leaves where the band ends.
float band_alpha(const FadeLattice &c, bool top, float y)
{
    const float h = std::max(0.001f, c.y1 - c.y0);
    float t = top ? (y - c.y0) / h : (c.y1 - y) / h;
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

void draw_lattice(ImDrawList *dl, const FadeLattice &c, bool top)
{
    if (c.cols <= 0 || c.rows <= 0)
        return;

    const int stride = c.cols + 1;
    const float sx = (c.x1 - c.x0) / (float)c.cols;
    const float sy = (c.y1 - c.y0) / (float)c.rows;
    for (int r = 0; r < c.rows; ++r)
    {
        const float y0 = c.y0 + sy * (float)r;
        const float y1 = y0 + sy;
        const float a0 = band_alpha(c, top, y0);
        const float a1 = band_alpha(c, top, y1);
        const ImVec4 *row0 = &c.colours[(size_t)r * (size_t)stride];
        const ImVec4 *row1 = &c.colours[(size_t)(r + 1) * (size_t)stride];
        for (int k = 0; k < c.cols; ++k)
        {
            const float x0 = c.x0 + sx * (float)k;
            const float x1 = x0 + sx;
            const ImVec4 &c00 = row0[k];
            const ImVec4 &c01 = row0[k + 1];
            const ImVec4 &c11 = row1[k + 1];
            const ImVec4 &c10 = row1[k];
            dl->AddRectFilledMultiColor(
                ImVec2(x0, y0), ImVec2(x1, y1),
                ImGui::GetColorU32(ImVec4(c00.x, c00.y, c00.z, a0)),
                ImGui::GetColorU32(ImVec4(c01.x, c01.y, c01.z, a0)),
                ImGui::GetColorU32(ImVec4(c11.x, c11.y, c11.z, a1)),
                ImGui::GetColorU32(ImVec4(c10.x, c10.y, c10.z, a1)));
        }
    }
}

void ensure_lattice(FadeLattice &c, const ImVec2 &pos, const ImVec2 &size,
                    float y0, float y1, int cols, int rows, const BackdropSampleInputs &bg)
{
    if (c.valid && c.x0 == pos.x && c.x1 == pos.x + size.x && c.y0 == y0 && c.y1 == y1 &&
        c.cols == cols && c.rows == rows && same_backdrop(c.bg, bg))
        return;

    c.x0 = pos.x;
    c.x1 = pos.x + size.x;
    c.y0 = y0;
    c.y1 = y1;
    c.cols = cols;
    c.rows = rows;
    c.bg = bg;
    build_lattice(c);
    c.valid = true;
}
} // namespace

void ui_scroll_top_begin()
{
    if (g_fade_cache.size() > 64)
        g_fade_cache.clear();
    WindowFadeCache &cache = g_fade_cache[ImGui::GetCurrentWindow()->ID];
    cache.top_submitted = false;

    const double now = ImGui::GetTime();
    const float max_scroll = ImGui::GetScrollMaxY();
    const float scroll = ImGui::GetScrollY();

    // Eased jump to the top. A wheel/touch gesture cancels it so the user
    // always wins; the animation steps the scroll target each frame, which
    // ImGui applies on the next Begin (same path as the smooth scroller).
    if (cache.top_animating)
    {
        if (std::fabs(g_frame_scroll_pixels) > 0.01f)
        {
            cache.top_animating = false;
        }
        else
        {
            const double elapsed = now - cache.top_anim_start;
            float t = cache.top_anim_duration > 0.0
                          ? (float)(elapsed / cache.top_anim_duration)
                          : 1.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            ImGui::SetScrollY(cache.top_anim_from * (1.0f - ui_anim_ease_out(t)));
            if (t < 1.0f)
                ui_anim_request();
            else
                cache.top_animating = false;
        }
    }
    if (cache.top_animating)
        g_scroll_top_animating = true;

    // Ease in once the list has been scrolled a meaningful amount; ease out
    // again near the top. Motion is optional (the button snaps without it).
    const float target = (max_scroll > 1.0f && scroll > ui_px(120.0f)) ? 1.0f : 0.0f;
    float alpha = cache.top_alpha;
    if (ui_animations_enabled())
    {
        alpha = ui_anim_approach(alpha, target, 16.0f);
        if (std::fabs(alpha - target) > 0.002f)
            ui_anim_request();
    }
    else
        alpha = target;
    cache.top_alpha = alpha;
    if (alpha <= 0.002f)
        return;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 clip_max = dl->GetClipRectMax();
    const UiMetrics &m = ui_m();
    const float diameter = ui_px(38.0f);
    const float margin = m.space_5;
    // Keep clear of the party chip, which lives in the same corner.
    const float stack = party_running()
                            ? std::max(32.0f * m.layout_scale, m.control_h * 0.78f) + m.space_4
                            : 0.0f;
    const ImVec2 pmin(clip_max.x - margin - diameter,
                      clip_max.y - margin - diameter - stack);
    cache.top_min = pmin;
    cache.top_max = ImVec2(pmin.x + diameter, pmin.y + diameter);

    // Claim the hit target now, before the rows are submitted: an item that
    // claimed hover first blocks later overlapping items, so the row beneath
    // the button cannot also react to the click.
    const ImVec2 saved = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(pmin);
    ImGui::InvisibleButton("##scroll_top", ImVec2(diameter, diameter));
    cache.top_hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    ImGui::SetCursorScreenPos(saved);

    float hover = cache.top_hover;
    const float hover_target = cache.top_hovered ? 1.0f : 0.0f;
    if (ui_animations_enabled())
    {
        hover = ui_anim_approach(hover, hover_target, 16.0f);
        if (std::fabs(hover - hover_target) > 0.002f)
            ui_anim_request();
    }
    else
        hover = hover_target;
    cache.top_hover = hover;

    if (cache.top_hovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Back to top");
    }
    if (clicked)
    {
        // The external jump must not be swallowed by the wheel's continuity
        // base (or an in-flight fling).
        g_scroll_applied_valid = false;
        reset_wheel_scroll();
        const float duration = ui_anim_duration(0.45f);
        if (duration > 0.001f && scroll > 1.0f)
        {
            cache.top_animating = true;
            cache.top_anim_from = scroll;
            cache.top_anim_start = now;
            cache.top_anim_duration = (double)duration;
        }
        else
        {
            ImGui::SetScrollY(0.0f);
        }
        cache.top_alpha = 1.0f;
        ui_anim_request();
    }

    cache.top_submitted = true;
}

bool ui_scroll_top_animating()
{
    return g_scroll_top_animating;
}

void ui_scroll_to_top()
{
    // An external jump: the next wheel frame must re-base on the real scroll,
    // and a fling or a back-to-top animation still in flight would otherwise
    // keep writing its own target over this one.
    g_scroll_applied_valid = false;
    reset_wheel_scroll();
    if (ImGuiWindow *win = ImGui::GetCurrentWindow())
    {
        auto it = g_fade_cache.find(win->ID);
        if (it != g_fade_cache.end())
            it->second.top_animating = false;
    }
    ImGui::SetScrollY(0.0f);
}

void ui_draw_scroll_edge_fades(float height)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // ---- Edge bands -------------------------------------------------
    if (height > 0.0f && ImGui::GetScrollMaxY() > 1.0f)
    {
        // Fade exactly the visible content rect: the window's inner rect
        // already excludes the padding and the scrollbar, so alpha is full at
        // the first pixel of the first visible row.
        const ImVec2 pos = dl->GetClipRectMin();
        const ImVec2 size(dl->GetClipRectMax().x - pos.x, dl->GetClipRectMax().y - pos.y);
        const float scroll = ImGui::GetScrollY();
        const float max_scroll = ImGui::GetScrollMaxY();
        const bool show_top = scroll > 1.0f;
        const bool show_bottom = scroll < max_scroll - 1.0f;
        const float fade = std::min(ui_px(height), size.y * 0.18f);
        if (size.x > 2.0f && size.y > 2.0f && (show_top || show_bottom) && fade > 0.5f)
        {
            const BackdropSampleInputs bg = ui_backdrop_sample_inputs();

            // The wash is bilinear and the blooms are broad, so ~64 px by
            // ~4 px cells keep the interpolation error below one 8-bit step.
            // With the backdrop off the background is a single flat colour:
            // one column is exact.
            const bool flat = bg.visibility <= 0.001f;
            const float cell_w = std::max(1.0f, ui_px(64.0f));
            const float cell_h = std::max(1.0f, ui_px(4.0f));
            const int cols = flat ? 1 : std::clamp((int)std::ceil(size.x / cell_w), 1, 48);
            const int rows = std::clamp((int)std::ceil(fade / cell_h), 1, 16);

            WindowFadeCache &cache = g_fade_cache[ImGui::GetCurrentWindow()->ID];
            if (show_top)
            {
                ensure_lattice(cache.top, pos, size, pos.y, pos.y + fade, cols, rows, bg);
                draw_lattice(dl, cache.top, true);
            }
            if (show_bottom)
            {
                ensure_lattice(cache.bottom, pos, size, pos.y + size.y - fade, pos.y + size.y,
                               cols, rows, bg);
                draw_lattice(dl, cache.bottom, false);
            }
        }
    }

    // ---- Back-to-top button ----------------------------------------
    // Painted last so it stays crisp over the bottom fade. The item itself
    // was submitted early by ui_scroll_top_begin().
    auto it = g_fade_cache.find(ImGui::GetCurrentWindow()->ID);
    if (it != g_fade_cache.end() && it->second.top_submitted)
    {
        const WindowFadeCache &cache = it->second;
        const float a = std::clamp(cache.top_alpha, 0.0f, 1.0f);
        const float hv = std::clamp(cache.top_hover, 0.0f, 1.0f);
        const ImVec2 c((cache.top_min.x + cache.top_max.x) * 0.5f,
                       (cache.top_min.y + cache.top_max.y) * 0.5f);
        const float r = (cache.top_max.x - cache.top_min.x) * 0.5f;

        // Solid surface base so the icon stays legible over album art, then
        // an accent wash and border. The accent is present at rest and
        // deepens on hover.
        const ImVec4 surf = ui_c().surface;
        dl->AddCircleFilled(ImVec2(c.x, c.y + ui_px(2.0f)), r,
                            IM_COL32(0, 0, 0, (int)(55.0f * a)), 40);
        dl->AddCircleFilled(c, r, ui_col(ImVec4(surf.x, surf.y, surf.z, a)), 40);
        dl->AddCircleFilled(c, r, ui_accent_u32((0.16f + 0.10f * hv) * a), 40);
        dl->AddCircle(c, r, ui_accent_u32((0.60f + 0.30f * hv) * a), 40, 1.0f);
        ui_draw_icon(dl, UiIcon::ChevronUp, c, r * 0.92f,
                     ui_with_alpha(ui_accent_text_u32(), a), 1.8f);
    }
}

