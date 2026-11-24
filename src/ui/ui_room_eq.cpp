#include "ui.h"

#include "calibration.h"
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
                 double ceil_db, ImU32 col, float thickness)
{
    if (hz.size() < 2 || db.size() != hz.size())
        return;
    ImVec2 prev;
    bool have = false;
    for (std::size_t i = 0; i < hz.size(); ++i)
    {
        const double v = std::max(floor_db, std::min(ceil_db, db[i]));
        const double t = (v - floor_db) / (ceil_db - floor_db);
        const ImVec2 p(plot_x(tl, br, hz[i]), br.y - (float)t * (br.y - tl.y));
        if (have)
            dl->AddLine(prev, p, col, thickness);
        prev = p;
        have = true;
    }
}

void draw_response_graph(const roomeq::Status &st)
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

    plot_series(dl, tl, br, st.hz, st.measured_db, floor_db, ceil_db, ui_overlay_u32(0.42f),
                1.3f * m.layout_scale);
    plot_series(dl, tl, br, st.hz, st.target_db, floor_db, ceil_db, ui_text_dim_u32(), 1.0f);
    plot_series(dl, tl, br, st.hz, st.corrected_db, floor_db, ceil_db, ui_accent_u32(0.95f),
                1.9f * m.layout_scale);

    ImGui::Dummy(size);
    // Legend
    const float dot = 4.0f * m.layout_scale;
    const float line_h = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    auto legend = [&](const char *label, ImU32 col) {
        dl->AddCircleFilled(ImVec2(p.x + dot, p.y + line_h * 0.5f), dot, col, 12);
        dl->AddText(ImVec2(p.x + dot * 2.0f + 6.0f * m.layout_scale, p.y), ui_text_muted_u32(), label);
        p.x += dot * 2.0f + 6.0f * m.layout_scale + ImGui::CalcTextSize(label).x +
               18.0f * m.layout_scale;
    };
    legend("measured", ui_overlay_u32(0.42f));
    legend("target", ui_text_dim_u32());
    legend("corrected", ui_accent_u32(0.95f));
    ImGui::Dummy(ImVec2(0.0f, line_h));
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
} // namespace

void draw_room_eq_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    Settings &st = state.settings;
    const roomeq::Status status = roomeq::status();

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

    room_gap();
    ImGui::TextUnformatted("Microphone");
    static const char *kMicLabels[] = {
        "Flat / calibrated mic", "Phone (generic)", "Galaxy S24 (approximate)",
        "Imported .cal file"};
    static const char *kMicIds[] = {"", "phone_generic", "galaxy_s24", "custom"};
    int mic_index = 0;
    for (int i = 0; i < 4; ++i)
        if (st.room_eq_mic == kMicIds[i])
            mic_index = i;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##rq_mic", &mic_index, kMicLabels, 4))
    {
        st.room_eq_mic = kMicIds[mic_index];
        save_settings(st);
    }
    if (st.room_eq_mic == "galaxy_s24" || st.room_eq_mic == "phone_generic")
        room_help("No per-unit calibration data exists for phone microphones, so this is a "
                  "generic shape. Corrections start at 90 Hz where the phone is least wrong; "
                  "treat the bass as approximate.");
    else if (st.room_eq_mic == "custom")
        room_help(roomeq::cal_file_path().c_str());
    else
        room_help("Use this for a measurement microphone (UMIK-1 and similar) with its .cal "
                  "file imported, or when the mic is already flat enough.");

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
    static const char *kTargetLabels[] = {"Flat", "Warm"};
    int target = st.room_eq_target == "warm" ? 1 : 0;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##rq_target", &target, kTargetLabels, 2))
    {
        st.room_eq_target = target == 1 ? "warm" : "flat";
        save_settings(st);
    }

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
            roomeq::cancel();
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
            ImGui::Text("%d filters · %.1f dB RMS -> %.1f dB RMS", bands, status.residual_before_db,
                        status.residual_after_db);
            room_gap(0.6f);
            ImGui::BeginChild("##rq_bands", ImVec2(0.0f, 118.0f * m.layout_scale), false);
            for (const dsp::Band &b : status.bands)
                ImGui::Text("%7.1f Hz   %+5.1f dB   Q %.2f", b.freq, b.gain, b.q);
            ImGui::EndChild();
            if (ImGui::Button("Apply correction", ImVec2(190.0f * m.layout_scale, 0.0f)))
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
                roomeq::cancel();
                start_phone_or_notify(state);
            }
        }
        else
        {
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
