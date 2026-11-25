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

std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                out += b;
            }
            else
                out += static_cast<char>(c);
        }
    }
    return out;
}

std::string trim_str(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r' || s[a] == 0xC2))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r'))
        --b;
    // A leading UTF-8 non-breaking space (C2 A0) survives the byte checks above.
    if (a + 1 < b && (unsigned char)s[a] == 0xC2 && (unsigned char)s[a + 1] == 0xA0)
        a += 2;
    return s.substr(a, b - a);
}

int parse_duration(const std::string &s)
{
    int parts[3] = {0, 0, 0};
    int n = 0, value = 0;
    bool any = false;
    for (char c : s)
    {
        if (c >= '0' && c <= '9')
        {
            value = value * 10 + (c - '0');
            any = true;
        }
        else if (c == ':')
        {
            if (n < 2)
                parts[n++] = value;
            value = 0;
        }
    }
    if (!any)
        return 0;
    if (n == 0)
        return value;
    if (n == 1)
        return parts[0] * 60 + value;
    return parts[0] * 3600 + parts[1] * 60 + value;
}

// Manifest text of a runs array ("a", " • ", "b" -> "a • b").
std::string runs_text(const jsonv::Value &runs)
{
    std::string out;
    if (!runs.is_array())
        return out;
    for (std::size_t i = 0; i < runs.size(); ++i)
        out += runs.at(i).str("text");
    return out;
}

// Split a subtitle's runs into segments at the " • " separators.
std::vector<std::string> split_runs(const jsonv::Value &runs)
{
    std::vector<std::string> out(1);
    if (!runs.is_array())
        return out;
    for (std::size_t i = 0; i < runs.size(); ++i)
    {
        const std::string t = runs.at(i).str("text");
        if (t.find("\xE2\x80\xA2") != std::string::npos) // UTF-8 bullet
        {
            out.push_back(std::string());
            continue;
        }
        out.back() += t;
    }
    for (std::string &s : out)
        s = trim_str(s);
    return out;
}

void collect_rows(const jsonv::Value &node, std::vector<const jsonv::Value *> &out,
                  int depth)
{
    if (depth > 64)
        return;
    if (node.is_object())
    {
        const jsonv::Value &r = node["musicResponsiveListItemRenderer"];
        if (!r.is_null())
            out.push_back(&r);
        for (const auto &kv : node.members())
            collect_rows(kv.second, out, depth + 1);
    }
    else if (node.is_array())
    {
        for (std::size_t i = 0; i < node.size(); ++i)
            collect_rows(node.at(i), out, depth + 1);
    }
}

std::string row_title(const jsonv::Value &r)
{
    const jsonv::Value &runs =
        r["flexColumns"].at(0)["musicResponsiveListItemFlexColumnRenderer"]["text"]["runs"];
    return trim_str(runs_text(runs));
}

std::vector<std::string> row_subtitle(const jsonv::Value &r)
{
    const jsonv::Value &runs =
        r["flexColumns"].at(1)["musicResponsiveListItemFlexColumnRenderer"]["text"]["runs"];
    return split_runs(runs);
}

std::string row_thumb(const jsonv::Value &r)
{
    const jsonv::Value &th =
        r["thumbnail"]["musicThumbnailRenderer"]["thumbnail"]["thumbnails"];
    std::string best;
    if (th.is_array())
        for (std::size_t i = 0; i < th.size(); ++i)
        {
            const std::string u = th.at(i).str("url");
            if (!u.empty())
                best = u;
        }
    return best;
}

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

bool player_call(const std::string &video_id, bool refresh, jsonv::Value &root,
                 ResolveTrace &trace)
{
    trace.video_id = video_id;
    trace.epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
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

    std::string parse_error;
    if (!jsonv::Value::parse(res.body, root, parse_error))
    {
        trace.error = "player json: " + parse_error;
        return false;
    }
    trace.status = root["playabilityStatus"].str("status");
    trace.reason = root["playabilityStatus"].str("reason");
    return true;
}

bool attempt(const std::string &video_id, StreamResult &out, bool refresh)
{
    ResolveTrace &trace = out.trace;
    jsonv::Value root;
    if (!player_call(video_id, refresh, root, trace))
        return false;
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

bool search_root(const std::string &query, const std::string &params,
                 jsonv::Value &root, ResolveTrace &trace, std::string &error)
{
    if (query.empty())
    {
        error = "Missing query";
        return false;
    }
    if (!http_available())
    {
        error = "native backend needs TLS (OpenSSL)";
        return false;
    }
    trace.client = "WEB_REMIX";
    trace.epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto start = std::chrono::steady_clock::now();
    if (!session_fresh(g_session) && !refresh_session(trace))
    {
        error = trace.error;
        record(trace);
        return false;
    }
    trace.session_hit = true;
    std::string cookies, visitor;
    {
        std::lock_guard<std::mutex> lock(g_session_mutex);
        cookies = g_session.cookies;
        visitor = g_session.visitor;
    }
    std::string body;
    body += "{\"context\":{\"client\":{";
    body += "\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20241104.01.00\",";
    body += "\"hl\":\"en\",\"gl\":\"US\",\"visitorData\":\"" + visitor + "\"";
    body += "}},\"query\":\"" + json_escape(query) + "\"";
    if (!params.empty())
        body += ",\"params\":\"" + params + "\"";
    body += "}";
    std::vector<HttpHeader> headers = {
        {"User-Agent", kPageUa},
        {"X-YouTube-Client-Name", "67"},
        {"X-YouTube-Client-Version", "1.20241104.01.00"},
        {"X-Goog-Visitor-Id", visitor},
        {"Origin", "https://music.youtube.com"},
        {"Referer", "https://music.youtube.com/"},
        {"Accept-Language", "en"},
    };
    const HttpResult res = http_post_json(
        "https://music.youtube.com/youtubei/v1/search?prettyPrint=false", body, headers,
        cookies);
    trace.player_ms = res.ms;
    trace.player_bytes = res.bytes;
    if (!res.ok)
    {
        trace.error = "search: " + res.error;
        error = trace.error;
        record(trace);
        return false;
    }
    std::string parse_error;
    if (!jsonv::Value::parse(res.body, root, parse_error))
    {
        trace.error = "search json: " + parse_error;
        error = trace.error;
        record(trace);
        return false;
    }
    trace.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    return true;
}

bool search(const std::string &query, int limit, std::string &json,
            std::string &error)
{
    jsonv::Value root;
    ResolveTrace trace;
    if (!search_root(query, "EgWKAQIIAWoMEA4QChADEAQQCRAF", root, trace, error))
        return false;
    std::vector<const jsonv::Value *> rows;
    collect_rows(root, rows, 0);

    std::string out = "[";
    int found = 0;
    std::vector<std::string> seen;
    const int cap = limit > 0 ? std::min(limit, 50) : 20;
    for (const jsonv::Value *row : rows)
    {
        if (found >= cap)
            break;
        const std::string id = trim_str(row->get("playlistItemData").str("videoId"));
        if (id.size() != 11 || std::find(seen.begin(), seen.end(), id) != seen.end())
            continue;
        const std::string title = row_title(*row);
        if (title.empty())
            continue;
        const std::vector<std::string> sub = row_subtitle(*row);
        const std::string artist = sub.empty() ? std::string() : sub[0];
        const std::string album = sub.size() > 1 ? sub[1] : std::string();
        const int duration = sub.size() > 2 ? parse_duration(sub[2]) : 0;
        std::string thumb = row_thumb(*row);
        if (thumb.empty())
            thumb = "https://i.ytimg.com/vi/" + id + "/hqdefault.jpg";
        seen.push_back(id);
        if (found++)
            out += ",";
        out += "{\"id\":\"" + json_escape(id) + "\",\"title\":\"" + json_escape(title);
        out += "\",\"artist\":\"" + json_escape(artist) + "\",\"artist_id\":\"\"";
        out += ",\"album\":\"" + json_escape(album) + "\",\"album_id\":\"\"";
        out += ",\"track_number\":0,\"thumbnail_url\":\"" + json_escape(thumb) + "\"";
        out += ",\"duration_sec\":" + std::to_string(duration);
        out += ",\"url\":\"https://www.youtube.com/watch?v=" + id + "\"}";
    }
    out += "]";
    trace.status = "SEARCH";
    trace.formats = found;
    record(trace);
    if (found == 0)
    {
        error = "No results";
        return false;
    }
    json = out;
    return true;
}

bool search_albums(const std::string &query, int limit, std::string &json,
                   std::string &error)
{
    jsonv::Value root;
    ResolveTrace trace;
    if (!search_root(query, "EgWKAQIYAWoMEA4QChADEAQQCRAF", root, trace, error))
        return false;
    std::vector<const jsonv::Value *> rows;
    collect_rows(root, rows, 0);
    std::string out = "[";
    int found = 0;
    std::vector<std::string> seen;
    const int cap = limit > 0 ? std::min(limit, 50) : 20;
    for (const jsonv::Value *row : rows)
    {
        if (found >= cap)
            break;
        const std::string id =
            trim_str(row->get("navigationEndpoint")["browseEndpoint"].str("browseId"));
        const std::string title = row_title(*row);
        if (id.empty() || title.empty() ||
            std::find(seen.begin(), seen.end(), id) != seen.end())
            continue;
        const std::vector<std::string> sub = row_subtitle(*row);
        // "Album | Daft Punk | 2013"
        const std::string artist = sub.size() > 1 ? sub[1] : std::string();
        const std::string year = sub.size() > 2 ? sub[2] : std::string();
        seen.push_back(id);
        if (found++)
            out += ",";
        out += "{\"browse_id\":\"" + json_escape(id) + "\",\"title\":\"" +
               json_escape(title) + "\",\"artist\":\"" + json_escape(artist) +
               "\",\"year\":\"" + json_escape(year) + "\",\"thumbnail_url\":\"" +
               json_escape(row_thumb(*row)) + "\",\"track_count\":0}";
    }
    out += "]";
    trace.status = "SEARCH";
    trace.formats = found;
    record(trace);
    if (found == 0)
    {
        error = "No results";
        return false;
    }
    json = out;
    return true;
}

bool search_artists(const std::string &query, int limit, std::string &json,
                    std::string &error)
{
    jsonv::Value root;
    ResolveTrace trace;
    if (!search_root(query, "EgWKAQIgAWoMEA4QChADEAQQCRAF", root, trace, error))
        return false;
    std::vector<const jsonv::Value *> rows;
    collect_rows(root, rows, 0);
    std::string out = "[";
    int found = 0;
    std::vector<std::string> seen;
    const int cap = limit > 0 ? std::min(limit, 50) : 20;
    for (const jsonv::Value *row : rows)
    {
        if (found >= cap)
            break;
        const std::string id =
            trim_str(row->get("navigationEndpoint")["browseEndpoint"].str("browseId"));
        const std::string name = row_title(*row);
        if (id.empty() || name.empty() ||
            std::find(seen.begin(), seen.end(), id) != seen.end())
            continue;
        const std::vector<std::string> sub = row_subtitle(*row);
        // "Artist | 65M monthly audience"
        const std::string subscribers = sub.size() > 1 ? sub[1] : std::string();
        seen.push_back(id);
        if (found++)
            out += ",";
        out += "{\"browse_id\":\"" + json_escape(id) + "\",\"name\":\"" +
               json_escape(name) + "\",\"thumbnail_url\":\"" +
               json_escape(row_thumb(*row)) + "\",\"subscribers\":\"" +
               json_escape(subscribers) + "\"}";
    }
    out += "]";
    trace.status = "SEARCH";
    trace.formats = found;
    record(trace);
    if (found == 0)
    {
        error = "No results";
        return false;
    }
    json = out;
    return true;
}

bool fetch_meta(const std::string &video_id, std::string &json, std::string &error)
{
    if (video_id.size() != 11)
    {
        error = "Invalid video id";
        return false;
    }
    if (!http_available())
    {
        error = "native backend needs TLS (OpenSSL)";
        return false;
    }
    ResolveTrace trace;
    jsonv::Value root;
    if (!player_call(video_id, false, root, trace))
    {
        error = trace.error;
        record(trace);
        return false;
    }
    const jsonv::Value &vd = root["videoDetails"];
    if (vd.is_null())
    {
        error = "Video not found";
        record(trace);
        return false;
    }
    const std::string title = vd.str("title");
    if (title.empty())
    {
        error = "Video not found";
        record(trace);
        return false;
    }
    if (vd.boolean("isLive"))
    {
        error = "Live streams are not supported";
        record(trace);
        return false;
    }
    const std::string author = vd.str("author");
    const long long length = vd["lengthSeconds"].as_int();
    std::string thumb;
    const jsonv::Value &thumbs = vd["thumbnail"]["thumbnails"];
    if (thumbs.is_array())
        for (std::size_t i = 0; i < thumbs.size(); ++i)
        {
            const std::string u = thumbs.at(i).str("url");
            if (!u.empty())
                thumb = u;
        }
    if (thumb.empty())
        thumb = "https://i.ytimg.com/vi/" + video_id + "/hqdefault.jpg";

    json = "{\"ok\":true,\"id\":\"" + json_escape(video_id) +
           "\",\"title\":\"" + json_escape(title) +
           "\",\"artist\":\"" + json_escape(author) +
           "\",\"durationSec\":" + std::to_string(length) +
           ",\"videoType\":\"" + json_escape(vd.str("musicVideoType")) +
           "\",\"thumbUrl\":\"" + json_escape(thumb) + "\"}";
    record(trace);
    return true;
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
