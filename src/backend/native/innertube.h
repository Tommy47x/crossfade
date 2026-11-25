#pragma once

#include <string>
#include <vector>

// Native YouTube stream resolver. Two requests and a JSON parse:
//   1. GET the watch page once per session -> cookies + VISITOR_DATA
//   2. POST youtubei/v1/player with the JSLess VISIONOS client -> plain URLs
// No yt-dlp, no deno, no PoToken, no cookies from the browser. Falls back to
// the Python backend (in ytdlp.cpp) if any step fails.
namespace native
{
struct ResolveTrace
{
    std::string video_id;
    long long epoch_ms = 0;
    std::string client = "VISIONOS";
    std::string status;
    std::string reason;
    std::string error;
    std::string url_prefix; // scheme+host+path only, never the query
    bool session_hit = false;
    bool session_refreshed = false;
    bool hls = false;
    std::size_t page_bytes = 0;
    std::size_t player_bytes = 0;
    long long session_ms = 0;
    long long player_ms = 0;
    long long total_ms = 0;
    int formats = 0;
    int audio_formats = 0;
    int itag = 0;
    int bitrate = 0;
};

struct StreamResult
{
    bool ok = false;
    std::string url;
    std::string error;
    int itag = 0;
    std::string mime;
    int bitrate = 0;
    bool hls = false;
    ResolveTrace trace;
};

struct SessionInfo
{
    bool valid = false;
    int cookies = 0;
    std::string visitor_prefix;
    long long age_seconds = 0;
};

// Resolves one audio stream URL. Thread-safe; callers can hammer it from the
// prefetch pool.
StreamResult resolve_audio(const std::string &video_id);

// Song search. Produces the Python backend's track-array JSON (id, title,
// artist, album, thumbnail_url, duration_sec, url). Returns false to fall back.
bool search(const std::string &query, int limit, std::string &json,
            std::string &error);
bool search_albums(const std::string &query, int limit, std::string &json,
                   std::string &error);
bool search_artists(const std::string &query, int limit, std::string &json,
                    std::string &error);

// Metadata-only lookup (title/artist/duration/thumbnail) from the same player
// response, no stream selection. Produces the Python backend's "meta" JSON
// shape. Returns false when the caller should fall back to Python.
bool fetch_meta(const std::string &video_id, std::string &json, std::string &error);

// Resolve a muxed video + audio pair from the same VISIONOS player reply.
// Produces {"video":"...","audio":"..."} (audio omitted for progressive
// formats) matching the Python play_video* reply.
bool resolve_video(const std::string &video_id, int max_height, bool fresh,
                   std::string &json, std::string &error);

SessionInfo session_info();
void reset_session();

// Internal, used by the other native command modules (browse, library):
// the watch-page session's cookies and VISITOR_DATA, opening a session when
// there is none. Returns false when the watch page could not be fetched.
bool get_session(std::string &cookies, std::string &visitor, std::string &error);
// Like get_session but never opens one: returns false when the current
// watch-page session is missing or stale. Browse/search work anonymously, so
// they use this to avoid paying for the watch-page fetch.
bool peek_session(std::string &cookies, std::string &visitor);
// Append a trace to the shared ring (used by commands that bypass
// resolve_audio, e.g. browse requests).
void record_trace(const ResolveTrace &trace);
// Chrome user agent used for music.youtube.com requests.
const char *web_user_agent();

// Most recent traces, newest first (bounded).
std::vector<ResolveTrace> recent_traces();
} // namespace native
