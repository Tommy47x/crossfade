#include "tasks.h"
#include "tunables.h"
#include "ui.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_notify.h"
#include "ui_theme.h"
#include "ytdlp.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
double steady_now_sec()
{
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void draw_youtube_panel(AppState &state)
{
    // Playlist fetches publish from workers; hold the same lock for the frame
    // so the vector and id/error strings cannot change mid-draw.
    std::lock_guard<std::recursive_mutex> state_lock(state.youtube_mutex);
    // Account playlists need a session; keep the tab useful by pointing at
    // Settings instead of firing account-only fetches.
    if (backend_is_guest())
    {
        if (ui_sign_in_state("Sign in to YouTube Music",
                             "Your account playlists need a signed-in session. "
                             "Search, albums, artists and radio work without one.") == 1)
        {
            state.settings_focus_auth = true;
            state.current_tab = Tab::Settings;
            state.repaint_requested = true;
        }
        return;
    }
    const UiMetrics &m = ui_m();
    const double now = steady_now_sec();

    // Account playlists (the "Liked Music" auto-playlist is included).
    if (!state.youtube_playlists_loaded)
    {
        std::string raw;
        if (!cache_older_than_auth(youtube_playlists_cache_path()) &&
            read_text_file(youtube_playlists_cache_path(), raw) && !raw.empty())
        {
            auto cached = parse_youtube_playlists_response(raw);
            if (!cached.empty())
            {
                state.youtube_playlists = std::move(cached);
                state.youtube_playlists_loaded = true;
                // A cached list is stale by definition: refresh it once below.
                state.youtube_playlists_refreshed = false;
            }
        }
    }

    if (!state.youtube_playlists_loaded && !state.youtube_playlists_loading.load())
    {
        state.youtube_playlists_loading = true;
        const uint64_t token =
            state.youtube_playlists_request.fetch_add(1, std::memory_order_relaxed) + 1;
        tasks::spawn("youtube", [&state, token]() {
            auto pls = fetch_youtube_playlists();
            BackendError err = take_backend_error();
            const bool got = !pls.empty();
            if (!got && err)
                ui_notify_backend_error(err);
            std::lock_guard<std::recursive_mutex> lock(state.youtube_mutex);
            if (token != state.youtube_playlists_request.load(std::memory_order_relaxed))
                return; // a newer session/retry owns the list
            if (got)
                state.youtube_playlists = std::move(pls);
            // Mark loaded even on failure so the empty-state spinner stops;
            // the left pane shows "No playlists found." + retry instead.
            state.youtube_playlists_loaded = true;
            state.youtube_playlists_loading = false;
            if (got || !err)
            {
                // A network response (even an empty one) is fresh: no second
                // fetch is needed right after the first.
                state.youtube_playlists_refreshed = true;
                state.youtube_playlists_retry_at = 0.0;
            }
            else
            {
                state.youtube_playlists_refreshed = false;
                state.youtube_playlists_retry_at =
                    steady_now_sec() + (double)tunable_int("youtube.retry_sec");
            }
            state.repaint_requested = true;
        });
    }
    else if (state.youtube_playlists_loaded && !state.youtube_playlists_loading.load() &&
             !state.youtube_playlists_refreshed && now >= state.youtube_playlists_retry_at)
    {
        // One refresh for a cached list; a failed attempt backs off instead of
        // re-queueing a backend command on every frame while the tab is open.
        state.youtube_playlists_loading = true;
        const uint64_t token =
            state.youtube_playlists_request.fetch_add(1, std::memory_order_relaxed) + 1;
        tasks::spawn("youtube", [&state, token]() {
            auto pls = fetch_youtube_playlists();
            BackendError err = take_backend_error();
            const bool got = !pls.empty();
            if (!got && err)
            {
                // Keep the cached list on screen; just tell the user why the
                // refresh didn't happen.
                ui_notify_backend_error(err);
            }
            std::lock_guard<std::recursive_mutex> lock(state.youtube_mutex);
            if (token != state.youtube_playlists_request.load(std::memory_order_relaxed))
                return;
            if (got)
            {
                state.youtube_playlists = std::move(pls);
                state.youtube_playlists_refreshed = true;
                state.youtube_playlists_retry_at = 0.0;
            }
            else if (err)
            {
                state.youtube_playlists_refreshed = false;
                state.youtube_playlists_retry_at =
                    steady_now_sec() + (double)tunable_int("youtube.retry_sec");
            }
            else
            {
                state.youtube_playlists_refreshed = true;
            }
            state.youtube_playlists_loading = false;
            state.repaint_requested = true;
        });
    }

    // Pre-cache a few playlist contents in the background so they are
    // available offline without the user having to open each one. This runs on
    // the prefetch channel, after the list refresh has settled, so it can never
    // hold up an interactive playlist/radio/Home request on the main channel.
    const int precache_count = std::max(0, tunable_int("youtube.precache_count"));
    if (state.youtube_playlists_loaded && state.youtube_playlists_refreshed &&
        !state.youtube_playlists_loading.load() && precache_count > 0 &&
        !state.youtube_playlists.empty() &&
        !state.youtube_playlist_contents_precaching.load() &&
        !state.youtube_playlist_contents_precached)
    {
        state.youtube_playlist_contents_precaching = true;
        std::vector<std::string> ids;
        for (const auto &pl : state.youtube_playlists)
            if (!pl.id.empty())
                ids.push_back(pl.id);
        tasks::spawn("youtube-playlist", [&state, ids, precache_count]() {
            int fetched = 0;
            for (const auto &pid : ids)
            {
                if (fetched >= precache_count)
                    break;
                // Skip if already cached by *this* session.
                std::string raw;
                const std::string cache_path = youtube_playlist_cache_path(pid);
                if (!cache_older_than_auth(cache_path) &&
                    read_text_file(cache_path, raw) && !raw.empty())
                    continue;
                auto tracks = fetch_youtube_playlist_prefetch(pid);
                ++fetched;
                if (tracks.empty())
                {
                    // A genuinely empty playlist also lands here, but with no
                    // backend error. On a real failure (network down, auth
                    // expired) stop for this session instead of hammering the
                    // backend playlist-by-playlist; opening one still caches it.
                    BackendError err = take_backend_error();
                    if (err)
                    {
                        ui_notify_backend_error(err);
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::max(0, tunable_int("youtube.precache_gap_ms"))));
            }
            {
                std::lock_guard<std::recursive_mutex> lock(state.youtube_mutex);
                state.youtube_playlist_contents_precached = true;
            }
            state.youtube_playlist_contents_precaching = false;
            state.repaint_requested = true;
        });
    }

    if (state.youtube_playlists_loading.load() && state.youtube_playlists.empty())
    {
        ui_empty_state(UiIcon::Playlist, "Loading playlists", "Fetching from your YouTube account");
        return;
    }

    static int selected = -1;
    if (selected >= (int)state.youtube_playlists.size())
        selected = -1;

    const float total_w = ImGui::GetContentRegionAvail().x;
    const float gap = m.space_4;
    float left_w = 260.0f * m.layout_scale;
    if (total_w < 600.0f * m.layout_scale)
        left_w = std::max(180.0f * m.layout_scale, total_w * 0.36f);
    left_w = std::min(left_w, total_w * 0.44f);

    ImGui::BeginChild("YTPlaylistList", ImVec2(left_w, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child();
    ui_scroll_top_begin();

    ui_section_header("Your YouTube Playlists", nullptr);
    for (int i = 0; i < (int)state.youtube_playlists.size(); ++i)
    {
        const YouTubePlaylist &pl = state.youtube_playlists[i];
        const float h = std::max(m.control_h, m.row_art * 0.86f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(i);
        ImGui::InvisibleButton("##yt_playlist_item", ImVec2(w, h),
                               ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const bool active = i == selected;
        if (clicked)
        {
            selected = i;
            // Supersede any contents fetch for a previously selected playlist.
            // The old worker checks this token before publishing, so a slow
            // reply can never replace the tracks of the new selection.
            state.youtube_playlist_request.fetch_add(1, std::memory_order_relaxed);
            state.youtube_playlist_loading_id.clear();
            state.youtube_playlist_loading = false;
            state.youtube_playlist_loaded_id.clear(); // force reload
            state.youtube_playlist_failed_id.clear(); // retry any earlier failure
            state.youtube_playlist_error_msg.clear();
        }
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (active)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui_accent_u32(0.13f), m.radius_sm);
        else if (hovered)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui_overlay_u32(0.045f), m.radius_sm);
        ui_draw_icon(dl, UiIcon::Playlist, ImVec2(pos.x + m.space_3 + 9.0f * m.layout_scale, pos.y + h * 0.5f),
                     18.0f * m.layout_scale, active ? ui_accent_text_u32() : ui_text_muted_u32(), 1.5f);
        const float tx = pos.x + m.space_3 + 22.0f * m.layout_scale;
        const float tw = w - (tx - pos.x) - m.space_2;
        std::string name = ui_ellipsize(g_ui_fonts.medium, 19.0f, pl.title.empty() ? "Untitled" : pl.title, tw);
        dl->AddText(g_ui_fonts.medium, 19.0f * m.text_scale,
                    ImVec2(tx, pos.y + (h - 19.0f * m.text_scale * 1.2f) * 0.5f),
                    active ? ui_accent_text_u32() : (hovered ? ui_text_u32() : ui_text_dim_u32()), name.c_str());
        if (pl.count > 0)
        {
            std::string c = std::to_string(pl.count);
            ImVec2 ts = g_ui_fonts.body->CalcTextSizeA(14.0f * m.text_scale, FLT_MAX, 0.0f, c.c_str());
            dl->AddText(g_ui_fonts.body, 14.0f * m.text_scale,
                        ImVec2(pos.x + w - ts.x - m.space_2, pos.y + (h - 14.0f * m.text_scale * 1.2f) * 0.5f),
                        ui_text_muted_u32(), c.c_str());
        }
        ImGui::PopID();
    }

    if (state.youtube_playlists.empty() && !state.youtube_playlists_loading.load())
    {
        ImGui::Dummy(ImVec2(0.0f, m.space_5));
        ui_push_small();
        ImGui::TextColored(ui_c().text_muted, "No playlists found.");
        ImGui::PopFont();
        if (state.youtube_playlists_loaded && ImGui::SmallButton("Retry"))
        {
            // Force both the initial load and the refresh to run again, and
            // invalidate any worker that is still in flight.
            state.youtube_playlists_request.fetch_add(1, std::memory_order_relaxed);
            state.youtube_playlists_loading = false;
            state.youtube_playlists_loaded = false;
            state.youtube_playlists_refreshed = false;
            state.youtube_playlists_retry_at = 0.0;
            state.repaint_requested = true;
        }
    }

    ui_draw_scroll_edge_fades(state.settings.edge_fade_height);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, gap);
    ImGui::BeginChild("YTPlaylistTracks", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child();
    ui_scroll_top_begin();

    if (selected < 0 || selected >= (int)state.youtube_playlists.size())
    {
        ui_empty_state(UiIcon::Playlist, "Select a playlist", "Your YouTube playlists live here");
    }
    else
    {
        const YouTubePlaylist &pl = state.youtube_playlists[selected];
        if (state.youtube_playlist_loaded_id != pl.id)
        {
            state.youtube_playlist_loaded_from_cache = false;
            std::string raw;
            const std::string cache_path = youtube_playlist_cache_path(pl.id);
            if (!cache_older_than_auth(cache_path) &&
                read_text_file(cache_path, raw) && !raw.empty())
            {
                auto cached = parse_youtube_playlist_tracks_response(raw);
                if (!cached.empty())
                {
                    std::atomic_store(&state.youtube_playlist_tracks,
                                      std::make_shared<const std::vector<Track>>(std::move(cached)));
                    state.youtube_playlist_loaded_id = pl.id;
                    state.youtube_playlist_loaded_from_cache = true;
                }
            }
        }

        if (state.youtube_playlist_loaded_id != pl.id &&
            state.youtube_playlist_failed_id != pl.id &&
            state.youtube_playlist_loading_id != pl.id)
        {
            state.youtube_playlist_loading_id = pl.id;
            state.youtube_playlist_loading = true;
            const uint64_t token =
                state.youtube_playlist_request.fetch_add(1, std::memory_order_relaxed) + 1;
            std::string pid = pl.id;
            tasks::spawn("youtube-playlist", [&state, pid, token]() {
                auto tracks = fetch_youtube_playlist(pid);
                BackendError err = take_backend_error();
                const bool got = !tracks.empty() || !err;
                {
                    std::lock_guard<std::recursive_mutex> lock(state.youtube_mutex);
                    // A newer selection owns the pane now: drop this reply
                    // instead of showing the previous playlist's tracks.
                    if (token != state.youtube_playlist_request.load(std::memory_order_relaxed))
                        return;
                    state.youtube_playlist_loading_id.clear();
                    state.youtube_playlist_loading = false;
                    if (got)
                    {
                        std::atomic_store(&state.youtube_playlist_tracks,
                                          std::make_shared<const std::vector<Track>>(std::move(tracks)));
                        state.youtube_playlist_loaded_id = pid;
                        if (state.youtube_playlist_failed_id == pid)
                        {
                            state.youtube_playlist_failed_id.clear();
                            state.youtube_playlist_error_msg.clear();
                        }
                    }
                    else
                    {
                        // Leave any cached tracks on screen; record the failure
                        // so the UI can offer a retry instead of a bare spinner.
                        state.youtube_playlist_failed_id = pid;
                        state.youtube_playlist_error_msg = backend_error_user_message(err);
                    }
                    state.repaint_requested = true;
                }
                if (!got)
                    ui_notify_backend_error(err);
            });
        }
        else if (state.youtube_playlist_loaded_id == pl.id && !state.youtube_playlist_loading.load() &&
                 state.youtube_playlist_loaded_from_cache &&
                 state.youtube_playlist_refreshed_id != pl.id)
        {
            state.youtube_playlist_refreshed_id = pl.id;
            state.youtube_playlist_loading = true;
            // This silent refresh supersedes a contents fetch the user walked
            // away from; make sure its stale loading id cannot block a later
            // click on that playlist.
            state.youtube_playlist_loading_id.clear();
            const uint64_t token =
                state.youtube_playlist_request.fetch_add(1, std::memory_order_relaxed) + 1;
            std::string pid = pl.id;
            tasks::spawn("youtube-playlist", [&state, pid, token]() {
                auto tracks = fetch_youtube_playlist(pid);
                BackendError err = take_backend_error();
                const bool got = !tracks.empty();
                bool notify = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(state.youtube_mutex);
                    if (token != state.youtube_playlist_request.load(std::memory_order_relaxed))
                        return;
                    if (got)
                    {
                        std::atomic_store(&state.youtube_playlist_tracks,
                                          std::make_shared<const std::vector<Track>>(std::move(tracks)));
                        state.youtube_playlist_loaded_id = pid;
                        if (state.youtube_playlist_failed_id == pid)
                        {
                            state.youtube_playlist_failed_id.clear();
                            state.youtube_playlist_error_msg.clear();
                        }
                    }
                    else if (err)
                    {
                        // Offline refresh: keep showing the saved tracks. The
                        // refreshed id stays set so this does not turn into a
                        // retry loop while the tab is open; clicking the
                        // playlist again forces a fresh load.
                        notify = true;
                    }
                    state.youtube_playlist_loading = false;
                    state.repaint_requested = true;
                }
                if (notify)
                    ui_notify_backend_error(err);
            });
        }

        // Only show the spinner when there is nothing on screen yet. If
        // cached tracks are already displayed, the background refresh swaps
        // them in silently — no blocking spinner on a cache hit.
        if (state.youtube_playlist_loading.load() && state.youtube_playlist_loaded_id != pl.id)
        {
            ui_empty_state(UiIcon::Playlist, "Loading tracks", "Fetching playlist");
        }
        else if (state.youtube_playlist_loaded_id != pl.id &&
                 state.youtube_playlist_failed_id == pl.id && !state.youtube_playlist_error_msg.empty())
        {
            // No cache available and the network fetch failed: say so and
            // offer a retry (clicking the playlist in the list also retries).
            ui_empty_state(UiIcon::Playlist, "Couldn't load playlist", state.youtube_playlist_error_msg.c_str());
            const UiMetrics &mm = ui_m();
            std::string label = "Retry";
            ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + (avail.x - ts.x) * 0.5f - mm.space_3,
                                             ImGui::GetCursorScreenPos().y + mm.space_4));
            if (ImGui::Button("Retry"))
            {
                state.youtube_playlist_failed_id.clear();
                state.youtube_playlist_error_msg.clear();
                state.repaint_requested = true;
            }
        }
        else
        {
            auto snap = std::atomic_load(&state.youtube_playlist_tracks);
            const std::vector<Track> *tracks = (state.youtube_playlist_loaded_id == pl.id && snap) ? snap.get() : nullptr;
            int count = tracks ? (int)tracks->size() : 0;

            std::string meta = pl.title + "  ·  " + std::to_string(count) + (count == 1 ? " track" : " tracks");
            ui_section_header(pl.title.empty() ? "Playlist" : pl.title.c_str(), meta.c_str());

            if (!tracks || tracks->empty())
            {
                ui_empty_state(UiIcon::Playlist, "No tracks", "This playlist is empty");
            }
            else
            {
                const double reveal_start = ui_anim_track_list_reveal("yt_playlist", *tracks);
                const float row_pitch = ui_track_row_pitch(ui_elem("list_thumb", 44.0f));
                ImGuiListClipper clipper;
                clipper.Begin(count, row_pitch);
                while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const Track &track = (*tracks)[i];
                    TrackRowOptions opt;
                    opt.show_duration = true;
                    opt.art_size = ui_elem("list_thumb", 44.0f);
                    // This view plays on single click; keep row-internal
                    // (double-click / touch-tap) playback off so a touch tap
                    // can't fire ui_play_track twice.
                    opt.auto_play_double_click = false;
                    opt.reveal_start = reveal_start;
                    // Stagger from the first visible row; see ui_reveal_base().
                    opt.reveal_index = std::max(0, i - clipper.DisplayStart);
                    TrackRowResult row = ui_track_row(state, track, i, opt);
                    if (row.clicked && !row.ctrl_clicked && !row.shift_clicked)
                    {
                        state.current_playlist_source = PlaylistSource::YOUTUBE_PLAYLIST;
                        ui_play_track(state, track, i);
                    }
                }
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_4));
    ui_draw_scroll_edge_fades(state.settings.edge_fade_height);
    ImGui::EndChild();
}