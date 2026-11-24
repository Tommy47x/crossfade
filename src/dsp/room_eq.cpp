#include "room_eq.h"

#include "fft.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>

namespace dsp
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

double db(double linear)
{
    return 20.0 * std::log10(std::max(linear, 1e-12));
}

double clampd(double v, double lo, double hi)
{
    return std::max(lo, std::min(hi, v));
}

// Tukey (tapered cosine) window over t in [0,1]. Flat in the middle so the
// early part of a room impulse is not attenuated, tapered so the truncation
// does not ring.
double tukey(double t, double alpha)
{
    t = clampd(t, 0.0, 1.0);
    if (alpha <= 0.0)
        return 1.0;
    if (t < alpha * 0.5)
        return 0.5 * (1.0 + std::cos(kPi * (2.0 * t / alpha - 1.0)));
    if (t > 1.0 - alpha * 0.5)
        return 0.5 * (1.0 + std::cos(kPi * (2.0 * t / alpha - 2.0 / alpha + 1.0)));
    return 1.0;
}

} // namespace

// ---------------------------------------------------------------- sweep ----

Sweep make_sweep(const SweepSpec &spec)
{
    Sweep out;
    const int sr = std::max(8000, spec.sample_rate);
    const double T = std::max(0.1, spec.seconds);
    const double f1 = clampd(spec.f1, 5.0, sr * 0.45);
    const double f2 = clampd(spec.f2, f1 * 1.5, sr * 0.49);
    const std::size_t n = (std::size_t)std::llround(T * (double)sr);
    const std::size_t lead = (std::size_t)std::llround(std::max(0.0, spec.lead_in) * (double)sr);
    const std::size_t tail = (std::size_t)std::llround(std::max(0.0, spec.tail) * (double)sr);
    const std::size_t gap = (std::size_t)std::llround(std::max(0.0, spec.gap) * (double)sr);

    std::vector<float> core(n, 0.0f);
    const double K = std::log(f2 / f1);
    // phi(t) = 2*pi*f1*T/K * (exp(t*K/T) - 1): instantaneous frequency sweeps
    // exponentially from f1 to f2.
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = (double)i / (double)sr;
        const double phase = 2.0 * kPi * f1 * T / K * (std::exp(t * K / T) - 1.0);
        core[i] = (float)(spec.amplitude * std::sin(phase));
    }

    // Raised-cosine ends stop the sweep from clicking and keep the reference
    // spectrum identical to what was actually played.
    const std::size_t fade = std::min<std::size_t>(
        (std::size_t)std::llround(std::max(0.0, spec.fade) * (double)sr), n / 2);
    for (std::size_t i = 0; i < fade; ++i)
    {
        const float w = (float)(0.5 - 0.5 * std::cos(kPi * (double)i / (double)fade));
        core[i] *= w;
        core[n - 1 - i] *= w;
    }

    out.audio.assign(lead + (std::size_t)std::max(1, spec.repeats) * n +
                         (std::size_t)(std::max(1, spec.repeats) - 1) * gap + tail,
                     0.0f);
    const std::size_t stride = n + gap;
    for (int r = 0; r < std::max(1, spec.repeats); ++r)
    {
        const std::size_t at = lead + (std::size_t)r * stride;
        std::copy(core.begin(), core.end(), out.audio.begin() + (std::ptrdiff_t)at);
    }
    out.core_begin = lead;
    out.core_len = n;
    out.repeat_stride = stride;
    return out;
}

bool write_wav16(const std::string &path, const std::vector<float> &samples,
                 int sample_rate, std::string &error)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
    {
        error = "cannot open " + path;
        return false;
    }
    const std::uint32_t data_bytes = (std::uint32_t)(samples.size() * 2);
    const std::uint32_t riff_bytes = 36 + data_bytes;
    const std::uint16_t channels = 1, bits = 16, align = 2;
    const std::uint32_t rate = (std::uint32_t)std::max(8000, sample_rate);
    const std::uint32_t byte_rate = rate * align;

    auto u32 = [&](std::uint32_t v) { f.write((const char *)&v, 4); };
    auto u16 = [&](std::uint16_t v) { f.write((const char *)&v, 2); };
    f.write("RIFF", 4);
    u32(riff_bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(channels);
    u32(rate);
    u32(byte_rate);
    u16(align);
    u16(bits);
    f.write("data", 4);
    u32(data_bytes);
    for (float s : samples)
    {
        const float c = std::max(-1.0f, std::min(1.0f, s));
        const std::int16_t v = (std::int16_t)std::lround(c * 32767.0f);
        f.write((const char *)&v, 2);
    }
    return (bool)f;
}

// ----------------------------------------------------------------- grid ----

void Grid::build()
{
    hz.resize((std::size_t)std::max(2, points));
    const double ratio = std::pow(f_hi / f_lo, 1.0 / (double)(hz.size() - 1));
    for (std::size_t i = 0; i < hz.size(); ++i)
        hz[i] = f_lo * std::pow(ratio, (double)i);
}

int Grid::nearest(double f) const
{
    if (hz.empty() || f < hz.front() || f > hz.back())
        return -1;
    const double t = (std::log(f) - std::log(hz.front())) /
                     (std::log(hz.back()) - std::log(hz.front()));
    int i = (int)std::lround(t * (double)(hz.size() - 1));
    return std::max(0, std::min((int)hz.size() - 1, i));
}

namespace
{
// Log-frequency position of each grid point in "bin units", used by the
// sliding-window smoothers.
double octave_span(const Grid &g)
{
    if (g.hz.size() < 2)
        return 1.0;
    return std::log2(g.hz.back() / g.hz.front()) / (double)(g.hz.size() - 1);
}
} // namespace

std::vector<double> smooth_octave(const Grid &g, const std::vector<double> &db_in,
                                  double fraction)
{
    const std::size_t n = g.hz.size();
    std::vector<double> out(db_in.size(), 0.0);
    if (n == 0 || db_in.size() != n)
        return out;
    const double span = std::max(1e-6, octave_span(g));
    const int half = std::max(0, (int)std::lround((fraction * 0.5) / span));

    std::vector<double> prefix(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i)
        prefix[i + 1] = prefix[i] + db_in[i];
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::size_t a = (i > (std::size_t)half) ? i - (std::size_t)half : 0;
        const std::size_t b = std::min(n - 1, i + (std::size_t)half);
        out[i] = (prefix[b + 1] - prefix[a]) / (double)(b - a + 1);
    }
    return out;
}

// ----------------------------------------------------------- measurement ----

Measurement measure(const std::vector<float> &recording, int sample_rate,
                    const SweepSpec &spec, const Grid &grid)
{
    Measurement m;
    const int sr = std::max(8000, sample_rate);
    const std::size_t n = recording.size();
    if (n < (std::size_t)(sr / 2))
    {
        m.error = "The recording is too short.";
        return m;
    }

    // Level, noise floor and clipping. The noise floor is the 10th percentile
    // of 50 ms block RMS, which ignores the sweep wherever it sits.
    float peak = 0.0f;
    std::size_t clipped = 0;
    for (float s : recording)
    {
        peak = std::max(peak, std::fabs(s));
        if (std::fabs(s) >= 0.9995f)
            ++clipped;
    }
    m.peak_dbfs = db((double)peak);
    m.clipped = (int)clipped;

    const std::size_t block = (std::size_t)(sr / 20);
    std::vector<double> levels;
    for (std::size_t i = 0; i + block <= n; i += block)
    {
        double e = 0.0;
        for (std::size_t k = 0; k < block; ++k)
            e += (double)recording[i + k] * (double)recording[i + k];
        levels.push_back(std::sqrt(e / (double)block));
    }
    if (!levels.empty())
    {
        std::sort(levels.begin(), levels.end());
        const double p10 = levels[levels.size() / 10];
        const double p95 = levels[(levels.size() * 95) / 100];
        m.noise_dbfs = db(p10);
        m.snr_db = db(p95) - db(p10);
    }

    if (m.peak_dbfs < -54.0)
    {
        m.error = "Nothing was recorded. Check that the microphone is allowed and the speakers are on.";
        return m;
    }
    if (m.clipped > (int)(n / 500))
    {
        m.error = "The recording clipped. Lower the volume and measure again.";
        return m;
    }

    // Deconvolution: H = Y * conj(X) / (|X|^2 + eps). The microphone's
    // arbitrary gain and the unknown start delay both drop out of |H|.
    Sweep ref = make_sweep(spec);
    const std::size_t len = ref.core_len;
    if (len == 0)
    {
        m.error = "Invalid sweep.";
        return m;
    }
    const std::size_t N = next_pow2(n + len);
    std::vector<cplx> X(N, cplx(0.0, 0.0)), Y(N, cplx(0.0, 0.0));
    for (std::size_t i = 0; i < len; ++i)
        X[i] = cplx((double)ref.audio[ref.core_begin + i], 0.0);
    for (std::size_t i = 0; i < n; ++i)
        Y[i] = cplx((double)recording[i], 0.0);
    fft(X, false);
    fft(Y, false);

    double max_energy = 0.0;
    for (const cplx &v : X)
        max_energy = std::max(max_energy, std::norm(v));
    const double eps = std::max(1e-30, max_energy * 1e-8);

    // One deconvolved transfer function, shared by the magnitude estimate and
    // the impulse-concentration check below.
    std::vector<cplx> H(N);
    for (std::size_t k = 0; k < N; ++k)
        H[k] = Y[k] * std::conj(X[k]) / (std::norm(X[k]) + eps);

    m.db.assign(grid.hz.size(), 0.0);
    std::vector<char> trusted(grid.hz.size(), 0);
    const double bin_hz = (double)sr / (double)N;
    double best_valid = grid.f_lo;
    bool have_valid = false;

    for (std::size_t i = 0; i < grid.hz.size(); ++i)
    {
        const double f = grid.hz[i];
        // Ignore the band edges where the sweep has no energy: the ratio is
        // pure noise there and would otherwise look like a huge correction.
        if (f < spec.f1 * 1.1 || f > spec.f2 / 1.1)
        {
            m.db[i] = std::nan("");
            continue;
        }
        const std::size_t k = (std::size_t)std::llround(f / bin_hz);
        if (k == 0 || k >= N / 2)
        {
            m.db[i] = std::nan("");
            continue;
        }
        m.db[i] = db(std::abs(H[k]));
        trusted[i] = 1;
        if (!have_valid)
        {
            best_valid = m.db[i];
            have_valid = true;
        }
    }

    // Impulse response: a sweep deconvolves to one sharp, time-localised
    // event, while noise deconvolves to something spread across the whole
    // buffer. That catches a recording made without the sweep much more
    // reliably than any level check.
    {
        // Band-limit the transfer function first: outside the sweep's band the
        // division is dominated by noise and would swamp the impulse.
        const double lo = spec.f1 * 1.05;
        const double hi = spec.f2 / 1.05;
        for (std::size_t k = 0; k < N; ++k)
        {
            const double fk = (k <= N / 2) ? (double)k * bin_hz
                                           : (double)(N - k) * bin_hz;
            if (fk < lo || fk > hi)
                H[k] = cplx(0.0, 0.0);
        }
        fft(H, true);
        double peak = 0.0, energy = 0.0;
        for (std::size_t i = 0; i < N; ++i)
        {
            const double v = std::abs(H[i]);
            peak = std::max(peak, v);
            energy += v * v;
        }
        const double rms = std::sqrt(energy / (double)N);
        m.ir_peak_ratio = peak / std::max(rms, 1e-30);
    }
    if (m.ir_peak_ratio < 8.0)
    {
        m.error = "The sweep was not found in the recording. Start the measurement "
                  "and let the sweep play while the mic records.";
        return m;
    }

    // ---- repeat averaging ------------------------------------------------
    // The measurement train contains several sweeps, so the deconvolved
    // response contains one room impulse per sweep. Windowing and power
    // averaging them is what makes repeated sweeps worth the extra seconds:
    // ambient noise and one-off glitches average down.
    const int repeats = std::max(1, spec.repeats);
    const std::size_t db_len = grid.hz.size();
    std::vector<double> spread_db(db_len, 0.0);
    if (repeats > 1 && ref.repeat_stride > 0)
    {
        double peak = 0.0;
        for (std::size_t i = 0; i < N; ++i)
            peak = std::max(peak, std::abs(H[i]));
        // Anchor on the first strong peak rather than the global one, so a
        // noisy later repeat cannot shift every window.
        std::size_t first = 0;
        for (std::size_t i = 0; i < N; ++i)
            if (std::abs(H[i]) > peak * 0.5)
            {
                first = i;
                break;
            }
        const long search = (long)(0.05 * (double)sr);
        auto local_peak = [&](std::size_t centre, double &level) {
            std::size_t best_i = centre;
            double best = -1.0;
            for (long d = -search; d <= search; ++d)
            {
                const std::size_t idx =
                    (std::size_t)(((long)centre + d + (long)N) % (long)N);
                const double v = std::abs(H[idx]);
                if (v > best)
                {
                    best = v;
                    best_i = idx;
                }
            }
            level = best;
            return best_i;
        };
        double first_level = 0.0;
        const std::size_t p0 = local_peak(first, first_level);

        const std::size_t half = (std::size_t)(1.5 * (double)sr);
        const std::size_t M = half * 2;
        const std::size_t NF = next_pow2(M);
        std::vector<double> power(grid.hz.size(), 0.0);
        std::vector<std::vector<double>> per_repeat;
        std::vector<cplx> seg(NF);
        int used = 0;
        for (int r = 0; r < repeats; ++r)
        {
            const std::size_t predicted = (p0 + (std::size_t)r * ref.repeat_stride) % N;
            double level = 0.0;
            const std::size_t centre = local_peak(predicted, level);
            // A missing repeat (a single-sweep upload) or one buried in noise
            // is skipped instead of being averaged in.
            if (r > 0 && level < first_level * 0.2)
                continue;
            std::fill(seg.begin(), seg.end(), cplx(0.0, 0.0));
            for (std::size_t i = 0; i < M; ++i)
            {
                const std::size_t idx = (centre + N - half + i) % N;
                const double w = tukey((double)i / (double)(M - 1), 0.5);
                seg[i] = cplx(H[idx].real() * w, 0.0);
            }
            fft(seg, false);
            std::vector<double> rep(db_len, std::nan(""));
            for (std::size_t i = 0; i < grid.hz.size(); ++i)
            {
                if (!trusted[i])
                    continue;
                const std::size_t k =
                    (std::size_t)std::llround(grid.hz[i] * (double)NF / (double)sr);
                if (k > 0 && k < NF / 2)
                {
                    power[i] += std::norm(seg[k]);
                    rep[i] = 10.0 * std::log10(std::max(1e-30, std::norm(seg[k])));
                }
            }
            per_repeat.push_back(std::move(rep));
            ++used;
        }
        if (used > 1)
        {
            for (std::size_t i = 0; i < grid.hz.size(); ++i)
                if (trusted[i] && power[i] > 0.0)
                    m.db[i] = 10.0 * std::log10(power[i] / (double)used);
            m.repeats_used = used;

            // Repeatability: a real response reproduces across sweeps, while a
            // noise-dominated bin scatters. The spread is what separates a
            // measurement from the noise floor.
            spread_db.assign(db_len, 0.0);
            for (std::size_t i = 0; i < db_len; ++i)
            {
                if (!trusted[i])
                    continue;
                double lo = 1e9, hi = -1e9;
                int seen = 0;
                for (const auto &rep : per_repeat)
                {
                    if (!std::isfinite(rep[i]))
                        continue;
                    lo = std::min(lo, rep[i]);
                    hi = std::max(hi, rep[i]);
                    ++seen;
                }
                if (seen >= 2)
                    spread_db[i] = hi - lo;
            }
        }
    }

    // Hold the edge values across the untrusted ends so plots stay sane.
    double last = std::nan("");
    for (std::size_t i = 0; i < m.db.size(); ++i)
        if (!std::isnan(m.db[i]))
            last = m.db[i];
        else if (!std::isnan(last))
            m.db[i] = last;
    last = std::nan("");
    for (std::size_t i = m.db.size(); i-- > 0;)
        if (!std::isnan(m.db[i]))
            last = m.db[i];
        else if (!std::isnan(last))
            m.db[i] = last;
    for (double &v : m.db)
        if (std::isnan(v))
            v = best_valid;

    // Normalise around the midband: a microphone measures relative level.
    auto midband_mean = [&](bool trust_only) {
        double sum = 0.0;
        int count = 0;
        for (std::size_t i = 0; i < grid.hz.size(); ++i)
            if (grid.hz[i] >= 250.0 && grid.hz[i] <= 2000.0 &&
                (!trust_only || m.valid[i]))
            {
                sum += m.db[i];
                ++count;
            }
        return count ? sum / (double)count : 0.0;
    };

    // Anything far outside the midband is either inaudible (a speaker that
    // cannot reach it) or an artifact. Correcting it only adds filters that
    // do nothing or make things worse, so those points are marked untrusted.
    // With repeated sweeps, repeatability separates signal from noise floor.
    m.valid.assign(db_len, 0);
    const double pre_offset = midband_mean(false);
    for (std::size_t i = 0; i < db_len; ++i)
    {
        if (!trusted[i])
            continue;
        const double rel = m.db[i] - pre_offset;
        bool ok = rel > -40.0 && rel < 25.0;
        if (m.repeats_used >= 2 && spread_db[i] > 3.5)
            ok = false;
        m.valid[i] = ok ? 1 : 0;
    }

    double offset = midband_mean(true);
    if (offset == 0.0)
        offset = pre_offset;
    for (std::size_t i = 0; i < db_len; ++i)
        m.db[i] -= offset;

    // Clean the mask: a real response is contiguous, while a single noise bin
    // can pass both tests by chance (and would then attract a filter).
    if (db_len >= 7)
    {
        std::vector<char> cleaned(db_len, 0);
        for (std::size_t i = 0; i < db_len; ++i)
        {
            int hits = 0;
            for (long d = -3; d <= 3; ++d)
            {
                const long j = (long)i + d;
                if (j >= 0 && j < (long)db_len && m.valid[(std::size_t)j])
                    ++hits;
            }
            cleaned[i] = hits >= 4 ? 1 : 0;
        }
        m.valid.swap(cleaned);
    }

    m.spread_db = spread_db;
    m.ok = true;
    return m;
}

// --------------------------------------------------------------- target ----

std::vector<double> target_curve(const Grid &g, TargetKind kind,
                                 const std::vector<double> &measured_db)
{
    std::vector<double> t(g.hz.size(), 0.0);
    if (kind == TargetKind::Flat || measured_db.size() != g.hz.size())
        return t;

    // Broadband trend of the measurement (1 octave). Correcting the difference
    // between this and the 1/6-octave curve leaves the speaker's voicing and
    // the room's bass gain alone while still taming modes and comb peaks.
    t = smooth_octave(g, measured_db, 1.0);
    if (kind == TargetKind::Warm)
    {
        for (std::size_t i = 0; i < g.hz.size(); ++i)
        {
            const double f = g.hz[i];
            const double bass = 2.5 / (1.0 + std::pow(f / 70.0, 2.0));
            const double treble = -1.0 / (1.0 + std::pow(4000.0 / f, 2.0));
            t[i] += bass + treble;
        }
    }
    return t;
}

// ---------------------------------------------------------------- design ----

double peaking_db(double f, int sample_rate, const Band &band)
{
    const double nyq = (double)sample_rate * 0.5;
    f = clampd(f, 1.0, nyq * 0.999);
    const double f0 = clampd(band.freq, 1.0, nyq * 0.999);
    const double q = std::max(0.05, band.q);
    const double A = std::pow(10.0, band.gain / 40.0);
    const double w0 = 2.0 * kPi * f0 / (double)sample_rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double b0 = 1.0 + alpha * A;
    const double b1 = -2.0 * std::cos(w0);
    const double b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A;
    const double a1 = -2.0 * std::cos(w0);
    const double a2 = 1.0 - alpha / A;

    const double w = 2.0 * kPi * f / (double)sample_rate;
    const double cw = std::cos(w), c2w = std::cos(2.0 * w);
    const double num = b0 * b0 + b1 * b1 + b2 * b2 +
                       2.0 * (b0 * b1 + b1 * b2) * cw + 2.0 * b0 * b2 * c2w;
    const double den = a0 * a0 + a1 * a1 + a2 * a2 +
                       2.0 * (a0 * a1 + a1 * a2) * cw + 2.0 * a0 * a2 * c2w;
    return 10.0 * std::log10(std::max(1e-30, num / den));
}

std::vector<double> response_db(const Grid &g, int sample_rate,
                                const std::vector<Band> &bands)
{
    std::vector<double> out(g.hz.size(), 0.0);
    for (const Band &b : bands)
        for (std::size_t i = 0; i < g.hz.size(); ++i)
            out[i] += peaking_db(g.hz[i], sample_rate, b);
    return out;
}

namespace
{
double dot(const std::vector<double> &a, const std::vector<double> &b)
{
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        s += a[i] * b[i];
    return s;
}

// Smooth window weights so the correction does not step at the band edges.
double window_weight(double f, double lo, double hi)
{
    const double lo_edge = lo / 1.2;
    const double hi_edge = hi * 1.2;
    if (f <= lo_edge || f >= hi_edge)
        return 0.0;
    if (f < lo)
        return clampd((f - lo_edge) / (lo - lo_edge), 0.0, 1.0);
    if (f > hi)
        return clampd((hi_edge - f) / (hi_edge - hi), 0.0, 1.0);
    return 1.0;
}

// Correction strength versus frequency. Above the room's transition region a
// single-point measurement is dominated by comb filtering and the direct
// sound, so a full-strength correction there makes things worse, not better.
// Bass modes get the full treatment; the top octave gets almost nothing.
double correction_taper(double f)
{
    if (f <= 400.0)
        return 1.0;
    if (f <= 2000.0)
        return 1.0 - 0.45 * std::log2(f / 400.0) / std::log2(5.0);
    if (f <= 10000.0)
        return 0.55 - 0.35 * std::log2(f / 2000.0) / std::log2(5.0);
    if (f <= 20000.0)
        return 0.20 - 0.20 * std::log2(f / 10000.0);
    return 0.0;
}
} // namespace

Design design(const Grid &g, const std::vector<double> &measured_db,
              const std::vector<double> &target_db, const DesignLimits &limits,
              int sample_rate, const std::vector<char> &valid)
{
    Design out;
    const std::size_t n = g.hz.size();
    if (n == 0 || measured_db.size() != n || target_db.size() != n)
        return out;
    if (limits.hi_hz <= limits.lo_hz * 1.01 || limits.max_bands <= 0)
        return out;
    const bool use_valid = valid.size() == n;

    // 1. Raw correction, faded out at the window edges, tapered with
    //    frequency and scaled by the user's strength, then clamped.
    std::vector<double> corr(n, 0.0), weight(n, 0.0);
    const double strength = clampd(limits.strength, 0.1, 2.0);
    for (std::size_t i = 0; i < n; ++i)
    {
        weight[i] = window_weight(g.hz[i], limits.lo_hz, limits.hi_hz);
        if (use_valid && !valid[i])
            weight[i] = 0.0;
        const double taper = correction_taper(g.hz[i]);
        const double c = (target_db[i] - measured_db[i]) * weight[i] * taper * strength;
        corr[i] = clampd(c, limits.max_cut_db, limits.max_boost_db * taper);
    }

    // 2. Never fill narrow nulls: a boost may not exceed the broad trend.
    const std::vector<double> trend = smooth_octave(g, corr, 1.0);
    for (std::size_t i = 0; i < n; ++i)
        if (corr[i] > 0.0)
            corr[i] = std::min(corr[i],
                               std::max(0.0, trend[i] + limits.boost_margin_db));

    // 3. Greedy band placement. For a fixed centre and Q the optimal gain has
    //    a closed form (least squares against the 1 dB response), so only the
    //    shape has to be searched.
    std::vector<double> residual = corr;
    const int max_bands = std::max(0, limits.max_bands);
    static const double kQs[] = {0.5, 0.7, 1.0, 1.4, 2.0, 2.8, 4.0, 5.6};

    const double lo = limits.lo_hz;
    const double hi = limits.hi_hz;
    (void)lo;
    (void)hi;

    // Candidate centres come only from trusted points. Otherwise the fitter
    // happily parks a wide filter in a region that must not be touched and
    // uses its skirt elsewhere (that produced -12 dB sub-bass holes).
    std::vector<double> centres;
    for (std::size_t i = 0; i < n; ++i)
        if (weight[i] > 0.5)
            centres.push_back(g.hz[i]);
    const std::size_t thin = 4; // 1/6 octave on the 1/24-octave grid
    if (centres.size() > thin)
    {
        std::vector<double> thinned;
        for (std::size_t i = 0; i < centres.size(); i += thin)
            thinned.push_back(centres[i]);
        centres.swap(thinned);
    }
    if (centres.empty())
        return out;

    for (int iter = 0; iter < max_bands; ++iter)
    {
        double best_sse = dot(residual, residual);
        if (best_sse < 1e-6)
            break;
        Band best;
        double best_shape_sse = best_sse;
        for (double f : centres)
        {
            for (double q : kQs)
            {
                Band unit{f, 1.0, q};
                std::vector<double> h1(n, 0.0);
                for (std::size_t i = 0; i < n; ++i)
                    h1[i] = peaking_db(g.hz[i], sample_rate, unit) * weight[i];
                const double den = dot(h1, h1);
                if (den < 1e-9)
                    continue;
                const double num = dot(residual, h1);
                const double gain = clampd(num / den, limits.max_cut_db,
                                           limits.max_boost_db);
                const double sse = std::max(0.0, best_sse - (num * num) / den);
                if (sse < best_shape_sse)
                {
                    best_shape_sse = sse;
                    best = Band{f, gain, q};
                }
            }
        }
        // Stop when a new band would not meaningfully help.
        if (best.gain == 0.0 || (best_sse - best_shape_sse) < best_sse * 0.01)
            break;

        out.bands.push_back(best);
        for (std::size_t i = 0; i < n; ++i)
            residual[i] -= peaking_db(g.hz[i], sample_rate, best) * weight[i];
    }

    // 4. Refine gain/Q with exact responses (the closed form above is only
    //    approximately linear in dB).
    static const double kGainSteps[] = {-1.0, -0.5, -0.25, 0.25, 0.5, 1.0};
    static const double kQSteps[] = {0.7, 0.85, 1.18, 1.4};
    auto cost = [&](const std::vector<Band> &bands) {
        const std::vector<double> resp = response_db(g, sample_rate, bands);
        double s = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double e = (corr[i] - resp[i] * weight[i]);
            s += e * e;
        }
        return s;
    };
    for (int pass = 0; pass < 3 && !out.bands.empty(); ++pass)
    {
        for (std::size_t bi = 0; bi < out.bands.size(); ++bi)
        {
            double best_cost = cost(out.bands);
            Band best = out.bands[bi];
            auto try_candidate = [&](const Band &cand) {
                std::vector<Band> trial = out.bands;
                trial[bi] = cand;
                const double c = cost(trial);
                if (c < best_cost)
                {
                    best_cost = c;
                    best = cand;
                }
            };
            const Band base = out.bands[bi];
            for (double dg : kGainSteps)
            {
                Band cand = base;
                cand.gain = clampd(base.gain + dg, limits.max_cut_db, limits.max_boost_db);
                try_candidate(cand);
            }
            for (double dq : kQSteps)
            {
                Band cand = best;
                cand.q = clampd(best.q * dq, 0.3, 8.0);
                try_candidate(cand);
            }
            out.bands[bi] = best;
        }
    }

    // 5. Drop filters that do nothing, then build the predicted curves.
    out.bands.erase(std::remove_if(out.bands.begin(), out.bands.end(),
                                   [](const Band &b) { return std::fabs(b.gain) < 0.4; }),
                    out.bands.end());

    out.predicted_db = response_db(g, sample_rate, out.bands);
    out.corrected_db.resize(n);
    double sse = 0.0;
    int counted = 0;
    double max_boost = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        out.corrected_db[i] = measured_db[i] + out.predicted_db[i];
        if (weight[i] > 0.5)
        {
            const double e = target_db[i] - out.corrected_db[i];
            sse += e * e;
            ++counted;
        }
        max_boost = std::max(max_boost, out.predicted_db[i]);
    }
    out.residual_rms_db = counted ? std::sqrt(sse / (double)counted) : 0.0;
    out.preamp_db = max_boost > 0.0 ? -std::min(24.0, max_boost + 0.5) : 0.0;
    return out;
}

// ---------------------------------------------------------- mic cal -------

bool parse_cal_text(const std::string &text, std::vector<CalPoint> &out,
                    std::string &error)
{
    out.clear();
    std::size_t pos = 0;
    int line_no = 0;
    while (pos <= text.size())
    {
        std::size_t end = text.find('\n', pos);
        const std::string raw = text.substr(pos, end == std::string::npos ? std::string::npos
                                                                          : end - pos);
        pos = (end == std::string::npos) ? text.size() + 1 : end + 1;
        ++line_no;

        std::string line;
        for (char c : raw)
            if (c == ',')
                line += ' ';
            else
                line += c;
        std::size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos)
            continue;
        if (line[a] == '#' || line[a] == '*' || line[a] == ';' || line[a] == '/')
            continue;
        // Strip a trailing inline comment.
        const std::size_t hash = line.find_first_of("#;*", a);
        if (hash != std::string::npos)
            line = line.substr(0, hash);

        double f = 0.0, v = 0.0;
        if (std::sscanf(line.c_str() + a, "%lf %lf", &f, &v) != 2)
            continue; // Tolerate prose headers in .cal files.
        if (f <= 0.0)
            continue;
        out.push_back({f, v});
    }
    if (out.size() < 2)
    {
        error = "That file has no frequency/gain pairs.";
        return false;
    }
    std::sort(out.begin(), out.end(),
              [](const CalPoint &x, const CalPoint &y) { return x.hz < y.hz; });
    return true;
}

std::vector<double> cal_curve(const Grid &g, const std::vector<CalPoint> &cal)
{
    std::vector<double> out(g.hz.size(), 0.0);
    if (cal.size() < 2)
        return out;
    for (std::size_t i = 0; i < g.hz.size(); ++i)
    {
        const double f = g.hz[i];
        if (f < cal.front().hz || f > cal.back().hz)
            continue;
        std::size_t hi = 1;
        while (hi < cal.size() && cal[hi].hz < f)
            ++hi;
        const CalPoint &a = cal[hi - 1];
        const CalPoint &b = cal[hi];
        const double t = (std::log(f) - std::log(a.hz)) /
                         std::max(1e-9, std::log(b.hz) - std::log(a.hz));
        out[i] = a.db + (b.db - a.db) * clampd(t, 0.0, 1.0);
    }
    return out;
}

std::vector<CalPoint> builtin_mic_cal(const std::string &profile_id)
{
    if (profile_id == "phone_generic")
        return {
            {20, -30}, {40, -20}, {60, -12}, {80, -7}, {100, -4}, {130, -2},
            {200, -0.5}, {400, 0}, {700, 0.8}, {1000, 0}, {2000, 1.0},
            {3000, 2.5}, {4000, 3.5}, {5000, 3.0}, {6000, 2.0}, {8000, 1.0},
            {10000, 0}, {12000, -1.5}, {14000, -3.5}, {16000, -6},
            {18000, -10}, {20000, -15},
        };
    if (profile_id == "galaxy_s24")
        return {
            // APPROXIMATE. No per-unit calibration data exists for the S24
            // microphone; this is a generic modern-flagship shape (bass
            // roll-off, presence lift, air roll-off) and is only good enough
            // to stop the app from trusting the phone below ~100 Hz.
            {20, -24}, {40, -15}, {60, -9}, {80, -5}, {100, -3}, {130, -1.5},
            {200, 0}, {500, 0}, {1000, 0}, {2000, 0.5}, {3000, 2.0},
            {4000, 2.8}, {5000, 2.2}, {6000, 1.2}, {8000, 0.5}, {10000, -0.5},
            {12000, -1.5}, {14000, -3.0}, {16000, -5.0}, {18000, -9.0},
            {20000, -14.0},
        };
    return {};
}

bool builtin_mic_is_approximate(const std::string &profile_id)
{
    return profile_id == "phone_generic" || profile_id == "galaxy_s24";
}
} // namespace dsp
