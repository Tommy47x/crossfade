#pragma once

#include "room_eq.h"

#include <string>
#include <vector>

class MpvPlayer;
struct AppState;

// Room-correction calibration: owns the measurement session, the phone server,
// the sweep playback and the analysis job. All functions except the setters
// marked otherwise are main-thread only.
namespace roomeq
{
enum class Stage
{
    Idle,
    Waiting,   // session open, waiting for the phone (or the sweep to start)
    Measuring, // sweep is playing / being recorded
    Analyzing, // recording received, designing filters
    Ready,
    Error,
};

struct Status
{
    Stage stage = Stage::Idle;
    std::string message;
    std::string url; // QR target while waiting
    bool secure = false;
    bool pc_mic = false;
    bool has_result = false;
    int sample_rate = 48000;

    double residual_before_db = 0.0;
    double residual_after_db = 0.0;

    std::vector<double> hz;
    std::vector<double> measured_db;  // smoothed, mic-corrected
    std::vector<double> target_db;
    std::vector<double> corrected_db; // measured + predicted filters
    std::vector<dsp::Band> bands;
    // Per grid point: 1 where the measurement was trustworthy enough to
    // correct (shown dimmed in the graph otherwise).
    std::vector<char> valid;
    // Microphone profile actually used for the result.
    std::string mic;
    // Filter ceiling the last design was fitted with.
    int max_bands = 12;
    // The phone page is still being served (so it can measure again).
    bool page_open = false;
    // Range that was actually corrected: the trusted part of the measurement
    // intersected with the correction window. An upper edge around 3-4 kHz
    // means the phone captured a narrowband (voice/Bluetooth) signal.
    double correct_lo_hz = 0.0;
    double correct_hi_hz = 0.0;
    // Highest frequency the recording itself carried signal at (independent of
    // what was trusted for correction).
    double content_hi_hz = 0.0;
};

void init();
void set_wake(void (*fn)());

// Main loop. Plays the sweep, collects the recording, runs the analysis.
void tick(AppState &state, const MpvPlayer &player);

// Starts a phone measurement and returns the URL in status().url().
bool start_phone(AppState &state, std::string &error);
// Records with a PC microphone (PulseAudio/PipeWire via parec).
bool start_pc_mic(AppState &state, const std::string &device, std::string &error);
// Plays the sweep once so an external recorder can capture it.
bool play_sweep(AppState &state, std::string &error);

// Stops the session. Pass the state so playback paused for the measurement is
// resumed; nullptr (shutdown) leaves playback alone.
void cancel(AppState *state = nullptr);
Status status();

bool apply(AppState &state);
void forget(AppState &state);
// Drops the pending (unapplied) measurement so a new one can start cleanly.
void reset_result();
// Re-fits the current measurement with the settings' filter ceiling. No new
// measurement needed; cheap enough for the main thread.
bool redesign(AppState &state);
// Restores the curves of the applied correction from the settings, so the
// response graph and the target/strength controls keep working after a restart
// or a saved-room switch. Returns false when there is nothing to restore.
bool restore_saved(AppState &state);
// Closes the phone page but keeps the current result on screen.
void close_page();
// Persists a REW-style .cal file and selects it as the microphone profile.
bool import_cal(AppState &state, const std::string &path, std::string &error);
std::string cal_file_path();

// PulseAudio/PipeWire source names, "Default device" first. Empty when the
// platform has no capture helper.
std::vector<std::string> input_devices();
} // namespace roomeq
