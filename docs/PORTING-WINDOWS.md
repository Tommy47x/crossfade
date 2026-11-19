# Windows platform notes

Windows is a first-class target: the same C++/OpenGL/ImGui app, feature set and
unit tests as Linux, built with MSYS2 UCRT64. CI
(`.github/workflows/build.yml`) builds and tests the Windows binary on every
push and ships it with each tagged release, alongside the Linux packages. The
desktop-integration layer (media controls, notifications) has been exercised
end-to-end on Windows 10/11. The only remaining gap is Discord RPC, which is a
silent stub on Windows.

This page records how each Linux facility maps onto Windows; the `P100_WINDOWS`
blocks in the sources are the implementation.

## What is done

| Area | Approach |
|---|---|
| Backend processes (`ytdlp.cpp`) | `CreateProcess` + two anonymous pipes; a reader thread per channel replaces `poll()`/`read()` with the same 45 s deadline and error payloads; `TerminateProcess` on stop |
| Shell-outs | `popen`/`system` kept (cmd.exe), `2>/dev/null` → `2>NUL`, `shell_quote` → double quotes, `yt-dlp.exe` resolution via the venv's `Scripts\` dir |
| Directory scans | `std::filesystem::directory_iterator` instead of `dirent.h` |
| Paths (`paths.cpp`) | exe path via `GetModuleFileNameA`, cwd via `GetCurrentDirectoryA`, data in `%APPDATA%\crossfade`, home via `APPDATA`/`USERPROFILE` |
| Notifications | WinRT toasts from a dedicated worker thread; the AUMID display name/icon is registered under `HKCU\Software\Classes\AppUserModelId`, so no Start-menu shortcut is required |
| MPRIS | `win32/mpris_windows.cpp` maps the same surface onto System Media Transport Controls (`win32/media_session.cpp`): media keys, media flyout, lock screen, timeline seek |
| Warm start | Per-user, per-session named pipe (`\\.\pipe\crossfade-warm-<user>-<session>`): `show`/`hide`/`toggle`/`quit` from a second launch, no console |
| Downloads | `yt-dlp[default]` (EJS) plus a bundled `deno.exe`; yt-dlp is launched directly (no `cmd.exe`), falls back to the cookie-free `visionos` client, and logs failures to `<config>/downloads.log` |
| Console window | The app links as a GUI subsystem binary; helper processes (Python backend, yt-dlp, curl, login) are launched with `CREATE_NO_WINDOW` (`src/core/win_process.cpp`) |
| Discord RPC | `win32/discord_rpc_stub.cpp` — silent no-op (named-pipe transport `\\.\pipe\discord-ipc-N` = future work) |
| Accent colour | XDG portal compiled out; daily-accent fallback and `P100_ACCENT` still work |
| Misc | `localtime_r` → `localtime_s`, `getenv("HOME")` → `USERPROFILE`, venv layout `Scripts\python.exe` |

Playback (libmpv), the UI (SDL2 + ImGui + OpenGL), the Python backend and the
progressive audio cache are platform-portable and shared.

## Feature parity on Windows (current)

Desktop integration is implemented natively:

- **Media controls** — playback is published to System Media Transport
  Controls (SMTC), so the Windows media flyout, hardware media keys, lock
  screen and the "now playing" overlay show title/artist/album/cover art and
  can play, pause, stop, skip and seek. `win32/media_session.cpp` owns the
  WinRT session on a dedicated MTA worker thread; `win32/mpris_windows.cpp`
  adapts it to the same `MprisPlayer` surface the Linux build uses.
- **Notifications** — new-track toasts use the Windows notification platform
  (`win32/notifications.cpp`), including cached cover art and the app name
  from the registered AppUserModelID. Delivery happens on a worker thread so
  the mpv event thread never blocks on COM. The Settings → Notifications
  toggle still applies.

Lost until the replacement lands: Discord Rich Presence. Everything else
works the same, including warm start (`--warm`, `--toggle`, `--quit`) and the
installer's opt-in "start hidden at sign-in" Run entry.

`tests/test_win_platform.cpp` is a standalone smoke executable for both
integrations. Run `scripts/windows-platform-smoke.ps1` from PowerShell (after
a CMake build) for the end-to-end check: it verifies the SMTC session and
metadata, sends media commands and confirms the app received them, and checks
that Windows accepted the toast.

## Building on Windows (MSYS2)

```bash
pacman -S mingw-w64-ucrt-x86_64-toolchain cmake mingw-w64-ucrt-x86_64-SDL2 \
          mingw-w64-ucrt-x86_64-mpv python curl

# Python side
python -m venv venv
venv/Scripts/pip install -U pip
venv/Scripts/pip install -r requirements.txt

# Auth: the in-app sign-in panel uses WebView2 through the optional
# pywebview package; without it, paste request headers in the panel.
venv/Scripts/pip install pywebview        # optional WebView2 wrapper
venv/Scripts/python scripts/crossfade-login-helper.py --probe
venv/Scripts/python scripts/crossfade-login-helper.py --signin auto
# Terminal fallback (paste headers, then Ctrl+Z+Enter):
venv/Scripts/python scripts/crossfade-login-helper.py --headers -

# Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMPV_INCLUDE_DIR=... -DMPV_LIBRARY=...
cmake --build build -j
./build/crossfade.exe
```

Unit tests run with `ctest --test-dir build`. The desktop integration has its
own end-to-end check (interactive session required):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/windows-platform-smoke.ps1
```

It drives `build/test_win_platform.exe`, verifies the SMTC session/metadata
through `GlobalSystemMediaTransportControlsSessionManager`, sends media
commands, and confirms the toast platform accepted a notification.

`MPV_INCLUDE_DIR` / `MPV_LIBRARY` point at the libmpv development files
(`mpv.h` + `libmpv.dll.a`/`mpv.lib` from the [mpv dev
package](https://mpv.io/)); MSYS2's `mingw-w64-ucrt-x86_64-mpv` provides
them via pkg-config, which CMake uses automatically on MSYS2.

## Updating

`src/net/updater.cpp` is shared by both platforms. A startup check (throttled
to once a day) fetches `releases/latest/download/version.txt` and compares it
with the compile-time `CROSSFADE_VERSION`; Settings -> Updates exposes the
same check manually. On Windows, the toast offers **Update now**, which
launches `install-crossfade.cmd` from the install directory, falling back to
downloading the current one from the release page for older installs. The
installer runs in its own console, replaces the app and asks whether to
restart it.

On Linux the same check now drives a self-updater for writable, user-owned
installs (AppImage, release tarball, `./install.sh` prefix): it downloads the
matching asset, verifies the release `SHA256SUMS`, stages it, and applies it
during shutdown or on the next launch before SDL starts. Package-managed
installs (`/usr`, `/usr/local`, deb/rpm/Arch/Gentoo) are never rewritten; the
action opens the release page instead. Safe mode skips automatic checks and
startup applies.

## Known-unknowns (first Windows run will tell)

- `imgui_impl_sdl2` + OpenGL 3.2 core on Windows drivers
- The mpv `stream-record` / `ytdl` hooks with Windows paths
- `curl.exe` availability (ships with Windows 10+) and cmd.exe quoting of
  googlevideo URLs (they are double-quoted, but `&` in URLs deserves a test)
- `subprocess` calls in the backend now set `CREATE_NO_WINDOW`; the GUI build
  itself no longer has a console to inherit
