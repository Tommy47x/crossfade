"""Create <config>/browser.json from pasted browser request headers.

This is the Windows shortcut path used by crossfade-login.cmd. It shares the
parser, verification and atomic replace logic with the in-app helper through
`cf_backend.login`, so every method accepts the same DevTools, HTTP/2 and cURL
formats.

Usage:
  python make_browser_json.py            # paste, finish with Ctrl+Z+Enter
  python make_browser_json.py <file>     # read headers from a file
  python make_browser_json.py clipboard  # use the Windows clipboard
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
for _share in (os.environ.get("P100_SHARE_DIR"), HERE, os.path.dirname(HERE),
               os.path.dirname(os.path.dirname(HERE))):
    if _share and os.path.isdir(os.path.join(_share, "py", "cf_backend")):
        sys.path.insert(0, os.path.join(_share, "py"))
        break

from cf_backend import login  # noqa: E402


def clipboard_text():
    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-Command", "Get-Clipboard"],
            capture_output=True, timeout=15,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return result.stdout.decode("utf-8", errors="replace")
    except Exception as e:
        print("Clipboard read failed:", e)
        return ""


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "clipboard":
        text = clipboard_text()
        if "cookie" not in text.lower():
            print("Clipboard has no request headers to use.")
            return 4
        print("Using headers from the clipboard ...")
    elif len(sys.argv) > 1:
        with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
            text = f.read()
        print("Read headers from", sys.argv[1])
    else:
        print("Paste the request headers now (whole block), then press")
        print("Ctrl+Z and Enter to finish. cURL format also works.\n")
        text = sys.stdin.buffer.read().decode("utf-8", errors="replace")

    ok, message = login.install_from_headers(text)
    print(message)
    if ok:
        print("LOGIN OK - YouTube Music library responded.")
        print("Restart crossfade and you are signed in.")
        return 0
    if "No Cookie header" in message or "Could not parse" in message:
        print("\nCopy the request headers of a POST request to music.youtube.com")
        print("while signed in, and try again.")
        return 2
    print("\nMost common causes:")
    print("  - music.youtube.com was not signed in when headers were copied")
    print("    (check for a 'Sign in' button on the page)")
    print("  - the browser profile differs from the one you are signed into")
    return 3


if __name__ == "__main__":
    sys.exit(main())
