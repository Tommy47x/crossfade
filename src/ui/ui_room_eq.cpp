#include "ui.h"

#include "calibration.h"
#include "rooms.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_notify.h"
#include "ui_theme.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "qrcodegen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

// ----------------------------------------------------------------------
// Room correction — Settings page.
//
// The measurement session, HTTP server and filter design live in
// src/eq/. This file only draws the guided flow: pick a microphone, show the
// QR code, then review the measured curve and apply it.
// ----------------------------------------------------------------------

namespace
{
void room_gap(float scale = 1.0f)
{
    ImGui::Dummy(ImVec2(0.0f, ui_m().space_3 * scale));
}

void room_help(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextWrapped("%s", text ? text : "");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

float draw_qr(ImDrawList *dl, const std::string &text, ImVec2 centre, float max_size)
{
    if (text.empty() || max_size <= 0.0f)
        return 0.0f;
    try
    {
        qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int modules = qr.getSize();
        const int border = 4;
        const int total = modules + border * 2;
        const float px = std::floor(max_size / (float)total);
        if (px < 2.0f)
            return 0.0f;
        const float side = px * (float)total;
        const ImVec2 tl(centre.x - side * 0.5f, centre.y - side * 0.5f);
        dl->AddRectFilled(tl, ImVec2(tl.x + side, tl.y + side), IM_COL32(255, 255, 255, 255),
                          ui_m().radius_sm);
        const ImU32 dark = IM_COL32(10, 12, 16, 255);
        for (int y = 0; y < modules; ++y)
            for (int x = 0; x < modules; ++x)
            {
                if (!qr.getModule(x, y))
                    continue;
                const ImVec2 a(tl.x + (float)(x + border) * px,
                               tl.y + (float)(y + border) * px);
                dl->AddRectFilled(a, ImVec2(a.x + px, a.y + px), dark);
            }
        return side;
    }
    catch (...)
    {
        return 0.0f;
    }
}

float plot_x(const ImVec2 &tl, const ImVec2 &br, double hz)
{
    const double lo = std::log10(20.0), hi = std::log10(20000.0);
    const double t = (std::log10(std::max(20.0, std::min(20000.0, hz))) - lo) / (hi - lo);
    return tl.x + (float)t * (br.x - tl.x);
}

void plot_series(ImDrawList *dl, const ImVec2 &tl, const ImVec2 &br,
                 const std::vector<double> &hz, const std::vector<double> &db, double floor_db,
                 double ceil_db, ImU32 col, float thickness, const std::vector<char> *mask,
                 bool dashed = false)
{
    if (hz.size() < 2 || db.size() != hz.size())
        return;
    ImVec2 prev;
    bool have = false;
    for (std::size_t i = 0; i < hz.size(); ++i)
    {
        if (mask && i < mask->size() && !(*mask)[i])
        {
            have = false;
            continue;
        }
        if (dashed && (i / 5) % 2 == 1)
        {
            have = false;
            continue;
        }
        const double v = std::max(floor_db, std::min(ceil_db, db[i]));
        const double t = (v - floor_db) / (ceil_db - floor_db);
        const ImVec2 p(plot_x(tl, br, hz[i]), br.y - (float)t * (br.y - tl.y));
        if (have)
            dl->AddLine(prev, p, col, thickness);
        prev = p;
        have = true;
    }
}

// Fixed colours instead of theme alpha: "which line is which" must survive any
// accent colour and both day and night themes.
const ImU32 kMeasuredCol = IM_COL32(236, 152, 74, 220); // orange
const ImU32 kTargetCol = IM_COL32(118, 188, 240, 235);  // sky blue

void draw_response_graph(const roomeq::Status &st, bool eq_only = false)
{
    const UiMetrics &m = ui_m();
    const float avail_w = std::max(240.0f * m.layout_scale, ImGui::GetContentRegionAvail().x);
    const ImVec2 size(avail_w, 210.0f * m.layout_scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 tl(origin.x + 34.0f * m.layout_scale, origin.y + 10.0f * m.layout_scale);
    const ImVec2 br(origin.x + size.x - 8.0f * m.layout_scale,
                    origin.y + size.y - 22.0f * m.layout_scale);

    double floor_db = -18.0, ceil_db = 18.0;
    for (const std::vector<double> *series : {&st.measured_db, &st.target_db, &st.corrected_db})
    {
        for (double v : *series)
        {
            if (!std::isfinite(v))
                continue;
            floor_db = std::min(floor_db, v - 1.0);
            ceil_db = std::max(ceil_db, v + 1.0);
        }
    }
    floor_db = std::max(floor_db, -36.0);
    ceil_db = std::min(ceil_db, 30.0);

    dl->AddRectFilled(tl, br, ui_overlay_u32(0.035f), m.radius_sm);
    for (double f : {100.0, 1000.0, 10000.0})
    {
        const float x = plot_x(tl, br, f);
        dl->AddLine(ImVec2(x, tl.y), ImVec2(x, br.y), ui_overlay_u32(0.09f), 1.0f);
        char label[16];
        std::snprintf(label, sizeof(label), f >= 1000.0 ? "%.0fk" : "%.0f", f >= 1000.0 ? f / 1000.0 : f);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(x - ts.x * 0.5f, br.y + 3.0f * m.layout_scale), ui_text_muted_u32(), label);
    }
    for (double v = -12.0; v <= 12.0 + 0.1; v += 6.0)
    {
        if (v < floor_db || v > ceil_db)
            continue;
        const double t = (v - floor_db) / (ceil_db - floor_db);
        const float y = br.y - (float)t * (br.y - tl.y);
        dl->AddLine(ImVec2(tl.x, y), ImVec2(br.x, y), ui_overlay_u32(v == 0.0 ? 0.16f : 0.07f), 1.0f);
        char label[16];
        std::snprintf(label, sizeof(label), "%+.0f", v);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(tl.x - ts.x - 4.0f * m.layout_scale, y - ts.y * 0.5f),
                    ui_text_muted_u32(), label);
    }

    // Untrusted parts stay visible but faded (a broken line read as "scarce").
    if (!eq_only)
        plot_series(dl, tl, br, st.hz, st.measured_db, floor_db, ceil_db,
                    ui_overlay_u32(0.12f), 1.2f * m.layout_scale, nullptr);

    // The EQ's own response, drawn as a filled band around 0 dB. Without it a
    // small correction looks like "nothing happened" because the corrected
    // curve sits on top of the measured one.
    if (st.corrected_db.size() == st.hz.size() && st.measured_db.size() == st.hz.size())
    {
        const double span = std::max(1e-6, ceil_db - floor_db);
        const float zero_y = br.y - (float)((0.0 - floor_db) / span) * (br.y - tl.y);
        const ImU32 fill = ui_accent_u32(0.10f);
        const ImU32 edge = ui_accent_u32(0.45f);
        ImVec2 prev;
        bool have = false;
        for (std::size_t i = 0; i < st.hz.size(); ++i)
        {
            const bool ok = st.valid.empty() || (i < st.valid.size() && st.valid[i]);
            const double gain = st.corrected_db[i] - st.measured_db[i];
            const double v = std::max(floor_db, std::min(ceil_db, gain));
            const float y = br.y - (float)((v - floor_db) / span) * (br.y - tl.y);
            const float x = plot_x(tl, br, st.hz[i]);
            if (!ok)
            {
                have = false;
                continue;
            }
            dl->AddLine(ImVec2(x, zero_y), ImVec2(x, y), fill, 1.2f);
            if (have)
                dl->AddLine(prev, ImVec2(x, y), edge, 1.0f);
            prev = ImVec2(x, y);
            have = true;
        }
    }

    if (!eq_only)
    {
        plot_series(dl, tl, br, st.hz, st.measured_db, floor_db, ceil_db, kMeasuredCol,
                    1.4f * m.layout_scale, &st.valid);
        plot_series(dl, tl, br, st.hz, st.target_db, floor_db, ceil_db, kTargetCol,
                    1.2f * m.layout_scale, nullptr, true);
    }
    plot_series(dl, tl, br, st.hz, st.corrected_db, floor_db, ceil_db, ui_accent_u32(0.95f),
                2.1f * m.layout_scale, &st.valid);

    ImGui::Dummy(size);
    // Legend: line swatches in the same style as the graph.
    const float line_h = ImGui::GetTextLineHeight();
    const float sample_w = 24.0f * m.layout_scale;
    ImVec2 p = ImGui::GetCursorScreenPos();
    auto legend = [&](const char *label, ImU32 col, bool dashed) {
        const float y = p.y + line_h * 0.5f;
        if (dashed)
        {
            for (float k = 0.0f; k < sample_w; k += 8.0f)
                dl->AddLine(ImVec2(p.x + k, y), ImVec2(p.x + std::min(k + 4.5f, sample_w), y),
                            col, 2.0f);
        }
        else
        {
            dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + sample_w, y), col, 2.0f);
        }
        dl->AddText(ImVec2(p.x + sample_w + 6.0f * m.layout_scale, p.y), ui_text_muted_u32(),
                    label);
        p.x += sample_w + 6.0f * m.layout_scale + ImGui::CalcTextSize(label).x +
               18.0f * m.layout_scale;
    };
    if (eq_only)
        legend("applied EQ", ui_accent_u32(0.95f), false);
    else
    {
        legend("measured", kMeasuredCol, false);
        legend("target", kTargetCol, true);
        legend("corrected", ui_accent_u32(0.95f), false);
    }

    // Correction fill swatch + trusted range.
    if (!eq_only)
    {
        const float y = p.y + line_h * 0.5f;
        const float x0 = p.x;
        dl->AddRectFilled(ImVec2(x0, y - 4.0f * m.layout_scale),
                          ImVec2(x0 + sample_w, y + 4.0f * m.layout_scale),
                          ui_accent_u32(0.18f));
        dl->AddLine(ImVec2(x0, y), ImVec2(x0 + sample_w, y), ui_accent_u32(0.55f), 1.0f);
        dl->AddText(ImVec2(x0 + sample_w + 6.0f * m.layout_scale, p.y), ui_text_muted_u32(),
                    "EQ gain");
    }
    ImGui::Dummy(ImVec2(0.0f, line_h));

    auto fmt_hz = [](double f) {
        char b[32];
        if (f >= 1000.0)
            std::snprintf(b, sizeof(b), "%.1f kHz", f / 1000.0);
        else
            std::snprintf(b, sizeof(b), "%.0f Hz", f);
        return std::string(b);
    };
    if (st.correct_hi_hz > st.correct_lo_hz && st.correct_lo_hz > 0.0)
        ImGui::TextColored(ImVec4(ui_c().text_muted.x, ui_c().text_muted.y, ui_c().text_muted.z,
                                  1.0f),
                           "Correcting %s – %s", fmt_hz(st.correct_lo_hz).c_str(),
                           fmt_hz(st.correct_hi_hz).c_str());
    else
        room_help("Nothing in this measurement is trusted enough to correct.");

    if (st.content_hi_hz > 0.0)
        ImGui::TextColored(ImVec4(ui_c().text_muted.x, ui_c().text_muted.y, ui_c().text_muted.z,
                                  1.0f),
                           "Recording reaches %s", fmt_hz(st.content_hi_hz).c_str());

    if (st.content_hi_hz > 0.0 && st.content_hi_hz < 6000.0)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.72f, 0.40f, 1.0f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextWrapped("The recording itself stops at %.1f kHz, so there is nothing above "
                           "that to correct. This is an 8 kHz voice/Bluetooth capture path, not "
                           "the speaker. Disconnect headsets, use the phone's own microphone (or "
                           "a USB mic), and measure again.",
                           st.content_hi_hz / 1000.0);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    else if (st.content_hi_hz > 0.0 && st.correct_hi_hz > 0.0 &&
             st.correct_hi_hz < st.content_hi_hz * 0.75)
    {
        room_help("The recording reaches further up, but the part above the corrected range "
                  "was too noisy or not repeatable enough to trust. Measure again in a quieter "
                  "moment, or turn the volume up slightly.");
    }
}

void start_phone_or_notify(AppState &state)
{
    std::string error;
    if (!roomeq::start_phone(state, error))
        ui_notify(UiNotifyType::Error, "Room correction", error, 7.0f);
}

void start_pc_or_notify(AppState &state, const std::string &device)
{
    std::string error;
    if (!roomeq::start_pc_mic(state, device, error))
        ui_notify(UiNotifyType::Error, "Room correction", error, 7.0f);
}

const char *mic_name(const std::string &id)
{
    if (id == "phone_generic")
        return "phone / consumer mic (approximate)";
    if (id == "custom")
        return "imported .cal file";
    return "flat (calibrated mic)";
}

// Corrections applied before the curves were persisted only have their EQ
// bands. Plot the summed response so the page still shows the calibrated
// correction instead of going blank after a restart.
void draw_applied_response_graph(const Settings &st)
{
    roomeq::Status s;
    dsp::Grid grid;
    grid.build();
    std::vector<dsp::Band> bands;
    bands.reserve(st.room_eq_bands.size());
    for (const EqBand &b : st.room_eq_bands)
        bands.push_back({(double)b.freq, (double)b.gain, (double)b.q});
    s.hz = grid.hz;
    s.corrected_db = dsp::response_db(grid, 48000, bands);
    if (s.corrected_db.size() != s.hz.size())
        return;
    s.measured_db.assign(s.hz.size(), 0.0);
    s.target_db.assign(s.hz.size(), 0.0);
    s.valid.assign(s.hz.size(), 1);
    s.bands = bands;
    s.residual_after_db = st.room_eq_residual_db;
    s.max_bands = st.room_eq_max_bands;
    s.mic = st.room_eq_mic;
    draw_response_graph(s, true);
}
} // namespace

void draw_room_eq_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    Settings &st = state.settings;
    roomeq::Status status = roomeq::status();
    // The applied correction's curves are persisted, so the response graph is
    // available after a restart or a saved-room switch, not only right after
    // a calibration.
    if (!status.has_result && !st.room_eq_curve.empty() &&
        roomeq::restore_saved(state))
        status = roomeq::status();

    ImGui::PushID("room_eq");
    room_help("Measure how your speakers actually sound where you sit, then let crossfade "
              "flatten the worst peaks. The correction runs in the player's audio chain, so it "
              "works on Linux and Windows and needs no extra driver.");
    room_gap();

    if (ImGui::Checkbox("Room correction", &st.room_eq_enabled))
    {
        if (st.room_eq_bands.empty() && st.room_eq_enabled)
        {
            st.room_eq_enabled = false;
            ui_notify(UiNotifyType::Info, "Room correction",
                      "Calibrate first: there is no measured correction yet.");
        }
        else
        {
            save_settings(st);
        }
    }
    if (!st.room_eq_label.empty())
        room_help((st.room_eq_label +
                   (st.room_eq_enabled ? " · active" : " · paused")).c_str());

    // ---- saved rooms ---------------------------------------------------
    static std::vector<rooms::Profile> s_rooms;
    static bool s_rooms_loaded = false;
    static int s_room_sel = 0;
    if (!s_rooms_loaded)
    {
        s_rooms = rooms::list();
        s_rooms_loaded = true;
    }
    ImGui::TextUnformatted("Room");
    std::vector<std::string> room_names;
    room_names.push_back("(unsaved / none)");
    for (const rooms::Profile &r : s_rooms)
        room_names.push_back(r.name);
    s_room_sel = 0;
    for (std::size_t i = 0; i < s_rooms.size(); ++i)
        if (s_rooms[i].id == st.room_eq_active_id)
            s_room_sel = (int)i + 1;
    std::vector<const char *> room_ptrs;
    for (const std::string &n : room_names)
        room_ptrs.push_back(n.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##rq_room", room_ptrs[s_room_sel]))
    {
        for (int i = 0; i < (int)room_ptrs.size(); ++i)
        {
            const bool selected = i == s_room_sel;
            if (ImGui::Selectable(room_ptrs[i], selected))
            {
                if (i == 0)
                {
                    rooms::deactivate(state.settings);
                    roomeq::reset_result();
                }
                else
                {
                    rooms::activate(state.settings, s_rooms[(std::size_t)i - 1].id);
                    roomeq::restore_saved(state);
                }
                save_settings(state.settings);
                status = roomeq::status();
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (s_room_sel > 0)
    {
        const rooms::Profile &active = s_rooms[(std::size_t)s_room_sel - 1];
        room_help(active.device.empty()
                      ? "Bound to no particular output."
                      : ("Bound to " + active.device).c_str());
        if (ui_ghost_button("Delete this room", ImVec2(170.0f * m.layout_scale, 0.0f)))
        {
            rooms::remove(active.id);
            rooms::deactivate(state.settings);
            roomeq::reset_result();
            save_settings(state.settings);
            s_rooms = rooms::list();
            status = roomeq::status();
        }
    }
    if (ImGui::Checkbox("Switch rooms with the output device", &st.room_eq_auto_room))
        save_settings(st);
    const std::string dev_label = rooms::output_device_label();
    room_help(dev_label.empty() ? "Output device: unknown (switch rooms manually)."
                                : ("Listening on: " + dev_label).c_str());

    room_gap();
    ImGui::TextUnformatted("Microphone");
    static const char *kMicLabels[] = {
        "Flat — calibrated mic only", "Phone / consumer mic (approximate)",
        "Imported .cal file"};
    static const char *kMicIds[] = {"", "phone_generic", "custom"};
    int mic_index = 0;
    for (int i = 0; i < 3; ++i)
        if (st.room_eq_mic == kMicIds[i])
            mic_index = i;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##rq_mic", &mic_index, kMicLabels, 3))
    {
        st.room_eq_mic = kMicIds[mic_index];
        save_settings(st);
    }
    if (st.room_eq_mic == "phone_generic")
        room_help("No per-unit calibration data exists for phone mics, so this is a generic "
                  "shape. Corrections start at 90 Hz and stop at 12 kHz; treat the bass as "
                  "approximate. A measured .cal file always wins.");
    else if (st.room_eq_mic == "custom")
        room_help(roomeq::cal_file_path().c_str());
    else
        room_help("\"Flat\" assumes the microphone is already accurate. Never pick it for a "
                  "phone or laptop mic: their own response would be corrected as if it were "
                  "the room. Use a measurement mic (UMIK-1 and similar) with its .cal imported.");

    static std::string s_cal_path;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##rq_cal", "Path to a microphone .cal file", &s_cal_path);
    if (ui_ghost_button("Import calibration file", ImVec2(210.0f * m.layout_scale, 0.0f)))
    {
        std::string error;
        if (roomeq::import_cal(state, s_cal_path, error))
            ui_notify(UiNotifyType::Info, "Room correction", "Microphone calibration imported.", 5.0f);
        else
            ui_notify(UiNotifyType::Error, "Room correction", error, 6.0f);
    }

    ImGui::TextUnformatted("Target");
    static const char *kTargetLabels[] = {"Balanced (recommended)", "Warm", "Dynamic",
                                          "Flat (reference)"};
    static const char *kTargetIds[] = {"balanced", "warm", "dynamic", "flat"};
    int target = 0;
    for (int i = 0; i < 4; ++i)
        if (st.room_eq_target == kTargetIds[i])
            target = i;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##rq_target", &target, kTargetLabels, 4))
    {
        st.room_eq_target = kTargetIds[target];
        save_settings(st);
        if (status.has_result)
            roomeq::redesign(state);
    }
    room_help(target == 3
                  ? "Flat flattens the whole band. It removes the room's bass gain and usually "
                    "sounds thin; use it for reference, not for listening."
                  : target == 2
                        ? "Dynamic keeps the balance, then adds bass and a real treble lift on "
                          "top (the closest pure-EQ thing to a loudness/crystalizer effect)."
                        : "Balanced keeps the speaker's own balance and only tames deviations "
                          "from it. Warm adds a gentle bass lift on top.");

    ImGui::TextUnformatted("Strength");
    static const char *kStrengthLabels[] = {"Gentle", "Balanced", "Strong"};
    int strength = st.room_eq_strength < 0.8f ? 0 : (st.room_eq_strength > 1.15f ? 2 : 1);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##rq_strength", &strength, kStrengthLabels, 3))
    {
        st.room_eq_strength = strength == 0 ? 0.6f : (strength == 2 ? 1.3f : 1.0f);
        save_settings(st);
        if (status.has_result)
            roomeq::redesign(state);
    }

    ImGui::TextUnformatted("Filter detail");
    static const char *kDetailLabels[] = {"12 filters (default)", "20 filters", "32 filters"};
    static const int kDetailValues[] = {12, 20, 32};
    int detail = st.room_eq_max_bands == 20 ? 1 : (st.room_eq_max_bands == 32 ? 2 : 0);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##rq_detail", &detail, kDetailLabels, 3))
    {
        st.room_eq_max_bands = kDetailValues[detail];
        save_settings(st);
        if (status.has_result)
            roomeq::redesign(state);
    }
    room_help("How many filters the fit may use. More is a closer fit to the same target, not a "
              "stronger correction. Changing it re-fits the result above instantly; press Apply "
              "again to hear it.");

    ImGui::TextUnformatted("Tone (live)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##rq_bass", &st.room_eq_bass_db, -6.0f, 6.0f, "Bass %+.1f dB");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save_settings(st);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##rq_treble", &st.room_eq_treble_db, -6.0f, 6.0f, "Treble %+.1f dB");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save_settings(st);
    room_help("Shelves on top of the correction (120 Hz / 4 kHz). They apply immediately, "
              "independently of the correction, and need no measurement at all; the curve "
              "above shows the correction only, not these.");

    room_gap();
    if (status.stage == roomeq::Stage::Waiting || status.stage == roomeq::Stage::Measuring ||
        status.stage == roomeq::Stage::Analyzing)
    {
        // ---- live session ------------------------------------------------
        ImGui::SeparatorText("Measurement");
        if (status.stage == roomeq::Stage::Waiting && !status.url.empty())
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float side = 190.0f * m.layout_scale;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 centre(p.x + ImGui::GetContentRegionAvail().x * 0.5f, p.y + side * 0.5f);
            draw_qr(dl, status.url, centre, side);
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, side + 6.0f * m.layout_scale));
            room_help("Point your phone's camera at this code. Keep the phone where you sit.");
            ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
            ImGui::TextWrapped("%s", status.url.c_str());
            ImGui::PopStyleColor();
            if (!status.secure)
                room_help("This connection is plain HTTP, so browsers block the microphone. "
                          "Use the computer microphone below, or import a recording.");
        }
        room_help(status.message.c_str());
        if (ImGui::Button("Cancel measurement", ImVec2(190.0f * m.layout_scale, 0.0f)))
            roomeq::cancel(&state);
        if (status.stage == roomeq::Stage::Waiting && !status.url.empty() &&
            ImGui::Button("Play the sweep once", ImVec2(190.0f * m.layout_scale, 0.0f)))
        {
            std::string error;
            if (!roomeq::play_sweep(state, error))
                ui_notify(UiNotifyType::Error, "Room correction", error, 5.0f);
        }
    }
    else
    {
        if (status.has_result)
        {
            // ---- result --------------------------------------------------
            ImGui::SeparatorText("Result");
            draw_response_graph(status);
            const int bands = (int)status.bands.size();
            if (status.max_bands > 0 && bands >= status.max_bands)
                ImGui::Text("%d filters (limit %d) · %.1f dB RMS -> %.1f dB RMS", bands,
                            status.max_bands, status.residual_before_db,
                            status.residual_after_db);
            else
                ImGui::Text("%d filters (converged) · %.1f dB RMS -> %.1f dB RMS", bands,
                            status.residual_before_db, status.residual_after_db);
            if (!status.mic.empty())
                ImGui::TextColored(ImVec4(ui_c().text_muted.x, ui_c().text_muted.y,
                                          ui_c().text_muted.z, 1.0f),
                                   "Microphone: %s", mic_name(status.mic));
            room_help("Above ~2 kHz the correction is deliberately gentle and narrow dips are "
                      "never filled — boosting them sounds worse than leaving them. If the top "
                      "end sounds dull, try the Dynamic target rather than expecting a sharp "
                      "dip to be filled.");
            bool any_invalid = false;
            for (char v : status.valid)
                any_invalid = any_invalid || !v;
            if (any_invalid)
                room_help("The dim part of the curve is below what this recording can resolve "
                          "(or was not repeatable), so no correction is applied there.");
            room_gap(0.6f);
            ImGui::BeginChild("##rq_bands", ImVec2(0.0f, 118.0f * m.layout_scale), false);
            for (const dsp::Band &b : status.bands)
                ImGui::Text("%7.1f Hz   %+5.1f dB   Q %.2f", b.freq, b.gain, b.q);
            ImGui::EndChild();
            static std::string s_save_name;
            if (s_save_name.empty())
                s_save_name = "Room";
            ImGui::SetNextItemWidth(190.0f * m.layout_scale);
            ImGui::InputTextWithHint("##rq_save_name", "Room name", &s_save_name);
            ImGui::SameLine();
            if (ImGui::Button("Save as room", ImVec2(150.0f * m.layout_scale, 0.0f)))
            {
                if (roomeq::apply(state))
                {
                    std::string error;
                    if (rooms::save_current(state.settings,
                                            s_save_name.empty() ? "Room" : s_save_name, error))
                    {
                        save_settings(state.settings);
                        s_rooms = rooms::list();
                        ui_notify(UiNotifyType::Info, "Room correction",
                                  "Saved as \"" + s_save_name + "\" and applied.", 5.0f);
                    }
                    else
                    {
                        ui_notify(UiNotifyType::Error, "Room correction", error, 6.0f);
                    }
                }
            }
            if (ImGui::Button("Apply without saving", ImVec2(190.0f * m.layout_scale, 0.0f)))
            {
                if (roomeq::apply(state))
                    ui_notify(UiNotifyType::Info, "Room correction",
                              "Correction applied. Adjust the tracks and listen.", 5.0f);
            }
            ImGui::SameLine();
            if (ui_ghost_button("Discard", ImVec2(120.0f * m.layout_scale, 0.0f)))
                roomeq::forget(state);
            ImGui::SameLine();
            if (ui_ghost_button("Measure again", ImVec2(150.0f * m.layout_scale, 0.0f)))
            {
                roomeq::reset_result();
                roomeq::cancel(&state);
                start_phone_or_notify(state);
            }
            if (status.page_open)
            {
                room_gap(0.4f);
                room_help("The phone page is still open: \"Measure again\" there works without "
                          "scanning a new code.");
                if (ui_ghost_button("Close phone page", ImVec2(180.0f * m.layout_scale, 0.0f)))
                    roomeq::close_page();
            }
        }
        else
        {
            if (!st.room_eq_bands.empty())
            {
                ImGui::SeparatorText("Applied correction");
                draw_applied_response_graph(st);
                room_help("This is the EQ response of the applied correction. The measured "
                          "and target curves are stored for new calibrations; calibrate "
                          "again to see the full room response.");
                room_gap(0.6f);
            }
            if (status.stage == roomeq::Stage::Error && !status.message.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.48f, 0.48f, 1.0f));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                       ImGui::GetContentRegionAvail().x);
                ImGui::TextWrapped("%s", status.message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                room_gap(0.5f);
            }
            ImGui::SeparatorText("Calibrate");
            if (ui_primary_button("Calibrate with your phone", ImVec2(-1.0f, 0.0f)))
                start_phone_or_notify(state);
            room_gap(0.5f);
            ImGui::SeparatorText("Computer microphone");
            static std::vector<std::string> s_devices;
            static int s_device = 0;
            static bool s_devices_loaded = false;
            if (!s_devices_loaded)
            {
                s_devices = roomeq::input_devices();
                s_devices_loaded = true;
            }
            std::string current = s_device < (int)s_devices.size() ? s_devices[s_device] : "";
            const char *preview = current.empty() ? "System default" : current.c_str();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##rq_device", preview))
            {
                for (int i = 0; i < (int)s_devices.size(); ++i)
                {
                    const bool sel = i == s_device;
                    const char *label = s_devices[i].empty() ? "System default" : s_devices[i].c_str();
                    if (ImGui::Selectable(label, sel))
                        s_device = i;
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (s_devices.size() <= 1)
                room_help("No capture sources found (PulseAudio/PipeWire). Install pactl, or use "
                          "the phone or a recording.");
            if (ui_ghost_button("Record with this microphone", ImVec2(-1.0f, 0.0f)))
                start_pc_or_notify(state, current);
            room_help("The sweep plays through your current output at the volume you set. "
                      "Stay quiet and keep the mic where you sit.");
        }
    }
    ImGui::PopID();
}
