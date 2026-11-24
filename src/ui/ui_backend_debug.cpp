#include "ui.h"
#include "ui_theme.h"
#include "ytdlp.h"

#include <cstdio>

// ----------------------------------------------------------------------
// Backend debug window: shows the backend channels (main / play / prefetch /
// meta) with their counters, plus a live, scrollable log of every command
// sent to the backend (search, lyrics, meta, prefetch, …), how long it waited
// for the channel, its duration, the response size, whether it errored and the
// error text. Semi-transparent and freely movable.
// ----------------------------------------------------------------------

namespace
{
ImVec4 err_col()
{
    return ImVec4(0.95f, 0.42f, 0.42f, 1.0f);
}

std::string fmt_rel_time(int64_t ts_ms)
{
    const int64_t s = ts_ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld.%03llds", (long long)(s),
                  (long long)(ts_ms % 1000));
    return buf;
}
} // namespace

void draw_backend_debug(AppState &state)
{
    if (!state.show_backend_debug)
        return;

    const UiMetrics &m = ui_m();
    ImGui::SetNextWindowBgAlpha(0.80f);
    ImGui::SetNextWindowSize(ImVec2(720.0f * m.layout_scale, 440.0f * m.layout_scale),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Backend debug", &state.show_backend_debug, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    const BackendDebugSnapshot snap = backend_debug_snapshot();

    // ---- Channel summary ----
    ImGui::TextColored(ui_c().text_muted, "Channels");
    for (const BackendChannelStats &c : snap.channels)
    {
        const char *state_str = c.dead ? "DEAD" : (c.ready ? "ready" : "starting");
        const ImVec4 state_col = c.dead ? err_col()
                                        : (c.ready ? ui_c().accent : ui_c().text_muted);
        ImGui::TextColored(state_col, "%-9s", c.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(ui_c().text_dim,
                           "%s  pid=%zu  %llu cmds  %llu err  pending %zu  "
                           "last %llums  max %llums",
                           state_str, (size_t)c.pid,
                           (unsigned long long)c.commands, (unsigned long long)c.errors,
                           c.pending, (unsigned long long)c.last_ms,
                           (unsigned long long)c.max_ms);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Controls ----
    if (ImGui::Button("Clear log"))
        backend_debug_clear_log();
    ImGui::SameLine();
    static bool errors_only = false;
    ImGui::Checkbox("Errors only", &errors_only);
    ImGui::SameLine();
    ImGui::TextColored(ui_c().text_muted, "%zu shown", snap.log.size());
    ImGui::Spacing();

    // ---- Log ----
    ImGui::BeginChild("##backend_log", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (snap.log.empty())
    {
        ImGui::TextColored(ui_c().text_muted, "No commands yet.");
    }
    for (const BackendLogEntry &e : snap.log)
    {
        if (errors_only && !e.error)
            continue;

        const ImVec4 col = e.error ? err_col() : ui_c().text;
        std::string line = fmt_rel_time(e.ts_ms) + "  ";
        line += "#" + std::to_string(e.seq) + "  ";
        line += "[" + e.channel + "]  ";
        line += std::to_string(e.ms) + "ms";
        if (e.queue_ms > 0 || e.pending > 0)
            line += " (queued " + std::to_string(e.queue_ms) + "ms" +
                    (e.pending ? ", " + std::to_string(e.pending) + " ahead" : "") +
                    ")";
        line += "  ";
        line += std::to_string(e.bytes) + "B  ";
        line += e.command;
        if (!e.detail.empty())
            line += "  -> " + e.detail;
        ImGui::TextColored(col, "%s", line.c_str());
    }
    ImGui::EndChild();

    ImGui::End();
}
