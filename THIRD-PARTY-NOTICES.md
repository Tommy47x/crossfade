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
  design of the native backend's client strategy (InnerTube client selection,
  the JSLess `VISIONOS` path, HLS handling). No Rust code has been copied.
  If a future change adapts a substantial portion of its implementation, the
  adapted code will be marked here and in the file header, as GPL-3.0 allows.
- **yt-dlp** is used as a fallback process at runtime, not vendored. See its
  own license at <https://github.com/yt-dlp/yt-dlp/blob/master/LICENSE>.
