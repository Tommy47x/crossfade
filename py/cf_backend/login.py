"""Shared browser-cookie sign-in helpers for crossfade.

Everything here is import-safe: ytmusicapi, yt-dlp, gi and Qt are imported
lazily inside the functions that need them, so the helper and its tests can
run on a machine with none of them installed.
"""

import hashlib
import json
import os
import re
import sys
import tempfile
import time

AUTH_COOKIE_NAMES = (
    "SAPISID", "__Secure-1PAPISID", "__Secure-3PAPISID",
    "SID", "HSID", "SSID", "APISID",
    "__Secure-1PSID", "__Secure-3PSID", "SIDCC",
    "__Secure-1PSIDTS", "__Secure-3PSIDTS",
)

PSEUDO = re.compile(r"^:[a-z-]+")
REQ_LINE = re.compile(r"^(?:GET|POST|PUT|HEAD|OPTIONS|DELETE) \S+ HTTP/")
DROP = {
    "host", "content-length", "content-encoding", "connection",
    "accept-encoding", "priority", "authority", "method", "path", "scheme",
}

DEFAULT_UA = (
    "Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0"
    if sys.platform != "win32" else
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) "
    "Gecko/20100101 Firefox/140.0")


def config_dir():
    env = os.environ.get("P100_CONFIG_DIR")
    if env:
        return env
    if sys.platform == "win32":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        return os.path.join(base, "crossfade")
    xdg = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(xdg, "crossfade")


def auth_path():
    """Same resolution order as the backend: P100_AUTH_FILE, else browser.json."""
    env = os.environ.get("P100_AUTH_FILE")
    if env:
        return env
    return os.path.join(config_dir(), "browser.json")


def _secure(path, mode=0o600):
    try:
        os.chmod(path, mode)
    except OSError:
        pass


def _ensure_config_dir():
    path = config_dir()
    os.makedirs(path, exist_ok=True)
    try:
        os.chmod(path, 0o700)
    except OSError:
        pass
    return path


def _clean_colon_block(text):
    """The familiar `name: value` / cURL forms."""
    lines = []
    for line in text.splitlines():
        line = line.strip().rstrip("\\").strip()
        if (not line or PSEUDO.match(line) or REQ_LINE.match(line)
                or line.lower().startswith(("curl ", "compressed"))):
            continue
        if ":" not in line and lines:
            # Wrapped ultra-long header (usually Cookie); re-attach it.
            lines[-1] += line
            continue
        lines.append(line)

    for pair in re.findall(r"-H\s*['\"]([^'\"]+)['\"]", text):
        lines.append(pair)
    m = re.search(r"(?:-b|--cookie)\s+['\"]([^'\"]+)['\"]", text)
    if m:
        lines.append("cookie: " + m.group(1))

    out = []
    seen = set()
    for line in lines:
        name = line.split(":", 1)[0].strip().lower()
        if not name or name in DROP or ":" not in line:
            continue
        if name in seen:
            if name == "cookie":
                for i, prev in enumerate(out):
                    if prev.split(":", 1)[0].strip().lower() == "cookie":
                        prev_val = prev.split(":", 1)[1].strip().rstrip(";")
                        add_val = line.split(":", 1)[1].strip().rstrip(";")
                        out[i] = "cookie: " + prev_val + "; " + add_val
                        break
            continue
        seen.add(name)
        out.append(line)
    return "\n".join(out)


def _clean_pair_block(text):
    """Chromium DevTools "Copy request headers": name line, value line.

    Example:
        :authority
        music.youtube.com
        cookie
        SID=...; SAPISID=...
        x-goog-authuser
        0
    """
    lines = text.splitlines()
    out = []
    i = 0
    while i < len(lines):
        name = lines[i].strip()
        i += 1
        if not name:
            continue
        if name.lower().startswith("decoded") or name.startswith("message "):
            break
        if PSEUDO.match(name):
            # Pseudo-header: drop it and its value line.
            if i < len(lines):
                i += 1
            continue
        if not re.fullmatch(r"[A-Za-z0-9-]+", name):
            continue
        while i < len(lines) and not lines[i].strip():
            i += 1
        if i >= len(lines):
            break
        value = lines[i].strip()
        i += 1
        if not value or name.lower() in DROP:
            continue
        out.append(f"{name}: {value}")
    return "\n".join(out)


def clean_headers(text):
    """Turn a DevTools/HTTP/2/cURL header paste into a plain header block.

    Handles Chromium's pseudo-header view, the two-line "Copy request headers"
    form (name line followed by value line), request/status lines, cURL
    -H/-b forms, long Cookie lines wrapped by the console, and Edge's multiple
    Cookie lines (they are merged instead of dropped).
    """
    cleaned = _clean_colon_block(text)
    if "cookie" in cleaned.lower():
        return cleaned
    paired = _clean_pair_block(text)
    if "cookie" in paired.lower():
        return paired
    return cleaned


def filter_cookies(cookies):
    """Keep only the Google auth cookies, as a name -> value dict."""
    return {name: value for name, value in cookies.items()
            if name in AUTH_COOKIE_NAMES and value}


def has_secure_cookie(cookies):
    # ytmusicapi's browser auth requires this cookie specifically; SAPISID
    # alone is not enough.
    return bool(cookies.get("__Secure-3PAPISID"))


def cookie_header(cookies):
    return "; ".join(f"{k}={v}" for k, v in cookies.items())


def sapisid_authorization(cookies, ts=None):
    sapisid = cookies.get("SAPISID") or cookies.get("__Secure-3PAPISID") or ""
    if not sapisid:
        raise RuntimeError("Google did not return a SAPISID cookie")
    ts = int(time.time() if ts is None else ts)
    h = hashlib.sha1(f"{ts} {sapisid} https://music.youtube.com".encode()).hexdigest()
    parts = [f"SAPISIDHASH {ts}_{h}"]
    for name, scheme in (("__Secure-1PAPISID", "SAPISID1PHASH"),
                         ("__Secure-3PAPISID", "SAPISID3PHASH")):
        if name in cookies:
            hh = hashlib.sha1(
                f"{ts} {cookies[name]} https://music.youtube.com".encode()).hexdigest()
            parts.append(f"{scheme} {ts}_{hh}")
    return " ".join(parts)


def build_browser_headers(cookies, ua=None):
    cookies = filter_cookies(cookies)
    return {
        "cookie": cookie_header(cookies),
        "authorization": sapisid_authorization(cookies),
        "x-goog-authuser": "0",
        "origin": "https://music.youtube.com",
        "x-origin": "https://music.youtube.com",
        "referer": "https://music.youtube.com/",
        "user-agent": ua or DEFAULT_UA,
        "accept": "*/*",
        "accept-language": "en-US,en;q=0.9",
    }


def _write_private_json(path, value):
    _ensure_config_dir()
    fd, tmp = tempfile.mkstemp(prefix=".crossfade-auth-", suffix=".json",
                               dir=os.path.dirname(path) or ".")
    try:
        _secure(tmp)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(value, f, indent=2, sort_keys=True)
        return tmp
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def verify_auth_file(path):
    """Check a candidate auth file against the real library. Returns (ok, msg)."""
    try:
        from ytmusicapi import YTMusic
    except Exception as e:
        return False, f"ytmusicapi is not available ({e})"
    try:
        yt = YTMusic(path)
        yt.get_library_playlists(limit=1)
        return True, "Signed in to YouTube Music."
    except Exception as e:
        msg = str(e).strip().splitlines()[0][:300] if str(e) else "unknown error"
        return False, f"YouTube Music rejected the session: {msg}"


def _commit(tmp, dest):
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    os.replace(tmp, dest)
    _secure(dest)


def install_from_headers(raw_headers, dest=None):
    """Parse pasted headers, verify, then atomically replace the auth file."""
    dest = dest or auth_path()
    cleaned = clean_headers(raw_headers)
    if "cookie" not in cleaned.lower():
        return False, ("No Cookie header found. Copy the request headers of a "
                       "POST request to music.youtube.com while signed in.")
    try:
        from ytmusicapi.auth.browser import setup_browser
    except Exception as e:
        return False, f"ytmusicapi is not available ({e})"
    try:
        fd, tmp = tempfile.mkstemp(prefix=".crossfade-auth-", suffix=".json",
                                   dir=os.path.dirname(dest) or ".")
        os.close(fd)
        _secure(tmp)
        setup_browser(filepath=tmp, headers_raw=cleaned)
    except Exception as e:
        try:
            if 'tmp' in locals():
                os.unlink(tmp)
        except OSError:
            pass
        return False, f"Could not parse the headers: {e}"
    ok, msg = verify_auth_file(tmp)
    if not ok:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        return False, msg
    _commit(tmp, dest)
    return True, msg


def install_from_cookies(cookies, dest=None, ua=None):
    """Build browser.json from a cookie map, verify, then replace atomically."""
    dest = dest or auth_path()
    cookies = filter_cookies(cookies)
    if not has_secure_cookie(cookies):
        return False, ("The browser session is missing the __Secure-3PAPISID "
                       "cookie. Make sure you are signed in to music.youtube.com.")
    headers = build_browser_headers(cookies, ua=ua)
    try:
        tmp = _write_private_json(dest, headers)
    except Exception as e:
        return False, f"Could not write the session file: {e}"
    ok, msg = verify_auth_file(tmp)
    if not ok:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        return False, msg
    _commit(tmp, dest)
    return True, msg


# ---------------------------------------------------------------------------
# Existing browser profiles (explicit import only)
# ---------------------------------------------------------------------------

def _firefox_roots():
    home = os.path.expanduser("~")
    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA") or home
        return [os.path.join(appdata, "Mozilla", "Firefox"),
                os.path.join(appdata, "librewolf")]
    return [os.path.join(home, ".mozilla", "firefox"),
            os.path.join(home, ".librewolf")]


def _firefox_cookie_dbs(root):
    import configparser
    import glob
    candidates = []
    ini = os.path.join(root, "profiles.ini")
    if os.path.exists(ini):
        cp = configparser.ConfigParser()
        try:
            cp.read(ini)
            for section in cp.sections():
                if cp.get(section, "IsRelative", fallback="1") == "1":
                    path = os.path.join(os.path.dirname(ini),
                                        cp.get(section, "Path", fallback=""))
                else:
                    path = cp.get(section, "Path", fallback="")
                if os.path.isdir(path):
                    candidates.append(path)
        except Exception:
            pass
    candidates += glob.glob(os.path.join(root, "*"))
    dbs = [os.path.join(path, "cookies.sqlite") for path in candidates]
    return [db for db in dbs if os.path.exists(db)]


def cookies_from_firefox(profile_root=None):
    """Read a Firefox-family profile's plaintext cookie DB. Returns a dict."""
    import sqlite3
    roots = [profile_root] if profile_root else _firefox_roots()
    dbs = []
    for root in roots:
        dbs += _firefox_cookie_dbs(root)
    if not dbs:
        raise RuntimeError("No Firefox profile with cookies.sqlite was found")
    last_error = None
    for db in dbs:
        fd, tmp = tempfile.mkstemp(prefix="crossfade-cookies-", suffix=".sqlite")
        os.close(fd)
        try:
            import shutil
            shutil.copy2(db, tmp)
            con = sqlite3.connect(tmp)
            try:
                rows = con.execute(
                    "select name,value from moz_cookies "
                    "where host like '%youtube.com' or host like '%google.com'"
                ).fetchall()
            finally:
                con.close()
            cookies = {name: value for name, value in rows
                       if name in AUTH_COOKIE_NAMES and value}
            if cookies:
                return cookies
            last_error = "that Firefox profile has no signed-in YouTube session"
        except Exception as e:
            last_error = str(e)
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass
    raise RuntimeError(last_error or "No signed-in Firefox session was found")


class _QuietYtdlpLogger:
    def debug(self, msg):
        pass

    def info(self, msg):
        pass

    def warning(self, msg):
        pass

    def error(self, msg):
        pass


def cookies_from_ytdlp_browser(browser, profile=None):
    """Extract cookies through yt-dlp (handles DPAPI/libsecret/Keychain)."""
    try:
        from yt_dlp.cookies import extract_cookies_from_browser
    except Exception as e:
        raise RuntimeError(f"yt-dlp is not available ({e})")
    try:
        jar = extract_cookies_from_browser(
            browser, profile=profile, logger=_QuietYtdlpLogger())
    except Exception as e:
        message = str(e)
        lowered = message.lower()
        if "could not copy" in lowered or "7271" in lowered:
            # yt-dlp issue #7271: the browser holds the cookie DB open, and
            # Chrome 127+ App-Bound Encryption can block this even when it is
            # closed. The paste panel is the reliable route.
            raise RuntimeError(
                f"{browser} is locking its cookie database (yt-dlp #7271). "
                "Quit the browser completely, including background processes, "
                "and retry — or use Paste request headers in Settings.") from e
        raise
    cookies = {}
    for cookie in jar:
        domain = (getattr(cookie, "domain", "") or "").lower()
        if "google.com" not in domain and "youtube.com" not in domain:
            continue
        if cookie.name in AUTH_COOKIE_NAMES and cookie.value:
            cookies[cookie.name] = cookie.value
    if not cookies:
        raise RuntimeError(f"{browser} has no signed-in YouTube session")
    return cookies
