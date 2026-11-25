#!/usr/bin/env python3
"""Crossfade backend using browser-cookie authentication.

Create the cookie file once with `ytmusicapi browser`. Without a usable file the
backend starts in guest mode (public browsing; account commands ask the user to
sign in). Everything else is in cf_backend.core so the cookie and OAuth entry
points stay behaviorally identical.
"""

import json
import os
import sys
import time

from ytmusicapi import YTMusic

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "py"))
from cf_backend.core import (  # noqa: E402
    _err,
    ensure_config_dir,
    login_state,
    run_command_loop,
    secure_auth_file,
)


def _config_dir():
    xdg = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.environ.get("P100_CONFIG_DIR") or os.path.join(xdg, "crossfade")


CONFIG_DIR = _config_dir()


def _resolve_auth_file():
    env = os.environ.get("P100_AUTH_FILE")
    if env:
        return env
    candidates = [
        os.path.join(CONFIG_DIR, "browser.json"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "browser.json"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return candidates[0]


AUTH_FILE = _resolve_auth_file()


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    ensure_config_dir(CONFIG_DIR)
    guest = True
    yt = None
    if os.path.exists(AUTH_FILE):
        secure_auth_file(AUTH_FILE)
        try:
            # Stored SAPISIDHASH expires; recompute it from the cookies before
            # ytmusicapi reads the file, otherwise a valid session looks
            # signed out to the fallback backend.
            try:
                from cf_backend.login import refresh_auth_file
                refresh_auth_file(AUTH_FILE)
            except Exception:
                pass
            yt = YTMusic(AUTH_FILE)
            # A cookie file can construct a YTMusic object and still be
            # anonymous (expired/incomplete cookies). Verify the account flag so
            # Home is not filled with a generic feed and cached as personalized.
            # A file written by the sign-in helper moments ago was already
            # verified, so skip the extra round-trip right after signing in.
            fresh = False
            try:
                fresh = time.time() - os.path.getmtime(AUTH_FILE) < 300
            except OSError:
                pass
            state = None if fresh else login_state(yt)
            if state is False:
                print("Stored session is signed out; starting as guest.",
                      file=sys.stderr)
                yt = None
            else:
                guest = False
        except Exception as e:
            # A broken session must not block public browsing: fall back to
            # guest and let the UI offer a re-sign-in.
            print(f"Auth file unusable ({e}); starting as guest.", file=sys.stderr)

    if yt is None:
        try:
            yt = YTMusic()  # anonymous default header; public browsing only
        except Exception as e:
            print(json.dumps(_err(f"Failed to initialize YTMusic: {str(e)}")))
            sys.stdout.flush()
            return

    print(json.dumps({"status": "ready", "guest": guest},
                     separators=(",", ":")))
    sys.stdout.flush()

    # Each concurrent command worker needs its own YTMusic (ytmusicapi's
    # requests session is not thread-safe). Guest workers browse anonymously.
    def _session_factory():
        return YTMusic() if guest else YTMusic(AUTH_FILE)

    run_command_loop(yt, guest=guest, session_factory=_session_factory)


if __name__ == "__main__":
    main()
