#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
VENV="$PREFIX/share/crossfade/venv"

missing=0
for cmd in cmake g++ pkg-config python3; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Missing dependency: $cmd" >&2
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    cat >&2 <<'EOF'

On Fedora, install the build dependencies with:

  sudo dnf install @development-tools cmake SDL2-devel mesa-libGL-devel \
                   mpv-devel glib2-devel libnotify-devel python3 python3-pip curl

Then rerun ./install.sh.
EOF
    exit 1
fi

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    cmake -S "$ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX"
    cmake --build "$BUILD_DIR" -j"$(nproc)"
fi

cmake --install "$BUILD_DIR" --prefix "$PREFIX"

# Use an absolute Exec path: a KDE menu launch does not always inherit
# ~/.local/bin in PATH. Custom PREFIX installs need the right path too.
DESKTOP_FILE="$PREFIX/share/applications/crossfade.desktop"
if [ -f "$DESKTOP_FILE" ]; then
    sed -i "s#^Exec=.*#Exec=$PREFIX/bin/crossfade#" "$DESKTOP_FILE"
fi

if [ ! -x "$VENV/bin/python" ]; then
    echo "Creating Python venv at $VENV"
fi
WHEELS=""
if [ -d "$ROOT/wheels" ]; then
    WHEELS="$ROOT/wheels"
    echo "Installing Python dependencies from $WHEELS"
fi
"$ROOT/scripts/bootstrap-venv.sh" "$VENV" "$ROOT/requirements.txt" "$WHEELS"

# yt-dlp needs a JavaScript runtime (Deno) to solve YouTube's challenges;
# without it most songs resolve to "Requested format is not available". The
# script is pinned + checksummed, skips when the binary is already there, and
# supports a pre-placed zip for offline installs.
if [ "${P100_SKIP_JS_RUNTIME:-0}" != "1" ]; then
    if ! "$ROOT/scripts/fetch-js-runtime.sh" "$PREFIX/share/crossfade/runtime"; then
        cat >&2 <<'EOF'
warning: the Deno runtime could not be installed, so playback may fail with
         "Requested format is not available". Re-run ./install.sh when online,
         or place the deno binary in $PREFIX/share/crossfade/runtime/deno.
EOF
    fi
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
fi

cat <<EOF

crossfade installed to $PREFIX

  binary : $PREFIX/bin/crossfade
  login  : $PREFIX/bin/crossfade-login
  data   : ${P100_CONFIG_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/crossfade}

If $PREFIX/bin is not on PATH, add it to your shell profile.
EOF
