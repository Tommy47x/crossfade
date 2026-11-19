# Security

## Reporting

Please report suspected vulnerabilities privately to the maintainers rather
than opening a public issue. Include the affected version, platform, and a
minimal reproduction if possible.

## Local data

- `browser.json` and `browseroauth.json` contain live YouTube Music
  credentials. Treat them like passwords. They are git-ignored and should be
  mode `0600`; the config directory is `0700`.
- Signing in may read cookies from a browser profile you choose (delegated to
  yt-dlp for Chromium-family stores), open a WebKitGTK/Qt/WebView2 window, or
  accept pasted request headers. Pasted headers are written to a `0600` temp
  file under the config directory, read once and deleted; cookie and header
  values are never written to logs. A candidate session is verified before it
  atomically replaces `browser.json`, so a rejected paste leaves the previous
  session intact. WebView2 uses a dedicated profile under the config
  directory, never your normal Edge profile.
- `library.db`, playlists, history and caches can reveal listening habits.
  Export/import archives contain them.
- The app sends no telemetry. The optional update check fetches the release's
  `version.txt` from GitHub at most once a day. On Linux, a self-update happens
  only after the user clicks **Update now**: it downloads the release asset and
  `SHA256SUMS`, verifies the SHA-256, and replaces only the app's own files.
  Checksums detect corrupted or truncated downloads; they are not a signature
  and do not protect against a compromised upstream release. Package-managed
  installs are never modified in place.
- Party mode stays on the local network.

## Party mode

The guest server binds to the LAN and validates the `Host` header, caps
request bodies, rate-limits per guest, and uses constant-time token checks.
Do not expose the party port to the public internet; it is designed for a
trusted local network.
