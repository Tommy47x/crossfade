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

// Metadata-only lookup (title/artist/duration/thumbnail) from the same player
// response, no stream selection. Produces the Python backend's "meta" JSON
// shape. Returns false when the caller should fall back to Python.
bool fetch_meta(const std::string &video_id, std::string &json, std::string &error);

SessionInfo session_info();
void reset_session();

// Most recent traces, newest first (bounded).
std::vector<ResolveTrace> recent_traces();
} // namespace native
