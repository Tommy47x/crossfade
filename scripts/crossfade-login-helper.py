#!/usr/bin/env python3
"""Sign-in helper for crossfade.

Modes:
  --probe                 print JSON describing available webview engines
  --signin [ENGINE]       open the best available sign-in window
                          (webkit, qt, webview2) and install the session
  --headers FILE|-        install a DevTools "Copy request headers" paste
  --import-cookies NAME   import an existing browser session (explicit only)
  --import-firefox        alias for --import-cookies firefox
  --google                alias for --signin auto
  --spotify               Spotify sp_dc sign-in (WebKitGTK / WebView2)

Engines run in-process when the current interpreter has them, otherwise the
helper re-execs itself with a system Python that does. Cookie extraction and
verification always use this interpreter (the app venv), where ytmusicapi and
yt-dlp live.
"""

import argparse
import atexit
import importlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

EXIT_OK = 0
EXIT_BAD_HEADERS = 2
EXIT_NO_ENGINE = 3
EXIT_CANCELLED = 4
EXIT_REJECTED = 5
EXIT_BLOCKED = 6

START_URL = ("https://accounts.google.com/ServiceLogin?service=youtube&continue="
             "https%3A%2F%2Fmusic.youtube.com%2F")
HOME_URL = "https://music.youtube.com/"
SPOTIFY_START = ("https://accounts.spotify.com/en/login?continue="
                 "https%3A%2F%2Fopen.spotify.com%2F")
SPOTIFY_HOME = "https://open.spotify.com/"
UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/140.0.0.0 Safari/537.36")

WEBKIT_SPECS = (
    ("WebKit", "6.0", "4.0"),
    ("WebKit2", "4.1", "3.0"),
    ("WebKit2", "4.0", "3.0"),
)
QT_BINDINGS = ("PyQt6", "PySide6", "PyQt5", "PySide2")


def _find_share_dir():
    env = os.environ.get("P100_SHARE_DIR")
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = []
    if env:
        candidates.append(env)
    candidates += [
        os.path.dirname(here),                # installed: <share>/scripts/..
        os.path.dirname(os.path.dirname(here)),  # repo: scripts/..
        here,
    ]
    for share in candidates:
        if os.path.isdir(os.path.join(share, "py", "cf_backend")):
            return share
    return ""


SHARE_DIR = _find_share_dir()
if SHARE_DIR:
    sys.path.insert(0, os.path.join(SHARE_DIR, "py"))

from cf_backend import login  # noqa: E402

LOG_PATH = os.path.join(login.config_dir(), "login.log")
RESULT = {"code": 1}
RESULT_FILE = None


def log(message):
    line = time.strftime("%H:%M:%S ") + message
    print(line, flush=True)
    # On Windows the helper is spawned without a console and without stdout
    # redirection, so keep its own copy for the Settings panel. On POSIX the
    # parent redirects stdout/stderr to this same file.
    if sys.platform == "win32":
        try:
            with open(LOG_PATH, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except OSError:
            pass


@atexit.register
def _write_result():
    if not RESULT_FILE:
        return
    try:
        with open(RESULT_FILE, "w", encoding="utf-8") as f:
            f.write(str(RESULT["code"]))
    except OSError:
        pass


def _truncate_log():
    try:
        os.makedirs(login.config_dir(), exist_ok=True)
        with open(LOG_PATH, "w", encoding="utf-8"):
            pass
    except OSError:
        pass


def _write_cookies(path, cookies, filter_auth=True):
    if filter_auth:
        cookies = login.filter_cookies(cookies)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".crossfade-login-", suffix=".json",
                               dir=os.path.dirname(path) or ".")
    try:
        os.chmod(tmp, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(cookies, f, sort_keys=True)
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _read_cookies(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# Capability probing
# ---------------------------------------------------------------------------

def _webkit_probe():
    """Return (imports, reason); reason explains a missing WebKitGTK."""
    try:
        import gi
    except Exception:
        return None, "PyGObject (python3-gobject) is not installed"
    reason = "WebKitGTK is not installed (no WebKit or WebKit2 typelib)"
    for ns, ver, gtk_ver in WEBKIT_SPECS:
        try:
            gi.require_version(ns, ver)
        except Exception:
            continue
        try:
            gi.require_version("Gtk", gtk_ver)
        except Exception:
            reason = f"GTK {gtk_ver.split('.')[0]} is not installed"
            continue
        try:
            Gtk = importlib.import_module("gi.repository.Gtk")
            WebKit = importlib.import_module("gi.repository." + ns)
            GLib = importlib.import_module("gi.repository.GLib")
            return (ns, ver, gtk_ver, Gtk, WebKit, GLib), ""
        except Exception as e:
            reason = f"{ns} {ver} is installed but could not be loaded ({e})"
    return None, reason


def _webkit_imports():
    return _webkit_probe()[0]


def _qt_binding():
    for name in QT_BINDINGS:
        try:
            if importlib.util.find_spec(name) is None:
                continue
            if importlib.util.find_spec(name + ".QtWebEngineWidgets") is None:
                continue
            return name
        except Exception:
            continue
    return ""


def _qt_reason():
    found = []
    for name in QT_BINDINGS:
        try:
            if importlib.util.find_spec(name) is not None:
                found.append(name)
        except Exception:
            continue
    if not found:
        return "no Qt for Python (PyQt6, PySide6, PyQt5 or PySide2) is installed"
    return ", ".join(found) + " is installed without QtWebEngineWidgets"


def _webview2_available():
    if sys.platform != "win32":
        return False
    try:
        return importlib.util.find_spec("webview") is not None
    except Exception:
        return False


def _engine_available_here(engine):
    if engine == "webkit":
        return _webkit_imports() is not None
    if engine == "qt":
        return bool(_qt_binding())
    if engine == "webview2":
        return _webview2_available()
    return False


_PREFIX_CACHE = {}


def _interpreter_prefix(path):
    """sys.prefix of `path`, or None when it cannot be asked."""
    if path in _PREFIX_CACHE:
        return _PREFIX_CACHE[path]
    prefix = None
    try:
        proc = subprocess.run([path, "-c", "import sys; print(sys.prefix)"],
                              capture_output=True, text=True, timeout=15)
        if proc.returncode == 0 and proc.stdout.strip():
            prefix = os.path.realpath(proc.stdout.strip())
    except Exception:
        prefix = None
    _PREFIX_CACHE[path] = prefix
    return prefix


def _interp_identity(path):
    """Identify an interpreter *environment*, not just its binary.

    A venv's bin/python is normally a symlink to the system interpreter, so
    comparing realpaths made every venv look like the system python and hid it.
    The app's venv has no system site-packages, so PyGObject/WebKitGTK lives
    only in the system interpreter and sign-in never found it (Fedora, Debian,
    Arch -- any distro where the venv links instead of copying).
    """
    binary = os.path.realpath(path)
    return binary, _interpreter_prefix(path)


def _system_pythons():
    env = os.environ.get("P100_SYSTEM_PYTHON")
    candidates = [env] if env else []
    candidates += ["/usr/bin/python3", "/usr/local/bin/python3", shutil.which("python3")]
    out = []
    seen = {_interp_identity(sys.executable)}
    for py in candidates:
        if not py or not os.path.exists(py) or not os.access(py, os.X_OK):
            continue
        key = _interp_identity(py)
        if key in seen:
            continue
        seen.add(key)
        out.append(py)
    return out


def _probe_engine_here(engine):
    if engine == "webkit":
        imports, reason = _webkit_probe()
        if imports:
            return True, f"WebKitGTK {imports[1]}"
        return False, reason
    if engine == "qt":
        binding = _qt_binding()
        if binding:
            return True, binding
        return False, _qt_reason()
    if engine == "webview2":
        if _webview2_available():
            return True, "WebView2 (pywebview)"
        return False, "pywebview is not installed (Windows only)"
    return False, "unknown engine"


def _probe_engine(engine):
    ok, label = _probe_engine_here(engine)
    if ok or engine == "webview2" or sys.platform == "win32":
        return ok, label
    # The app venv usually lacks PyGObject/Qt, while the system interpreter
    # may have them. Keep every attempt's reason so the panel can say what is
    # actually missing instead of a bare "unavailable".
    reasons = ["this python: " + label]
    for py in _system_pythons():
        try:
            proc = subprocess.run(
                [py, os.path.abspath(__file__), "--probe-engine", engine],
                capture_output=True, text=True, timeout=20)
            data = {}
            if proc.returncode == 0 and proc.stdout.strip():
                data = json.loads(proc.stdout.strip().splitlines()[-1])
            if data.get("available"):
                return True, data.get("label") or py
            reasons.append("%s: %s" % (py, data.get("label") or "not available"))
        except Exception:
            reasons.append("%s: could not be started" % py)
    return False, "; ".join(reasons)


def probe_payload():
    engines = []
    for engine in ("webkit", "qt", "webview2"):
        available, label = _probe_engine(engine)
        engines.append({"id": engine, "available": bool(available),
                        "label": label if available else "", "reason": "" if available else label})
    return {
        "python": sys.executable,
        "share": SHARE_DIR,
        "engines": engines,
        "browsers": _detect_browsers(),
        "paste": True,
    }


def _browser_candidates():
    home = os.path.expanduser("~")
    if sys.platform == "win32":
        local = os.environ.get("LOCALAPPDATA") or os.path.join(home, "AppData", "Local")
        roaming = os.environ.get("APPDATA") or os.path.join(home, "AppData", "Roaming")
        return {
            "firefox": [os.path.join(roaming, "Mozilla", "Firefox")],
            "librewolf": [os.path.join(roaming, "librewolf")],
            "chrome": [os.path.join(local, "Google", "Chrome", "User Data")],
            "chromium": [os.path.join(local, "Chromium", "User Data")],
            "edge": [os.path.join(local, "Microsoft", "Edge", "User Data")],
            "brave": [os.path.join(local, "BraveSoftware", "Brave-Browser", "User Data")],
        }
    return {
        "firefox": [os.path.join(home, ".mozilla", "firefox")],
        "librewolf": [os.path.join(home, ".librewolf")],
        "chrome": [os.path.join(home, ".config", "google-chrome"),
                   os.path.join(home, ".config", "google-chrome-beta")],
        "chromium": [os.path.join(home, ".config", "chromium")],
        "edge": [os.path.join(home, ".config", "microsoft-edge")],
        "brave": [os.path.join(home, ".config", "BraveSoftware", "Brave-Browser")],
    }


def _detect_browsers():
    found = []
    for name, paths in _browser_candidates().items():
        if any(os.path.isdir(path) for path in paths):
            found.append(name)
    return found


# ---------------------------------------------------------------------------
# WebKitGTK engine (YouTube or Spotify)
# ---------------------------------------------------------------------------

def run_webkit(out_path, site="google"):
    imports = _webkit_imports()
    if not imports:
        return EXIT_NO_ENGINE, "No usable WebKitGTK (gi) installation was found"
    _ns, version, _gtk_ver, Gtk, WebKit, GLib = imports

    start = SPOTIFY_START if site == "spotify" else START_URL
    home = SPOTIFY_HOME if site == "spotify" else HOME_URL
    required = "sp_dc" if site == "spotify" else "__Secure-3PAPISID"
    state = {"cookies": {}, "done": False, "closed": False, "error": ""}

    def cookie_manager(view):
        try:
            return view.get_network_session().get_cookie_manager()
        except Exception:
            return view.get_cookie_manager()

    def persist(cookie_list):
        mapping = {}
        for cookie in cookie_list:
            try:
                mapping[cookie.get_name()] = cookie.get_value()
            except Exception:
                pass
        state["cookies"] = mapping
        if not mapping.get(required):
            return False
        if site == "spotify":
            _write_cookies(os.path.join(login.config_dir(), "spotify.json"),
                           {"sp_dc": mapping["sp_dc"]}, filter_auth=False)
        else:
            _write_cookies(out_path, mapping)
        state["done"] = True
        return True

    gtk4 = _gtk_ver == "4.0"
    if gtk4:
        app = Gtk.Application(application_id="dev.crossfade.login")

        def on_activate(application):
            win = Gtk.ApplicationWindow(application=application)
            win.set_title("Sign in to crossfade")
            win.set_default_size(560, 840)
            view = WebKit.WebView()
            try:
                view.get_settings().set_user_agent(UA)
            except Exception:
                pass
            win.set_child(view)
            manager = cookie_manager(view)
            try:
                manager.set_accept_policy(WebKit.CookieAcceptPolicy.ALWAYS)
            except Exception:
                pass

            def on_cookies(mgr, task, *_):
                if state["done"]:
                    return
                try:
                    cookies = mgr.get_cookies_finish(task)
                except Exception as e:
                    state["error"] = str(e)
                    return
                if persist(cookies):
                    application.quit()

            def poll():
                if state["done"]:
                    return False
                manager.get_cookies(home, None, on_cookies)
                return True

            def on_close(*_):
                state["closed"] = True
                application.quit()
                return False

            win.connect("close-request", on_close)
            view.load_uri(start)
            win.present()
            GLib.timeout_add(650, poll)

            def on_timeout():
                application.quit()
                return False

            GLib.timeout_add_seconds(900, on_timeout)

        app.connect("activate", on_activate)
        app.run([])
    else:
        win = Gtk.Window(title="Sign in to crossfade")
        win.set_default_size(560, 840)
        view = WebKit.WebView()
        try:
            view.get_settings().set_user_agent(UA)
        except Exception:
            pass
        win.add(view)
        manager = cookie_manager(view)
        try:
            manager.set_accept_policy(WebKit.CookieAcceptPolicy.ALWAYS)
        except Exception:
            pass
        loop = GLib.MainLoop()

        def on_cookies(mgr, task, *_):
            if state["done"]:
                return
            try:
                cookies = mgr.get_cookies_finish(task)
            except Exception as e:
                state["error"] = str(e)
                return
            if persist(cookies):
                loop.quit()

        def poll():
            if state["done"]:
                return False
            manager.get_cookies(home, None, on_cookies)
            return True

        def on_close(*_):
            state["closed"] = True
            loop.quit()

        win.connect("destroy", on_close)
        view.load_uri(start)
        win.show_all()
        GLib.timeout_add(650, poll)

        def on_timeout():
            loop.quit()
            return False

        GLib.timeout_add_seconds(900, on_timeout)
        loop.run()

    if state["done"]:
        return EXIT_OK, f"WebKitGTK {version} sign-in completed"
    if state["closed"]:
        return EXIT_CANCELLED, "Sign-in window closed"
    if state["error"]:
        return EXIT_REJECTED, state["error"]
    return EXIT_REJECTED, ("No Google sign-in cookie appeared. Google may have "
                           "blocked the embedded browser.")


# ---------------------------------------------------------------------------
# Qt WebEngine engine
# ---------------------------------------------------------------------------

def _qt_classes(binding):
    core = importlib.import_module(binding + ".QtCore")
    widgets = importlib.import_module(binding + ".QtWidgets")
    wew = importlib.import_module(binding + ".QtWebEngineWidgets")
    profile_cls = getattr(wew, "QWebEngineProfile", None)
    try:
        wec = importlib.import_module(binding + ".QtWebEngineCore")
        profile_cls = getattr(wec, "QWebEngineProfile", profile_cls)
    except Exception:
        pass
    return core, widgets, wew, profile_cls


def _cookie_text(value):
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    try:
        return bytes(value).decode("utf-8", "replace")
    except Exception:
        return str(value)


def run_qt(out_path):
    binding = _qt_binding()
    if not binding:
        return EXIT_NO_ENGINE, "No Qt WebEngine Python bindings were found"
    try:
        core, widgets, wew, profile_cls = _qt_classes(binding)
        QApplication = widgets.QApplication
        QWebEngineView = wew.QWebEngineView
        QUrl = core.QUrl
        QTimer = core.QTimer
    except Exception as e:
        return EXIT_NO_ENGINE, f"Qt WebEngine could not be imported: {e}"

    app = QApplication.instance() or QApplication([])
    profile = profile_cls(app) if profile_cls else None
    try:
        profile.setHttpUserAgent(UA)
    except Exception:
        pass
    store = profile.cookieStore() if profile else None
    if store is None:
        return EXIT_NO_ENGINE, "The Qt WebEngine cookie store is unavailable"

    state = {"cookies": {}, "done": False, "closed": False, "error": ""}

    def on_cookie(cookie):
        # Only the YouTube host's cookies are valid for music.youtube.com.
        # The store also sees accounts.google.com during the login flow, and
        # those copies of HSID/SSID/__Secure-*PSIDTS differ per domain -- mixing
        # them makes Google treat the session as anonymous.
        try:
            domain = (cookie.domain() or "").lower().lstrip(".")
            if domain and not domain.endswith("youtube.com"):
                return
            state["cookies"][cookie.name()] = _cookie_text(cookie.value())
        except Exception:
            pass

    try:
        store.cookieAdded.connect(on_cookie)
    except Exception as e:
        return EXIT_NO_ENGINE, f"Qt cookie signals are unavailable: {e}"
    try:
        store.loadAllCookies()
    except Exception:
        pass

    class View(QWebEngineView):
        def closeEvent(self, event):
            state["closed"] = True
            app.quit()
            event.accept()

    view = View()
    view.setWindowTitle("Sign in to crossfade")
    view.resize(560, 840)
    view.setUrl(QUrl(START_URL))
    view.show()

    def poll():
        if state["done"]:
            return
        cookies = dict(state["cookies"])
        if cookies.get("__Secure-3PAPISID"):
            try:
                _write_cookies(out_path, cookies)
                state["done"] = True
            except Exception as e:
                state["error"] = str(e)
            app.quit()

    timer = QTimer()
    timer.timeout.connect(poll)
    timer.start(700)
    timeout = QTimer()
    timeout.setSingleShot(True)
    timeout.timeout.connect(app.quit)
    timeout.start(900 * 1000)
    if hasattr(app, "exec"):
        app.exec()
    else:
        app.exec_()

    if state["done"]:
        return EXIT_OK, f"{binding} sign-in completed"
    if state["closed"]:
        return EXIT_CANCELLED, "Sign-in window closed"
    if state["error"]:
        return EXIT_REJECTED, state["error"]
    return EXIT_REJECTED, ("No Google sign-in cookie appeared. Google may have "
                           "blocked the embedded browser.")


# ---------------------------------------------------------------------------
# Windows WebView2 through optional pywebview
# ---------------------------------------------------------------------------

def _cookie_map(cookies):
    """Normalise pywebview's cookie results across versions.

    pywebview's EdgeChromium backend appends http.cookies.SimpleCookie
    instances (one morsel each), while some versions return objects with
    .name/.value or plain dicts. Reading `.name` on a SimpleCookie silently
    produced an empty map, so a successful WebView2 login was never noticed.
    """
    mapping = {}
    if cookies is None:
        return mapping
    if hasattr(cookies, "items"):
        cookies = [cookies]
    for cookie in cookies:
        try:
            if hasattr(cookie, "items"):  # SimpleCookie or a plain dict
                for name, morsel in cookie.items():
                    value = getattr(morsel, "value", morsel)
                    if name and value is not None:
                        mapping[str(name)] = str(value)
            else:
                name = getattr(cookie, "name", None)
                value = getattr(cookie, "value", None)
                if name and value is not None:
                    mapping[str(name)] = str(value)
        except Exception:
            continue
    return mapping


def run_webview2(out_path, site="google"):
    if sys.platform != "win32":
        return EXIT_NO_ENGINE, "WebView2 is only available on Windows"
    try:
        import webview
    except Exception as e:
        return EXIT_NO_ENGINE, f"pywebview is not installed ({e})"
    window_cls = getattr(webview, "Window", None)
    if window_cls is None or not hasattr(window_cls, "get_cookies"):
        return EXIT_NO_ENGINE, "The installed pywebview cannot read cookies"

    spotify = site == "spotify"
    start = SPOTIFY_START if spotify else START_URL
    required = "sp_dc" if spotify else "__Secure-3PAPISID"
    target = os.path.join(login.config_dir(), "spotify.json") if spotify else out_path
    title = "Sign in to Spotify" if spotify else "Sign in to crossfade"
    state = {"ok": False, "closed": False, "error": "",
             "read_error": "", "seen": ""}
    window = webview.create_window(title, start, width=580, height=860)

    def on_closed():
        state["closed"] = True

    try:
        window.events.closed += on_closed
    except Exception:
        pass

    def worker(win):
        deadline = time.time() + 900
        last_seen = 0.0
        while time.time() < deadline:
            if state["closed"]:
                break
            try:
                cookies = win.get_cookies() or []
            except Exception as e:
                # get_cookies() raises until the first page has loaded; that is
                # expected while the window is opening.
                if "before the page has loaded" not in str(e) and not state["read_error"]:
                    state["read_error"] = str(e)
                    log(f"WebView2 cookie read failed: {e}")
                cookies = []
            mapping = _cookie_map(cookies)
            if mapping and time.time() - last_seen > 15:
                # Names only: never log cookie values.
                names = ", ".join(sorted(mapping)[:20])
                if names != state["seen"]:
                    state["seen"] = names
                    log(f"WebView2 cookies seen: {names}")
                last_seen = time.time()
            if mapping.get(required):
                try:
                    if spotify:
                        _write_cookies(target, {"sp_dc": mapping["sp_dc"]},
                                       filter_auth=False)
                    else:
                        _write_cookies(target, mapping)
                    state["ok"] = True
                except Exception as e:
                    state["error"] = str(e)
                try:
                    win.destroy()
                except Exception:
                    pass
                return
            time.sleep(0.8)
        try:
            win.destroy()
        except Exception:
            pass

    try:
        webview.start(worker, window, gui="edgechromium", private_mode=False,
                      storage_path=os.path.join(
                          login.config_dir(),
                          "login-spotify" if spotify else "login-webview2"))
    except Exception as e:
        return EXIT_NO_ENGINE, f"WebView2 could not start: {e}"

    if state["ok"]:
        return EXIT_OK, ("Spotify sign-in completed" if spotify
                         else "WebView2 sign-in completed")
    if state["closed"]:
        return EXIT_CANCELLED, "Sign-in window closed"
    if state["error"]:
        return EXIT_REJECTED, state["error"]
    if state["read_error"]:
        return EXIT_REJECTED, f"WebView2 cookies could not be read: {state['read_error']}"
    if spotify:
        return EXIT_REJECTED, ("No Spotify sign-in cookie appeared. Spotify may "
                               "have blocked the embedded browser.")
    return EXIT_REJECTED, ("No Google sign-in cookie appeared. Google may have "
                           "blocked the embedded browser.")


# ---------------------------------------------------------------------------
# Engine cascade
# ---------------------------------------------------------------------------

def _run_engine(engine, out_path):
    if engine == "webkit":
        return run_webkit(out_path)
    if engine == "qt":
        return run_qt(out_path)
    if engine == "webview2":
        return run_webview2(out_path)
    return EXIT_NO_ENGINE, f"Unknown engine: {engine}"


def _invoke_engine(engine, out_path):
    if _engine_available_here(engine):
        return _run_engine(engine, out_path)
    if engine == "webview2":
        return EXIT_NO_ENGINE, "pywebview is not installed"
    # The app venv rarely has PyGObject/Qt; fall back to a system interpreter
    # that has system site-packages (the venv symlinks its binary, so this is
    # also the case where the system python must not be deduped away).
    reasons = ["this python: " + _probe_engine_here(engine)[1]]
    for py in _system_pythons():
        try:
            rc = subprocess.call(
                [py, os.path.abspath(__file__), "--engine", engine, "--out", out_path])
            if rc != EXIT_NO_ENGINE:
                return rc, f"{engine} (via {py})"
            reasons.append("%s: no engine" % py)
        except Exception as e:
            reasons.append("%s: %s" % (py, e))
    return EXIT_NO_ENGINE, "; ".join(reasons)


def cmd_signin(choice):
    _truncate_log()
    order = ["webkit", "qt", "webview2"] if choice == "auto" else [choice]
    if sys.platform != "win32":
        order = [engine for engine in order if engine != "webview2"]
    out_path = os.path.join(login.config_dir(), "login-cookies.json")
    attempted = False
    for engine in order:
        log(f"Trying {engine} ...")
        rc, detail = _invoke_engine(engine, out_path)
        log(f"{engine}: {detail} (code {rc})")
        if rc != EXIT_NO_ENGINE:
            attempted = True
        if rc == EXIT_OK:
            try:
                cookies = _read_cookies(out_path)
            except Exception as e:
                log(f"{engine}: could not read cookies ({e})")
                continue
            ok, message = login.install_from_cookies(cookies)
            log(message)
            if ok:
                return EXIT_OK
            attempted = True
            continue
        if rc == EXIT_CANCELLED:
            return EXIT_CANCELLED
    log("No embedded sign-in engine is available; sign in with your browser "
        "and import the session instead.")
    return EXIT_BLOCKED if attempted else EXIT_NO_ENGINE


def cmd_import_cookies(choice):
    _truncate_log()
    if choice == "auto":
        names = ["firefox", "librewolf", "chrome", "chromium", "edge", "brave"]
    else:
        names = [choice]
    for name in names:
        try:
            if name in ("firefox", "librewolf"):
                root = None
                if name == "librewolf":
                    root = _browser_candidates()["librewolf"][0]
                sessions = login.firefox_sessions(profile_root=root)
            else:
                sessions = [(name, login.cookies_from_ytdlp_browser(name))]
        except Exception as e:
            log(f"{name}: {e}")
            continue
        if not sessions:
            log(f"{name}: no signed-in session found")
            continue
        for profile, cookies in sessions:
            ok, message = login.install_from_cookies(cookies)
            log(f"{name} [{os.path.basename(profile)}]: {message}")
            if ok:
                return EXIT_OK
    log("No usable signed-in browser session was found.")
    return EXIT_REJECTED


def cmd_headers(path):
    try:
        if path == "-":
            text = sys.stdin.read()
        else:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
    except Exception as e:
        return EXIT_BAD_HEADERS, f"Could not read the headers: {e}"
    finally:
        # Only remove the app's own temp file, never a path the user chose.
        try:
            if (path != "-" and
                    os.path.dirname(os.path.abspath(path)) ==
                    os.path.abspath(login.config_dir())):
                os.unlink(path)
        except OSError:
            pass
    ok, message = login.install_from_headers(text)
    if ok:
        return EXIT_OK, message
    if "No Cookie header" in message or "Could not parse" in message:
        return EXIT_BAD_HEADERS, message
    return EXIT_REJECTED, message


def _dispatch(args):
    if args.probe_engine:
        ok, label = _probe_engine_here(args.probe_engine)
        print(json.dumps({"available": bool(ok), "label": label},
                         separators=(",", ":")))
        return EXIT_OK
    if args.probe:
        print(json.dumps(probe_payload(), separators=(",", ":")))
        return EXIT_OK
    if args.engine:
        if not args.out:
            print("--engine requires --out", file=sys.stderr)
            return EXIT_NO_ENGINE
        rc, detail = _run_engine(args.engine, args.out)
        if detail:
            log(detail)
        return rc
    if args.headers:
        rc, message = cmd_headers(args.headers)
        log(message)
        return rc
    if args.import_cookies:
        return cmd_import_cookies(args.import_cookies)
    if args.import_firefox:
        return cmd_import_cookies("firefox")
    if args.spotify:
        # WebKitGTK is Linux-only; Windows signs in through the WebView2
        # wrapper (pywebview) that the Google flow already uses.
        if sys.platform == "win32":
            rc, detail = run_webview2(None, site="spotify")
        else:
            rc, detail = run_webkit(None, site="spotify")
        log(detail)
        return rc
    if args.signin:
        return cmd_signin(args.signin)
    if args.google:
        return cmd_signin("auto")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", action="store_true")
    ap.add_argument("--probe-engine", choices=["webkit", "qt", "webview2"])
    ap.add_argument("--engine", choices=["webkit", "qt", "webview2"])
    ap.add_argument("--out")
    ap.add_argument("--signin", nargs="?", const="auto",
                    choices=["auto", "webkit", "qt", "webview2"])
    ap.add_argument("--import-cookies", nargs="?", const="auto",
                    choices=["auto", "firefox", "librewolf", "chrome", "chromium",
                             "edge", "brave"])
    ap.add_argument("--headers")
    ap.add_argument("--google", action="store_true")
    ap.add_argument("--import-firefox", action="store_true")
    ap.add_argument("--spotify", action="store_true")
    ap.add_argument("--result-file")
    args = ap.parse_args()

    global RESULT_FILE
    RESULT_FILE = args.result_file
    code = _dispatch(args)
    if code is None:
        print("choose --signin, --headers, --import-cookies, --probe or --spotify",
              file=sys.stderr)
        code = EXIT_BAD_HEADERS
    RESULT["code"] = code
    return code


if __name__ == "__main__":
    sys.exit(main())
