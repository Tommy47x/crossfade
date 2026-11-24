# Room correction

Measure your speakers where you actually sit, then let crossfade flatten the
worst peaks. The correction lives in the same `af` chain as the equalizer
(`equalizer=` biquads fed to mpv), so it needs no driver, no virtual device and
no extra process — and it works identically on Linux and Windows.

## How to use it

Settings → **Room correction**.

1. Pick a microphone profile.
2. Press **Calibrate with your phone** and scan the QR code, **or** press
   **Record with this microphone** (Linux: PulseAudio/PipeWire), **or** play the
   sweep once and upload a recording from the phone page.
3. Review the curve: gray is what the mic heard, the thin line is the target,
   green is the predicted result. Apply or discard.

The page stays open after a measurement, so **Measure again** on the phone
works without scanning a new code (it closes on cancel, "Close phone page", or
after 30 idle minutes).

The sweep pauses whatever is playing, plays three calibration sweeps at the
current volume, and resumes playback afterwards. The three responses are
averaged, which is what makes the estimate stable and lets the app tell signal
from noise.

### Three ways to capture

| Input | How | Accuracy |
|---|---|---|
| Phone microphone | Scan the QR, tap Start; the page records and uploads | Approximate above ~90 Hz |
| PC microphone | `parec` (PipeWire/PulseAudio) source picker, Linux only | Depends on the mic |
| Uploaded recording | Record the sweep yourself, decode it in the page | Depends on the recorder |

A calibrated measurement mic (UMIK-1 and similar) is the accurate option:
import its `.cal` file (REW format, `freq dB` per line) in the panel.

## What it does to the sound

- **Target** (default *Balanced*) follows the speaker's own broadband balance
  and only corrects deviations from it. This is the important part: flattening
  the whole in-room curve (*Flat*) removes the room's bass gain and makes a good
  speaker sound thin, which is why Flat is labelled "reference" and is not the
  default. *Warm* adds a gentle bass lift on top of Balanced.
- **Strength**: Gentle / Balanced / Strong scales the correction amount.
- Designs up to 12 peaking filters inside a correction window (40 Hz–16 kHz for
  a calibrated mic, 90 Hz–12 kHz for a phone), with at most +3 dB of boost and
  −12 dB of cut.
- Correction strength tapers with frequency: full below 400 Hz, ~55% at 2 kHz,
  ~20% at 10 kHz. Above the room's transition region a single-point measurement
  is dominated by comb filtering, so correcting it makes things worse.
- Never fills a narrow null: positive corrections are capped by the 1-octave
  trend, and the fitter can only place filters where the measurement is trusted.
- Adds a preamp trim for the summed boost so the output cannot clip (usually
  0 dB with Balanced, since it hardly boosts).
- Applies *after* the manual equalizer, as a separate band set, so the two never
  overwrite each other. Turning room correction off leaves the manual EQ alone.

## Trusted vs. untrusted parts of the curve

A microphone cannot measure what is not there. If a laptop speaker has no
output at 60 Hz, the deconvolved curve at 60 Hz is just the recording's noise
floor, and designing a filter from it produces the classic "it boosted/cut
bass that does not exist" result. The analysis therefore marks a point
untrustworthy when it is:

- more than 40 dB below the midband (nothing audible to correct) or more than
  25 dB above it (implausible),
- not repeatable between the three sweeps (more than 3.5 dB spread, which is
  what noise looks like),
- part of an isolated run of one or two points (a real response is contiguous).

Untrusted points are drawn dimmed, get no correction, and never attract a
filter. That is why the graph can show a big dip below a speaker's cutoff
without the EQ trying to "fix" it.

## Accuracy and honest limits

- **Phone mics are the weak link.** They are not flat, vary by unit, and the OS
  may apply voice processing. The page asks the browser to disable echo
  cancellation, noise suppression and AGC, but that is a request, not a
  guarantee. If no profile is selected and the measurement comes from a phone,
  the host reads the browser's user agent and picks the matching profile (S24)
  or the generic consumer-mic curve — it never silently assumes "flat".
- **No S24 calibration data exists publicly.** The built-in "Galaxy S24
  (approximate)" curve is a generic modern-flagship shape (bass roll-off,
  presence lift, air roll-off), labelled approximate in the UI. It is there so
  the app does not trust a phone below ~90 Hz, not because it is measured. If
  you can measure your phone against a reference mic, export a `.cal` file and
  import it — that always wins.
- **Relative, not absolute.** No SPL calibration; the curve is normalised around
  250 Hz–2 kHz.
- **One listening position, magnitude only.** No multi-seat averaging, no phase
  or time alignment, no bass management/crossovers. It is a room EQ, not a
  Dirac-class corrector.
- **Stereo playback only.** It shapes the player's output, not the whole OS.

## How the measurement works

1. **Excitation** — three 4 s exponential sine sweeps, 25 Hz–19 kHz, separated by
   1 s of silence, with lead-in, tail and raised-cosine fades
   (`dsp::make_sweep`).
2. **Capture** — the phone records the whole train (~16 s) at 48 kHz mono
   (resampled in the browser if the AudioContext picked another rate) and
   uploads raw 16-bit PCM.
3. **Deconvolution** — `H = Y·conj(X) / (|X|²+ε)` over a zero-padded FFT. The
   start delay and the microphone gain both drop out of `|H|`, so no clock sync
   is needed; drift between the PC's DAC and the phone's ADC is irrelevant for
   magnitude.
4. **Averaging** — the deconvolved response contains one room impulse per
   sweep. Each is windowed (±1.5 s, Tukey) and their power spectra averaged.
5. **Sanity checks** — silence, clipping, "the sweep is not in this file"
   (impulse peak versus the response's RMS) and the trusted/untrusted mask
   above.
6. **Analysis** — 1/6-octave smoothing on a 1/24-octave log grid, normalise at
   midband, subtract the mic calibration, derive the target.
7. **Design** — greedy band placement where the optimal gain for a centre/Q pair
   has a closed form, then coordinate refinement against the exact biquad
   response, restricted to trusted points. Reports the residual RMS before/after.
8. **Apply** — the band list goes into `settings.room_eq_*` and the existing
   `crossfade::audio_filter()` emits `loudnorm? → volume(preamp)? → equalizer…`.

## The QR / HTTPS detail

Browsers only expose the microphone on a secure origin, and a LAN IP over plain
HTTP is not one. The measurement page therefore runs HTTPS with a
self-signed certificate generated on first use (stored under
`<config>/room_eq/`, SAN includes the LAN IP). On Android Chrome you will see a
certificate warning once: **Advanced → Proceed**. The mic works after that.

When crossfade is built without OpenSSL the page falls back to HTTP, the phone
microphone is disabled and the panel says so; the PC mic and uploaded-recording
paths still work. That is also why the feature is Linux-first: Windows gets the
correction (mpv handles it) but not the `parec` capture path yet.

## Files

| File | Role |
|---|---|
| `src/dsp/fft.*` | radix-2 FFT |
| `src/dsp/room_eq.*` | sweep, deconvolution, smoothing, targets, filter design, mic calibration — no I/O, unit-tested |
| `src/eq/eq_server.*` | short-lived LAN HTTP(S) server for the phone page |
| `src/eq/eq_page.h` | single-file phone UI |
| `src/eq/calibration.*` | session state machine, sweep playback, PC capture, analysis job |
| `src/ui/ui_room_eq.cpp` | Settings panel, QR, curve graph |
| `tests/test_room_eq.cpp` | synthetic-room round trip, rejection cases, cal parsing |

## Tests

```bash
./build/test_room_eq
```

The test filters the same sweep through a known "room" (two modes plus a treble
shelf), deconvolves it, and checks that the recovered magnitude matches the
truth, that the design halves the deviation from flat, and that silence,
clipping and sweep-less noise are rejected.
