#!/usr/bin/env python3
"""Regression tests for the shared sign-in helpers and the login helper CLI.

Import-safe: ytmusicapi, yt-dlp, gi and Qt are never imported at module scope,
so this runs in the CI python job that installs only ruff.
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "py"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import cf_backend.login as login  # noqa: E402


def check(cond, what, failures):
    if not cond:
        print("FAIL:", what, file=sys.stderr)
        failures.append(what)


def test_clean_headers(failures):
    rare = (
        ":authority: music.youtube.com\n"
        ":method: POST\n"
        ":path: /youtubei/v1/browse\n"
        ":scheme: https\n"
        "POST /youtubei/v1/browse HTTP/2\n"
        "host: music.youtube.com\n"
        "content-length: 123\n"
        "cookie: SID=one; HSID=two\n"
        "Cookie: SAPISID=three; __Secure-3PAPISID=four\n"
        "authorization: SAPISIDHASH 1_x\n"
        "accept-encoding: gzip\n"
        "x-goog-authuser: 0\n"
    )
    cleaned = login.clean_headers(rare)
    check("cookie: sid=one; hsid=two; sapisid=three; __secure-3papisid=four"
          in cleaned.lower(), "merges duplicate Cookie lines", failures)
    check(":authority" not in cleaned, "drops HTTP/2 pseudo-headers", failures)
    check("POST /youtubei" not in cleaned, "drops the request line", failures)
    check("host:" not in cleaned.lower(), "drops host", failures)
    check("accept-encoding" not in cleaned.lower(), "drops hop-by-hop headers", failures)

    wrapped = ("cookie: SID=abc; SAPISID=def;\n"
               " __Secure-3PAPISID=ghi\n"
               "x-goog-authuser: 0\n")
    cleaned = login.clean_headers(wrapped)
    check("__Secure-3PAPISID=ghi" in cleaned, "re-attaches a wrapped Cookie line", failures)

    curl = ("curl 'https://music.youtube.com/youtubei/v1/browse' \\\n"
            "  -H 'cookie: SID=a; __Secure-3PAPISID=b' \\\n"
            "  -H 'x-goog-authuser: 0' \\\n"
            "  -b 'SAPISID=c'\n")
    cleaned = login.clean_headers(curl)
    check("__Secure-3PAPISID=b" in cleaned, "parses cURL -H cookie headers", failures)
    check("SAPISID=c" in cleaned, "parses cURL -b cookie jars", failures)

    # Chromium's two-line "Copy request headers": name on one line, value on
    # the next. This was the format that reported "no Cookie header found".
    two_line = (
        ":authority\n"
        "music.youtube.com\n"
        ":method\n"
        "POST\n"
        "cookie\n"
        "PREF=x; SAPISID=s; __Secure-3PAPISID=p\n"
        "x-goog-authuser\n"
        "0\n"
        "accept-encoding\n"
        "gzip, deflate, br\n"
        "Decoded:\n"
        "message ClientVariations {\n"
        "  repeated int32 variation_id = [1];\n"
        "}\n"
    )
    cleaned = login.clean_headers(two_line)
    check("cookie: PREF=x; SAPISID=s; __Secure-3PAPISID=p" in cleaned,
          "parses the two-line Copy request headers format", failures)
    check("x-goog-authuser: 0" in cleaned, "keeps x-goog-authuser from pairs", failures)
    check(":authority" not in cleaned, "drops pseudo-header pairs", failures)
    check("accept-encoding" not in cleaned, "drops hop-by-hop pairs", failures)
    check("ClientVariations" not in cleaned, "stops at the Decoded block", failures)


def test_cookie_mapping(failures):
    cookies = {"SAPISID": "s", "__Secure-3PAPISID": "p", "IGNORE": "x"}
    filtered = login.filter_cookies(cookies)
    check("IGNORE" not in filtered, "drops non-auth cookies", failures)
    check(login.has_secure_cookie(filtered), "detects __Secure-3PAPISID", failures)
    check(not login.has_secure_cookie({"SAPISID": "s"}),
          "SAPISID alone is not enough", failures)

    headers = login.build_browser_headers(cookies, ua="test-ua")
    check(headers["cookie"].startswith("SAPISID=s"), "builds a cookie header", failures)
    check(headers["authorization"].startswith("SAPISIDHASH "),
          "builds a SAPISIDHASH authorization", failures)
    check(headers["user-agent"] == "test-ua", "keeps the supplied UA", failures)
    expected = login.hashlib.sha1(b"1000 s https://music.youtube.com").hexdigest()
    auth = login.sapisid_authorization(cookies, ts=1000)
    check(f"SAPISIDHASH 1000_{expected}" in auth, "SAPISIDHASH vector", failures)


def test_atomic_install(failures):
    root = tempfile.mkdtemp(prefix="crossfade-login-test-")
    dest = os.path.join(root, "browser.json")
    with open(dest, "w", encoding="utf-8") as f:
        f.write("old-session")

    original = login.verify_auth_file
    try:
        login.verify_auth_file = lambda path: (False, "expired")
        ok, message = login.install_from_cookies(
            {"SAPISID": "s", "__Secure-3PAPISID": "p"}, dest=dest)
        check(not ok and "expired" in message, "rejects a bad session", failures)
        with open(dest, encoding="utf-8") as f:
            check(f.read() == "old-session",
                  "leaves the old auth file intact on failure", failures)
        leftovers = [name for name in os.listdir(root) if name.startswith(".crossfade-auth-")]
        check(not leftovers, "removes the temporary auth file on failure", failures)

        login.verify_auth_file = lambda path: (True, "signed in")
        ok, _ = login.install_from_cookies(
            {"SAPISID": "s", "__Secure-3PAPISID": "p"}, dest=dest)
        check(ok, "installs a verified session", failures)
        with open(dest, encoding="utf-8") as f:
            data = json.load(f)
        check(data.get("authorization", "").startswith("SAPISIDHASH"),
              "writes an authorization header", failures)
        if os.name != "nt":
            check((os.stat(dest).st_mode & 0o777) == 0o600,
                  "auth file is 0600", failures)
    finally:
        login.verify_auth_file = original


def test_ytdlp_jar(failures):
    import types

    class FakeCookie:
        def __init__(self, name, value, domain):
            self.name, self.value, self.domain = name, value, domain

    fake_pkg = types.ModuleType("yt_dlp")
    fake = types.ModuleType("yt_dlp.cookies")
    fake.extract_cookies_from_browser = lambda *a, **k: [
        FakeCookie("SAPISID", "s", ".google.com"),
        FakeCookie("__Secure-3PAPISID", "p", ".youtube.com"),
        FakeCookie("HSID", "google-hsid", ".google.com"),
        FakeCookie("HSID", "youtube-hsid", ".youtube.com"),
        FakeCookie("NOPE", "x", ".google.com"),
        FakeCookie("AUTH", "a", ".example.com"),
    ]
    old_pkg, old_mod = sys.modules.get("yt_dlp"), sys.modules.get("yt_dlp.cookies")
    sys.modules["yt_dlp"], sys.modules["yt_dlp.cookies"] = fake_pkg, fake
    try:
        cookies = login.cookies_from_ytdlp_browser("chrome")
        check(cookies.get("__Secure-3PAPISID") == "p", "reads Google cookies", failures)
        check(cookies.get("HSID") == "youtube-hsid",
              "yt-dlp imports prefer the youtube.com copy too", failures)
        check("NOPE" not in cookies, "filters unrelated cookies", failures)
        check("AUTH" not in cookies, "ignores other domains", failures)
    finally:
        if old_pkg is None:
            sys.modules.pop("yt_dlp", None)
        else:
            sys.modules["yt_dlp"] = old_pkg
        if old_mod is None:
            sys.modules.pop("yt_dlp.cookies", None)
        else:
            sys.modules["yt_dlp.cookies"] = old_mod


def test_helper_cli(failures):
    helper = os.path.join(ROOT, "scripts", "crossfade-login-helper.py")
    root = tempfile.mkdtemp(prefix="crossfade-helper-test-")
    env = dict(os.environ, P100_CONFIG_DIR=root)

    proc = subprocess.run([sys.executable, helper, "--probe"],
                          capture_output=True, text=True, timeout=60, env=env)
    check(proc.returncode == 0, "probe exits 0", failures)
    try:
        probe = json.loads(proc.stdout.strip().splitlines()[-1])
        check(probe.get("paste") is True, "probe reports paste available", failures)
        check(any(e.get("id") == "webkit" for e in probe.get("engines", [])),
              "probe reports the engines it checked", failures)
    except Exception as e:
        check(False, f"probe output is JSON ({e})", failures)

    headers = os.path.join(root, "login-headers.txt")
    with open(headers, "w", encoding="utf-8") as f:
        f.write("not request headers\n")
    result = os.path.join(root, "login-result.txt")
    proc = subprocess.run([sys.executable, helper, "--headers", headers,
                           "--result-file", result],
                          capture_output=True, text=True, timeout=60, env=env)
    check(proc.returncode == 2, "bad headers exit 2", failures)
    check(not os.path.exists(headers), "helper removes its own header temp file", failures)
    with open(result, encoding="utf-8") as f:
        check(f.read().strip() == "2", "helper records the exit code", failures)


def test_webview2_cookie_map(failures):
    """pywebview returns SimpleCookie morsels, not .name/.value objects."""
    import importlib.util
    from http.cookies import SimpleCookie

    helper = os.path.join(ROOT, "scripts", "crossfade-login-helper.py")
    spec = importlib.util.spec_from_file_location("crossfade_login_helper", helper)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    first = SimpleCookie()
    first["SAPISID"] = "s"
    first["SAPISID"]["domain"] = ".youtube.com"
    second = SimpleCookie()
    second["__Secure-3PAPISID"] = "p"
    mapping = module._cookie_map([first, second])
    check(mapping.get("SAPISID") == "s", "reads SimpleCookie values", failures)
    check(mapping.get("__Secure-3PAPISID") == "p", "reads the secure cookie", failures)

    class Cookie:
        name = "SID"
        value = "v"

    check(module._cookie_map([Cookie()]).get("SID") == "v",
          "still reads attribute-style cookies", failures)
    check(module._cookie_map([{"HSID": "h"}]).get("HSID") == "h",
          "still reads dict-style cookies", failures)


def test_chrome_lock_hint(failures):
    """yt-dlp #7271 gets an actionable message instead of a raw traceback."""
    import types

    fake_pkg = types.ModuleType("yt_dlp")
    fake = types.ModuleType("yt_dlp.cookies")

    def fail(*args, **kwargs):
        raise RuntimeError("ERROR: Could not copy Chrome cookie database. "
                           "See https://github.com/yt-dlp/yt-dlp/issues/7271")

    fake.extract_cookies_from_browser = fail
    old_pkg, old_mod = sys.modules.get("yt_dlp"), sys.modules.get("yt_dlp.cookies")
    sys.modules["yt_dlp"], sys.modules["yt_dlp.cookies"] = fake_pkg, fake
    try:
        try:
            login.cookies_from_ytdlp_browser("chrome")
            check(False, "locked Chrome DB raises", failures)
        except RuntimeError as e:
            text = str(e)
            check("7271" in text and "Paste request headers" in text,
                  "locked Chrome DB explains the workaround", failures)
    finally:
        if old_pkg is None:
            sys.modules.pop("yt_dlp", None)
        else:
            sys.modules["yt_dlp"] = old_pkg
        if old_mod is None:
            sys.modules.pop("yt_dlp.cookies", None)
        else:
            sys.modules["yt_dlp.cookies"] = old_mod


def test_system_python_not_shadowed(failures):
    """A venv symlink must not hide the system interpreter.

    Fedora regression: the app venv's bin/python is a symlink to the system
    binary and has no system site-packages, so deduping by realpath skipped
    /usr/bin/python3 and PyGObject/WebKitGTK was never found.
    """
    import importlib.util

    helper = os.path.join(ROOT, "scripts", "crossfade-login-helper.py")
    spec = importlib.util.spec_from_file_location("crossfade_login_helper_py", helper)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    root = tempfile.mkdtemp(prefix="crossfade-interp-")
    venv = os.path.join(root, "venv")
    venv_bin = os.path.join(venv, "bin")
    os.makedirs(venv_bin)
    venv_py = os.path.join(venv_bin, "python")
    real = os.path.realpath(sys.executable)
    try:
        os.symlink(real, venv_py)
    except (OSError, NotImplementedError):
        return  # no symlinks on this platform; nothing to check
    with open(os.path.join(venv, "pyvenv.cfg"), "w", encoding="utf-8") as f:
        f.write("home = %s\ninclude-system-site-packages = false\n"
                % os.path.dirname(real))

    here = module._interp_identity(venv_py)
    check(here != module._interp_identity(sys.executable),
          "a venv symlink is not mistaken for the system interpreter", failures)

    old = os.environ.get("P100_SYSTEM_PYTHON")
    os.environ["P100_SYSTEM_PYTHON"] = venv_py
    try:
        candidates = module._system_pythons()
    finally:
        if old is None:
            os.environ.pop("P100_SYSTEM_PYTHON", None)
        else:
            os.environ["P100_SYSTEM_PYTHON"] = old
    check(venv_py in candidates,
          "an explicit interpreter that shares the binary is still tried", failures)


def _write_cookie_db(path, cookies, wal=False):
    import sqlite3
    os.makedirs(os.path.dirname(path), exist_ok=True)
    con = sqlite3.connect(path)
    if wal:
        con.execute("pragma journal_mode=wal")
    con.execute("create table moz_cookies (id integer primary key, host text,"
                " name text, value text, expiry integer)")
    for host, name, value in cookies:
        con.execute("insert into moz_cookies (host, name, value, expiry)"
                    " values (?, ?, ?, 0)", (host, name, value))
    con.commit()
    return con


def test_firefox_sessions(failures):
    """Every Firefox profile is tried, and cookies still in the WAL are read."""
    root = tempfile.mkdtemp(prefix="crossfade-ff-")
    with open(os.path.join(root, "profiles.ini"), "w", encoding="utf-8") as f:
        f.write("[Profile0]\nName=a\nIsRelative=1\nPath=a.default\n"
                "[Profile1]\nName=p100\nIsRelative=1\nPath=b.p100\n")
    _write_cookie_db(os.path.join(root, "a.default", "cookies.sqlite"),
                     [(".google.com", "HSID", "g1"),
                      (".youtube.com", "HSID", "y1"),
                      (".youtube.com", "SAPISID", "old")]).close()
    writer = _write_cookie_db(
        os.path.join(root, "b.p100", "cookies.sqlite"),
        [(".google.com", "HSID", "g2"),
         (".youtube.com", "HSID", "y2"),
         (".youtube.com", "SAPISID", "fresh")], wal=True)
    # Append a signed-in cookie and keep the writer open so the row lives only
    # in cookies.sqlite-wal, as it does while Firefox runs.
    writer.execute("insert into moz_cookies (host, name, value, expiry) values"
                   " ('.youtube.com', '__Secure-3PAPISID', 'walonly', 0)")
    writer.commit()
    try:
        sessions = login.firefox_sessions(profile_root=root)
        by_profile = {}
        for profile, cookies in sessions:
            by_profile.setdefault(os.path.basename(profile), {}).update(cookies)
        check("a.default" in by_profile and "b.p100" in by_profile,
              "reads every Firefox profile", failures)
        check(by_profile.get("b.p100", {}).get("__Secure-3PAPISID") == "walonly",
              "reads cookies that are still in the WAL", failures)
        check(by_profile.get("b.p100", {}).get("HSID") == "y2",
              "the youtube.com HSID wins inside a profile", failures)
        check(login.cookies_from_firefox(profile_root=root).get("SAPISID") == "old",
              "cookies_from_firefox keeps its first-profile behaviour", failures)
    finally:
        writer.close()


def test_cookie_host_preference(failures):
    """music.youtube.com only accepts the .youtube.com copy of each cookie.

    Google sets different HSID/SSID/SIDCC/__Secure-*PSIDTS per domain. Merging
    them in row order (the old behaviour) made the API treat a valid session
    as anonymous depending on which row was read last.
    """
    rows = [
        (".google.com", "SID", "sid"),
        (".youtube.com", "SID", "sid"),
        (".google.com", "HSID", "google-hsid"),
        (".youtube.com", "HSID", "youtube-hsid"),
        (".google.com", "SIDCC", "google-sidcc"),
        (".youtube.com", "SIDCC", "youtube-sidcc"),
        (".google.com", "__Secure-3PSIDTS", "google-psidts"),
        (".youtube.com", "__Secure-3PSIDTS", "youtube-psidts"),
        (".google.com", "SAPISID", "sapisid"),
        (".youtube.com", "__Secure-3PAPISID", "secure"),
        (".google.com", "NOPE", "x"),
    ]
    cookies = login.prefer_youtube_cookies(rows)
    check(cookies.get("HSID") == "youtube-hsid",
          "the youtube HSID wins over google.com", failures)
    check(cookies.get("SIDCC") == "youtube-sidcc",
          "the youtube SIDCC wins over google.com", failures)
    check(cookies.get("__Secure-3PSIDTS") == "youtube-psidts",
          "the youtube rotating token wins over google.com", failures)
    check(cookies.get("SAPISID") == "sapisid",
          "google-only auth names are kept as a fallback", failures)
    check("NOPE" not in cookies, "unrelated cookies are dropped", failures)

    # Reverse row order must not change the outcome.
    shuffled = login.prefer_youtube_cookies(list(reversed(rows)))
    check(shuffled.get("HSID") == "youtube-hsid",
          "row order no longer decides which host wins", failures)


def test_verify_auth_file(failures):
    """A signed-out cookie file must never verify as a session."""
    import types

    class FakeYT:
        def __init__(self, path):
            pass

        def get_library_playlists(self, limit=None):
            return []  # what ytmusicapi returns for an anonymous session

    fake = types.ModuleType("ytmusicapi")
    fake.YTMusic = FakeYT
    old_mod = sys.modules.get("ytmusicapi")
    old_state = login.server_login_state
    sys.modules["ytmusicapi"] = fake
    try:
        login.server_login_state = lambda yt: False
        ok, message = login.verify_auth_file("/tmp/crossfade-test-auth")
        check(not ok and "signed out" in message.lower(),
              "a signed-out session is rejected", failures)

        login.server_login_state = lambda yt: True
        ok, _ = login.verify_auth_file("/tmp/crossfade-test-auth")
        check(ok, "a server-confirmed session is accepted", failures)

        class OfflineYT(FakeYT):
            def get_library_playlists(self, limit=None):
                raise RuntimeError("network is down")

        fake.YTMusic = OfflineYT
        login.server_login_state = lambda yt: None
        ok, message = login.verify_auth_file("/tmp/crossfade-test-auth")
        check(not ok and "could not verify" in message.lower(),
              "an unverifiable session is not reported as signed in", failures)
    finally:
        login.server_login_state = old_state
        if old_mod is None:
            sys.modules.pop("ytmusicapi", None)
        else:
            sys.modules["ytmusicapi"] = old_mod


def main():
    failures = []
    test_clean_headers(failures)
    test_cookie_mapping(failures)
    test_atomic_install(failures)
    test_ytdlp_jar(failures)
    test_webview2_cookie_map(failures)
    test_chrome_lock_hint(failures)
    test_helper_cli(failures)
    test_system_python_not_shadowed(failures)
    test_firefox_sessions(failures)
    test_cookie_host_preference(failures)
    test_verify_auth_file(failures)
    if not failures:
        print("login: all checks passed")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
