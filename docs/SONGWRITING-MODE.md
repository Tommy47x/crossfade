# Songwriting Mode — Implementation Plan

> Handoff document. Describes **what** to build and **how** to build it inside
> this codebase, without code. Target audience: the model that will implement it.

---

## 1. Goal

Add a first-class **Songwriting (Studio) mode** to crossfade with four pillars:

1. **Local songs** — the user can add audio files on disk and treat them like
   library tracks (play, search, use as a backing beat).
2. **A Studio tab** — a dedicated view to write lyrics over a beat, where the
   beat is either a local file or any YouTube Music search result.
3. **A waveform timeline** — a scrolling waveform of the beat with a *stationary*
   playhead; scroll-wheel zooms in/out while keeping the playhead position fixed
   on screen.
4. **A cue system** — a comprehensive set of time-anchored markers (sections,
   beats, notes) that can be **linked to lyric lines**, with beat detection to
   snap to the grid.

Non-goals for this pass: recording/exporting the finished song, pitch/tempo
correction, multi-track editing, audio FX. Design should not block them, but they
are out of scope.

---

## 2. Concepts & vocabulary

| Term | Meaning |
|---|---|
| **Beat track** | The backing audio the user writes over: either a local file or a YouTube track (which resolves to a cached local file exactly like normal playback). |
| **Studio project** | One "song idea": a beat reference + its lyrics + its cues + session UI state. Persisted as one JSON document. |
| **Cue** | A named, colored marker at a specific time, e.g. *Intro, Verse 1, Hook, Build, Drop, Note*. Optionally linked to one lyric line. |
| **Lyric line** | One line of the user's lyrics, optionally synced to a cue time. |
| **Waveform** | A precomputed, zoom-aware amplitude envelope (peaks/RMS) used to draw the beat's shape. |
| **Analysis** | The result of beat/onset detection: BPM estimate, beat times, downbeat times, confidence. |
| **Playhead** | The single position indicator; fixed at a constant screen x on the timeline. |

---

## 3. UX specification

### 3.1 The Studio tab, at a glance

- New sidebar entry **"Studio"** (BROWSE group, near Party/Smart). Keyboard
  shortcut optional (e.g. `Ctrl+Shift+S`), documented in the `?` overlay.
- The tab is a full-height, single-panel view (like Party/Stats). Layout is a
  vertical stack, top to bottom:
  1. **Beat picker header** — shows the active beat (art + title + artist) with a
     "Change beat" button, transport controls (play/pause, back 5s, forward 5s),
     a BPM readout, and a **Follow** toggle.
  2. **Timeline** — the waveform + beat ruler + cue lane + playhead. The bulk of
     the vertical space.
  3. **Lyrics editor** — a left-pane lyric list (synced, highlight-follows-playback)
     and a right-pane text editor for the selected line, with a "link to cue" /
     "add cue here" control.
  4. **Cue list** — a compact table of cues (time, type, label, linked line),
     sorted by time, with add/edit/delete/jump.

  A sensible alternate is a two-column layout (left: lyric editor + cue list,
  right: timeline). Either is fine; keep the timeline prominent.

### 3.2 Local songs

- **Adding:** a "Add local songs" button (in Studio, and also in Downloads) opens
  the native file picker. Multi-select audio files. Supported formats are whatever
  mpv/ffmpeg can decode (mp3, m4a/aac, opus, ogg, flac, wav).
- **Metadata:** title/artist/album/duration are read from the file's tags (fall
  back to filename). Embedded art becomes the thumbnail when present.
- **Where they appear:** a new **Local** section in the Studio beat picker, and a
  "Local files" entry in the Downloads list (or a dedicated list). They are
  searchable via the existing library search.
- **Missing files:** if a referenced file disappears, show it greyed with a
  "file missing" badge and let the user re-locate or remove it — never crash.

### 3.3 Timeline interaction (the hard part)

- The **playhead is a fixed vertical line** at a constant x position (e.g. the
  left third or center of the timeline). The waveform scrolls *underneath* it.
- **While playing and following**, the timeline moves so the playhead always sits
  at "now". This is the "timeline moves, indicator stationary" requirement.
- **Scroll wheel over the timeline = zoom.** Zooming changes the horizontal scale
  (pixels per second) but **keeps the time under the playhead fixed**, so the
  current position does not drift while zooming. Wheel up = zoom in, wheel down =
  zoom out.
- **Drag (horizontal) = pan.** The user can scroll to inspect other parts; this
  *temporarily detaches* the view from the playhead. Pressing **Follow** (or a
  quick double-click on the ruler, or resuming playback) re-attaches.
- **Click/drag on the ruler or waveform = scrub.** Seek mpv to that time.
- **Right-click on the waveform or ruler = add a cue** at that time (snapped to
  beat when snap is on).
- **Zoom levels:** show a readable time ruler that re-labels (e.g. `0:01`, `0:02`
  … `0:10`, `0:20`…) as the scale changes, like an audio editor.

### 3.4 Cues

- Cue **types**: `Marker` (free-form), `Section` (Intro/Verse/Chorus/Hook/Bridge/
  Outro/Other), `Bar`/`Beat` (auto-placed from analysis, optional to show), and
  `Note` (a sticky for the user).
- Each cue: label, color, time, type. Cues are shown as flags/diamonds on the
  cue lane above the waveform, plus rows in the cue list.
- **Adding:** right-click on timeline (snapped), or "Add cue" in the cue list
  (uses the current playhead time).
- **Editing:** drag a cue along the lane to move it (snap-to-beat honored);
  rename/type/recolor in the cue list; delete from either place.
- **Clicking a cue** seeks playback to it. Double-clicking a cue in the list jumps
  the playhead there and focuses it.
- **Snap-to-beat:** a toggle that quantizes cue drags/creates to the nearest beat
  (or bar) from the analysis. Off = free-form.

### 3.5 Linking lyrics ↔ cues

- Each **lyric line can be linked to at most one cue**, and each **cue to at most
  one line** (an optional 1:1 link). Section cues carry no line.
- Linking is explicit: while a lyric line is selected, "Link to cue" snaps the
  line's time to the nearest/selected cue; or "Create cue at playhead" creates a
  cue *and* links the selected line to it.
- Once linked, **moving the cue updates the line's time, and vice versa** —
  the two share a single time value, so they can never drift.
- In the lyric list, linked lines show a small colored dot matching their cue, and
  the active (currently-playing) line is highlighted/enlarged, reusing the existing
  lyrics-panel style.

---

## 4. Data model

All new types live in `src/core/types.h` (or a new `src/ui/studio_types.h`),
following the existing `LyricsView`/`Lyrics` pattern.

### 4.1 Analysis (produced by the backend, cached)

- `Waveform` — a **multi-resolution peak pyramid**: several arrays of
  `(min,max)` sample pairs at different resolutions (e.g. 32, 256, 2048, 8192
  buckets across the track). The renderer picks the resolution nearest the current
  zoom. Storing a pyramid means zooming never re-runs analysis.
- `BeatAnalysis` — `bpm` (float), `beats` (vector of seconds), `downbeats`
  (vector of seconds), `confidence` (0..1), `duration_sec`.

### 4.2 Song project (user data)

- `SongProject`:
  - `id`, `title` (defaults to beat title + " (demo)"), `created_at`, `updated_at`.
  - `beat`: a `Track`-shaped reference — for local files it's `{id:"local:…",
    url:"/abs/path", …}`; for YouTube it's the normal `Track` fields.
  - `lyrics`: vector of `LyricLine { id, text, cue_id (optional) }`.
  - `cues`: vector of `Cue { id, time_sec, type, label, color_index,
    lyric_line_id (optional) }`.
  - `ui_state`: `zoom`, `view_time`, `following`, `snap_to_beat`, `selected_line`
    (best-effort restore).
- IDs are short random strings (no crypto need) or monotonically increasing
  integers; must be stable across saves.

### 4.3 Local track registration

- Represent a local file as a `Track` where `id = "local:" + <hash of resolved
  absolute path>`, `url = absolute path`, and title/artist/album/duration/art are
  populated from tags. The `local:` prefix is the single source of truth for "do
  not run stream resolution on this track".
- Store the local-track index in `library.db` (new table + `library::` functions),
  consistent with how downloads metadata is stored. The audio stays where the user
  left it (referenced by path), not copied.

---

## 5. Component design & exact integration points

### 5.1 New tab wiring (mechanical, do first)

Touch these in order:

- `src/core/types.h` — add `Studio` to `enum class Tab`.
- `src/core/types.h` — add `STUDIO` to `enum class PlaylistSource`.
- `src/ui/ui_helpers.cpp` — `playlist_source_for_tab()`: add
  `case Tab::Studio: return PlaylistSource::STUDIO;`.
- `src/ui/ui_top.cpp` — `draw_sidebar()`: add `nav_item(state, Tab::Studio,
  UiIcon::*, "Studio", rail);` (pick an unused icon; add one to `ui_icons` if none
  fits). Also add a `case Tab::Studio` to `tab_title()` and `tab_subtitle()`.
- `src/app/app.cpp` — the tab-name mapping (~line 398) and session/log handling:
  add `case Tab::Studio`.
- `src/ui/ui_main.cpp` — `draw_main_ui()`:
  - add `Tab::Studio` to the `single_panel` condition (~line 1341),
  - add `else if (state.current_tab == Tab::Studio) draw_studio_panel(state,
    player);` to the panel dispatch (~line 1412).
- `src/ui/ui.h` — declare `void draw_studio_panel(AppState &state, MpvPlayer
  &player);` and any helpers.
- New file `src/ui/ui_studio.cpp` (and optional `src/ui/studio_*.cpp`) added to
  `SOURCES` in `CMakeLists.txt`.

This empty tab should render and navigate before any feature work begins.

### 5.2 Local songs

- **Metadata probe:** extend the existing `probe_audio_file_duration()` idea into a
  `probe_local_metadata(path, Track&)` helper (in `src/playback/mpvplayer.cpp` or
  a new `src/net/local_audio.cpp`). Use a throwaway libmpv instance and read
  `duration`, `metadata/title`, `metadata/artist`, `metadata/album`, and the
  attached-picture for art. mpv already has a duration prober, so this is the
  lowest-dependency path (no new C++ libs).
- **Persistence:** add `library::save_local_tracks(...)` / `load_local_tracks()`
  in `src/data/library.h/.cpp` plus a DB migration step (the schema already
  versions migrations).
- **Picker:** use the existing native file dialog path if one exists, otherwise an
  SDL/ImGui-native path. Verify no new dependency is introduced (a native GTK
  portal via existing GLib is acceptable; a new dialog library is not).
- **Playback short-circuit:** in `src/app/app_playback.cpp` `resolve_audio()` (and
  `begin_play_track`), if `t.id` starts with `"local:"`, set the resolved path to
  `t.url` directly and skip `find_cached_audio_path` / `fetch_stream_url`. Also
  ensure **no `stream-record` is started** for local files (do not write them into
  `audio_cache/`). The recording/finalize path (`finalize_recording`) must treat
  local playback as "nothing to record".
- **Listing/search:** surface local tracks in `state.downloaded_tracks`-style list
  or a new `state.local_tracks` vector loaded on demand; make them visible in
  library search (FTS index needs a source — either a new column/table or include
  local tracks in the search by id-prefix). Simplest: keep a separate
  `local_tracks` vector and filter it in the existing local-search path.

### 5.3 Waveform + beat analysis (backend)

New commands in `py/cf_backend/core.py` `run_command_loop()` (both backends share
this, so one change covers cookie + OAuth):

- `analyze <json>` — input `{ "path": "<local file>" }` (the C++ side always
  hands the backend a real file path: local files directly, YouTube tracks via
  their already-cached `audio_cache/<id>.*` path; if not cached yet, the C++ side
  triggers a background download first). Returns the `Waveform` pyramid +
  `BeatAnalysis`.
- `waveform <json>` — optional split command returning only the waveform pyramid,
  if the implementer prefers lazy beat detection (waveform is fast and cheap, beat
  detection is slower). Recommended: keep one `analyze` command but compute the
  waveform first and stream/return it promptly, then compute beats — or two
  commands for two different load states. Decide during implementation.

**Decode strategy (no new C++ deps):**

- Shell out to **ffmpeg** (already assumed available on Linux/macOS; on Windows
  the installer can bundle it or fall back) to decode to raw mono float PCM at a
  fixed low rate, e.g. `-f f32le -ac 1 -ar 22050 -`. Read the byte stream into
  Python.
- **Waveform:** compute min/max peak pairs per bucket at each pyramid resolution
  from the PCM (pure Python with `array` is fine; `numpy` if present is faster).
- **Beat detection:** a time-domain **onset detector** is sufficient and
  dependency-free: split the signal into a few fixed bands (simple biquad/one-pole
  filters), compute an onset-strength function (half-wave-rectified energy flux),
  then peak-pick with an adaptive threshold (sliding mean + multiplier, minimum
  spacing ~0.2 s). Estimate tempo from the inter-onset-interval histogram
  (autocorrelation of the onset envelope); snap beat times to the estimated grid;
  mark downbeats by bar periodicity. Optionally upgrade to a short-time FFT via
  `numpy` if the maintainers approve it as a new Python dependency — flag this in
  the PR rather than silently adding it.

**Caching:** write results to `<config>/studio/analysis/<source_id>.json`
(analogous to `lyrics/`). Cache is keyed by a stable source id (local path hash or
YouTube video id). Re-analysis is skipped when a fresh cache exists; offer a
"Re-analyze" button for when detection is wrong.

**C++ side:** add `fetch_analysis(...)`/`fetch_waveform(...)` in
`src/net/ytdlp.h/.cpp`, a parser for the JSON, and atomic snapshot publishing
(`std::atomic_load` on `AppState`), mirroring `fetch_lyrics` / `state.lyrics`.

### 5.4 Timeline renderer (new, the centerpiece)

State lives in the Studio panel (main-thread render state, like `LyricsAnim` in
`ui_lyrics.cpp`), keyed by the current beat's id:

- `zoom` (pixels per second), clamped `[min, max]`.
- `view_time` (the time currently pinned to the playhead x).
- `following` (bool), `playhead_px` (constant).

**Coordinate math (get this exactly right):**

- `screen_x(t) = playhead_px + (t - view_time) * zoom`.
- **Follow:** while `following && playing`, set `view_time = player.cached_time_pos()`
  each frame (optionally eased). The playhead line is drawn at `playhead_px`, and
  the waveform shifts so `t = view_time` sits there.
- **Zoom (wheel):** `zoom *= exp(-wheel * k)` for a constant `k` (~0.2–0.3 gives a
  natural feel), then clamp. `view_time` is left unchanged — this is what keeps the
  position stationary while zooming. If the playhead is following, the zoom is
  anchored at the playhead automatically because `view_time == playhead_time`.
- **Pan (drag):** `view_time -= delta_px / zoom` (drag right = look earlier). This
  sets `following = false`.
- **Scrub (click/drag on ruler or waveform):** compute `t = view_time + (mouse_x -
  playhead_px) / zoom`, clamp to `[0, duration]`, call `player.seek(t)`. While
  scrubbing, set `following = true` (or keep it, but re-sync `view_time` to the
  seek target) so the playhead snaps to the click.

**Drawing:**

- **Waveform bars:** for each pixel column, read the `(min,max)` pair at the
  appropriate pyramid resolution (chosen so there's ~1–4 samples per pixel) and
  draw a vertical line. Center on a midline; color the portion *before* the
  playhead with the accent and the portion after with a dimmer shade (common DAW
  convention, nice-to-have).
- **Beat ruler:** above the waveform, draw a time axis whose tick density adapts
  to `zoom`. Optionally draw beat/downbeat markers from the analysis.
- **Cue lane:** between ruler and waveform, render cue flags at
  `screen_x(cue.time_sec)`.
- **Playhead:** a fixed accent-colored vertical line + a small triangle handle at
  `playhead_px`, full height of the timeline.
- **Hover/scrub cursor, section tint bands** (e.g. shade chorus regions) are
  polish items.

Performance notes: drawing is a few thousand `AddLine` calls — fine. Reuse the
`set_lyrics_animating()` idiom to hold 60 FPS while the timeline is animating
(waveform scrolls continuously during playback, so it will effectively hold a
higher frame rate while the Studio tab is visible and playing). Respect low-power
mode by degrading to a slower cadence.

### 5.5 Cue system + lyric linking

- The **cue list** and **cue lane** are two views over the same `SongProject.cues`
  vector. Edits mutate the project and mark it dirty (autosave on a short timer +
  on tab close/app quit, matching how playlists/session save).
- The **lyric list** mirrors the existing synced-lyrics highlight behavior
  (active line by `player.cached_time_pos()`), but lines are the user's own and
  each carries an optional `cue_id`.
- **Two-way link invariant:** maintain a function that, on any cue-time or
  lyric-time change, updates the other side when linked. Keep it in one place so
  drag, list-edit, and "link" all go through it.
- **Undo:** optional but strongly recommended — reuse the existing Undo-toast
  pattern (snapshot the project before a mutation, allow one-step undo). Low
  priority.

### 5.6 Playback integration

- Studio transport is just `player.play()`/`seek()`/`toggle_pause()` on the
  existing player. Do **not** run Studio playback through the queue/crossfade
  machinery — it's a scrubbing/looping surface, not the main Up Next flow. The
  simplest correct approach: Studio playback uses the main `MpvPlayer` directly
  and suspends normal queue playback while Studio is active, restoring it when the
  user leaves (or keeping it paused). This is a design decision to settle during
  implementation — the key invariant is that the timeline's position source is
  always `player.cached_time_pos()`.
- **Loop region** (optional, valuable for writing): allow selecting a start/end
  and looping it via mpv's `ab-loop-a`/`ab-loop-b` properties (already an mpv
  feature, no new code beyond setting two properties and drawing the loop region).

---

## 6. Persistence & caching summary

| What | Where |
|---|---|
| Studio projects | `<config>/studio/projects/<id>.json` (one JSON per project, like `playlists/`). |
| Analysis (waveform + beats) | `<config>/studio/analysis/<source_id>.json` (like `lyrics/`). |
| Local track index | `library.db` new table (like downloads metadata). |
| Local audio | stays in place, referenced by absolute path (not copied). |
| Settings (defaults, snap, zoom) | `settings.json` via the existing `Settings` struct. |

All user data stays in the config directory, never next to the binary — per the
project's ground rules.

---

## 7. Settings additions

Add a **Studio** section in `src/ui/ui_settings.cpp`:

- Default beat-snap on/off.
- Default zoom / whether to remember zoom per project.
- Analysis quality (faster vs. more accurate onset detection).
- (Optional) loop behavior defaults.

Each field flows through `Settings` in `src/core/types.h` **and its `operator==`**
(the equality check at the bottom of the struct must include new fields or periodic
saves won't fire).

---

## 8. Phased implementation order

Each phase ends with a buildable, runnable state.

1. **Phase 0 — skeleton.** Add the `Studio` tab end-to-end (empty panel), sidebar
   entry, title/subtitle, dispatch. Verify navigation and layout.
2. **Phase 1 — local songs.** Metadata probe, local-track index in `library.db`,
   "Add local songs" picker, local-track playback short-circuit (no stream resolve,
   no recording). Verify a local file plays and appears in search.
3. **Phase 2 — beat picker.** In Studio, choose a beat: local files list or YouTube
   search results. Reuse existing search/back-end flows for the YouTube side.
   Resolve/cache the beat's audio path.
4. **Phase 3 — analysis.** Backend `analyze` command (ffmpeg decode → waveform
   pyramid + onset/beat detection), caching, C++ fetch + snapshot plumbing. Verify
   with a cached JSON you can inspect by hand.
5. **Phase 4 — timeline renderer.** Stationary playhead, follow mode, scroll-wheel
   zoom anchored at the playhead, drag-to-pan, click-to-scrub, beat ruler. This is
   the highest-risk piece; get the coordinate math right before adding cues.
6. **Phase 5 — cues.** Cue data model, cue lane rendering, add/drag/edit/delete,
   snap-to-beat, click-to-jump, cue list.
7. **Phase 6 — lyrics + linking.** Lyric editor, synced highlight, link/unlink to
   cues, two-way time binding, project save/load/autosave.
8. **Phase 7 — polish.** Undo, loop region, section tint bands, low-power behavior,
   re-analyze, keyboard shortcuts, missing-file recovery.

---

## 9. Testing & acceptance criteria

- **Unit tests** (extend `tests/`, C++ and `tests/test_backend_protocol.py`):
  - Local-track id derivation and `local:` playback short-circuit decision.
  - Project JSON round-trip (save/load) and the cue↔lyric link invariant.
  - Coordinate math: `screen_x(t)` inversion (screen→time and time→screen),
    zoom-anchor invariance (changing `zoom` with fixed `view_time` leaves the
    time under the playhead unchanged).
  - Backend: LRC-free analysis — waveform pyramid bucket counts, beat-list
    monotonicity, BPM within a sane range; `analyze` error handling for a
    corrupt/missing file.
- **Manual acceptance:**
  - A local mp3/flac plays, shows tags, and survives a restart (index intact).
  - Playhead stays visually fixed while the timeline scrolls during playback.
  - Zooming in/out with the wheel never moves the current position off the
    playhead line.
  - Panning detaches the view; **Follow** re-attaches instantly.
  - Scrub seeks accurately at multiple zoom levels.
  - Adding a cue at a beat snaps to that beat; dragging a linked cue moves its
    lyric line time and vice versa.
  - A YouTube beat that isn't cached yet downloads, analyzes, and plays.
  - Everything works with animations off and in low-power mode (no crash, correct
    if less smooth).

---

## 10. Risks & decisions to settle during implementation

1. **Waveform/beat decode dependency.** Plan assumes ffmpeg on the host. Decide:
   bundle ffmpeg with Windows installs, or detect-and-gracefully-disable analysis
   when absent (show "ffmpeg required for waveform/beat analysis" with a clear
   hint). Do **not** add a C++ audio library.
2. **numpy/librosa in the Python backend.** The plan targets a dependency-free
   time-domain onset detector. If quality is unacceptable, propose `numpy` (not
   `librosa`) as an explicit new Python dependency and get sign-off — the repo
   ground rules say "no new dependencies without discussion."
3. **Studio playback vs. the global queue.** Decide whether Studio uses the main
   player directly (simplest, recommended) and how it interacts with the running
   Up Next queue/crossfade. Ensure the bottom bar's "now playing" doesn't fight
   the Studio transport.
4. **Where local tracks surface.** Downloads list vs. a dedicated "Local files"
   list vs. Studio-only. Recommend: a shared local index visible in both Studio's
   beat picker and a Downloads subsection, to avoid a second source of truth.
5. **Playhead x position.** Left-third (DAW convention) vs. center. Pick one
   constant and keep it stable across resize (recompute as a fraction of width).
6. **Analysis cache invalidation.** Key by source id + file mtime/size for local
   files (so re-adding a changed file re-analyzes); by video id for YouTube.

---

## 11. Naming conventions to follow

- New UI files: `src/ui/ui_studio.cpp` (+ `.h` if needed), consistent with
  `ui_lyrics.cpp`, `ui_party.cpp`.
- Namespace `studio::` for pure helpers (project IO, link invariant, coordinate
  math) so they can be unit-tested without a GL context.
- Follow the existing idioms: `std::atomic_load`/`atomic_store` snapshots for
  worker-fed data, main-thread-owned vectors for user edits, detached
  `tasks::spawn` workers for network/analysis, and JSON sidecars in the config dir.
