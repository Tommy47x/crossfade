#include "crossfade.h"
#include "tunables.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace crossfade
{
ManualAction plan_manual(bool manual, bool pending, bool active, double elapsed,
                         double seconds)
{
    if (!manual)
        return ManualAction::None;
    if (!pending && !active)
        return ManualAction::Cancel;
    if (active && elapsed >= seconds)
        return ManualAction::Finish;
    if (!active && elapsed >= 1.0)
        return ManualAction::StartFailed;
    return ManualAction::None;
}

bool should_start_natural(const NaturalInputs &in)
{
    if (in.crossfade_duration <= 0.0)
        return false;
    if (in.already_fading)
        return false;
    if (!in.playing || in.loading)
        return false;
    if (in.repeat_one)
        return false;
    if (in.track_duration <= 0.0)
        return false;

    const double cf = std::min(in.crossfade_duration,
                               in.track_duration * tunable("playback.crossfade_cap_fraction"));
    if (cf <= 0.0)
        return false;
    if (in.position <= 0.0 || in.track_duration - in.position > cf)
        return false;
    return in.target_cached;
}

Gains gains(double progress)
{
    const double t = std::clamp(progress, 0.0, 1.0);
    // Humans hear loudness roughly as amplitude^0.6 (Stevens' law), so an
    // equal-gain ramp sounds like it is over after ~1/3 of its length. Raising
    // the incoming gain to t^(1/0.6) makes its perceived level rise linearly
    // across the whole fade. The outgoing gain is the constant-power
    // complement (in^2 + out^2 == 1), so the two tracks' powers sum to one and
    // the overall level doesn't sag in the middle.
    Gains g;
    g.incoming = std::pow(t, 1.0 / 0.6);
    g.outgoing = std::sqrt(std::max(0.0, 1.0 - g.incoming * g.incoming));
    return g;
}

std::string audio_filter(const Settings &s)
{
    std::string af;
    auto add = [&af](const char *filter) {
        if (!af.empty())
            af += ',';
        af += filter;
    };
    if (s.volume_normalization)
    {
        char norm[96];
        std::snprintf(norm, sizeof(norm), "loudnorm=I=%g:TP=%g:LRA=%g",
                      (double)tunable("playback.normalize_i"),
                      (double)tunable("playback.normalize_tp"),
                      (double)tunable("playback.normalize_lra"));
        af = norm;
    }
    auto add_band = [&](const EqBand &b) {
        if (std::fabs(b.gain) < 0.05f || b.freq < 20.0f || b.freq > 20000.0f)
            return; // flat bands cost nothing
        char band[80];
        std::snprintf(band, sizeof(band), "equalizer=f=%.1f:t=q:w=%.3f:g=%.2f",
                      (double)b.freq, (double)b.q, (double)b.gain);
        add(band);
    };
    if (s.eq_enabled)
        for (const EqBand &b : s.eq_bands)
            add_band(b);
    if (s.room_eq_enabled)
    {
        // Trim first so boosted room filters cannot clip the output.
        if (s.room_eq_preamp < -0.05f)
        {
            char trim[48];
            std::snprintf(trim, sizeof(trim), "volume=volume=%.2fdB",
                          (double)s.room_eq_preamp);
            add(trim);
        }
        for (const EqBand &b : s.room_eq_bands)
            add_band(b);
    }
    // Live tone trim. Deliberately independent of room_eq_enabled: moving a
    // tone slider should be audible immediately, even before a measurement is
    // applied (and it is not part of the plotted correction).
    if (std::fabs(s.room_eq_bass_db) >= 0.05f)
    {
        char shelf[56];
        std::snprintf(shelf, sizeof(shelf), "bass=g=%.2f:f=120", (double)s.room_eq_bass_db);
        add(shelf);
    }
    if (std::fabs(s.room_eq_treble_db) >= 0.05f)
    {
        char shelf[56];
        std::snprintf(shelf, sizeof(shelf), "treble=g=%.2f:f=4000",
                      (double)s.room_eq_treble_db);
        add(shelf);
    }
    return af;
}
} // namespace crossfade
