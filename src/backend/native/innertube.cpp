#include "innertube.h"

#include "http.h"
#include "json_value.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace native
{
namespace
{
// Client config used by yt-dlp for its "JSLess" path. VISIONOS currently
// returns plain streaming URLs and never asks for a JS challenge or a PoToken,
// which is why this resolver needs neither.
constexpr const char *kClientName = "VISIONOS";
constexpr const char *kClientVersion = "1.02";
constexpr const char *kClientId = "101";
constexpr const char *kClientUa =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/26.0 Safari/605.1.15";
constexpr const char *kPageUa =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36";

struct Session
{
    std::string cookies;
    std::string visitor;
    std::chrono::steady_clock::time_point at{};
    bool valid = false;
};

std::mutex g_session_mutex;
Session g_session;

std::mutex g_trace_mutex;
std::vector<ResolveTrace> g_traces;

constexpr long long kSessionTtlSec = 3 * 60 * 60;
constexpr std::size_t kTraceMax = 64;

long long age_seconds(const Session &s)
{
    if (!s.valid)
        return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - s.at)
        .count();
}

bool session_fresh(const Session &s)
{
    return s.valid && age_seconds(s) < kSessionTtlSec && !s.visitor.empty();
}

std::string extract_visitor(const std::string &html)
{
    const std::string needle = "\"VISITOR_DATA\":\"";
    const std::size_t at = html.find(needle);
    if (at == std::string::npos)
        return "";
    const std::size_t begin = at + needle.size();
    const std::size_t end = html.find('"', begin);
    if (end == std::string::npos || end == begin)
        return "";
    return html.substr(begin, end - begin);
}

// scheme://host/path, query stripped: URLs carry the client IP and signature.
std::string url_prefix(const std::string &url)
{
    const std::size_t scheme = url.find("://");
    const std::size_t path = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    return url.substr(0, path == std::string::npos ? url.size() : path);
}

bool refresh_session(ResolveTrace &trace)
{
    std::string jar;
    std::vector<HttpHeader> headers = {
        {"User-Agent", kPageUa},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
    };
    // The watch page sets the visitor cookies and embeds VISITOR_DATA in
    // ytcfg. Any video id works; the session is not tied to the content.
    HttpResult res = http_get("https://www.youtube.com/watch?v=dQw4w9WgXcQ", headers, jar);
    trace.page_bytes = res.bytes;
    trace.session_ms = res.ms;
    if (!res.ok)
    {
        trace.error = "session: " + res.error;
        return false;
    }
    const std::string visitor = extract_visitor(res.body);
    if (visitor.empty())
    {
        trace.error = "session: no VISITOR_DATA";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_session_mutex);
    g_session.cookies = jar;
    g_session.visitor = visitor;
    g_session.at = std::chrono::steady_clock::now();
    g_session.valid = true;
    return true;
}

std::string player_body(const std::string &video_id, const std::string &visitor)
{
    // Constructed by hand: a few fixed fields, no user data, no escaping
    // issues (video ids are [A-Za-z0-9_-]{11} and the visitor id is
    // URL-safe base64).
    std::string body;
    body.reserve(512);
    body += "{\"context\":{\"client\":{";
    body += "\"clientName\":\"" + std::string(kClientName) + "\",";
    body += "\"clientVersion\":\"" + std::string(kClientVersion) + "\",";
    body += "\"deviceMake\":\"Apple\",";
    body += "\"deviceModel\":\"RealityDevice17,1\",";
    body += "\"userAgent\":\"" + std::string(kClientUa) + "\",";
    body += "\"osName\":\"visionOS\",";
    body += "\"osVersion\":\"26.5.23O471\",";
    body += "\"hl\":\"en\",\"gl\":\"US\",";
    body += "\"visitorData\":\"" + visitor + "\"";
    body += "}},\"videoId\":\"" + video_id + "\",";
    body += "\"contentCheckOk\":true,\"racyCheckOk\":true}";
    return body;
}

bool pick_format(const jsonv::Value &root, StreamResult &out, ResolveTrace &trace)
{
    const jsonv::Value &sd = root["streamingData"];
    if (sd.is_null())
    {
        trace.error = "no streamingData";
        return false;
    }

    struct Candidate
    {
        int score;
        int itag;
        int bitrate;
        std::string mime;
        std::string url;
    };
    std::vector<Candidate> found;
    auto scan = [&](const jsonv::Value &list) {
        if (!list.is_array())
            return;
        trace.formats += (int)list.size();
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            const jsonv::Value &f = list.at(i);
            const std::string mime = f.str("mimeType");
            const std::string url = f.str("url");
            if (mime.rfind("audio/", 0) != 0 || url.empty())
                continue;
            const int itag = (int)f["itag"].as_int();
            const int bitrate = (int)f["bitrate"].as_int();
            // Prefer the known audio-only itags, then bitrate.
            int score = bitrate;
            if (itag == 251)
                score += 10000000; // opus 160k
            else if (itag == 250)
                score += 5000000;
            else if (itag == 140)
                score += 2000000;
            if (mime.find("opus") != std::string::npos || mime.find("webm") != std::string::npos)
                score += 100000;
            found.push_back({score, itag, bitrate, mime, url});
        }
    };
    scan(sd["adaptiveFormats"]);
    scan(sd["formats"]);
    trace.audio_formats = (int)found.size();

    if (!found.empty())
    {
        const Candidate &best =
            *std::max_element(found.begin(), found.end(),
                              [](const Candidate &a, const Candidate &b) {
                                  return a.score < b.score;
                              });
        out.ok = true;
        out.url = best.url;
        out.itag = best.itag;
        out.bitrate = best.bitrate;
        out.mime = best.mime;
        return true;
    }

    // Some clients only expose a master HLS playlist. mpv/ffmpeg can play it.
    const std::string hls = sd.str("hlsManifestUrl");
    if (!hls.empty())
    {
        out.ok = true;
        out.url = hls;
        out.hls = true;
        out.mime = "application/vnd.apple.mpegurl";
        return true;
    }
    trace.error = "no audio formats";
    return false;
}

bool attempt(const std::string &video_id, StreamResult &out, bool refresh)
{
    ResolveTrace &trace = out.trace;
    trace.video_id = video_id;
    const auto total_start = std::chrono::steady_clock::now();

    if (refresh || !session_fresh(g_session))
    {
        trace.session_hit = false;
        trace.session_refreshed = refresh;
        if (!refresh_session(trace))
            return false;
    }
    else
        trace.session_hit = true;

    std::string cookies, visitor;
    {
        std::lock_guard<std::mutex> lock(g_session_mutex);
        cookies = g_session.cookies;
        visitor = g_session.visitor;
    }

    std::vector<HttpHeader> headers = {
        {"User-Agent", kClientUa},
        {"X-YouTube-Client-Name", kClientId},
        {"X-YouTube-Client-Version", kClientVersion},
        {"X-Goog-Visitor-Id", visitor},
        {"Origin", "https://www.youtube.com"},
        {"Referer", "https://www.youtube.com/"},
        {"Accept-Language", "en"},
    };
    const HttpResult res = http_post_json(
        "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
        player_body(video_id, visitor), headers, cookies);
    trace.player_ms = res.ms;
    trace.player_bytes = res.bytes;
    trace.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - total_start)
                         .count();
    if (!res.ok)
    {
        trace.error = "player: " + res.error;
        return false;
    }

    jsonv::Value root;
    std::string parse_error;
    if (!jsonv::Value::parse(res.body, root, parse_error))
    {
        trace.error = "player json: " + parse_error;
        return false;
    }
    trace.status = root["playabilityStatus"].str("status");
    trace.reason = root["playabilityStatus"].str("reason");
    if (trace.status != "OK")
    {
        trace.error = trace.reason.empty() ? ("playability " + trace.status)
                                           : trace.reason;
        return false;
    }
    if (!pick_format(root, out, trace))
        return false;
    trace.itag = out.itag;
    trace.bitrate = out.bitrate;
    trace.hls = out.hls;
    trace.url_prefix = url_prefix(out.url);
    return true;
}

void record(const ResolveTrace &trace)
{
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    g_traces.insert(g_traces.begin(), trace);
    if (g_traces.size() > kTraceMax)
        g_traces.resize(kTraceMax);
}
} // namespace

StreamResult resolve_audio(const std::string &video_id)
{
    StreamResult out;
    out.trace.video_id = video_id;
    if (video_id.size() != 11)
    {
        out.error = "bad video id";
        out.trace.error = out.error;
        record(out.trace);
        return out;
    }
    if (!http_available())
    {
        out.error = "native backend needs TLS (OpenSSL)";
        out.trace.error = out.error;
        record(out.trace);
        return out;
    }

    // One retry with a fresh session: stale visitor cookies are the usual
    // reason a player call starts returning LOGIN_REQUIRED.
    if (!attempt(video_id, out, false))
    {
        StreamResult retry;
        retry.trace = out.trace;
        if (attempt(video_id, retry, true))
        {
            retry.trace.session_refreshed = true;
            record(retry.trace);
            return retry;
        }
        out.error = retry.trace.error;
        out.trace = retry.trace;
    }
    if (!out.ok)
        out.error = out.trace.error;
    record(out.trace);
    return out;
}

SessionInfo session_info()
{
    std::lock_guard<std::mutex> lock(g_session_mutex);
    SessionInfo info;
    info.valid = session_fresh(g_session);
    info.age_seconds = age_seconds(g_session);
    info.visitor_prefix = g_session.visitor.substr(0, std::min<std::size_t>(8, g_session.visitor.size()));
    int count = 0;
    std::size_t pos = 0;
    while (pos < g_session.cookies.size())
    {
        const std::size_t end = g_session.cookies.find("; ", pos);
        ++count;
        if (end == std::string::npos)
            break;
        pos = end + 2;
    }
    info.cookies = count;
    return info;
}

void reset_session()
{
    std::lock_guard<std::mutex> lock(g_session_mutex);
    g_session = Session{};
}

std::vector<ResolveTrace> recent_traces()
{
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    return g_traces;
}
} // namespace native
