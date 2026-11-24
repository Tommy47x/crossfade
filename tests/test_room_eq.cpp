#include "room_eq.h"
#include "fft.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using testutil::check;

namespace
{
constexpr double kPi = 3.14159265358979323846;

// Time-domain RBJ peaking filter, matching dsp::peaking_db().
std::vector<float> apply_band(const std::vector<float> &x, int fs, dsp::Band band)
{
    const double A = std::pow(10.0, band.gain / 40.0);
    const double w0 = 2.0 * kPi * band.freq / (double)fs;
    const double alpha = std::sin(w0) / (2.0 * band.q);
    const double b0 = 1.0 + alpha * A, b1 = -2.0 * std::cos(w0), b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A, a1 = -2.0 * std::cos(w0), a2 = 1.0 - alpha / A;
    std::vector<float> y(x.size(), 0.0f);
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double yn = (b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
        x2 = x1;
        x1 = x[i];
        y2 = y1;
        y1 = yn;
        y[i] = (float)yn;
    }
    return y;
}

double grid_value(const dsp::Grid &g, const std::vector<double> &v, double hz)
{
    const int i = g.nearest(hz);
    return i < 0 ? 0.0 : v[(std::size_t)i];
}

double midband_mean(const dsp::Grid &g, const std::vector<double> &v)
{
    double s = 0;
    int c = 0;
    for (std::size_t i = 0; i < g.hz.size(); ++i)
        if (g.hz[i] >= 250 && g.hz[i] <= 2000)
        {
            s += v[i];
            ++c;
        }
    return c ? s / c : 0.0;
}
} // namespace

int main()
{
    // ---- FFT ------------------------------------------------------------
    {
        std::vector<dsp::cplx> a(1024, dsp::cplx(0, 0));
        a[7] = dsp::cplx(1, 0);
        std::vector<dsp::cplx> b = a;
        dsp::fft(b, false);
        bool flat = true;
        for (const auto &v : b)
            flat = flat && std::fabs(std::abs(v) - 1.0) < 1e-9;
        check(flat, "fft of an impulse has flat magnitude");

        dsp::fft(b, true);
        double err = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            err = std::max(err, std::abs(a[i] - b[i]));
        check(err < 1e-9, "fft round trip");
    }

    // ---- peaking filter response ---------------------------------------
    {
        dsp::Band b{1000.0, 6.0, 1.0};
        const double at_centre = dsp::peaking_db(1000.0, 48000, b);
        check(std::fabs(at_centre - 6.0) < 0.05, "peaking gain at centre");
        check(std::fabs(dsp::peaking_db(1000.0, 48000, dsp::Band{1000, 0, 1})) < 1e-9,
              "zero-gain band is flat");
        check(dsp::peaking_db(100.0, 48000, b) < 0.5, "peaking rolls off away from centre");
        check(dsp::peaking_db(10000.0, 48000, b) < 0.5, "peaking rolls off above centre");
    }

    // ---- synthetic room ------------------------------------------------
    dsp::SweepSpec spec;
    spec.sample_rate = 48000;
    spec.f1 = 30.0;
    spec.f2 = 17000.0;
    spec.seconds = 2.0;
    spec.lead_in = 0.2;
    spec.tail = 0.3;

    // Two room modes and a treble shelf: a plausible small-room response.
    const std::vector<dsp::Band> room = {
        {55.0, 9.0, 4.0},
        {120.0, -7.0, 3.0},
        {900.0, -3.0, 0.8},
        {6000.0, 4.0, 0.7},
    };

    dsp::Sweep sweep = dsp::make_sweep(spec);
    std::vector<float> recording = sweep.audio;
    for (const dsp::Band &b : room)
        recording = apply_band(recording, spec.sample_rate, b);

    dsp::Grid grid;
    grid.f_lo = 20.0;
    grid.f_hi = 20000.0;
    grid.points = 480;
    grid.build();

    dsp::Measurement m = dsp::measure(recording, spec.sample_rate, spec, grid);
    check(m.ok, "measurement succeeds on a synthetic room");
    if (m.ok)
    {
        const std::vector<double> truth = dsp::response_db(grid, spec.sample_rate, room);
        const double off_m = midband_mean(grid, m.db);
        const double off_t = midband_mean(grid, truth);
        bool close = true;
        for (double hz : {60.0, 90.0, 150.0, 400.0, 900.0, 2500.0, 6000.0, 10000.0})
        {
            const double got = grid_value(grid, m.db, hz) - off_m;
            const double want = grid_value(grid, truth, hz) - off_t;
            if (std::fabs(got - want) > 2.0)
            {
                std::printf("  mismatch at %.0f Hz: got %.1f want %.1f\n", hz, got, want);
                close = false;
            }
        }
        check(close, "deconvolution recovers the room response");
        check(m.ir_focus > 0.5, "real sweep response is time-localised");
        std::printf("  ir focus %.3f\n", m.ir_focus);
    }

    // ---- design --------------------------------------------------------
    {
        const std::vector<double> measured = dsp::smooth_octave(grid, m.db, 1.0 / 6.0);
        const std::vector<double> target = dsp::target_curve(grid, dsp::TargetKind::Flat);

        double before = 0;
        int count = 0;
        for (std::size_t i = 0; i < grid.hz.size(); ++i)
            if (grid.hz[i] >= 40 && grid.hz[i] <= 16000)
            {
                const double e = target[i] - measured[i];
                before += e * e;
                ++count;
            }
        before = std::sqrt(before / count);

        dsp::DesignLimits lim;
        lim.lo_hz = 40.0;
        lim.hi_hz = 16000.0;
        lim.max_bands = 10;
        dsp::Design d = dsp::design(grid, measured, target, lim, spec.sample_rate);

        check(!d.bands.empty(), "design produces filters");
        check(d.bands.size() <= 10, "design respects the band cap");
        check((int)d.predicted_db.size() == (int)grid.hz.size(), "predicted curve sized");
        check(d.residual_rms_db < before * 0.5,
              "design halves the deviation from flat");
        check(d.preamp_db <= 0.0, "preamp never boosts");
        std::printf("  room eq: %.1f dB rms -> %.1f dB rms, %zu bands\n", before,
                    d.residual_rms_db, d.bands.size());
    }

    // ---- clipping / silence rejection ----------------------------------
    {
        std::vector<float> quiet(48000, 0.0001f);
        dsp::Measurement q = dsp::measure(quiet, 48000, spec, grid);
        check(!q.ok, "silence is rejected");

        std::vector<float> hot(48000, 1.0f);
        dsp::Measurement h = dsp::measure(hot, 48000, spec, grid);
        check(!h.ok, "clipping is rejected");

        // A recording that never contained the sweep must not produce a
        // design (the impulse response stays spread out like noise).
        std::vector<float> noise(48000 * 3, 0.0f);
        unsigned seed = 987654321u;
        for (float &v : noise)
        {
            seed = seed * 1103515245u + 12345u;
            v = ((float)(seed >> 16) / 65535.0f - 0.5f) * 0.2f;
        }
        dsp::Measurement n = dsp::measure(noise, 48000, spec, grid);
        check(!n.ok, "noise without the sweep is rejected");
        check(n.ir_focus < 0.25, "noise has a spread-out impulse response");
    }

    // ---- calibration files ---------------------------------------------
    {
        std::vector<dsp::CalPoint> cal;
        std::string err;
        const std::string text =
            "# REW calibration\n"
            "* some header\n"
            "20 -12.5\n"
            "1000 0.0\n"
            "20000 -6\n";
        check(dsp::parse_cal_text(text, cal, err), "parse calibration text");
        check(cal.size() == 3, "calibration point count");
        const std::vector<double> curve = dsp::cal_curve(grid, cal);
        check(std::fabs(grid_value(grid, curve, 20.0) + 12.5) < 0.1, "cal at low end");
        check(std::fabs(grid_value(grid, curve, 1000.0)) < 0.1, "cal at mid");
        check(grid_value(grid, curve, 10.0) == 0.0, "cal is zero outside its range");

        const auto s24 = dsp::builtin_mic_cal("galaxy_s24");
        check(s24.size() > 4, "galaxy s24 profile exists");
        check(dsp::builtin_mic_is_approximate("galaxy_s24"),
              "galaxy s24 profile is marked approximate");
        check(dsp::builtin_mic_cal("").empty(), "flat profile has no calibration");
    }

    if (testutil::failures == 0)
        std::printf("room_eq: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
