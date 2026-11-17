# Known issues

Reviewed findings that are intentionally **not** fixed yet. Each entry records
what is wrong, why it was deferred, and where to start. The verification fixes
that did land (2026-11-17, `cda5e89` and the follow-up working tree) are listed
at the end for context.

---

## Deferred

### MPRIS transport commands target the outgoing player during a crossfade
**Ref:** F-0003 — `src/playback/mpris.cpp:258`

`Impl::m_player` is `g_player_raw`, which during a fade points at the outgoing
main player, while metadata and position reporting already use
`display_player()` (`src/app/app.cpp:199`), which switches to the incoming
player once fade progress reaches 0.5. Play/Pause are effectively covered now:
they update `g_state.playing` and the fade ramp mirrors that onto both players.
Stop, Seek and SetPosition still act on the outgoing player.

**Why deferred:** Routing them through the display player is a user-visible
behaviour change (mid-fade seeks would move the incoming track) and needs a
decision on Stop semantics. The Windows SMTC implementation
(`src/win32/mpris_windows.cpp`) must stay behaviour-compatible.

**Start:** give `MprisPlayer` a player-provider callback (or expose
`display_player()`), use it for Stop/Seek/SetPosition and `m_position`
bookkeeping, and mirror the change in the Windows path.

### Cover cache is keyed only by scope string
**Ref:** F-0027 — `src/ui/ui_components.cpp:538`

`s_scope_cover` maps `scope -> last URL`. List call sites share one scope
(`"radio"` for every seed tile, `"party"` for two different tiles,
`"search_suggest"` for all suggestions, `"spotify_library"` for all entries),
so the "never downgrade" fallback can paint another item's cover while a new
texture loads.

**Why deferred:** requires a per-slot identity from every call site. Keys that
are too unique disable the fallback (placeholder flashes); keys that collide
keep the wrong art.

**Start:** extend `ui_draw_cover()` with a slot id (track / entry / suggestion
id), key `s_scope_cover` by that id, and keep `scope` for animation identity.

### Radio seed memoization can serve stale seeds
**Ref:** F-0030 — `src/ui/ui_radio.cpp:255`

The memo signature mixes list sizes, the newest play-log timestamp, the
recommendations size and `stats_dirty`. Equal-size content swaps (like one
track and like another; refresh recommendations with the same count; replace a
download) do not invalidate it, so seed ordering/art can be stale.

**Why deferred:** tightening the signature risks recomputing the full seed walk
(which scans history, liked, downloads, stats and recommendations) every frame.

**Start:** add an atomic `library_generation` to `AppState`, bump it at every
library mutation, and mix it into the signature.

### Generic JSON object scan is not string-aware
**Ref:** F-0034 — `src/net/ytdlp.cpp:947`

`json_find_object()` uses `rfind('{', key_pos)`, so a `{` inside a preceding
string value selects the wrong object. It feeds `parse_track_array()`
(lines ~1081/1104), `parse_youtube_playlists_response()` (~1734) and
`parse_youtube_playlist_tracks_response()` (~1813). The `"id"` scan is usually
safe because `id` is the first key; the `"videoId"`/`"playlistId"` scans can
silently drop items when a title contains `{`.

**Why deferred:** highest blast radius in the batch — this parser feeds search,
radio, liked persistence, YouTube playlists and library import.

**Start:** use a string/escape-aware backward depth scan (or enumerate array
elements the way `parse_playlist_item_pairs()` now does), and add brace-in-string
tests for all four callers.

### ZIP import extraction bytes are not capped (accepted limitation)
**Ref:** F-0024 — `py/cf_backend/transfer.py`

`manifest.json` and the entry count are now bounded. Total extracted bytes are
intentionally not capped: a legitimate backup that includes the audio cache is
multi-GB. A crafted archive can still fill the disk.

**If stricter protection is wanted:** add a free-disk preflight and per-entry
uncompressed-size sanity checks, not a total-size cap.

### A throwing task terminates the process
**Ref:** adjacent to F-0026 — `src/core/tasks.cpp:89`

`tasks::spawn()`'s thread wrapper calls `fn()` with no catch; any exception
escaping a task body calls `std::terminate`. The export path's own spawn-call
failure is now guarded (it reports "Could not start the export worker." and
clears the busy flag), but a throw *inside* any task still kills the process.

**Why deferred:** swallowing exceptions can hide bugs; choose a policy first
(log + user notification vs. crash on purpose).

**Start:** wrap `fn()` in try/catch, record the failure in the task record and
the diagnostics bundle.

---

## Reviewed, no action

- **F-0017** (`_warm_client` in `py/cf_backend/core.py`) — CPython's GIL makes
  the assignment atomic. Threads can briefly prefer different player clients;
  there is no memory-unsafe race.
- **F-0018** (`_stream_disk_loaded` set before loading) — it is an "attempt
  once" flag; a failed initial load is recovered per-id by
  `_stream_disk_lookup()` on every in-memory cache miss.
- **F-0020** (`album_for_track` broad `except` in `py/cf_backend/core.py`) —
  the lookup is best-effort metadata enrichment; failure intentionally falls
  through to the title+artist search. Narrowing the catch would only change
  which failures reach the fallback, not the outcome.
- **F-0021** (`_stream_disk_lookup` swallowing read/parse errors) — the shared
  on-disk URL cache is explicitly best-effort; a broken/absent file must not
  fail a resolve, and the caller already falls back to fresh extraction.
- **F-0022** (`_ydl_cls`/`_ydl_tried` import race) — under the GIL the worst
  case is a thread seeing `_ydl_tried` set while `_ydl_cls` is still `None`,
  which falls back to the yt-dlp subprocess for that resolve. No partial
  object or memory-unsafe access; only reachable with
  `P100_WARM_EXTRACTOR=1`.
- **F-0004** — duplicate of F-0005 (manual crossfade promotion forcing play);
  fixed with the `cda5e89` batch.

---

## Fixed (for reference)

`cda5e89` (pushed): playlist write-back parser (`F-0032`), artist-detail mutex
(`F-0008`), album worker locking (`F-0009`), MPRIS PlayPause state (`F-0001`),
session-restore track identity (`F-0002`), crossfade pause promotion (`F-0005`),
local radio seeds (`F-0029`), backend UTF-8 stdio (`F-0031`), yt-dlp timeout
(`F-0016`), ZIP backslash traversal (`F-0023`).

Follow-up working tree: podcast/Youtube worker locking (`F-0010`, `F-0011`),
home/search/Studio error-state locks (`F-0012`, `F-0013`, `F-0015`),
`backend_status` accessors and the deferred `status_text` flag (`F-0014`),
in-memory stream-cache pruning (`F-0019`), ZIP manifest/entry bounds
(`F-0024`, partial), Windows oversized-reply cap (`F-0033`) and partial-write
handling (`F-0035`).

Latest pass: endless-radio retry after an empty refill (`F-0006`), manual-fade
Cancel state cleanup (`F-0007`), symlink-free export trees (`F-0025` —
symlinks *inside* cache trees are skipped; a symlinked section root is still
archived), fresh context-menu playlist field (`F-0028`), tolerant playlist
count parsing (`F-0036`), and the export spawn guard (`F-0026`, partial).
