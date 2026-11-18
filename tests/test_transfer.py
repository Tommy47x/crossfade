#!/usr/bin/env python3
"""Round-trip test for the crossfade ZIP data transfer helper."""

import json
import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HELPER = ROOT / "py" / "cf_backend" / "transfer.py"


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def main():
    failures = 0

    def check(cond, what):
        nonlocal failures
        if not cond:
            print("FAIL:", what, file=sys.stderr)
            failures += 1

    with tempfile.TemporaryDirectory(prefix="cf-transfer-") as td:
        base = Path(td)
        src = base / "src"
        dst = base / "dst"
        archive = base / "backup.zip"
        write(src / "settings.json", '{"crossfade_duration":5}')
        write(src / "browser.json", '{"cookies":[]}')
        write(src / "library.db", "sqlite")
        write(src / "liked.json", "[]")
        write(src / "playlists" / "Test.json", "[]")
        write(src / "thumb_cache" / "a.jpg", "thumb")
        write(src / "audio_cache" / "a.webm", "audio")

        # Symlinks inside cache trees must not be followed: they could pull
        # unrelated files into the backup or loop the walk.
        link_ok = True
        try:
            write(base / "outside" / "secret.txt", "secret")
            os.symlink(base / "outside" / "secret.txt",
                       src / "thumb_cache" / "link.jpg")
            os.symlink(base / "outside", src / "thumb_cache" / "linkdir")
        except (OSError, NotImplementedError):
            link_ok = False

        rc = subprocess.run(
            [sys.executable, str(HELPER), "export", "--zip", str(archive),
             "--config", str(src), "--sections", "settings,auth,library,thumbs"],
            check=False)
        check(rc.returncode == 0, "export exits 0")
        check(archive.is_file(), "archive created")
        with zipfile.ZipFile(archive) as zf:
            names = zf.namelist()
            manifest = json.loads(zf.read("manifest.json"))
        if link_ok:
            check("thumb_cache/link.jpg" not in names, "file symlink excluded")
            check(not any(n.startswith("thumb_cache/linkdir") for n in names),
                  "directory symlink not followed")
        check("settings.json" in names, "settings included")
        check("browser.json" in names, "auth included")
        check("library.db" in names, "library included")
        check("thumb_cache/a.jpg" in names, "thumbs included")
        check("audio_cache/a.webm" not in names, "audio excluded")
        check(manifest["app"] == "crossfade", "manifest app")
        check("settings" in manifest["sections"], "manifest sections")

        write(dst / "settings.json", '{"old":true}')
        rc = subprocess.run(
            [sys.executable, str(HELPER), "import", "--zip", str(archive),
             "--config", str(dst)],
            check=False)
        check(rc.returncode == 0, "import exits 0")
        check(dst.joinpath("settings.json").read_text() == '{"crossfade_duration":5}',
              "settings replaced")
        check(dst.joinpath("thumb_cache/a.jpg").is_file(), "thumbs restored")
        check(dst.joinpath("library.db").is_file(), "library restored")
        backups = list(base.glob("crossfade-pre-import-*"))
        check(len(backups) == 1, "safety backup created")
        check(backups and (backups[0] / "settings.json").is_file(), "old settings backed up")

        # Bad archives must not be able to write outside the destination.
        evil = base / "evil.zip"
        with zipfile.ZipFile(evil, "w") as zf:
            zf.writestr("manifest.json", json.dumps(
                {"app": "crossfade", "sections": ["settings"],
                 "files": ["../escape.txt"], "dirs": []}))
            zf.writestr("../escape.txt", "nope")
        rc = subprocess.run(
            [sys.executable, str(HELPER), "import", "--zip", str(evil),
             "--config", str(dst)],
            check=False)
        check(rc.returncode != 0, "unsafe archive rejected")
        check(not (base / "escape.txt").exists(), "no path traversal")

        # Backslash traversal: Windows reads "\" as a separator, so the
        # archive must be rejected there. On POSIX it is a literal filename
        # and must stay inside the destination.
        evil_bs = base / "evil-backslash.zip"
        bs_name = "..\\..\\escape.txt"
        with zipfile.ZipFile(evil_bs, "w") as zf:
            zf.writestr("manifest.json", json.dumps(
                {"app": "crossfade", "sections": ["settings"],
                 "files": [bs_name], "dirs": []}))
            zf.writestr(bs_name, "nope")
        rc = subprocess.run(
            [sys.executable, str(HELPER), "import", "--zip", str(evil_bs),
             "--config", str(dst)],
            check=False)
        if os.name == "nt":
            check(rc.returncode != 0, "backslash archive rejected on Windows")
        else:
            check(rc.returncode == 0, "literal backslash name imported on POSIX")
            check(dst.joinpath(bs_name).is_file(),
                  "literal backslash name kept in destination")
        check(not (base / "escape.txt").exists(), "no backslash traversal")

    if failures:
        print(f"transfer: {failures} failure(s)", file=sys.stderr)
        return 1
    print("transfer: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
