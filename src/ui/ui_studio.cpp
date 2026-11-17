#include "ui.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "local_audio.h"
#include "library.h"
#include "studio_project.h"
#include "mpvplayer.h"

#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <numeric>
#include <string>
#include <system_error>
#include <vector>

// ----------------------------------------------------------------------
// Studio (songwriting) panel.
//
// Phases 1-6: local songs, beat selection, waveform/beat analysis, the
// stationary-playhead timeline, cues, and a lyric editor with 1:1 cue links.
//
// Timeline model
// --------------
//     screen_x(t) = playhead_x + (t - view_time) * zoom
//
// Wheel zoom changes only `zoom`, so the time under the fixed playhead stays
// put. Following keeps `view_time` on playback; dragging pans and detaches.
//
// Cue/lyric link invariant: a cue stores the time, a lyric line stores the
// link. Moving a cue therefore moves its line's sync point with no second copy
// to keep in sync.
// ----------------------------------------------------------------------

namespace
{
char g_local_path[1024] = "";
char g_search_query[512] = "";

struct TimelineView
{
    std::string beat_id;
    float zoom = 90.0f;     // pixels per second
    double view_time = 0.0; // time pinned under the playhead
    bool following = true;
};
TimelineView g_tl;
std::string g_selected_cue_id;
bool g_restore_tried = false;
std::string g_lyrics_project;
int g_last_active = -2;

struct ProjectCache
{
    std::string beat_id;
    std::vector<std::pair<std::string, std::string>> items; // (id, title)
};
ProjectCache g_projects;
bool g_projects_dirty = true;
// A waveform click made while the beat is still loading: applied as soon as
// the player reports it is ready.
double g_pending_scrub = -1.0;

const ImVec4 kCueColors[] = {
    ImVec4(0.95f, 0.38f, 0.38f, 1.0f), // red
    ImVec4(0.95f, 0.64f, 0.28f, 1.0f), // orange
    ImVec4(0.58f, 0.84f, 0.42f, 1.0f), // green
    ImVec4(0.38f, 0.70f, 0.96f, 1.0f), // blue
    ImVec4(0.72f, 0.52f, 0.96f, 1.0f), // purple
    ImVec4(0.96f, 0.52f, 0.76f, 1.0f), // pink
};
constexpr int kCueColorCount = (int)(sizeof(kCueColors) / sizeof(kCueColors[0]));
const char *kCueTypes[] = {"Marker", "Section", "Note"};

int cue_color_index(int index)
{
    return ((index % kCueColorCount) + kCueColorCount) % kCueColorCount;
}
ImU32 cue_color(int index)
{
    return ImGui::ColorConvertFloat4ToU32(kCueColors[cue_color_index(index)]);
}
ImVec4 cue_color_vec(int index)
{
    return kCueColors[cue_color_index(index)];
}

bool file_exists(const std::string &path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::string time_text(double seconds)
{
    if (seconds < 0.0)
        seconds = 0.0;
    return ui_format_duration((int)(seconds + 0.5));
}

void mark_dirty(AppState &state)
{
    state.studio_project_dirty = true;
    state.studio_project_edit_time = ImGui::GetTime();
}

// Snapshot the project before an edit. Cheap (a few KB) and capped, so every
// structural change (add/delete/link, cue drag, focus into a text field) can
// be undone without a bespoke per-operation undo log.
void push_undo(AppState &state)
{
    if (!state.studio_project_loaded)
        return;
    state.studio_undo.push_back(state.studio_project);
    if (state.studio_undo.size() > 40)
        state.studio_undo.erase(state.studio_undo.begin());
}

void undo_project(AppState &state)
{
    if (!state.studio_project_loaded || state.studio_undo.empty())
        return;
    state.studio_project = std::move(state.studio_undo.back());
    state.studio_undo.pop_back();
    mark_dirty(state);
}

// Push the project's A-B loop region to mpv. Called whenever the region or the
// selected beat changes; "no" clears it.
void apply_loop(MpvPlayer &player, const SongProject &p)
{
    if (p.loop_enabled && p.loop_a >= 0.0 && p.loop_b > p.loop_a + 0.05)
    {
        char a[32], b[32];
        std::snprintf(a, sizeof(a), "%.3f", p.loop_a);
        std::snprintf(b, sizeof(b), "%.3f", p.loop_b);
        player.set_property("ab-loop-a", a);
        player.set_property("ab-loop-b", b);
    }
    else
    {
        player.set_property("ab-loop-a", "no");
        player.set_property("ab-loop-b", "no");
    }
}

void save_project_if_due(AppState &state)
{
    if (!state.studio_project_loaded || !state.studio_project_dirty)
        return;
    if (ImGui::GetTime() - state.studio_project_edit_time < 0.5)
        return;
    state.studio_project.zoom = g_tl.zoom;
    state.studio_project.following = g_tl.following;
    save_song_project(state.studio_project);
    state.studio_project_dirty = false;
}

void save_now(AppState &state)
{
    if (!state.studio_project_loaded)
        return;
    state.studio_project.zoom = g_tl.zoom;
    state.studio_project.following = g_tl.following;
    if (state.studio_project_dirty)
        save_song_project(state.studio_project);
    state.studio_project_dirty = false;
}

void request_analysis(AppState &state)
{
    if (state.studio_beat.id.empty())
        return;
    state.studio_analysis_seq.fetch_add(1);
    std::atomic_store(&state.studio_analysis, std::shared_ptr<const StudioAnalysis>{});
    state.studio_analyzing = false;
    state.studio_analysis_error.clear();
    state.studio_analysis_requested_id = state.studio_beat.id;
    state.studio_analysis_dirty = true;
    state.repaint_requested = true;
}

void load_project_for_beat(AppState &state, const Track &beat)
{
    const std::string pid = studio_project_id_for_beat(beat.id);
    SongProject project;
    if (!load_song_project(pid, project))
    {
        project = SongProject{};
        project.id = pid;
        project.title = beat.title;
    }
    project.beat = beat;
    state.studio_project = std::move(project);
    state.studio_project_loaded = true;
    state.studio_project_dirty = false;
    state.studio_undo.clear();
    g_projects_dirty = true;
    g_tl = TimelineView{};
    g_tl.beat_id = beat.id;
    g_tl.zoom = state.studio_project.zoom > 1.0f ? state.studio_project.zoom : 90.0f;
    // Always start following: a saved "follow off" from a previous pan used to
    // leave the timeline frozen on open, which read as broken.
    g_tl.following = true;
    g_selected_cue_id.clear();
}

void set_beat(AppState &state, const Track &track)
{
    if (track.id.empty())
        return;
    save_now(state);
    state.studio_beat = track;
    load_project_for_beat(state, track);
    request_analysis(state);
    g_tl.following = true; // a freshly chosen beat should scroll with playback
    ui_play_track(state, track, -1);
}

void clear_beat(AppState &state)
{
    save_now(state);
    state.studio_beat = Track{};
    state.studio_project = SongProject{};
    state.studio_project_loaded = false;
    state.studio_analysis_requested_id.clear();
    std::atomic_store(&state.studio_analysis, std::shared_ptr<const StudioAnalysis>{});
    state.studio_analysis_error.clear();
    state.studio_analyzing = false;
    state.studio_analysis_seq.fetch_add(1);
    state.studio_undo.clear();
    g_selected_cue_id.clear();
    g_projects_dirty = true;
}

void refresh_projects(AppState &state)
{
    g_projects.beat_id = state.studio_beat.id;
    g_projects.items = list_projects_for_beat(state.studio_beat.id);
    const std::string &current = state.studio_project.id;
    if (!current.empty())
    {
        const bool present = std::any_of(
            g_projects.items.begin(), g_projects.items.end(),
            [&](const auto &item) { return item.first == current; });
        if (!present)
            g_projects.items.emplace_back(current, state.studio_project.title);
    }
    g_projects_dirty = false;
}

void switch_project(AppState &state, const std::string &id)
{
    if (id.empty() || id == state.studio_project.id)
        return;
    save_now(state);
    SongProject project;
    if (!load_song_project(id, project))
        return;
    project.beat = state.studio_beat;
    state.studio_project = std::move(project);
    state.studio_project_loaded = true;
    state.studio_project_dirty = false;
    state.studio_undo.clear();
    g_tl.zoom = state.studio_project.zoom > 1.0f ? state.studio_project.zoom : 90.0f;
    g_tl.following = true;
    g_selected_cue_id.clear();
    g_projects_dirty = true;
}

void new_project(AppState &state)
{
    if (state.studio_beat.id.empty())
        return;
    save_now(state);
    SongProject project;
    project.id = new_project_id(state.studio_beat.id);
    project.title = state.studio_beat.title;
    project.beat = state.studio_beat;
    project.zoom = g_tl.zoom;
    project.following = g_tl.following;
    project.snap = state.studio_project.snap;
    state.studio_project = std::move(project);
    state.studio_project_loaded = true;
    state.studio_project_dirty = true;
    state.studio_undo.clear();
    g_selected_cue_id.clear();
    mark_dirty(state);
    g_projects_dirty = true;
}

// ---- Cue/lyric helpers ----
Cue *find_cue(SongProject &p, const std::string &id)
{
    for (Cue &c : p.cues)
        if (c.id == id)
            return &c;
    return nullptr;
}
LyricLine *find_line(SongProject &p, const std::string &id)
{
    for (LyricLine &l : p.lyrics)
        if (l.id == id)
            return &l;
    return nullptr;
}

void link_line_cue(SongProject &p, const std::string &line_id, const std::string &cue_id)
{
    LyricLine *line = find_line(p, line_id);
    Cue *cue = find_cue(p, cue_id);
    if (!line || !cue)
        return;
    // Break the line's previous link.
    if (!line->cue_id.empty())
    {
        if (Cue *old = find_cue(p, line->cue_id))
            old->lyric_id.clear();
    }
    // Break the cue's previous line link.
    if (!cue->lyric_id.empty() && cue->lyric_id != line_id)
    {
        if (LyricLine *old = find_line(p, cue->lyric_id))
            old->cue_id.clear();
    }
    line->cue_id = cue_id;
    cue->lyric_id = line_id;
}

void unlink_cue(SongProject &p, Cue &cue)
{
    if (!cue.lyric_id.empty())
    {
        if (LyricLine *line = find_line(p, cue.lyric_id))
            line->cue_id.clear();
        cue.lyric_id.clear();
    }
}

void unlink_line(SongProject &p, LyricLine &line)
{
    if (!line.cue_id.empty())
    {
        if (Cue *cue = find_cue(p, line.cue_id))
            cue->lyric_id.clear();
        line.cue_id.clear();
    }
}

double snap_time(const AppState &state, double t)
{
    if (!state.studio_project.snap)
        return t;
    auto analysis = std::atomic_load(&state.studio_analysis);
    if (!analysis || !analysis->ok || analysis->id != state.studio_beat.id ||
        analysis->beats.empty())
        return t;
    auto it = std::lower_bound(analysis->beats.begin(), analysis->beats.end(), t);
    double best = t;
    double best_d = 1.0e9;
    if (it != analysis->beats.end())
    {
        const double d = std::fabs(*it - t);
        if (d < best_d)
        {
            best_d = d;
            best = *it;
        }
    }
    if (it != analysis->beats.begin())
    {
        const double d = std::fabs(*std::prev(it) - t);
        if (d < best_d)
            best = *std::prev(it);
    }
    return best;
}

void add_cue_at(AppState &state, double t)
{
    if (!state.studio_project_loaded)
        return;
    Cue cue;
    cue.id = new_studio_id("cue");
    cue.time = std::max(0.0, snap_time(state, t));
    cue.type = 1; // Section
    cue.color = (int)state.studio_project.cues.size() % kCueColorCount;
    cue.label = "Section " + std::to_string(state.studio_project.cues.size() + 1);
    state.studio_project.cues.push_back(cue);
    g_selected_cue_id = cue.id;
    mark_dirty(state);
}

void set_loop_point(AppState &state, bool start)
{
    SongProject &p = state.studio_project;
    if (!state.studio_project_loaded)
        return;
    push_undo(state);
    const double t = std::max(0.0, snap_time(state, g_tl.view_time));
    if (start)
        p.loop_a = t;
    else
        p.loop_b = t;
    if (p.loop_a >= 0.0 && p.loop_b > p.loop_a + 0.05)
        p.loop_enabled = true;
    mark_dirty(state);
}

// Studio-only keys. Ignored while a text field has focus so typing lyrics
// never turns into commands.
void handle_studio_shortcuts(AppState &state, MpvPlayer &player)
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return;

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    {
        undo_project(state);
        return;
    }
    if (state.studio_project_loaded && ImGui::IsKeyPressed(ImGuiKey_C, false))
    {
        push_undo(state);
        add_cue_at(state, std::max(0.0, g_tl.view_time));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false))
        set_loop_point(state, true);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false))
        set_loop_point(state, false);
    if (state.studio_project_loaded && ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        g_tl.following = true;
        if (state.current_track_id == state.studio_beat.id)
            g_tl.view_time = std::max(0.0, player.cached_time_pos());
    }
}

void delete_cue(AppState &state, const std::string &id)
{
    SongProject &p = state.studio_project;
    if (Cue *cue = find_cue(p, id))
        unlink_cue(p, *cue);
    p.cues.erase(std::remove_if(p.cues.begin(), p.cues.end(),
                                [&](const Cue &c) { return c.id == id; }),
                 p.cues.end());
    if (g_selected_cue_id == id)
        g_selected_cue_id.clear();
    mark_dirty(state);
}

int nearest_unlinked_cue(const SongProject &p, double t)
{
    int best = -1;
    double best_d = 1.0e9;
    for (int i = 0; i < (int)p.cues.size(); ++i)
    {
        if (!p.cues[i].lyric_id.empty())
            continue;
        const double d = std::fabs(p.cues[i].time - t);
        if (d < best_d)
        {
            best_d = d;
            best = i;
        }
    }
    return best;
}

// ---- Drawing ----
void draw_transport(AppState &state, MpvPlayer &player)
{
    const Track &beat = state.studio_beat;
    const float content_w = std::max(1.0f, ImGui::GetContentRegionAvail().x);

    const bool current = !beat.id.empty() && state.current_track_id == beat.id;
    const bool playing = current && state.playing.load();

    if (ui_icon_button("##beat_play", playing ? UiIcon::Pause : UiIcon::Play,
                       ui_px(38.0f), false, 0, playing ? "Pause" : "Play"))
    {
        if (!current)
        {
            ui_play_track(state, beat, -1);
            g_tl.following = true;
        }
        else if (playing)
        {
            state.user_pause_requested = true;
        }
        else
        {
            state.user_play_requested = true;
            g_tl.following = true; // resume with the timeline scrolling again
        }
    }

    ImGui::SameLine();
    const double pos = current ? std::max(0.0, player.cached_time_pos()) : 0.0;
    if (ui_icon_button("##beat_back", UiIcon::Prev, ui_px(34.0f), false, 0, "Back 5s") && current)
        player.seek(std::max(0.0, pos - 5.0));
    ImGui::SameLine();
    if (ui_icon_button("##beat_fwd", UiIcon::Next, ui_px(34.0f), false, 0, "Forward 5s") && current)
        player.seek(pos + 5.0);

    ImGui::SameLine();
    const double dur = current && player.cached_duration() > 0.0
                           ? player.cached_duration()
                           : (double)beat.duration_sec;
    ImGui::TextColored(ui_c().text_dim, "%s / %s", time_text(pos).c_str(),
                       time_text(dur).c_str());

    ImGui::SameLine();
    if (ui_secondary_button(g_tl.following ? "Following" : "Follow"))
    {
        g_tl.following = true;
        if (current)
            g_tl.view_time = pos;
        mark_dirty(state);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep the playhead on the current playback position");

    ImGui::SameLine();
    if (ui_ghost_button("Fit"))
    {
        auto a = std::atomic_load(&state.studio_analysis);
        const double d = a && a->ok ? a->duration : (double)beat.duration_sec;
        if (d > 0.0)
        {
            g_tl.zoom = std::clamp((float)(content_w / d), 6.0f, 4000.0f);
            g_tl.view_time = std::clamp(d * 0.33, 0.0, d);
            g_tl.following = false;
            mark_dirty(state);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Zoom so the whole beat fits the timeline");

    ImGui::SameLine();
    if (ui_ghost_button("Re-analyze"))
    {
        std::remove(studio_analysis_cache_path(beat.id).c_str());
        request_analysis(state);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Discard the cached waveform/beat grid and analyse again");

    auto analysis = std::atomic_load(&state.studio_analysis);
    if (analysis && analysis->ok && analysis->id == beat.id)
    {
        ImGui::SameLine();
        ImGui::TextColored(ui_c().text_muted, "  BPM %.0f  \xC2\xB7  %d beats",
                           analysis->bpm, (int)analysis->beats.size());
    }
}

void draw_loop_controls(AppState &state)
{
    SongProject &p = state.studio_project;
    ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
    ImGui::TextUnformatted("LOOP");
    ImGui::PopStyleColor();

    if (ui_ghost_button("A (in)"))
        set_loop_point(state, true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Set the loop start at the playhead  ( [ )");

    ImGui::SameLine();
    if (ui_ghost_button("B (out)"))
        set_loop_point(state, false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Set the loop end at the playhead  ( ] )");

    ImGui::SameLine();
    bool on = p.loop_enabled;
    if (ImGui::Checkbox("Loop", &on))
    {
        push_undo(state);
        p.loop_enabled = on && p.loop_a >= 0.0 && p.loop_b > p.loop_a + 0.05;
        mark_dirty(state);
    }

    ImGui::SameLine();
    if (ui_ghost_button("Clear loop"))
    {
        push_undo(state);
        p.loop_a = -1.0;
        p.loop_b = -1.0;
        p.loop_enabled = false;
        mark_dirty(state);
    }

    if (p.loop_a >= 0.0 || p.loop_b >= 0.0)
    {
        ImGui::SameLine();
        ImGui::TextColored(ui_c().text_muted, "  A %s   B %s",
                           p.loop_a >= 0.0 ? time_text(p.loop_a).c_str() : "--",
                           p.loop_b >= 0.0 ? time_text(p.loop_b).c_str() : "--");
    }
}

void draw_timeline(AppState &state, MpvPlayer &player)
{
    const UiMetrics &m = ui_m();
    auto analysis = std::atomic_load(&state.studio_analysis);
    const bool ready = analysis && analysis->ok && analysis->id == state.studio_beat.id;

    const float avail_w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float tl_h = std::max(140.0f, 230.0f * m.layout_scale);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##studio_timeline", ImVec2(avail_w, tl_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const float w = std::max(1.0f, size.x);
    const float h = std::max(1.0f, size.y);
    ImGui::InvisibleButton("##tl_hit", ImVec2(w, h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), m.radius_md);
    dl->AddRect(p0, p1, ui_hairline_u32(), m.radius_md, 0, 1.0f);
    dl->PushClipRect(p0, p1, true);

    const double dur = ready ? analysis->duration : 0.0;
    const double raw_pos = ready ? std::max(0.0, player.cached_time_pos()) : 0.0;

    // mpv's time-pos observer updates coarsely (tens of ms). Glide the display
    // position with wall-clock time between updates so the waveform moves
    // smoothly instead of stepping at the observer's cadence. Bounded so it
    // can never drift far from what is actually audible.
    static double s_smooth_pos = 0.0;
    static double s_last_raw_pos = -1.0;
    double pos = raw_pos;
    const bool playing_here = ready && state.playing.load() &&
                              state.current_track_id == state.studio_beat.id;
    if (playing_here && g_tl.following)
    {
        const double dt = std::clamp((double)ImGui::GetIO().DeltaTime, 0.0, 0.1);
        const bool beat_changed = g_tl.beat_id != state.studio_beat.id;
        if (s_last_raw_pos < 0.0 || beat_changed ||
            std::fabs(raw_pos - s_last_raw_pos) > 0.25)
            s_smooth_pos = raw_pos; // seek / track change: snap
        else
        {
            s_smooth_pos += dt;
            s_smooth_pos = std::clamp(s_smooth_pos, raw_pos - 0.05, raw_pos + 0.12);
        }
        pos = dur > 0.0 ? std::clamp(s_smooth_pos, 0.0, dur) : s_smooth_pos;
        s_last_raw_pos = raw_pos;
    }
    else
    {
        s_smooth_pos = raw_pos;
        s_last_raw_pos = raw_pos;
    }

    // Keep mpv's A-B loop in sync with the project (also clears it when the
    // beat/project changes).
    static std::string s_loop_sig;
    char loop_sig[128];
    std::snprintf(loop_sig, sizeof(loop_sig), "%s|%.3f|%.3f|%d",
                  state.studio_project.id.c_str(), state.studio_project.loop_a,
                  state.studio_project.loop_b, state.studio_project.loop_enabled ? 1 : 0);
    if (s_loop_sig != loop_sig)
    {
        s_loop_sig = loop_sig;
        apply_loop(player, state.studio_project);
    }

    if (g_tl.beat_id != state.studio_beat.id)
    {
        g_tl = TimelineView{};
        g_tl.beat_id = state.studio_beat.id;
        g_pending_scrub = -1.0;
    }

    const float playhead_off = w * 0.33f;
    const float playhead_x = p0.x + playhead_off;
    const float ruler_h = 22.0f * m.layout_scale;
    const float cue_lane_h = 20.0f * m.layout_scale;
    const float lane_top = p0.y + ruler_h;
    const float lane_bot = lane_top + cue_lane_h;
    dl->AddRectFilled(ImVec2(p0.x, lane_top), ImVec2(p1.x, lane_bot),
                      ui_overlay_u32(0.025f));
    dl->AddLine(ImVec2(p0.x, lane_bot), ImVec2(p1.x, lane_bot), ui_hairline_u32(), 1.0f);

    const float wave_top = lane_bot;
    const float mid = wave_top + (p1.y - wave_top) * 0.5f;
    const float half = std::max(8.0f, (p1.y - wave_top) * 0.47f);

    if (!ready)
    {
        const char *msg = state.studio_analyzing && !state.studio_beat.id.empty()
                              ? "Analysing waveform and beats..."
                              : (state.studio_beat.id.empty()
                                     ? "Pick a beat to see its waveform"
                                     : "No waveform yet");
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + (h - ts.y) * 0.5f),
                    ui_text_muted_u32(), msg);
        if (!state.studio_analysis_error.empty() && !state.studio_beat.id.empty())
        {
            const ImVec2 es = ImGui::CalcTextSize(state.studio_analysis_error.c_str());
            dl->AddText(ImVec2(p0.x + (w - es.x) * 0.5f, p0.y + h * 0.5f + 22.0f),
                        ui_col(g_accent_text), state.studio_analysis_error.c_str());
        }
        dl->PopClipRect();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    ImGuiIO &io = ImGui::GetIO();

    enum class DragMode
    {
        None,
        Scrub,
        Pan,
        Cue
    };
    static DragMode s_drag = DragMode::None;
    static std::string s_drag_cue;
    static bool s_was_following = true;
    static double s_last_scrub = 0.0;
    static bool s_scrub_paused_before = false;
    static bool s_scrub_moved = false;
    static double s_last_seek_time = -1.0;

    // ---- Input: zoom / pan / scrub / cue drag ----
    if (hovered && io.MouseWheel != 0.0f)
    {
        // view_time stays fixed: the time under the playhead does not move.
        // Wheel up (positive in ImGui) zooms in.
        g_tl.zoom = std::clamp(g_tl.zoom * std::exp(io.MouseWheel * 0.25f),
                               6.0f, 4000.0f);
        mark_dirty(state);
    }

    // Freeze the view while a cue is dragged so it cannot slide under the
    // mouse as playback advances.
    if (g_tl.following && s_drag != DragMode::Cue && pos >= 0.0)
        g_tl.view_time = pos;

    auto time_at = [&](float mouse_x) {
        return std::clamp(g_tl.view_time + (mouse_x - playhead_x) / g_tl.zoom, 0.0, dur);
    };
    auto seek_at = [&](float mouse_x) {
        // Never seek a different track that happens to be playing.
        if (state.current_track_id != state.studio_beat.id)
            return;
        s_last_scrub = time_at(mouse_x);
        // The beat may still be resolving/loading: remember the target and
        // apply it once the player is ready instead of seeking the old track.
        if (state.loading.load())
        {
            g_pending_scrub = s_last_scrub;
            return;
        }
        player.seek(s_last_scrub);
    };

    // Apply a seek queued while the beat was loading.
    if (g_pending_scrub >= 0.0 && !state.loading.load() &&
        state.current_track_id == state.studio_beat.id)
    {
        player.seek(g_pending_scrub);
        g_pending_scrub = -1.0;
    }
    auto cue_hit = [&](float mouse_x) -> int {
        int best = -1;
        float best_d = 8.0f;
        for (int i = 0; i < (int)state.studio_project.cues.size(); ++i)
        {
            const float cx = playhead_x +
                             (float)((state.studio_project.cues[i].time - g_tl.view_time) * g_tl.zoom);
            const float d = std::fabs(mouse_x - cx);
            if (d <= best_d)
            {
                best_d = d;
                best = i;
            }
        }
        return best;
    };

    const bool in_cue_lane = io.MousePos.y >= lane_top && io.MousePos.y <= lane_bot;
    const bool pan_mod = io.KeyShift; // Shift-drag (or middle-drag) pans

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && in_cue_lane)
    {
        push_undo(state);
        add_cue_at(state, time_at(io.MousePos.x));
        s_drag = DragMode::None;
    }
    else if (ImGui::IsItemClicked(ImGuiMouseButton_Middle))
    {
        s_was_following = g_tl.following;
        g_tl.following = false;
        s_drag = DragMode::Pan;
    }
    else if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
    {
        s_was_following = g_tl.following;
        s_drag_cue.clear();
        const int hit = in_cue_lane ? cue_hit(io.MousePos.x) : -1;
        if (hit >= 0)
        {
            s_drag = DragMode::Cue;
            s_drag_cue = state.studio_project.cues[hit].id;
            g_selected_cue_id = s_drag_cue;
            push_undo(state);
        }
        else if (pan_mod)
        {
            g_selected_cue_id.clear();
            g_tl.following = false;
            s_drag = DragMode::Pan;
        }
        else
        {
            // Click or drag anywhere on the waveform/ruler seeks there.
            g_selected_cue_id.clear();
            g_tl.following = false;
            s_drag = DragMode::Scrub;
            s_scrub_paused_before =
                (state.current_track_id == state.studio_beat.id) && !state.playing.load();
            s_scrub_moved = false;
            s_last_seek_time = -1.0;
            if (state.current_track_id != state.studio_beat.id)
            {
                // Clicking the waveform can start the beat at that point.
                ui_play_track(state, state.studio_beat, -1);
                g_pending_scrub = time_at(io.MousePos.x);
            }
            else
                seek_at(io.MousePos.x);
        }
    }

    if (s_drag != DragMode::None && active)
    {
        if (s_drag == DragMode::Pan && io.MouseDelta.x != 0.0f)
        {
            g_tl.view_time -= io.MouseDelta.x / g_tl.zoom;
            g_tl.following = false;
        }
        else if (s_drag == DragMode::Scrub &&
                 (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
        {
            // Make the scrub audible: if playback was paused, start it on the
            // first real movement so each seek plays a snippet (vinyl-ish).
            if (!s_scrub_moved)
            {
                s_scrub_moved = true;
                if (s_scrub_paused_before &&
                    state.current_track_id == state.studio_beat.id)
                    state.user_play_requested = true;
                player.set_property("hr-seek", "yes"); // precise scrub seeks
            }
            // Throttle: mpv seeks are not free; ~28 per second is plenty and
            // keeps the UI responsive.
            const double now = ImGui::GetTime();
            if (s_last_seek_time < 0.0 || now - s_last_seek_time >= 0.035)
            {
                s_last_seek_time = now;
                seek_at(io.MousePos.x);
            }
        }
        else if (s_drag == DragMode::Cue && io.MouseDelta.x != 0.0f)
        {
            if (Cue *cue = find_cue(state.studio_project, s_drag_cue))
            {
                cue->time = std::clamp(snap_time(state, time_at(io.MousePos.x)), 0.0, dur);
                mark_dirty(state);
            }
        }
    }
    if (s_drag != DragMode::None && ImGui::IsItemDeactivated())
    {
        if (s_drag == DragMode::Cue)
        {
            if (const Cue *cue = find_cue(state.studio_project, s_drag_cue))
                player.seek(cue->time);
        }
        else if (s_drag == DragMode::Scrub)
        {
            seek_at(io.MousePos.x); // final position
            if (s_scrub_moved && s_scrub_paused_before &&
                state.current_track_id == state.studio_beat.id)
                state.user_pause_requested = true; // restore the paused state
            s_last_seek_time = -1.0;
            if (s_was_following)
                g_tl.following = true; // re-anchor on playback next frame
        }
        else if (s_drag == DragMode::Pan)
        {
            mark_dirty(state); // persist the new view
        }
        s_drag = DragMode::None;
        s_drag_cue.clear();
    }
    (void)s_last_scrub;
    g_tl.view_time = std::clamp(g_tl.view_time, 0.0, dur);

    const double t_left = g_tl.view_time - playhead_off / g_tl.zoom;
    const double t_right = g_tl.view_time + (w - playhead_off) / g_tl.zoom;

    // ---- A-B loop region ----
    if (state.studio_project.loop_a >= 0.0 &&
        state.studio_project.loop_b > state.studio_project.loop_a)
    {
        const float xa = playhead_x +
                         (float)((state.studio_project.loop_a - g_tl.view_time) * g_tl.zoom);
        const float xb = playhead_x +
                         (float)((state.studio_project.loop_b - g_tl.view_time) * g_tl.zoom);
        const float lx = std::max(p0.x, xa);
        const float rx = std::min(p1.x, xb);
        if (rx > lx)
        {
            const float a = state.studio_project.loop_enabled ? 0.10f : 0.045f;
            dl->AddRectFilled(ImVec2(lx, wave_top), ImVec2(rx, p1.y), ui_accent_u32(a));
        }
        if (xa >= p0.x && xa <= p1.x)
            dl->AddLine(ImVec2(xa, wave_top), ImVec2(xa, p1.y),
                        ui_accent_u32(0.55f), 1.2f);
        if (xb >= p0.x && xb <= p1.x)
            dl->AddLine(ImVec2(xb, wave_top), ImVec2(xb, p1.y),
                        ui_accent_u32(0.55f), 1.2f);
    }

    // ---- Waveform ----
    const StudioWaveLevel *level = nullptr;
    for (const StudioWaveLevel &lv : analysis->waves)
    {
        level = &lv;
        if ((float)lv.buckets >= w)
            break;
    }
    if (level && level->buckets > 0 && dur > 0.0)
    {
        const int x0 = (int)std::floor(p0.x);
        const int x1 = (int)std::ceil(p1.x);
        for (int px = x0; px < x1; ++px)
        {
            const double t = t_left + (px - p0.x) / g_tl.zoom;
            if (t < 0.0 || t > dur)
                continue;
            int b = (int)(t / dur * level->buckets);
            b = std::clamp(b, 0, level->buckets - 1);
            float lo = level->mins[b];
            float hi = level->maxs[b];
            float y0 = mid - hi * half;
            float y1 = mid - lo * half;
            if (y0 > y1)
                std::swap(y0, y1);
            if (y1 - y0 < 1.0f)
                y1 = y0 + 1.0f;
            const ImU32 col = ((float)px < playhead_x)
                                  ? ui_accent_u32(0.80f)
                                  : ui_with_alpha(ui_text_dim_u32(), 0.55f);
            dl->AddLine(ImVec2((float)px, y0), ImVec2((float)px, y1), col, 1.0f);
        }
    }

    // ---- Beat grid ----
    if (analysis->beats.size() >= 2)
    {
        const double period = analysis->beats[1] - analysis->beats[0];
        if (period > 0.0 && period * g_tl.zoom > 3.0)
        {
            for (double bt : analysis->beats)
            {
                if (bt < t_left - 1.0 || bt > t_right + 1.0)
                    continue;
                const float bx = playhead_x + (float)((bt - g_tl.view_time) * g_tl.zoom);
                dl->AddLine(ImVec2(bx, wave_top), ImVec2(bx, p1.y),
                            ui_with_alpha(ui_text_muted_u32(), 0.12f), 1.0f);
            }
            for (double bt : analysis->downbeats)
            {
                if (bt < t_left - 1.0 || bt > t_right + 1.0)
                    continue;
                const float bx = playhead_x + (float)((bt - g_tl.view_time) * g_tl.zoom);
                dl->AddLine(ImVec2(bx, wave_top), ImVec2(bx, p1.y),
                            ui_with_alpha(ui_accent_u32(1.0f), 0.20f), 1.0f);
            }
        }
    }

    // ---- Time ruler ----
    static const double kSteps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300};
    double step = kSteps[sizeof(kSteps) / sizeof(kSteps[0]) - 1];
    for (double candidate : kSteps)
    {
        if (candidate * g_tl.zoom >= 72.0)
        {
            step = candidate;
            break;
        }
    }
    const double first_tick = std::floor(std::max(0.0, t_left) / step) * step;
    for (double t = first_tick; t <= t_right; t += step)
    {
        const float tx = playhead_x + (float)((t - g_tl.view_time) * g_tl.zoom);
        dl->AddLine(ImVec2(tx, lane_top - 5.0f), ImVec2(tx, lane_top),
                    ui_text_muted_u32(), 1.0f);
        dl->AddText(ImVec2(tx + 3.0f, p0.y + 2.0f),
                    ui_with_alpha(ui_text_dim_u32(), 0.8f), time_text(t).c_str());
    }
    dl->AddLine(ImVec2(p0.x, lane_top), ImVec2(p1.x, lane_top), ui_hairline_u32(), 1.0f);

    // ---- Cues ----
    for (const Cue &cue : state.studio_project.cues)
    {
        if (cue.time < t_left - 1.0 || cue.time > t_right + 1.0)
            continue;
        const float cx = playhead_x + (float)((cue.time - g_tl.view_time) * g_tl.zoom);
        const bool selected = cue.id == g_selected_cue_id;
        const ImU32 col = cue_color(cue.color);
        dl->AddLine(ImVec2(cx, lane_top + 2.0f), ImVec2(cx, lane_bot),
                    ui_with_alpha(col, selected ? 1.0f : 0.85f), selected ? 2.5f : 1.8f);
        dl->AddTriangleFilled(ImVec2(cx - 5.0f, lane_top + 1.0f),
                              ImVec2(cx + 5.0f, lane_top + 1.0f),
                              ImVec2(cx, lane_top + 8.0f), col);
        if (selected)
            dl->AddRect(ImVec2(cx - 7.0f, lane_top), ImVec2(cx + 7.0f, lane_bot),
                        ui_accent_u32(0.5f), 3.0f, 0, 1.0f);
        if (!cue.label.empty())
        {
            const float label_x = cx + 7.0f;
            const float label_w = ImGui::CalcTextSize(cue.label.c_str()).x;
            if (label_x + label_w < p1.x)
                dl->AddText(ImVec2(label_x, lane_top + 3.0f),
                            ui_with_alpha(col, 0.9f), cue.label.c_str());
        }
    }

    // ---- Playhead (stationary) ----
    dl->AddLine(ImVec2(playhead_x, p0.y), ImVec2(playhead_x, p1.y),
                ui_accent_u32(0.95f), 1.6f);
    dl->AddTriangleFilled(ImVec2(playhead_x - 6.0f, lane_top - 6.0f),
                          ImVec2(playhead_x + 6.0f, lane_top - 6.0f),
                          ImVec2(playhead_x, lane_top),
                          ui_accent_u32(0.95f));

    dl->PopClipRect();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void draw_cue_list(AppState &state, MpvPlayer &player)
{
    const UiMetrics &m = ui_m();
    SongProject &p = state.studio_project;
    const std::string meta = std::to_string(p.cues.size()) +
                             (p.cues.size() == 1 ? " cue" : " cues");
    if (ui_section_header("Cues", meta.c_str()))
    {
        // The header action column is free-form; the explicit Add button is
        // below so the label can be clearer.
    }

    if (ui_primary_button("Add cue at playhead"))
    {
        push_undo(state);
        add_cue_at(state, std::max(0.0, g_tl.view_time));
    }
    ImGui::SameLine();
    bool snap = p.snap;
    if (ImGui::Checkbox("Snap to beat", &snap))
    {
        push_undo(state);
        p.snap = snap;
        mark_dirty(state);
    }

    if (p.cues.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
        ImGui::TextUnformatted("No cues yet. Add one at the playhead, or double-click the cue lane.");
        ImGui::PopStyleColor();
        return;
    }

    std::vector<int> order(p.cues.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return p.cues[a].time < p.cues[b].time; });

    const float list_h = std::min(240.0f * m.layout_scale,
                                  (float)p.cues.size() * 30.0f * m.layout_scale + 8.0f);
    ImGui::BeginChild("##cue_list", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders);

    std::string delete_id;
    for (int idx : order)
    {
        if (idx < 0 || idx >= (int)p.cues.size())
            continue;
        Cue &cue = p.cues[idx];
        ImGui::PushID(cue.id.c_str());

        if (ImGui::SmallButton(time_text(cue.time).c_str()))
        {
            player.seek(cue.time);
            g_selected_cue_id = cue.id;
        }
        ImGui::SameLine();

        if (ImGui::ColorButton("##color", cue_color_vec(cue.color),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f)))
        {
            push_undo(state);
            cue.color = (cue.color + 1) % kCueColorCount;
            mark_dirty(state);
        }
        ImGui::SameLine();

        ImGui::SetNextItemWidth(120.0f * m.text_scale);
        const bool label_edited = ImGui::InputText("##label", &cue.label);
        if (ImGui::IsItemActivated())
            push_undo(state);
        if (label_edited)
            mark_dirty(state);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(100.0f * m.text_scale);
        int type = cue.type;
        const bool type_edited = ImGui::Combo("##type", &type, kCueTypes, 3);
        if (ImGui::IsItemActivated())
            push_undo(state);
        if (type_edited)
        {
            cue.type = type;
            mark_dirty(state);
        }
        ImGui::SameLine();

        if (!cue.lyric_id.empty())
        {
            if (ImGui::SmallButton("unlink"))
            {
                push_undo(state);
                unlink_cue(p, cue);
                mark_dirty(state);
            }
            ImGui::SameLine();
        }
        else
        {
            ImGui::TextColored(ui_c().text_muted, "\xC2\xB7");
            ImGui::SameLine();
        }

        if (ImGui::SmallButton("x"))
        {
            push_undo(state);
            delete_id = cue.id;
        }

        ImGui::PopID();
    }
    if (!delete_id.empty())
        delete_cue(state, delete_id);

    ImGui::EndChild();
}

void draw_lyrics(AppState &state, MpvPlayer &player)
{
    const UiMetrics &m = ui_m();
    SongProject &p = state.studio_project;
    const std::string meta = std::to_string(p.lyrics.size()) +
                             (p.lyrics.size() == 1 ? " line" : " lines");
    if (ui_section_header("Lyrics", meta.c_str()))
    {
    }

    if (ui_primary_button("Add line"))
    {
        push_undo(state);
        LyricLine line;
        line.id = new_studio_id("lyr");
        p.lyrics.push_back(std::move(line));
        mark_dirty(state);
    }

    const double pos = std::max(0.0, player.cached_time_pos());
    if (g_lyrics_project != p.id)
    {
        g_lyrics_project = p.id;
        g_last_active = -2;
    }
    int active = -1;
    double active_t = -1.0;
    for (int i = 0; i < (int)p.lyrics.size(); ++i)
    {
        const LyricLine &line = p.lyrics[i];
        if (line.cue_id.empty())
            continue;
        if (const Cue *cue = find_cue(p, line.cue_id))
        {
            if (cue->time <= pos && cue->time > active_t)
            {
                active_t = cue->time;
                active = i;
            }
        }
    }

    if (p.lyrics.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
        ImGui::TextUnformatted("No lyrics yet. Add a line, then link it to a cue.");
        ImGui::PopStyleColor();
        return;
    }

    const float list_h = std::min(320.0f * m.layout_scale,
                                  (float)p.lyrics.size() * 42.0f * m.layout_scale + 8.0f);
    ImGui::BeginChild("##lyric_list", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders);

    std::string delete_id;
    for (int i = 0; i < (int)p.lyrics.size(); ++i)
    {
        LyricLine &line = p.lyrics[i];
        ImGui::PushID(line.id.c_str());

        const Cue *cue = line.cue_id.empty() ? nullptr : find_cue(p, line.cue_id);
        if (cue)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, cue_color_vec(cue->color));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
        }

        const bool is_active = (i == active);
        if (is_active)
            ImGui::PushStyleColor(ImGuiCol_Text, g_accent_text);

        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 250.0f * m.layout_scale));
        const bool line_edited = ImGui::InputText("##line", &line.text);
        if (ImGui::IsItemActivated())
            push_undo(state);
        if (line_edited)
            mark_dirty(state);
        if (is_active)
            ImGui::PopStyleColor();
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (cue)
        {
            if (ImGui::SmallButton(time_text(cue->time).c_str()))
            {
                player.seek(cue->time);
                g_selected_cue_id = cue->id;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("unlink"))
            {
                push_undo(state);
                unlink_line(p, line);
                mark_dirty(state);
            }
        }
        else
        {
            if (ImGui::SmallButton("link"))
            {
                push_undo(state);
                const double t = std::max(0.0, g_tl.view_time);
                int idx = nearest_unlinked_cue(p, t);
                if (idx < 0)
                {
                    add_cue_at(state, t);
                    idx = (int)p.cues.size() - 1;
                }
                if (idx >= 0)
                    link_line_cue(p, line.id, p.cues[idx].id);
                mark_dirty(state);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("cue here"))
            {
                push_undo(state);
                const double t = std::max(0.0, g_tl.view_time);
                add_cue_at(state, t);
                if (!p.cues.empty())
                    link_line_cue(p, line.id, p.cues.back().id);
                mark_dirty(state);
            }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
        {
            push_undo(state);
            delete_id = line.id;
        }

        if (is_active && active != g_last_active)
            ImGui::SetScrollHereY(0.5f);

        ImGui::PopID();
    }
    g_last_active = active;
    if (!delete_id.empty())
    {
        if (LyricLine *line = find_line(p, delete_id))
            unlink_line(p, *line);
        p.lyrics.erase(std::remove_if(p.lyrics.begin(), p.lyrics.end(),
                                      [&](const LyricLine &l) { return l.id == delete_id; }),
                       p.lyrics.end());
        mark_dirty(state);
    }

    ImGui::EndChild();
}

void draw_local_songs(AppState &state)
{
    const UiMetrics &m = ui_m();
    ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
    ImGui::TextUnformatted("LOCAL SONGS");
    ImGui::PopStyleColor();

    if (ui_primary_button("Add local songs..."))
    {
        const std::vector<std::string> files = pick_audio_files();
        if (!files.empty())
        {
            bool changed = false;
            for (const std::string &path : files)
            {
                if (path.empty())
                    continue;
                Track track;
                if (!build_local_track(path, track))
                    continue;
                const bool exists = std::any_of(
                    state.local_tracks.begin(), state.local_tracks.end(),
                    [&](const Track &t) { return t.id == track.id; });
                if (exists)
                    continue;
                state.local_tracks.push_back(std::move(track));
                changed = true;
            }
            if (changed)
                library::save_local_tracks(state.local_tracks);
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 80.0f * m.layout_scale));
    const bool submitted = ImGui::InputTextWithHint(
        "##local_path", "Paste a path to an audio file", g_local_path, sizeof(g_local_path),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Add") || submitted)
    {
        Track track;
        if (build_local_track(g_local_path, track))
        {
            const bool exists = std::any_of(
                state.local_tracks.begin(), state.local_tracks.end(),
                [&](const Track &t) { return t.id == track.id; });
            if (!exists)
            {
                state.local_tracks.push_back(std::move(track));
                library::save_local_tracks(state.local_tracks);
            }
        }
        g_local_path[0] = '\0';
    }

    if (state.local_tracks.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
        ImGui::TextUnformatted("No local songs yet.");
        ImGui::PopStyleColor();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
    ImGui::TextUnformatted("Double-click a song to use it as the beat.");
    ImGui::PopStyleColor();

    // Reuse the shared track row so local songs look and behave like every
    // other list in the app.
    const float art = ui_elem("list_thumb", 48.0f);
    const float pitch = ui_track_row_pitch(art);
    const float list_h = std::min(260.0f * m.layout_scale,
                                  std::max(pitch, (float)state.local_tracks.size() * pitch) + 8.0f);
    ImGui::BeginChild("##local_songs", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders);

    std::string remove_id;
    for (int i = 0; i < (int)state.local_tracks.size(); ++i)
    {
        const Track &track = state.local_tracks[i];
        const bool missing = !file_exists(track.url);
        const bool is_beat = state.studio_beat.id == track.id;

        TrackRowOptions opt;
        opt.art_size = art;
        opt.show_duration = !missing;
        opt.show_delete = true;
        opt.delete_label = "Remove";
        opt.show_context_menu = false; // no YouTube-only actions on local files
        opt.auto_play_double_click = false;
        opt.selected = is_beat;
        if (missing)
        {
            opt.show_duration = false;
            opt.right_text = "missing";
            opt.secondary = track.artist.empty() ? "File missing"
                                                 : track.artist + "  \xC2\xB7  file missing";
        }

        TrackRowResult row = ui_track_row(state, track, i, opt);
        if (row.double_clicked && !missing)
            set_beat(state, track);
        if (row.deleted)
            remove_id = row.deleted_id;
    }
    if (!remove_id.empty())
    {
        state.local_tracks.erase(
            std::remove_if(state.local_tracks.begin(), state.local_tracks.end(),
                           [&](const Track &t) { return t.id == remove_id; }),
            state.local_tracks.end());
        library::save_local_tracks(state.local_tracks);
        if (state.studio_beat.id == remove_id)
            clear_beat(state);
    }

    ImGui::EndChild();
}

void draw_beat_search(AppState &state)
{
    const UiMetrics &m = ui_m();
    ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
    ImGui::TextUnformatted("SEARCH YOUTUBE MUSIC");
    ImGui::PopStyleColor();

    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 90.0f * m.layout_scale));
    const bool submitted = ImGui::InputTextWithHint(
        "##beat_search", "Search for a beat or song", g_search_query, sizeof(g_search_query),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Search") || submitted)
    {
        if (g_search_query[0])
        {
            state.studio_search_seq.fetch_add(1);
            state.studio_queued_search = g_search_query;
            state.studio_search_loading = true;
            state.studio_search_error.clear();
            state.studio_search_dirty = true;
            state.repaint_requested = true;
        }
    }

    if (state.studio_search_loading.load())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
        ImGui::TextUnformatted("Searching...");
        ImGui::PopStyleColor();
    }
    else if (!state.studio_search_error.empty())
    {
        ImGui::TextColored(g_accent_text, "%s", state.studio_search_error.c_str());
    }

    auto results = std::atomic_load(&state.studio_search_results);
    if (!results || results->empty())
        return;

    ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
    ImGui::TextUnformatted("Double-click a result to use it as the beat.");
    ImGui::PopStyleColor();

    const float art = ui_elem("list_thumb", 44.0f);
    const float pitch = ui_track_row_pitch(art, true);
    const float list_h = std::min(280.0f * m.layout_scale,
                                  std::max(pitch, (float)results->size() * pitch) + 8.0f);
    ImGui::BeginChild("##beat_results", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders);
    for (int i = 0; i < (int)results->size(); ++i)
    {
        const Track &track = (*results)[i];
        const bool is_beat = state.studio_beat.id == track.id;

        TrackRowOptions opt;
        opt.compact = true;
        opt.art_size = art;
        opt.show_duration = true;
        opt.show_context_menu = false;
        opt.auto_play_double_click = false;
        opt.selected = is_beat;
        TrackRowResult row = ui_track_row(state, track, i, opt);
        if (row.double_clicked)
            set_beat(state, track);
    }
    ImGui::EndChild();
}
} // namespace

void draw_studio_panel(AppState &state, MpvPlayer &player)
{
    const UiMetrics &m = ui_m();

    ensure_local_tracks_loaded(state);

    // Restore the most recently edited project once per session.
    if (!g_restore_tried && state.studio_beat.id.empty())
    {
        g_restore_tried = true;
        if (!state.studio_project_loaded)
        {
            SongProject project;
            if (load_latest_song_project(project) && !project.beat.id.empty())
            {
                state.studio_beat = project.beat;
                state.studio_project = std::move(project);
                state.studio_project_loaded = true;
                state.studio_undo.clear();
                g_projects_dirty = true;
                g_tl = TimelineView{};
                g_tl.beat_id = state.studio_beat.id;
                g_tl.zoom = state.studio_project.zoom > 1.0f ? state.studio_project.zoom : 90.0f;
                g_tl.following = true;
                request_analysis(state);
            }
        }
    }

    if (state.studio_project_loaded)
    {
        // Keep the persisted view state current even between debounced saves.
        state.studio_project.zoom = g_tl.zoom;
        state.studio_project.following = g_tl.following;
    }
    save_project_if_due(state);
    handle_studio_shortcuts(state, player);

    if (ui_section_header("Studio", "Songwriting"))
    {
    }

    // ---- Current beat ----
    if (state.studio_beat.id.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
        ImGui::TextUnformatted("NO BEAT SELECTED");
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::PushFont(state.title_font);
        ImGui::TextUnformatted(state.studio_beat.title.empty()
                                   ? "Untitled beat"
                                   : state.studio_beat.title.c_str());
        ImGui::PopFont();
        if (!state.studio_beat.artist.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_dim);
            ImGui::TextUnformatted(state.studio_beat.artist.c_str());
            ImGui::PopStyleColor();
        }

        // Project switcher: several lyric/cue ideas can share one beat.
        if (g_projects_dirty || g_projects.beat_id != state.studio_beat.id)
            refresh_projects(state);
        const std::string current_label = state.studio_project.title.empty()
                                              ? "Untitled project"
                                              : state.studio_project.title;
        const float project_row_w = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(240.0f * m.layout_scale);
        if (ImGui::BeginCombo("##project_select", current_label.c_str()))
        {
            for (const auto &item : g_projects.items)
            {
                ImGui::PushID(item.first.c_str());
                const bool selected = item.first == state.studio_project.id;
                const std::string label = item.second.empty() ? "Untitled" : item.second;
                if (ImGui::Selectable(label.c_str(), selected))
                    switch_project(state, item.first);
                if (selected)
                    ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Selectable("New project"))
                new_project(state);
            ImGui::EndCombo();
        }
        if (project_row_w > 520.0f * m.layout_scale)
            ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f * m.layout_scale);
        if (ImGui::InputTextWithHint("##project_title", "Project title",
                                     &state.studio_project.title))
            mark_dirty(state);

        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        draw_transport(state, player);

        // Actions sit on their own row; the transport row is already full.
        if (!state.current_track_id.empty() && state.current_track_id != state.studio_beat.id)
        {
            if (ui_ghost_button("Use now playing"))
            {
                Track t;
                t.id = state.current_track_id;
                t.title = state.current_title;
                t.artist = state.current_artist;
                t.thumbnail_url = state.current_thumbnail_url;
                t.url = state.current_track_url;
                t.album_id = state.current_album_id;
                t.artist_id = state.current_artist_id;
                t.duration_sec = (int)player.cached_duration();
                set_beat(state, t);
            }
        }
        ImGui::SameLine();
        if (ui_ghost_button("Clear"))
            clear_beat(state);

        ImGui::SameLine();
        ImGui::BeginDisabled(state.studio_undo.empty());
        if (ui_ghost_button("Undo"))
            undo_project(state);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && state.studio_undo.empty())
            ImGui::SetTooltip("Nothing to undo");

        if (state.studio_project_loaded)
        {
            ImGui::Dummy(ImVec2(0.0f, m.space_2));
            draw_loop_controls(state);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_2));

    draw_timeline(state, player);

    if (state.studio_project_loaded)
    {
        ui_push_small();
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
        ImGui::TextUnformatted(
            "Click/drag seek  \xC2\xB7  Shift-drag pan  \xC2\xB7  wheel zoom  "
            "\xC2\xB7  C cue  \xC2\xB7  [ ] loop  \xC2\xB7  F follow  \xC2\xB7  Ctrl+Z undo");
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_3));

    if (!state.studio_project_loaded)
    {
        ui_empty_state(UiIcon::Studio, "No project yet",
                       "Pick a beat from your local songs or search YouTube Music to start "
                       "writing and cueing lyrics.");
    }
    else
    {
        draw_cue_list(state, player);
        ImGui::Dummy(ImVec2(0.0f, m.space_3));
        draw_lyrics(state, player);
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_3));
    draw_local_songs(state);
    ImGui::Dummy(ImVec2(0.0f, m.space_3));
    draw_beat_search(state);

    // Hold the fast render cadence while Studio is visible. The waveform
    // glides under the fixed playhead during playback; without this request
    // the app's power-saving playback cadence (10 FPS) makes it step.
    if (state.studio_project_loaded || state.studio_analyzing.load())
        ui_anim_request();
}
