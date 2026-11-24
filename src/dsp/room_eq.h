#pragma once

#include <string>
#include <vector>

// Pure room-correction maths: sweep generation, deconvolution, smoothing,
// target curves, parametric EQ fitting. No I/O, no threads, no settings — so
// tests can drive it with synthetic "rooms".
namespace dsp
{
// ---------------------------------------------------------------- sweep ----

struct SweepSpec
{
    int sample_rate = 48000;
    double f1 = 30.0;          // Hz
    double f2 = 17000.0;       // Hz
    double seconds = 6.0;      // sweep length, excluding lead-in/tail
    double amplitude = 0.30;   // linear peak (~ -10.5 dBFS)
    double lead_in = 0.8;      // silence before the sweep, seconds
    double tail = 1.2;         // silence after the sweep, seconds
    double fade = 0.02;        // raised-cosine fade at both sweep ends

    double total_seconds() const { return lead_in + seconds + tail; }
};

struct Sweep
{
    std::vector<float> audio; // lead-in + sweep + tail
    std::size_t core_begin = 0;
    std::size_t core_len = 0;
};

// Exponential (logarithmic) sine sweep — the excitation signal. The return
// value keeps the exact core slice used as the deconvolution reference, so
// the fades are part of the reference too.
Sweep make_sweep(const SweepSpec &spec);

// 16-bit mono PCM WAV.
bool write_wav16(const std::string &path, const std::vector<float> &samples,
                 int sample_rate, std::string &error);

// ------------------------------------------------------------- response ----

// Uniform-in-log-frequency reporting grid. Responses are always relative
// (0 dB around the midband), because a microphone cannot tell us absolute SPL.
struct Grid
{
    double f_lo = 20.0;
    double f_hi = 20000.0;
    int points = 480; // 1/24 octave
    std::vector<double> hz;

    void build();
    // Index of the grid point nearest `f`, or -1 when outside the grid.
    int nearest(double f) const;
};

struct Measurement
{
    bool ok = false;
    std::string error;
    double peak_dbfs = -120.0;
    double noise_dbfs = -120.0;
    double snr_db = 0.0;
    // Fraction of the deconvolved impulse energy that sits within ±300 ms of
    // its peak. A sweep response is concentrated there; broadband noise is
    // spread evenly, which is how "you forgot to play the sweep" is caught.
    double ir_focus = 0.0;
    int clipped = 0;
    std::vector<double> db; // deconvolved magnitude, normalised at midband
};

// Estimates the transfer function of playback->room->microphone from the
// recording of `spec`. Works on the whole buffer: the deconvolution is a
// frequency-domain division, so an unknown start delay does not matter (only
// |H| is used). Returns ok=false with a human-readable reason when the capture
// is unusable (silence, clipping, no sweep).
Measurement measure(const std::vector<float> &recording, int sample_rate,
                    const SweepSpec &spec, const Grid &grid);

// Fractional-octave smoothing (fraction = 0.1667 for 1/6 octave).
std::vector<double> smooth_octave(const Grid &grid, const std::vector<double> &db,
                                  double fraction);

// --------------------------------------------------------------- target ----

enum class TargetKind
{
    Flat = 0,
    Warm = 1,
};

// Correction target relative to the normalised measurement. `Flat` is a
// neutral in-room response; `Warm` adds a small bass lift and treble tilt.
std::vector<double> target_curve(const Grid &grid, TargetKind kind);

// ---------------------------------------------------------------- design ----

struct Band
{
    double freq = 1000.0;
    double gain = 0.0;
    double q = 1.0;
};

struct DesignLimits
{
    double lo_hz = 40.0;       // correction window
    double hi_hz = 16000.0;
    double max_boost_db = 6.0;
    double max_cut_db = -12.0;
    int max_bands = 12;
    // Positive corrections are additionally capped by the 1-octave smoothed
    // correction, so a narrow null is never filled with a huge boost.
    double boost_margin_db = 0.0;
};

struct Design
{
    std::vector<Band> bands;
    std::vector<double> predicted_db; // summed response of the bands
    std::vector<double> corrected_db; // measured + predicted
    double residual_rms_db = 0.0;
    double preamp_db = 0.0; // headroom so boosted bands cannot clip
};

// Designs parametric peaking filters that move `measured_db` toward
// `target_db` inside the correction window.
Design design(const Grid &grid, const std::vector<double> &measured_db,
              const std::vector<double> &target_db, const DesignLimits &limits,
              int sample_rate);

// |H| in dB of one peaking biquad at `f` (Audio EQ Cookbook, as implemented by
// ffmpeg's `equalizer` filter and therefore by mpv's audio filter chain).
double peaking_db(double f, int sample_rate, const Band &band);

std::vector<double> response_db(const Grid &grid, int sample_rate,
                                const std::vector<Band> &bands);

// ---------------------------------------------------------- mic cal -------

struct CalPoint
{
    double hz = 0.0;
    double db = 0.0;
};

// Parses REW-style calibration text: "freq db" per line, blank lines and
// lines starting with '#', '*', ';' or "//" ignored. Sorted ascending.
bool parse_cal_text(const std::string &text, std::vector<CalPoint> &out,
                    std::string &error);

// Interpolates a calibration curve onto the grid (0 outside its range).
std::vector<double> cal_curve(const Grid &grid, const std::vector<CalPoint> &cal);

// Built-in, deliberately coarse microphone profiles. These are approximations,
// not measurements: a phone mic varies by unit, case and grip. The host UI
// labels them as such and a real .cal file always wins.
std::vector<CalPoint> builtin_mic_cal(const std::string &profile_id);
// True when the profile only exists so the app can be honest about its limits.
bool builtin_mic_is_approximate(const std::string &profile_id);
} // namespace dsp
