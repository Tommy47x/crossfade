# Changelog

All notable changes to crossfade are documented here.

## 0.5.1 - 2026-11-19

### Added

- **Resilient sign-in.** Settings → YouTube Music now has a sign-in panel that
  tries whatever webview engine is installed, with no hard dependency:
  WebKitGTK 6.0/4.1/4.0, Qt WebEngine (PyQt6/PySide6/PyQt5/PySide2), and
  Windows WebView2 through the optional `pywebview` package. It shows what was
  detected, surfaces the helper's output and errors in-app, and falls back to a
  paste box that accepts any browser's DevTools output. The old path only
  caught `RuntimeError`, so missing `gi`/WebKit typelibs skipped the fallback
  and left no visible error.
- **Import an existing browser session** from Firefox, LibreWolf, Chrome,
  Chromium, Edge or Brave. It is an explicit action (never automatic), and the
  imported snapshot is verified before it atomically replaces `browser.json`,
  so an expired session leaves the current one untouched. Chromium-family
  decryption is delegated to the already-required yt-dlp.
- A shared `cf_backend.login` module owns header cleaning, SAPISIDHASH,
  cookie filtering, verification and atomic replacement; `make_browser_json.py`
  is now a thin CLI over it. The webview cookie check now requires
  `__Secure-3PAPISID` as ytmusicapi does.
- Python regression tests for header cleaning, cookie mapping, SAPISIDHASH,
  atomic install, yt-dlp cookie extraction and the helper CLI/`--probe`.

### Changed

- **Refresh rate now defaults to Auto**, so 120/144/240 Hz monitors render at
  their native rate while the interface is moving. 60 Hz displays behave
  exactly as before; playback and idle budgets are unchanged.
- **OAuth is hidden from Settings** for now. The backend remains available as
  an experimental CLI path with your own Google Cloud client. An existing
  `auth_mode: "oauth"` config shows a one-click switch back to browser cookies.

## 0.5.0 - 2026-11-19

### Added

- **Display refresh rate setting.** Settings -> Appearance now offers 60, 120,
  144, 240 and Auto. Auto follows the monitor's current refresh rate (capped at
  240 Hz). The setting only raises the frame budget while the interface is
  animating, scrolling, handling input or showing video; playback and idle
  cadences are unchanged, so high-refresh rendering costs nothing while parked.
  The default stays 60, so existing setups keep their current power use. This
  replaces the old `ui.frame_active_ms` tunable.
- **Update checks on Linux.** The existing GitHub release check now runs on
  every platform (at most once a day), and **Settings -> Updates** shows the
  installed and latest versions, the last check, a manual **Check now**, and a
  toggle for automatic checks. A newer release still surfaces as a toast.
- **Linux self-update** for writable user installs: AppImage, the release
  tarball, and prefixes created by `./install.sh`. **Update now** downloads the
  matching asset, verifies it against the release's new `SHA256SUMS`, stages
  it, and then applies it on **Restart & update** or the next normal launch.
  Settings, library, downloads and the Python venv are preserved; only the venv
  requirements are refreshed when they change. Package-managed installs
  (deb/rpm/Arch/Gentoo, `/usr`, `/usr/local`) are never rewritten and fall back
  to opening the release page.
- The release workflow now publishes `SHA256SUMS`, and the rpm package now
  depends on `curl` (already required for thumbnails and update checks).

## 0.4.2 - 2026-11-19

### Added

- **Rebindable keyboard shortcuts.** Settings -> Shortcuts is now a full editor:
  click a binding, press the new keys (Esc cancels), reset per row or all,
  with duplicate bindings flagged. Overrides are stored in
  `<config>/shortcuts.json`. The shortcut help popup and the README table now
  follow the same registry.
- **Scroll to top** — press **T** (rebindable) to jump the current list back to
  the start, the same action as the floating back-to-top button.
- **Toggle low power mode** — press **Shift+P** with a toast confirming the new
  state (and a note when the battery setting keeps it on).
- **Settings -> Tunables.** A searchable panel with every value that used to be
  hardcoded: playback retries and fades, search/typeahead debounce and limits,
  prefetch counts, video sync thresholds, frame budgets, scroll physics, toast
  timing, layout spacing and radii, thumbnail decode limits, party rate limits,
  diagnostics watchdogs, and the backend's cache/timeout/alignment constants.
  Overrides are stored in `<config>/tunables.json`, each row has a reset button,
  and there is a **Reset all**. Backend values are marked `restart` because the
  Python process reads them at startup.
- The backend now reads `tunables.json` at startup, so tunables marked
  `restart` reach both C++ and Python without a rebuild.

### Fixed

- **Repeated touch drags on the bottom bar.** One drag could leave the tracked
  finger latched (a missed or rewritten FINGERUP), after which ImGui filtered
  every new press as a duplicate and the volume/seek sliders stopped responding
  to touch. A new touch now clears a stale gesture, a release from the same
  touch device ends it even if the finger id changed, and the input layer
  reconciles with ImGui's button state each frame.

## 0.4.1 - 2026-11-19

### Added

- **Search field on the guest Home page.** With no account the feed is empty, so
  Home now shows a search box centred on the page, with a "Log in to see
  YouTube's feed" hint and sign-in kept one button away. Signed-in users keep
  the populated feed and the header field.
- **Video and lyrics side by side.** The music-video page now replaces only the
  list: an open lyrics panel stays docked beside the picture. Theater mode
  still takes the full width.
- **Videos work from search results.** Starting a search still drops the video
  page, but pressing **V** on a search result now opens that song's video while
  the search stays up; before, the page closed itself on the next frame.
- **Scroll edge fades** on list pages and the lyrics panel (configurable height,
  0 disables them) with a floating **back to top** button.
- **Touch drags on the bottom bar.** The seek strip, the seek slider and the
  volume slider now follow a finger; the seek commits once, on release, so a
  drag does not hammer mpv.
- **Video stream recovery.** A video whose cached URL 403s, or a stream that
  never finishes loading, is re-resolved once in the background and falls back
  to audio-only with a notice when it still cannot play.

### Changed

- **Glowing separators are off by default.** The option is unchanged in
  Settings -> Appearance; new installs get the plain hairline.
- The ambient backdrop gets deeper glow spots and slow drift, and UI elements
  can be scaled individually.
- A new search query jumps the list back to the top instead of keeping the
  previous scroll offset.

### Documentation

- README: Windows is documented as a first-class platform alongside Linux, and
  the source-size table now lists line counts per folder.

## 0.4.0 - 2026-11-18

### Added

- **Studio (songwriting) mode.** A new **Studio** tab keeps song ideas as
  projects: pick a beat from the queue or from local audio files, see its
  waveform and beat analysis on a stationary-playhead timeline, set cues, and
  write lyrics with a 1:1 cue link per line. Each project is one JSON document,
  so it survives restarts and exports with the rest of your data.
- **Music video playback.** Press **V** (or the bottom-bar video button) to
  show the current track's video in the main area while the audio keeps playing
  on its own player — pause, seek and track changes stay in lockstep. The
  track's own upload appears immediately and a matched official video is
  hot-swapped in at the same position, with the audio/video offset measured
  once by cross-correlation. Matches are cached for 30 days, resolved stream
  URLs for an hour, matched videos can be downloaded for offline use, and
  **F** or double-click toggles theater mode.
- **Public party links.** A party can now be shared over a Cloudflare tunnel as
  well as over the same Wi-Fi. `cloudflared` is detected, and Settings → Party
  says so when it is missing.
- **Party guests can move a song to the top of the queue.** Every song in
  **Up next** on the guest page is now tappable (and keyboard accessible):
  tapping one moves it ahead of everything else so it plays next.
- **Optional guest media controls.** A new **Settings → Party → Let guests
  control playback** toggle (off by default) adds play/pause, previous and next
  buttons to the guest page, rate-limited per guest.
- **Guest mode.** Without `browser.json` the app now starts unauthenticated
  instead of failing: search, albums, artists, radio, lyrics, podcasts,
  downloads and local playlists work, and the Home, YouTube and Podcasts tabs
  link straight to sign-in.
- **Top result.** Search shows a hero band with the best match above the other
  results.
- **Local audio files** can be added to the library and used as Studio beats.
- **Backend debug window** (**F2**) shows the backend channels and their recent
  traffic.
- **Gentoo packaging** under `packaging/gentoo/media-sound/crossfade`.

### Fixed

- Playlist write-back parses ytmusicapi item ids correctly again, so removals
  and reorders reach the server.
- Scrolling over the bottom-bar volume slider changes the volume (5% per notch,
  with fractional touchpad deltas accumulating) as the README always claimed.
- MPRIS PlayPause updates the app's own play state, and promoting a crossfade
  preserves a pause.
- Session restore only seeks the restored track, local files are excluded from
  radio seeds, and restored queues keep their paused state.
- Endless radio retries with backoff when a refill comes back empty instead of
  silently stopping.
- A manual track pick cancels a stale fade target instead of leaving it set.

### Changed

- **Thread safety:** podcast and YouTube library state is published under a
  per-view mutex, artist detail data is locked, inline error strings share a
  leaf mutex, and backend status is read through accessors.
- The backend prunes and caps its in-memory stream URL cache like the disk
  cache; backend stdio is pinned to UTF-8 and yt-dlp resolves are time-bounded.
- ZIP import bounds the manifest size, the entry count and the staging path
  (including Windows backslash traversal), and export no longer follows
  symlinks when walking cache trees.
- On Windows, the backend writer loops until a command is fully sent and
  oversized replies are capped.
- Reviewed-but-deferred issues are recorded in `docs/KNOWN-ISSUES.md`.

## 0.3.3 - 2026-11-08

### Fixed

- **Smooth 60 FPS on Windows.** The main loop timed the next frame from the
  moment `SDL_GL_SwapWindow()` returned, so its 16 ms sleep stacked on top of
  vsync's own block and animated frames landed ~33 ms apart — a hard ~30 FPS
  cap, and lower once a frame took a few milliseconds to build. The deadline is
  now anchored to the start of the frame, so vsync alone paces 60 FPS and the
  sleep only fills whatever time is left. Linux looked fine because Mesa and
  compositing window managers return from the swap immediately.

## 0.3.2 - 2026-11-08

### Added

- **Right-click → Download is back.** Any track list can keep a song offline
  from its context menu; cached songs offer **Remove download** instead.
  Downloads appear in the Downloads tab as soon as the worker finishes, and
  the removal action updates the list immediately.

## 0.3.1 - 2026-11-08

### Added

- **Windows desktop integration.** Native System Media Transport Controls
  (media flyout, hardware media keys, lock screen, album art, timeline seek)
  and WinRT track notifications with cached cover art. Signing in now uses a
  Tkinter window plus your default browser instead of requiring WebKitGTK,
  Firefox cookie import works on Windows, and an already-running app picks up
  a new session automatically.
- **Windows warm start.** The single-instance warm start now works on Windows
  through a per-session named pipe: `--warm`, `--toggle`, `--quit` and
  `--new-instance` behave like the Linux build. The Windows installer asks
  whether to start crossfade hidden at sign-in, so reopening it from the Start
  Menu is instant.
- **No console window on Windows.** The app links as a GUI application; helper
  processes (backend, yt-dlp, curl, login) are started without console windows.
- **Reliable predownloading.** The installer now provides Deno and
  `yt-dlp[default]` (the EJS challenge solver), which current yt-dlp releases
  require to download YouTube audio. Downloads are launched directly (no
  `cmd.exe` quoting), fall back to the cookie-free `visionos` client, and
  failures are recorded in `<config>/downloads.log` instead of being silent.

### Fixed

- Windows sign-in no longer fails with `No module named 'gi'`, and the backend
  is found through a dev-tree or installed venv.
- Seeking on Windows no longer occasionally jumps to the next song: a natural
  crossfade is suppressed for a settling window after any seek, and the media
  flyout's duration refreshes when mpv reports the new track.

## 0.3.0 - 2026-11-08

### Added

- **Warm start.** Closing the window can park crossfade resident behind a
  single-instance socket. Launching `crossfade` again is **0 s of startup**:
  it raises the existing window instead of rebuilding the UI, backend and
  caches. `--warm`, `--toggle`, `--quit` and `--new-instance` expose the
  behaviour from the shell, and Settings chooses whether the backend stays
  warm or is parked.
- **Configurable multi-band equalizer.** Choose 5, 10, 15, 31 or a custom
  number of bands, with presets, per-band frequency/gain/Q, and vertically
  scrollable faders that stay usable at every UI scale.
- **In-app ZIP data transfer.** Settings → System → Data transfer exports
  selectable sections (settings, sign-in, library, album/artist caches, lyrics,
  thumbnails, downloaded audio, logs) to one portable ZIP and restores it on
  another machine. Import runs after the app closes, keeps a
  `crossfade-pre-import-*` safety copy, validates paths, and rebases old
  Linux/Windows audio paths by track id.
- **Full diagnostics report.** One bundle with system/CPU/GPU/SDL/mpv/Python
  versions, startup phase timings, task/backend timing, RSS/PSS, cache sizes,
  live state and recent logs, plus a history toggle.
- **Low power mode.** A Settings → System → Power toggle caps the UI at ~4 FPS
  during playback, disables motion, and skips speculative stream warming,
  background downloads and cover sweeps. It can turn itself on when the system
  reports battery power, and `--low-power` / `P100_LOW_POWER=1` force it for a
  session without changing saved settings. Playback itself is unaffected.
- **Shader-generated hero blur.** A cached glass/cover blur with aspect-preserving
  sampling replaces the old per-frame hero blur work.
- **Podcasts.** A first-class Podcasts tab backed by `ytmusicapi`: browse saved
  shows, search the YouTube Music podcast catalogue, open a show with its
  recent episodes, play episodes through the normal queue, save shows locally,
  and download a single episode or a whole show for offline listening.
- **Spotify playlist import.** Paste a public Spotify playlist link in
  Playlists; crossfade reads its track list, matches each song on YouTube
  Music, then creates a local playlist. Optional Spotify sign-in stores an
  `sp_dc` cookie for private playlists.
- **Seamless Google sign-in.** Settings can open a real WebKitGTK sign-in popup
  (WebKit 6/GTK4 or WebKit2GTK 4.1/GTK3) and write `browser.json` directly once
  the session cookie appears. If WebKitGTK is missing it imports the existing
  Firefox session instead. No OAuth client keys or manual header copying.

### Changed

- **Startup is much faster.** The Python backend, library data and optional
  workers load in parallel with window/GL init; the UI is shown before the
  backend is ready, and early commands are gated on readiness. Cold start to
  first audio is now ~0.35 s on the author's machine, with the window painted
  in ~0.30 s.
- **Settings redesigned.** A two-pane layout with a vertical category rail,
  rounded section cards, a live scale preview, custom sliders, switches and
  segmented controls. The centred column is gone; content stays left-aligned
  and reflows cleanly at 75%, 100% and 200% scale.
- **Fine-tune works for every element.** Top navigation, search bar, bottom bar,
  track thumbnails, row spacing, bottom cover, transport controls, seek/volume
  bars and the lyrics cover all honour their individual multiplier.
- **Downloads sync is ~60× faster.** The page updates from cached audio
  metadata in ~20 ms instead of ~1.2 s, and cached-audio lookups probe known
  extensions instead of scanning the whole cache.
- **AppImage** now ships `libjack.so.0` and puts the bundled library directory
  on the loader path, fixing the startup failure on systems without JACK.
- **Theme.** Spotify-style green accent and a header credit; albums, artists and
  podcasts share the same animated hero and staggered lists.

### Removed

- **Visualizer.** The experimental audio-reactive page and its `parec` capture
  thread were removed. It kept capturing system audio after the page was closed
  and pinned the UI at 60 FPS, so it was a poor trade for power.

### Fixed

- **Seek bar pinned the UI at 60 FPS.** The knob spring never settled while a
  track was playing (the target moved by more than its snap threshold each
  frame), so every song ran the full layout + GL redraw at 60 FPS. The spring
  now only runs while hovering/seeking or briefly after a seek; steady
  playback returns to the 10 FPS budget.
- **Settings pinned the UI at 60 FPS.** The animated equalizer in the live
  preview requested a frame forever. It is now static.
- **Per-frame style rescaling.** `apply_ui_scale()` no longer re-runs
  `ScaleAllSizes()` and the full palette assignment on every rendered frame.
- **Bottom bar read mpv properties synchronously every frame.** It now uses
  the cached time/duration observers.
- **Thumbnails were uploaded at full 1280×720 resolution.** Covers are now
  box-downscaled to at most 512 px before the GPU upload.
- **UI freezes after background work.** The worker registry could deadlock:
  a task published its `finished` flag before releasing the registry mutex, so
  `spawn()` could join it from inside that same mutex while the task waited for
  it. Browsing Radio, Podcasts or History could hang the main thread
  permanently. Finished is now published after the mutex is released, with a
  stress test covering it.
- **Hang diagnostics.** A watchdog now detects a main loop that has not ticked
  for ~60 s, writes `hang-<time>.log`, and signals the stuck thread so the
  normal crash log includes its stack.
- **Party search no longer freezes.** Speculative stream warming moved to a
  dedicated backend channel, so a slow yt-dlp resolve cannot hold up a guest
  search (or the app's own search/metadata requests). A failed lookup now
  reports "search unavailable" instead of being cached as no matches.
- **Radio station error panel.** The retry panel no longer trips ImGui's
  `SetCursorPos` boundary assert when the page is closed.
- **Download and history crashes.** Deletes are deferred and serialized against
  the background metadata finalizer, so a delete can no longer race a
  half-written sidecar or a widget rebuild.
- **yt-dlp cookies and user agent.** `cookiesfrombrowser` is passed as a tuple
  in the warm extractor, and the broken custom user-agent option was dropped.
- **Queue/session restore.** The startup split no longer drops the restored
  queue, and quitting with no track loaded keeps the last session instead of
  erasing it.
- **Album fetch race.** An album page closed while its fetch was still running
  can no longer touch freed state.
- **Persistence escaping.** JSON writes are escaped consistently, including
  notes, party names and playlist metadata.

## 0.2.0 - 2026-11-05

### Added

- **Local SQLite library.** Likes, playlists, the queue, history, play counts and log,
  saved albums/artists, and downloaded-track metadata now live in `library.db`, backed
  by the vendored SQLite amalgamation with FTS5. Audio files stay in `audio_cache/`.
  `settings.json` and `session.json` remain JSON. Existing JSON data is imported once,
  backed up to `library-backup-*`, and left in place as a fallback if the database can't
  be opened.
- **Local library in every search.** Normal search now merges library matches ahead of
  YouTube results, so downloaded and liked tracks are one query away. Search animates
  its first batch once and only new rows after that.
- **Diagnostics panel** in Settings: copy a diagnostics bundle, run an SQLite integrity
  check, back up the library, open the config folder, view recent log lines, and toggle
  **Safe mode on next launch**. Crashes now write local `crash-*.log` (Linux) or
  `crash-*.dmp` (Windows) files.
- **Undo** for clearing Up Next, deleting a playlist, and the Danger Zone reset buttons.
- **Onboarding copy** in the empty Home state.
- **Tests and CI.** CTest now covers the library, party queue/link helpers, crossfade
  decisions, cache eviction, JSON/lyrics parsing, sessions, playlists and settings.
  CI runs GCC and Clang builds, ASan/UBSan, TSan, the Python backend protocol test, and
  a `scripts/bench.sh` resource report.

### Changed

- Empty Home states now point at search, party mode, and the shortcut help instead of
  only suggesting a refresh.
- Search and Refresh stay pinned to the right edge and adapt to narrow windows.
- The source tree is split under `src/` (app, core, playback, data, net, ui, party).
- Cookie and OAuth backends share one command loop in `py/cf_backend/core.py`.
- Short-lived workers are tracked and drained on shutdown; SIGTERM exits cleanly and
  closes SQLite without leaving a WAL behind.

## 0.1.10 - 2026-11-04

### Added

- Artist pages now keep the original five top songs and offer a **Load 5 more
  songs** button, paging through the artist's full songs shelf. The backend
  caches the full list per artist, and expanded lists persist in the artist
  cache.

### Changed

- Crossfade visuals now switch the bottom bar metadata, progress, lyrics and
  backdrop around the visual midpoint. The volume slider stays at the user
  volume instead of showing the temporary fade gain.
- Added the optional **Smooth backdrop shader**: one full-screen pass with
  in-shader dithering, replacing the layered ImGui ellipses. It is toggleable
  in Settings and syncs its colour wash with the crossfade midpoint.
- Removed the dark panel-wide tab-switch fade; scroll edges now fade the
  content itself (track rows, media cards and lyric lines) so the backdrop is
  never covered by a sampled colour band.
- Radio seed cards keep their entrance pop but no longer fade their cover
  images, and the Station Tracks list waits for the seed cards to settle.
- README Party-mode docs refreshed with new host and Present QR screenshots.

## 0.1.9 - 2026-11-04

### Added

- **Windows auto-update.** At startup the app checks the latest GitHub release
  and offers **Update now** in a toast. The updater launches the one-click
  installer in its own console, which installs the new build and asks whether
  to restart crossfade. Linux remains on package-manager/manual updates.

## 0.1.8 - 2026-11-04

### Fixed

- Party guest page: cover art in Up Next now resolves for host playlist tracks
  too, not only the current song and guest requests. The host registers cached
  artwork for the next 40 queue rows and downloads missing thumbnails in the
  background; the phone retries until they are ready.

## 0.1.7 - 2026-11-04

### Added

- `Q` opens the Up Next queue, matching the bottom-bar queue button.

### Fixed

- **One global Up Next queue.** Starting a playlist/album/artist/radio now
  replaces the queue with that list, and both manual Next and natural
  end-of-track pop the same queue. Previously manual Next skipped the queue,
  so guest party requests (and other queued songs) only played when a track
  ended by itself.
- Guest requests still outrank the host playlist but stay below explicit
  **Play next** pins; the party queue cap now counts guest requests instead of
  the whole host playlist.

## 0.1.6 - 2026-11-04

### Changed

- Guest party page: ambient accent glow and glassy cards that follow the app's
  palette, clearer "Search YouTube or paste a link" copy, and an Add/Search
  button that switches automatically.
- Search result thumbnails retry the local art endpoint before falling back to
  a music-note placeholder, so results no longer show broken images while the
  host finishes caching artwork.

## 0.1.5 - 2026-11-04

### Added

- **Party mode** — start a local-network listening party from the new **Party**
  view (or press `P`). The host shows a QR code and link; guests on the same
  Wi-Fi open a self-contained mobile page to add YouTube / YouTube Music songs.
- Optional host approval for guest requests, fair round-robin guest queueing,
  per-guest limits (songs, cooldown, video length) and a queue cap.
- Fullscreen QR code for a TV or projector, with `Esc` to return.
- Guest page search with add, undo, now-playing progress and Up Next.
- `meta <video_id>` command in both Python backends for lightweight track
  metadata lookups without resolving a stream.
- Party settings persist across restarts; the party token, guest ids and guest
  names never touch disk, and the party never auto-starts.
- Android 16+ connection help: Chrome may require the **Nearby devices**
  permission before it can open a LAN address such as the party link.

### Changed

- **Party tab overhaul.** The tab now uses the same animated hero style as
  Radio. **Start party** puts a working QR code on the page immediately; setup
  fields moved to **Settings → Party**, with a direct shortcut from the hero.
- **Present QR** is now a true top-most modal: it dims the app, blocks clicks
  to tabs and lists, and closes with `Esc` or the close button. Starting a
  party no longer auto-opens fullscreen.
- The floating party indicator moved out of the now-playing panel and toast
  stack, and the connection help includes a `/healthz` link for phone-side
  diagnostics.

### Security

- Party HTTP server binds to one LAN IPv4 address, validates `Host`, requires a
  per-session token plus an HttpOnly `SameSite=Strict` cookie, and rejects
  state-changing requests without the `X-CF: 1` header.
- Inputs, guest count, queue length and the request inbox are bounded; only
  validated artwork ids are served from disk.

### Testing

- Added unit tests for link parsing, queue interleaving and input sanitising,
  plus a headless HTTP smoke test. Tests are opt-in with
  `-DCROSSFADE_BUILD_TESTS=ON`; `./mk` is unchanged.
