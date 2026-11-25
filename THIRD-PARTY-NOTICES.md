# Third-party notices

crossfade is distributed under **GPL-3.0** (see `LICENSE`). The following
third-party code and contributions are included or adapted, with their own
terms. This file is informational; the license texts that ship in-tree remain
authoritative.

## Vendored code

| Component | Files | License |
|---|---|---|
| Dear ImGui | `external/imgui/` | MIT (`external/imgui/LICENSE.txt`) |
| stb_image | `external/stb/stb_image.h` | MIT / public domain |
| qrcodegen | `external/qrcodegen.*` | MIT (`external/qrcodegen.LICENSE`) |
| cpp-httplib | `external/httplib.h` | MIT (`external/httplib.LICENSE`) |
| SQLite | `external/sqlite/` | Public domain |
| Inter font | `assets/fonts/` | SIL Open Font License 1.1 |

Any future vendoring (for example QuickJS, MIT) will be listed here with its
license text next to the source.

## Contributions

- **Immersive lyrics** (`src/ui/ui_lyrics_immersive.cpp`,
  `src/ui/ui_audio_reactive.*`) by Tommy47x, merged from PR #6. Contributed
  under the project's previous MIT license; those files retain their MIT
  terms and are included here in a GPL-3.0 work, which MIT permits.

## Inspiration and protocol references

- **[ytm-rs](https://github.com/grapedjuice/ytm-rs)** (GPL-3.0) informed the
  native backend's client strategy (InnerTube client selection, the JSLess
  `VISIONOS` path, HLS handling). Its word-timing provider cascade and
  translation/romanization flow are the reference for the native lyrics
  payload (`src/backend/native/browse.cpp`). The implementation here is C++,
  not a copy of the Rust binary; where its algorithms or constants are adapted,
  this notice and the file headers are the attribution. The wide-window
  cover column in the immersive view is also inspired by ytm-rs's default
  split layout. Both projects are GPL-3.0.
- **[Unison](https://unison.betterlyrics.org/)** (Better Lyrics) provides the
  word/syllable-timed TTML and enhanced LRC plus the translation/romanization
  endpoint used by the native lyrics path. Lyrics remain the property of their
  respective providers; crossfade only displays them to the user.
- **yt-dlp** is used as a fallback process at runtime, not vendored. See its
  own license at <https://github.com/yt-dlp/yt-dlp/blob/master/LICENSE>.
