#pragma once

#include "types.h"

#include <string>
#include <vector>

// Saved room corrections ("profiles"). Each one is a complete playback-time
// correction: bands, preamp, live tone trim, plus the measurement metadata for
// reference. Stored one file per room under <config>/rooms/.
namespace rooms
{
struct Profile
{
    std::string id;     // stable file id ("room_<ms>")
    std::string name;   // human label
    std::string device; // output sink this room was measured on ("" = any)
    std::vector<EqBand> bands;
    float preamp = 0.0f;
    float bass_db = 0.0f;
    float treble_db = 0.0f;
    std::string mic;
    std::string target;
    std::string label;
    double residual_db = 0.0;
    long long saved_at = 0;
    // Response curves that produced this correction (empty in profiles saved
    // before the graph was persisted).
    RoomEqCurve curve;
};

void init();
std::vector<Profile> list();
bool load(const std::string &id, Profile &out);
bool remove(const std::string &id);

// Current playback correction as a profile, bound to the current output.
Profile from_settings(const Settings &settings, const std::string &name);
bool save_profile(const Profile &profile, std::string &error);

// Applies a saved room to `settings`. The caller persists the settings.
bool activate(Settings &settings, const std::string &id);
void deactivate(Settings &settings);
// Saves the current settings as a room and makes it the active one.
bool save_current(Settings &settings, const std::string &name, std::string &error);

// ---- output device ------------------------------------------------------
// Refreshes the cached default output sink (Linux: PipeWire/PulseAudio).
// Returns the sink name, or "" when it cannot be determined.
std::string refresh_output_device();
std::string output_device();
// Human-readable form of the cached device (sink description when available).
std::string output_device_label();
// Profile bound to `device`. `prefer` wins when it is itself bound to that
// device; otherwise the most recently saved match is returned. "" when none.
std::string match_device(const std::string &device, const std::string &prefer = "");
} // namespace rooms
