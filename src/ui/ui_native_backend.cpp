#include "ui.h"

#include "backend/native/http.h"
#include "backend/native/innertube.h"
#include "ui_components.h"
#include "ui_theme.h"
#include "ytdlp.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

// ----------------------------------------------------------------------
// Native backend debug page.
//
// Everything the C++ resolver does, per request: session state, the watch-page
// fetch, the InnerTube player call, the chosen format, cache/join behaviour and
// failures. Deliberately verbose — this is the page to open when a track fails
// to resolve and the Python fallback is suspected.
// ----------------------------------------------------------------------

namespace
{
ImVec4 native_err_col()
{
    return ImVec4(0.95f, 0.42f, 0.42f, 1.0f);
}

std::string fmt_clock(long long epoch_ms)
{
    const std::time_t secs = (std::time_t)(epoch_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

double percentile(std::vector<long long> v, double p)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t idx = (std::size_t)((v.size() - 1) * p);
    return (double)v[idx];
}

void small_hint(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextWrapped("%s", text ? text : "");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
} // namespace

void draw_native_backend_panel(AppState &state)
{
    (void)state;
    const UiMetrics &m = ui_m();
    const std::vector<native::ResolveTrace> traces = native::recent_traces();
    const native::SessionInfo sess = native::session_info();

    ImGui::PushID("native_backend");

    small_hint("The native resolver talks to YouTube directly: one watch-page "
               "request to open a session, then the InnerTube player call with the "
               "JSLess VISIONOS client. No Python, yt-dlp, deno or PoToken is involved. "
               "Every failure falls back to the Python backend.");

    ImGui::Dummy(ImVec2(0.0f, m.space_2));

    bool enabled = native_backend_enabled();
    if (ImGui::Checkbox("Try the native resolver first", &enabled))
        set_native_backend_enabled(enabled);
    small_hint("P100_BACKEND=python overrides this at launch and forces the old backend.");

    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    ImGui::SeparatorText("Transport");
    ImGui::Text("TLS client: %s", native::http_available() ? "OpenSSL (available)" : "unavailable");

    ImGui::SeparatorText("Session");
    ImGui::Text("State: %s", sess.valid ? "valid" : "cold");
    if (sess.valid)
    {
        ImGui::Text("Cookies: %d   visitor: %s...   age: %llds (TTL 3h)", sess.cookies,
                    sess.visitor_prefix.c_str(), sess.age_seconds);
    }
    else
    {
        ImGui::Text("The next resolve fetches the watch page to open a session.");
    }
    if (ui_ghost_button("Reset session", ImVec2(150.0f * m.layout_scale, 0.0f)))
        native::reset_session();

    // ---- statistics ------------------------------------------------------
    int ok = 0, failed = 0, warm = 0, cold = 0;
    std::vector<long long> totals, players;
    for (const native::ResolveTrace &t : traces)
    {
        if (t.error.empty() && !t.status.empty() && t.status == "OK")
            ++ok;
        else
            ++failed;
        if (t.session_hit)
            ++warm;
        else
            ++cold;
        if (t.total_ms > 0)
            totals.push_back(t.total_ms);
        if (t.player_ms > 0)
            players.push_back(t.player_ms);
    }
    ImGui::SeparatorText("Resolves");
    ImGui::Text("%d ok / %d failed   session warm %d / cold %d", ok, failed, warm, cold);
    ImGui::Text("total  p50 %.0f ms  p95 %.0f ms      player  p50 %.0f ms  p95 %.0f ms",
                percentile(totals, 0.50), percentile(totals, 0.95),
                percentile(players, 0.50), percentile(players, 0.95));
    small_hint("Warm resolves reuse the session (cookies + visitor Data); cold ones pay "
               "for the watch-page fetch (~1 s) that opens it.");

    // ---- copy as JSON ----------------------------------------------------
    if (ui_ghost_button("Copy traces as JSON", ImVec2(190.0f * m.layout_scale, 0.0f)))
    {
        // Build the array by serialising each row as an object string; the
        // debug payload is small and this keeps the code obvious.
        std::string out = "[";
        bool first = true;
        for (const native::ResolveTrace &t : traces)
        {
            if (!first)
                out += ",";
            first = false;
            char buf[2048];
            std::snprintf(buf, sizeof(buf),
                          "{\"id\":\"%s\",\"epoch_ms\":%lld,\"client\":\"%s\","
                          "\"status\":\"%s\",\"reason\":\"%s\",\"error\":\"%s\","
                          "\"itag\":%d,\"bitrate\":%d,\"hls\":%s,\"session_hit\":%s,"
                          "\"session_refreshed\":%s,\"page_bytes\":%llu,"
                          "\"player_bytes\":%llu,\"session_ms\":%lld,"
                          "\"player_ms\":%lld,\"total_ms\":%lld,"
                          "\"url_prefix\":\"%s\"}",
                          t.video_id.c_str(), t.epoch_ms, t.client.c_str(),
                          t.status.c_str(), t.reason.c_str(), t.error.c_str(), t.itag,
                          t.bitrate, t.hls ? "true" : "false",
                          t.session_hit ? "true" : "false",
                          t.session_refreshed ? "true" : "false",
                          (unsigned long long)t.page_bytes,
                          (unsigned long long)t.player_bytes, t.session_ms, t.player_ms, t.total_ms,
                          t.url_prefix.c_str());
            out += buf;
        }
        out += "]";
        ImGui::SetClipboardText(out.c_str());
    }

    // ---- per-resolve table ----------------------------------------------
    ImGui::SeparatorText("Recent resolves");
    if (ImGui::BeginChild("##native_traces", ImVec2(0.0f, 300.0f * m.layout_scale),
                          true, ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGui::TextColored(ImVec4(ui_c().text_muted.x, ui_c().text_muted.y,
                                  ui_c().text_muted.z, 1.0f),
                           "time      id           status  itag  kbps   session  page   "
                           "player  total   host");
        ImGui::Separator();
        if (traces.empty())
            small_hint("No resolves yet. Play an uncached track.");
        for (const native::ResolveTrace &t : traces)
        {
            const bool ok_row = t.error.empty() && t.status == "OK";
            ImGui::Text("%s  %-11s  %-6s  %4d  %4d  %-7s  %5lld  %6lld  %5lld  %s",
                        fmt_clock(t.epoch_ms).c_str(), t.video_id.c_str(),
                        t.status.empty() ? "-" : t.status.c_str(), t.itag,
                        t.bitrate / 1000,
                        t.session_hit ? "warm" : (t.session_refreshed ? "refresh" : "cold"),
                        t.session_ms, t.player_ms, t.total_ms,
                        t.url_prefix.empty() ? "-" : t.url_prefix.c_str());
            if (!ok_row)
            {
                const std::string why = !t.error.empty() ? t.error : t.reason;
                if (!why.empty())
                    ImGui::TextColored(native_err_col(), "          %s", why.c_str());
            }
            if (ImGui::IsItemHovered() && !t.url_prefix.empty())
                ImGui::SetTooltip("%s\nclient %s\npage %zu B, player %zu B", t.url_prefix.c_str(),
                                  t.client.c_str(), t.page_bytes, t.player_bytes);
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
        }
    }
    ImGui::EndChild();

    ImGui::PopID();
}
