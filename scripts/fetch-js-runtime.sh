#!/usr/bin/env bash
# Provision the JavaScript runtime yt-dlp needs to solve YouTube's challenges.
#
#   fetch-js-runtime.sh <target-dir>
#
# Downloads a pinned Deno release, verifies its SHA-256 and places the binary
# as <target-dir>/deno. The app passes the path to yt-dlp with --js-runtimes;
# nothing else uses it. Re-running is a no-op once the binary exists, so the
# installer and the AppImage launcher can call this unconditionally.
#
# Offline installs: put the release zip at <target-dir>/deno.zip (or point
# P100_DENO_ZIP at it) or pre-place <target-dir>/deno; no network is used then.
set -euo pipefail

DEST="${1:?usage: fetch-js-runtime.sh <target-dir>}"
VERSION="${P100_DENO_VERSION:-2.9.8}"
BASE="${P100_DENO_BASE_URL:-https://github.com/denoland/deno/releases/download/v$VERSION}"

case "$(uname -s)" in
    Linux) os=unknown-linux-gnu ;;
    *)
        echo "fetch-js-runtime: unsupported OS $(uname -s)" >&2
        exit 1
        ;;
esac
case "$(uname -m)" in
    x86_64|amd64) arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *)
        echo "fetch-js-runtime: unsupported architecture $(uname -m)" >&2
        exit 1
        ;;
esac

# SHA-256 of deno-$arch-$os.zip for v$VERSION (deno releases publish these).
case "$arch" in
    x86_64) sha256=e890975bc2549807bbcc0b584ffafda72cc7c93349dd99c80483dd784f6c4609 ;;
    aarch64) sha256=d49596c20b9788cf8f5d8ddf644103d4bde56ed239753f0bdb92ba9361d51905 ;;
    *) echo "fetch-js-runtime: no checksum for $arch" >&2; exit 1 ;;
esac

mkdir -p "$DEST"

# Already provisioned: running --version also rejects a truncated download.
if [ -x "$DEST/deno" ] && "$DEST/deno" --version >/dev/null 2>&1; then
    exit 0
fi
if [ -f "$DEST/deno" ]; then
    chmod +x "$DEST/deno" 2>/dev/null || true
    if "$DEST/deno" --version >/dev/null 2>&1; then
        exit 0
    fi
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/crossfade-deno.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

ZIP="${P100_DENO_ZIP:-}"
if [ -z "$ZIP" ] && [ -f "$DEST/deno.zip" ]; then
    ZIP="$DEST/deno.zip"
fi

if [ -z "$ZIP" ]; then
    ZIP="$TMP/deno.zip"
    URL="$BASE/deno-$arch-$os.zip"
    echo "fetch-js-runtime: downloading Deno $VERSION ($arch) ..."
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 --connect-timeout 20 -o "$ZIP" "$URL"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$ZIP" "$URL"
    else
        echo "fetch-js-runtime: neither curl nor wget is available" >&2
        exit 1
    fi
fi

check_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum -c <<<"$sha256  $1" >/dev/null
    else
        python3 - "$1" "$sha256" <<'EOF'
import hashlib, sys
h = hashlib.sha256()
with open(sys.argv[1], 'rb') as f:
    for chunk in iter(lambda: f.read(1 << 20), b''):
        h.update(chunk)
raise SystemExit(0 if h.hexdigest() == sys.argv[2] else 1)
EOF
    fi
}

if ! check_sha256 "$ZIP"; then
    rm -f "$ZIP"
    echo "fetch-js-runtime: checksum mismatch for $ZIP (removed)" >&2
    exit 1
fi

"${P100_PYTHON:-python3}" - "$ZIP" "$TMP" <<'EOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    archive.extract("deno", sys.argv[2])
EOF

if [ ! -f "$TMP/deno" ]; then
    echo "fetch-js-runtime: deno was not found in the archive" >&2
    exit 1
fi

chmod 755 "$TMP/deno"
mv -f "$TMP/deno" "$DEST/deno"
echo "fetch-js-runtime: installed $("$DEST/deno" --version | head -n1) at $DEST/deno"
