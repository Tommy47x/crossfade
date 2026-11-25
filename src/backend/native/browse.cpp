#include "browse.h"

#include "http.h"
#include "innertube.h"
#include "json_value.h"
#include "native_util.h"

#ifdef P100_HAVE_OPENSSL
#include <openssl/sha.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

// ----------------------------------------------------------------------
// YouTube Music browse/search/suggestion commands, without Python.
//
// The public music.youtube.com endpoints (browse, next, search, suggestions)
// return nested renderer trees. These helpers walk for the renderer they need
// instead of hard-coding a path: YouTube reshuffles shelf nesting regularly,
// and a fixed path then silently parses nothing.
//
// Auth is optional. When browser.json exists (the same file the Python
// backend imports), its cookies are sent and a fresh SAPISIDHASH is computed;
// without it the same endpoints still serve public content anonymously.
// ----------------------------------------------------------------------

namespace native
{
namespace
{
using util::escape;
using util::looks_duration;
using util::parse_duration;
using util::Part;
using util::Row;
using util::runs_parts;
using util::runs_text;
using util::trim;

constexpr const char *kMusicClientVersion = "1.20241104.01.00";
constexpr const char *kMusicOrigin = "https://music.youtube.com";

std::string config_dir()
{
    if (const char *env = std::getenv("P100_CONFIG_DIR"))
        if (*env)
            return env;
#ifdef _WIN32
    const char *appdata = std::getenv("APPDATA");
    return std::string(appdata ? appdata : ".") + "/crossfade";
#else
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
        if (*xdg)
            return std::string(xdg) + "/crossfade";
    const char *home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/crossfade";
#endif
}

bool read_file(const std::string &path, std::string &out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

std::string sha1_hex(const std::string &text)
{
#ifdef P100_HAVE_OPENSSL
    unsigned char md[20];
    SHA1(reinterpret_cast<const unsigned char *>(text.data()), text.size(), md);
    char out[41];
    for (int i = 0; i < 20; ++i)
        std::snprintf(out + i * 2, 3, "%02x", md[i]);
    out[40] = 0;
    return out;
#else
    (void)text;
    return {};
#endif
}

std::string cookie_value(const std::string &cookies, const std::string &name)
{
    std::size_t pos = 0;
    while (pos < cookies.size())
    {
        const std::size_t end = cookies.find(';', pos);
        std::string pair = cookies.substr(pos, end == std::string::npos ? std::string::npos
                                                                        : end - pos);
        pair = trim(pair);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == name)
            return pair.substr(eq + 1);
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return {};
}

// browser.json: the header map the Python backend uses. The stored
// authorization expires (its timestamp is part of the hash), so recompute it
// from the SAPISID cookies when possible and keep the file's value as a
// fallback for builds without OpenSSL.
struct WebAuth
{
    std::string cookie;
    std::string authorization;
    std::string user_agent;
    std::string authuser = "0";
    std::string language = "en-US,en;q=0.9";
    bool loaded = false;
};

std::string sapisid_hash(const WebAuth &auth)
{
    const std::string sapisid = cookie_value(auth.cookie, "SAPISID");
    if (sapisid.empty())
        return {};
    const long long ts = (long long)std::time(nullptr);
    std::string out = "SAPISIDHASH " + std::to_string(ts) + "_" +
                      sha1_hex(std::to_string(ts) + " " + sapisid + " " + kMusicOrigin);
    for (const auto &pair : {std::make_pair("__Secure-1PAPISID", "SAPISID1PHASH"),
                             std::make_pair("__Secure-3PAPISID", "SAPISID3PHASH")})
    {
        const std::string value = cookie_value(auth.cookie, pair.first);
        if (value.empty())
            continue;
        out += " " + std::string(pair.second) + " " + std::to_string(ts) + "_" +
               sha1_hex(std::to_string(ts) + " " + value + " " + kMusicOrigin);
    }
    return out;
}

WebAuth load_auth()
{
    WebAuth auth;
    std::string text;
    if (!read_file(config_dir() + "/browser.json", text))
        return auth;
    jsonv::Value root;
    std::string parse_error;
    if (!jsonv::Value::parse(text, root, parse_error) || !root.is_object())
        return auth;
    auth.cookie = root.str("cookie");
    auth.authorization = root.str("authorization");
    auth.user_agent = root.str("user-agent");
    const std::string authuser = root.str("x-goog-authuser");
    if (!authuser.empty())
        auth.authuser = authuser;
    const std::string language = root.str("accept-language");
    if (!language.empty())
        auth.language = language;
    auth.loaded = !auth.cookie.empty();
    const std::string fresh = sapisid_hash(auth);
    if (!fresh.empty())
        auth.authorization = fresh;
    return auth;
}

std::string web_context(const std::string &visitor)
{
    std::string ctx = "\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
                      "\"clientVersion\":\"";
    ctx += kMusicClientVersion;
    ctx += "\",\"hl\":\"en\",\"gl\":\"US\"";
    if (!visitor.empty())
        ctx += ",\"visitorData\":\"" + escape(visitor) + "\"";
    ctx += "}}";
    return ctx;
}

struct WebReply
{
    jsonv::Value root;
    ResolveTrace trace;
};

// POST one youtubei/v1/<endpoint> call. `fields` is the body without the
// context, e.g. "\"browseId\":\"MPREb_x\"".
bool web_call(const std::string &endpoint, const std::string &fields, WebReply &reply,
             std::string &error)
{
    if (!http_available())
    {
        error = "native backend needs TLS (OpenSSL)";
        return false;
    }
    reply.trace = ResolveTrace{};
    reply.trace.client = "WEB_REMIX";
    reply.trace.epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const auto start = std::chrono::steady_clock::now();

    // Browse/search work anonymously (and do not need VISITOR_DATA), so use
    // an already-open watch-page session when there is one and never open a
    // new one just for a browse request: that would add ~1 s to the first
    // search/album click after launch.
    std::string session_cookies, visitor;
    reply.trace.session_hit = peek_session(session_cookies, visitor);
    const WebAuth auth = load_auth();
    std::string cookie_jar = auth.cookie.empty() ? session_cookies : auth.cookie;

    std::vector<HttpHeader> headers = {
        {"User-Agent", auth.user_agent.empty() ? web_user_agent() : auth.user_agent},
        {"X-YouTube-Client-Name", "67"},
        {"X-YouTube-Client-Version", kMusicClientVersion},
        {"Origin", kMusicOrigin},
        {"Referer", std::string(kMusicOrigin) + "/"},
        {"Accept", "*/*"},
        {"Accept-Language", auth.language},
    };
    if (!auth.authorization.empty())
        headers.push_back({"Authorization", auth.authorization});
    if (!auth.authuser.empty())
        headers.push_back({"X-Goog-AuthUser", auth.authuser});
    if (!visitor.empty())
        headers.push_back({"X-Goog-Visitor-Id", visitor});

    const std::string body = "{" + web_context(visitor) +
                             (fields.empty() ? "" : "," + fields) + "}";
    const HttpResult res = http_post_json(
        std::string("https://music.youtube.com/youtubei/v1/") + endpoint +
            "?prettyPrint=false",
        body, headers, cookie_jar);
    reply.trace.player_ms = res.ms;
    reply.trace.player_bytes = res.bytes;
    if (!res.ok)
    {
        reply.trace.error = endpoint + ": " + (res.error.empty() ? std::to_string(res.status)
                                                                : res.error);
        error = reply.trace.error;
        record_trace(reply.trace);
        return false;
    }
    std::string parse_error;
    if (!jsonv::Value::parse(res.body, reply.root, parse_error))
    {
        reply.trace.error = endpoint + " json: " + parse_error;
        error = reply.trace.error;
        record_trace(reply.trace);
        return false;
    }
    reply.trace.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    return true;
}

void finish_trace(WebReply &reply, const std::string &status, int count)
{
    reply.trace.status = status;
    reply.trace.formats = count;
    record_trace(reply.trace);
}

const jsonv::Value *find_renderer(const jsonv::Value &root, const char *key)
{
    std::vector<const jsonv::Value *> found;
    util::collect_renderer(root, key, found);
    return found.empty() ? nullptr : found.front();
}

void collect_all(const jsonv::Value &root, const char *key,
                 std::vector<const jsonv::Value *> &out)
{
    util::collect_renderer(root, key, out);
}

std::string find_string(const jsonv::Value &value, const char *key, int depth = 0)
{
    if (depth > 32)
        return {};
    if (value.is_object())
    {
        const jsonv::Value &v = value[key];
        if (v.is_string())
            return v.as_string();
        for (const auto &kv : value.members())
        {
            const std::string r = find_string(kv.second, key, depth + 1);
            if (!r.empty())
                return r;
        }
    }
    else if (value.is_array())
    {
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            const std::string r = find_string(value.at(i), key, depth + 1);
            if (!r.empty())
                return r;
        }
    }
    return {};
}

std::string first_int_text(const std::vector<std::string> &parts, const char *word,
                           int *number = nullptr)
{
    for (const std::string &p : parts)
    {
        if (p.find(word) == std::string::npos)
            continue;
        int value = 0;
        for (char c : p)
        {
            if (c >= '0' && c <= '9')
                value = value * 10 + (c - '0');
            else if (value > 0)
                break;
        }
        if (number)
            *number = value;
        return p;
    }
    return {};
}

std::string field_text(const jsonv::Value &section, const char *field)
{
    return trim(runs_text(section.get(field).get("runs")));
}


} // namespace

std::string find_continuation(const jsonv::Value &root)
{
    // The track shelf's "load more" lives in a continuationItemRenderer at
    // the end of the shelf; section-level continuations usually point at
    // unrelated carousels, so prefer the item token.
    std::string item_token;
    std::string section_token;
    std::function<void(const jsonv::Value &)> walk = [&](const jsonv::Value &v) {
        if (v.is_object())
        {
            if (item_token.empty())
            {
                const jsonv::Value &item = v["continuationItemRenderer"];
                if (item.is_object())
                    item_token = item["continuationEndpoint"]["continuationCommand"]
                                     .str("token");
                const jsonv::Value &cmd = v["continuationCommand"];
                if (cmd.is_object() && item_token.empty())
                    item_token = cmd.str("token");
            }
            if (section_token.empty())
            {
                const jsonv::Value &c = v["continuation"];
                if (c.is_string())
                    section_token = c.as_string();
            }
            for (const auto &kv : v.members())
                walk(kv.second);
        }
        else if (v.is_array())
            for (std::size_t i = 0; i < v.size(); ++i)
                walk(v.at(i));
    };
    walk(root);
    return !item_token.empty() ? item_token : section_token;
}

void append_shelf_rows(const jsonv::Value &root, std::vector<Row> &rows,
                       std::vector<std::string> &seen, std::size_t cap)
{
    std::vector<const jsonv::Value *> shelves;
    collect_all(root, "musicShelfRenderer", shelves);
    collect_all(root, "musicPlaylistShelfRenderer", shelves);
    for (const jsonv::Value *shelf : shelves)
    {
        std::vector<const jsonv::Value *> renderers;
        util::collect_renderer(*shelf, "musicResponsiveListItemRenderer", renderers);
        for (const jsonv::Value *r : renderers)
        {
            if (rows.size() >= cap)
                return;
            Row t;
            if (!util::parse_row(*r, t))
                continue;
            if (std::find(seen.begin(), seen.end(), t.id) != seen.end())
                continue;
            seen.push_back(t.id);
            rows.push_back(std::move(t));
        }
    }
}

// First browse page plus every continuation page, for the long shelves
// (playlists, an artist's full song list).
bool browse_all_rows(const std::string &browse_id, std::size_t cap,
                     std::vector<Row> &rows, WebReply &reply, std::string &error)
{
    if (!web_call("browse", "\"browseId\":\"" + escape(browse_id) + "\"", reply, error))
        return false;
    std::vector<std::string> seen;
    append_shelf_rows(reply.root, rows, seen, cap);
    std::string continuation = find_continuation(reply.root);
    int pages = 0;
    while (!continuation.empty() && rows.size() < cap && pages < 20)
    {
        WebReply more;
        std::string more_error;
        if (!web_call("browse",
                      "\"browseId\":\"" + escape(browse_id) +
                          "\",\"continuation\":\"" + escape(continuation) + "\"",
                      more, more_error))
            break;
        // Continuation replies use musicPlaylistShelfContinuation (or another
        // *Continuation renderer), not the shelf renderer of the first page.
        std::vector<const jsonv::Value *> renderers;
        util::collect_renderer(more.root, "musicResponsiveListItemRenderer", renderers);
        for (const jsonv::Value *r : renderers)
        {
            if (rows.size() >= cap)
                break;
            Row t;
            if (!util::parse_row(*r, t))
                continue;
            if (std::find(seen.begin(), seen.end(), t.id) != seen.end())
                continue;
            seen.push_back(t.id);
            rows.push_back(std::move(t));
        }
        const std::string next = find_continuation(more.root);
        if (next == continuation)
            break;
        continuation = next;
        ++pages;
    }
    return true;
}

// ---------------------------------------------------------------- suggestions

bool suggest(const std::string &query, std::string &json, std::string &error)
{
    WebReply reply;
    if (!web_call("music/get_search_suggestions",
                  "\"input\":\"" + escape(query) + "\"", reply, error))
        return false;

    std::string queries = "[";
    std::string items = "[";
    bool first_q = true, first_i = true;
    const jsonv::Value &sections = reply.root["contents"];
    for (std::size_t s = 0; s < sections.size(); ++s)
    {
        const jsonv::Value &contents =
            sections.at(s)["searchSuggestionsSectionRenderer"]["contents"];
        for (std::size_t i = 0; i < contents.size(); ++i)
        {
            const jsonv::Value &entry = contents.at(i);
            const jsonv::Value &sugg = entry["searchSuggestionRenderer"];
            const jsonv::Value &hist = entry["historySuggestionRenderer"];
            const jsonv::Value *renderer = !sugg.is_null() ? &sugg : (!hist.is_null() ? &hist : nullptr);
            if (renderer)
            {
                const std::string text =
                    trim(runs_text((*renderer)["suggestion"]["runs"]));
                if (text.empty())
                    continue;
                if (!first_q)
                    queries += ",";
                first_q = false;
                queries += "{\"text\":\"" + escape(text) + "\",\"history\":" +
                           (hist.is_null() ? "false" : "true") + "}";
                continue;
            }
            const jsonv::Value &row = entry["musicResponsiveListItemRenderer"];
            if (row.is_null())
                continue;
            Row t;
            bool parsed = util::parse_row(row, t);
            const std::string row_title = trim(runs_text(
                row["flexColumns"].at(0)["musicResponsiveListItemFlexColumnRenderer"]["text"]
                    ["runs"]));
            const std::vector<Part> parts = runs_parts(
                row["flexColumns"].at(1)["musicResponsiveListItemFlexColumnRenderer"]["text"]
                    ["runs"]);
            const std::string browse =
                trim(row["navigationEndpoint"]["browseEndpoint"].str("browseId"));
            if (!parsed)
            {
                // Artist/album suggestions have a browse id but no video id.
                if (row_title.empty() || browse.empty())
                    continue;
                t.title = row_title;
                t.thumb = util::renderer_thumb(row);
                for (const Part &p : parts)
                {
                    if (p.browse_id.rfind("UC", 0) == 0 && t.artist.empty())
                    {
                        t.artist = p.browse_id == browse ? p.text : p.text;
                        t.artist_id = p.browse_id;
                    }
                }
                if (browse.rfind("UC", 0) == 0)
                    t.kind = "Artist";
                else if (browse.rfind("MPREb", 0) == 0)
                    t.kind = "Album";
                parsed = true;
            }
            if (!parsed)
                continue;
            std::string id = t.id;
            std::string kind = t.kind;
            if (!browse.empty())
            {
                id = browse;
                if (browse.rfind("MPREb", 0) == 0)
                    kind = "album";
                else if (browse.rfind("UC", 0) == 0)
                    kind = "artist";
            }
            std::string lower = kind;
            for (char &c : lower)
                c = (char)std::tolower((unsigned char)c);
            if (lower.empty())
                lower = "song";
            std::string subtitle;
            if (!t.artist.empty())
                subtitle = t.artist;
            if (!t.album.empty())
                subtitle += (subtitle.empty() ? "" : " \xC2\xB7 ") + t.album;
            if (!first_i)
                items += ",";
            first_i = false;
            items += "{\"kind\":\"" + escape(lower) + "\",\"id\":\"" + escape(id) +
                     "\",\"title\":\"" + escape(t.title) + "\",\"artist\":\"" +
                     escape(t.artist) + "\",\"album\":\"" + escape(t.album) +
                     "\",\"subtitle\":\"" + escape(subtitle) +
                     "\",\"thumbnail_url\":\"" + escape(t.thumb) + "\"}";
        }
    }
    queries += "]";
    items += "]";
    json = "{\"queries\":" + queries + ",\"items\":" + items + "}";
    finish_trace(reply, "SUGGEST", (int)(first_q && first_i ? 0 : 1));
    return true;
}

// ---------------------------------------------------------------------- album

std::string header_title(const jsonv::Value &header)
{
    return trim(runs_text(header["title"]["runs"]));
}

bool album(const std::string &browse_id, std::string &json, std::string &error)
{
    if (browse_id.empty())
    {
        error = "Missing album id";
        return false;
    }
    WebReply reply;
    std::vector<Row> rows;
    if (!browse_all_rows(browse_id, 2000, rows, reply, error))
        return false;

    const jsonv::Value *header = find_renderer(reply.root, "musicResponsiveHeaderRenderer");
    if (!header)
    {
        error = "Album not found";
        return false;
    }
    const std::string title = header_title(*header);
    if (title.empty())
    {
        error = "Album not found";
        return false;
    }
    const std::string artist = field_text(*header, "straplineTextOne");
    // Individual album rows often omit the artist entirely; ytmusicapi fills
    // it from the header, and so must we (otherwise the queue and the lyrics
    // panel show "Unknown artist").
    const std::string artist_id = trim(
        (*header)["straplineTextOne"]["runs"].at(0)["navigationEndpoint"]
            ["browseEndpoint"].str("browseId"));
    std::string year;
    {
        const std::vector<Part> parts = runs_parts((*header)["subtitle"]["runs"]);
        for (const Part &p : parts)
            if (p.text.size() == 4 &&
                std::all_of(p.text.begin(), p.text.end(),
                            [](char c) { return c >= '0' && c <= '9'; }))
                year = p.text;
    }
    const std::vector<Part> second = runs_parts((*header)["secondSubtitle"]["runs"]);
    int track_count = 0;
    std::string duration;
    for (const Part &p : second)
    {
        if (p.text.find("track") != std::string::npos ||
            p.text.find("song") != std::string::npos)
        {
            std::vector<std::string> one{p.text};
            first_int_text(one, "track", &track_count);
            if (track_count == 0)
                first_int_text(one, "song", &track_count);
        }
        else if (p.text.find("hour") != std::string::npos ||
                 p.text.find("minute") != std::string::npos)
            duration = p.text;
    }
    std::string description;
    if (const jsonv::Value *desc = find_renderer(reply.root, "musicDescriptionShelfRenderer"))
        description = trim(runs_text((*desc)["description"]["runs"]));
    if (description.empty())
        description = trim(runs_text((*header)["description"]["runs"]));
    const std::string thumb = util::renderer_thumb(*header);
    const std::string audio_playlist_id = find_string((*header)["buttons"], "playlistId");

    if (rows.empty())
    {
        finish_trace(reply, "BROWSE", 0);
        error = "Album has no playable tracks.";
        return false;
    }

    std::string tracks = "[";
    bool first = true;
    int number = 0;
    for (const Row &row : rows)
    {
        if (!first)
            tracks += ",";
        first = false;
        Row t = row;
        if (t.artist.empty())
        {
            t.artist = artist;
            t.artist_id = artist_id;
        }
        if (t.track_number == 0)
            t.track_number = ++number;
        else
            number = t.track_number;
        tracks += util::track_json(t, title, browse_id);
    }
    tracks += "]";
    if (track_count == 0)
        track_count = (int)rows.size();
    json = "{\"browse_id\":\"" + escape(browse_id) + "\",\"title\":\"" + escape(title) +
           "\",\"artist\":\"" + escape(artist) + "\",\"year\":\"" + escape(year) +
           "\",\"track_count\":" + std::to_string(track_count) + ",\"duration\":\"" +
           escape(duration) + "\",\"description\":\"" + escape(description) +
           "\",\"thumbnail_url\":\"" + escape(thumb) + "\",\"audio_playlist_id\":\"" +
           escape(audio_playlist_id) + "\",\"tracks\":" + tracks + "}";
    finish_trace(reply, "ALBUM", (int)rows.size());
    return true;
}

// --------------------------------------------------------------------- artist

struct TwoRow
{
    std::string browse_id;
    std::string title;
    std::string subtitle;
    std::string thumb;
};

TwoRow parse_two_row(const jsonv::Value &r)
{
    TwoRow out;
    out.browse_id = trim(r["navigationEndpoint"]["browseEndpoint"].str("browseId"));
    out.title = trim(runs_text(r["title"]["runs"]));
    out.subtitle = trim(runs_text(r["subtitle"]["runs"]));
    out.thumb = util::renderer_thumb(r);
    return out;
}

std::string section_title(const jsonv::Value &section)
{
    if (const jsonv::Value *sh = section["musicShelfRenderer"].is_null()
                                     ? nullptr
                                     : &section["musicShelfRenderer"])
        return trim(runs_text((*sh)["title"]["runs"]));
    return trim(runs_text(
        section["musicCarouselShelfRenderer"]["header"]["musicCarouselShelfBasicHeaderRenderer"]
            ["title"]["runs"]));
}

bool artist(const std::string &browse_id, std::string &json, std::string &error)
{
    if (browse_id.empty())
    {
        error = "Missing artist id";
        return false;
    }
    WebReply reply;
    if (!web_call("browse", "\"browseId\":\"" + escape(browse_id) + "\"", reply, error))
        return false;

    const jsonv::Value *header = find_renderer(reply.root, "musicImmersiveHeaderRenderer");
    if (!header)
        header = find_renderer(reply.root, "musicResponsiveHeaderRenderer");
    if (!header)
    {
        error = "Artist not found";
        return false;
    }
    const std::string name = header_title(*header);
    if (name.empty())
    {
        error = "Artist not found";
        return false;
    }
    std::string description = trim(runs_text((*header)["description"]["runs"]));
    std::string subscribers;
    if (const jsonv::Value *sub_button =
            find_renderer((*header)["subscriptionButton"], "subscribeButtonRenderer"))
        subscribers = trim(runs_text((*sub_button)["subscriberCountText"]["runs"]));
    std::string monthly = trim(runs_text((*header)["monthlyListenerCount"]["runs"]));
    const std::string thumb = util::renderer_thumb(*header);

    // The tab content holds the shelves; fall back to the whole reply.
    const jsonv::Value &tabs = reply.root["contents"]["singleColumnBrowseResultsRenderer"]
                                           ["tabs"].at(0)["tabRenderer"]["content"]
                                           ["sectionListRenderer"]["contents"];
    const jsonv::Value &sections = tabs.is_array() ? tabs : reply.root["contents"];

    std::vector<Row> songs;
    std::string songs_browse_id;
    std::vector<TwoRow> albums;
    for (std::size_t i = 0; i < sections.size(); ++i)
    {
        const jsonv::Value &section = sections.at(i);
        const std::string title = section_title(section);
        if (!section["musicShelfRenderer"].is_null() &&
            title.find("Top songs") != std::string::npos)
        {
            std::vector<const jsonv::Value *> renderers;
            util::collect_renderer(section, "musicResponsiveListItemRenderer", renderers);
            util::append_tracks(renderers, songs, 50);
            songs_browse_id = trim(section["musicShelfRenderer"]["title"]["runs"].at(0)
                                       ["navigationEndpoint"]["browseEndpoint"]
                                       .str("browseId"));
            continue;
        }
        if (section["musicCarouselShelfRenderer"].is_null())
            continue;
        const bool singles = title.find("Singles") != std::string::npos ||
                             title.find("single") != std::string::npos;
        const bool album_shelf = title.find("Albums") != std::string::npos ||
                                 title.find("albums") != std::string::npos;
        if (!singles && !album_shelf)
            continue;
        const jsonv::Value &items = section["musicCarouselShelfRenderer"]["contents"];
        for (std::size_t j = 0; j < items.size(); ++j)
        {
            const jsonv::Value &item = items.at(j)["musicTwoRowItemRenderer"];
            if (item.is_null())
                continue;
            TwoRow row = parse_two_row(item);
            if (row.browse_id.empty() || row.title.empty())
                continue;
            bool dup = false;
            for (const TwoRow &seen : albums)
                if (seen.browse_id == row.browse_id)
                {
                    dup = true;
                    break;
                }
            if (!dup)
                albums.push_back(std::move(row));
        }
    }
    if (description.empty())
        if (const jsonv::Value *desc = find_renderer(reply.root, "musicDescriptionShelfRenderer"))
            description = trim(runs_text((*desc)["description"]["runs"]));

    std::string songs_json = "[";
    for (std::size_t i = 0; i < songs.size(); ++i)
    {
        if (i)
            songs_json += ",";
        Row t = songs[i];
        if (t.artist_id.empty())
            t.artist_id = browse_id;
        if (t.artist.empty())
            t.artist = name;
        songs_json += util::track_json(t);
    }
    songs_json += "]";

    std::string albums_json = "[";
    for (std::size_t i = 0; i < albums.size(); ++i)
    {
        if (i)
            albums_json += ",";
        // Two-row album subtitles are usually just the year; the artist is
        // implied by the page. Mark the type from the shelf title.
        albums_json += "{\"browse_id\":\"" + escape(albums[i].browse_id) + "\",\"title\":\"" +
                       escape(albums[i].title) + "\",\"year\":\"" +
                       escape(albums[i].subtitle) + "\",\"thumbnail_url\":\"" +
                       escape(albums[i].thumb) + "\",\"type\":\"Album\"}";
    }
    albums_json += "]";

    json = "{\"browse_id\":\"" + escape(browse_id) + "\",\"name\":\"" + escape(name) +
           "\",\"description\":\"" + escape(description) + "\",\"subscribers\":\"" +
           escape(subscribers) + "\",\"monthly_listeners\":\"" + escape(monthly) +
           "\",\"thumbnail_url\":\"" + escape(thumb) + "\",\"songs\":" + songs_json +
           ",\"songs_browse_id\":\"" + escape(songs_browse_id) + "\",\"albums\":" +
           albums_json + "}";
    finish_trace(reply, "ARTIST", (int)songs.size());
    return true;
}

bool artist_songs(const std::string &browse_id, int offset, int count,
                  std::string &json, std::string &error)
{
    if (browse_id.empty())
    {
        error = "Missing songs browse id";
        return false;
    }
    std::string id = browse_id;
    if (id.rfind("VL", 0) != 0 && id.rfind("OLA", 0) != 0)
        id = "VL" + id;
    WebReply reply;
    std::vector<Row> rows;
    if (!browse_all_rows(id, 5000, rows, reply, error))
        return false;
    if (rows.empty())
    {
        finish_trace(reply, "BROWSE", 0);
        error = "No songs";
        return false;
    }
    const int from = std::max(0, offset);
    const int to = std::min<int>((int)rows.size(), from + std::max(0, count));
    std::string out = "[";
    for (int i = from; i < to; ++i)
    {
        if (i != from)
            out += ",";
        out += util::track_json(rows[(std::size_t)i]);
    }
    out += "]";
    json = out;
    finish_trace(reply, "BROWSE", to - from);
    return true;
}

// ------------------------------------------------------------------- playlist

bool playlist(const std::string &playlist_id, std::string &json, std::string &error)
{
    if (playlist_id.empty())
    {
        error = "Missing playlist_id";
        return false;
    }
    std::string browse_id = playlist_id;
    if (browse_id.rfind("VL", 0) != 0)
        browse_id = "VL" + browse_id;
    WebReply reply;
    std::vector<Row> rows;
    if (!browse_all_rows(browse_id, 2000, rows, reply, error))
        return false;

    const jsonv::Value *header = find_renderer(reply.root, "musicResponsiveHeaderRenderer");
    if (!header)
    {
        error = "Playlist not found";
        return false;
    }
    const std::string title = header_title(*header);
    if (title.empty())
    {
        error = "Playlist not found";
        return false;
    }
    const std::vector<Part> second = runs_parts((*header)["secondSubtitle"]["runs"]);
    int track_count = 0;
    std::string duration;
    for (const Part &p : second)
    {
        if (p.text.find("track") != std::string::npos)
        {
            std::vector<std::string> one{p.text};
            first_int_text(one, "track", &track_count);
        }
        else if (p.text.find("hour") != std::string::npos ||
                 p.text.find("minute") != std::string::npos)
            duration = p.text;
    }
    std::string description = trim(runs_text((*header)["description"]["runs"]));
    const std::string thumb = util::renderer_thumb(*header);
    std::string author = field_text(*header, "straplineTextOne");
    if (author.empty())
        author = "YouTube Music";
    std::string year;
    for (const Part &p : runs_parts((*header)["subtitle"]["runs"]))
        if (p.text.size() == 4 &&
            std::all_of(p.text.begin(), p.text.end(),
                        [](char c) { return c >= '0' && c <= '9'; }))
            year = p.text;

    if (rows.empty() && track_count > 0)
    {
        finish_trace(reply, "BROWSE", 0);
        error = "Could not read the playlist tracks";
        return false;
    }
    std::string tracks = "[";
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        if (i)
            tracks += ",";
        tracks += util::track_json(rows[i]);
    }
    tracks += "]";
    if (track_count == 0)
        track_count = (int)rows.size();
    json = "{\"id\":\"" + escape(playlist_id) + "\",\"title\":\"" + escape(title) +
           "\",\"privacy\":\"PUBLIC\",\"description\":\"" + escape(description) +
           "\",\"author\":{\"name\":\"" + escape(author) + "\"},\"year\":\"" +
           escape(year) + "\",\"duration\":\"" + escape(duration) +
           "\",\"trackCount\":" + std::to_string(track_count) +
           ",\"thumbnails\":[{\"url\":\"" + escape(thumb) + "\"}],\"tracks\":" +
           tracks + "}";
    finish_trace(reply, "PLAYLIST", (int)rows.size());
    return true;
}

// ---------------------------------------------------------------------- radio

bool parse_panel(const jsonv::Value &root, std::vector<Row> &rows, std::string &continuation,
                 std::size_t cap)
{
    const jsonv::Value *panel = find_renderer(root, "playlistPanelRenderer");
    const jsonv::Value *cont_panel = find_renderer(root, "playlistPanelContinuation");
    if (!panel && !cont_panel)
        return false;
    const jsonv::Value &contents = panel ? (*panel)["contents"] : (*cont_panel)["contents"];
    for (std::size_t i = 0; i < contents.size() && rows.size() < cap; ++i)
    {
        const jsonv::Value &r = contents.at(i)["playlistPanelVideoRenderer"];
        if (r.is_null())
            continue;
        Row t;
        t.id = r.str("videoId");
        t.title = trim(runs_text(r["title"]["runs"]));
        t.duration = parse_duration(trim(runs_text(r["lengthText"]["runs"])));
        t.thumb = util::thumb_from(r.get("thumbnail"));
        const std::vector<Part> byline = runs_parts(r["longBylineText"]["runs"]);
        if (!byline.empty())
        {
            t.artist = byline[0].text;
            t.artist_id = byline[0].browse_id;
        }
        if (t.id.size() != 11 || t.title.empty())
            continue;
        bool dup = false;
        for (const Row &seen : rows)
            if (seen.id == t.id)
            {
                dup = true;
                break;
            }
        if (!dup)
            rows.push_back(std::move(t));
    }
    continuation = find_string((panel ? (*panel) : (*cont_panel))["continuations"],
                               "continuation");
    return true;
}

bool radio(const std::string &video_id, int limit, std::string &json, std::string &error)
{
    if (video_id.size() != 11)
    {
        error = "Missing video_id";
        return false;
    }
    const int cap = limit > 0 ? std::min(limit, 100) : 50;
    WebReply reply;
    std::string fields = "\"videoId\":\"" + escape(video_id) +
                         "\",\"playlistId\":\"RDAMVM" + escape(video_id) +
                         "\",\"params\":\"wAEB\",\"enablePersistentPlaylistPanel\":true,"
                         "\"isAudioOnly\":true,\"tunerSettingValue\":\"AUTOMIX_SETTING_NORMAL\"";
    if (!web_call("next", fields, reply, error))
        return false;
    std::vector<Row> rows;
    std::string continuation;
    if (!parse_panel(reply.root, rows, continuation, (std::size_t)cap) || rows.empty())
    {
        finish_trace(reply, "RADIO", 0);
        error = "Radio returned no playable tracks.";
        return false;
    }
    // One continuation request covers the rest of a 50-track station.
    if ((int)rows.size() < cap && !continuation.empty())
    {
        WebReply more;
        std::string more_error;
        if (web_call("next",
                     "\"continuation\":\"" + escape(continuation) +
                         "\",\"enablePersistentPlaylistPanel\":true",
                     more, more_error))
        {
            std::string ignored;
            parse_panel(more.root, rows, ignored, (std::size_t)cap);
        }
    }
    json = util::row_array(rows);
    finish_trace(reply, "RADIO", (int)rows.size());
    return true;
}

// ----------------------------------------------------------------------- home

std::string collect_home_rows(const jsonv::Value &root, std::size_t cap,
                              std::vector<Row> &rows)
{
    std::vector<const jsonv::Value *> renderers;
    collect_all(root, "musicResponsiveListItemRenderer", renderers);
    util::append_tracks(renderers, rows, cap);
    // Quick picks / Listen again / community cards carry the song id on the
    // card's watchEndpoint. Album cards have a browseEndpoint instead.
    std::vector<const jsonv::Value *> cards;
    collect_all(root, "musicTwoRowItemRenderer", cards);
    for (const jsonv::Value *card : cards)
    {
        if (rows.size() >= cap)
            break;
        Row t;
        t.id = (*card)["navigationEndpoint"]["watchEndpoint"].str("videoId");
        if (t.id.size() != 11)
            continue;
        bool dup = false;
        for (const Row &seen : rows)
            if (seen.id == t.id)
            {
                dup = true;
                break;
            }
        if (dup)
            continue;
        t.title = trim(runs_text((*card)["title"]["runs"]));
        if (t.title.empty())
            continue;
        const std::vector<Part> parts = runs_parts((*card)["subtitle"]["runs"]);
        for (const Part &p : parts)
        {
            if (p.browse_id.rfind("UC", 0) == 0 && t.artist.empty())
            {
                t.artist = p.text;
                t.artist_id = p.browse_id;
            }
            else if (t.artist.empty() && !util::is_noise(p.text) &&
                     p.text != "Song" && p.text != "Video")
                t.artist = p.text;
        }
        t.thumb = util::renderer_thumb(*card);
        rows.push_back(std::move(t));
    }
    return find_continuation(root);
}

std::string home_payload(const std::vector<Row> &rows, const std::string &continuation)
{
    return "{\"tracks\":" + util::row_array(rows) + ",\"continuation\":\"" +
           escape(continuation) + "\"}";
}

bool home(int limit, std::string &json, std::string &error)
{
    const int cap = limit > 0 ? std::min(limit, 100) : 40;
    WebReply reply;
    if (!web_call("browse", "\"browseId\":\"FEmusic_home\"", reply, error))
        return false;
    std::vector<Row> rows;
    const std::string continuation =
        collect_home_rows(reply.root, (std::size_t)cap, rows);
    if (rows.empty())
    {
        finish_trace(reply, "HOME", 0);
        error = "No individual tracks found in the home feed.";
        return false;
    }
    json = home_payload(rows, continuation);
    finish_trace(reply, "HOME", (int)rows.size());
    return true;
}

// Next page of the personalized Home feed. YouTube sometimes answers with a
// shelf that has no playable songs (shows, playlists); walk a few pages until
// there is something to append or the token chain ends.
bool home_more(const std::string &continuation, int limit, std::string &json,
               std::string &error)
{
    if (continuation.empty())
    {
        error = "No home continuation";
        return false;
    }
    const int cap = limit > 0 ? std::min(limit, 100) : 40;
    std::vector<Row> rows;
    std::string token = continuation;
    WebReply last;
    bool any_reply = false;
    for (int page = 0; page < 4; ++page)
    {
        WebReply reply;
        std::string page_error;
        if (!web_call("browse", "\"continuation\":\"" + escape(token) + "\"", reply,
                      page_error))
        {
            if (!any_reply)
            {
                error = page_error;
                return false;
            }
            break;
        }
        any_reply = true;
        last = reply;
        const std::string next = collect_home_rows(reply.root, (std::size_t)cap, rows);
        if (rows.size() >= (std::size_t)cap)
        {
            token = next;
            break;
        }
        if (next.empty() || next == token)
        {
            token.clear();
            break;
        }
        token = next;
        if (!rows.empty())
            break; // return what this page had; the caller asks again later
    }
    if (rows.empty() && token.empty())
    {
        finish_trace(last, "HOME", 0);
        error = "No more home tracks";
        return false;
    }
    json = home_payload(rows, token);
    if (any_reply)
        finish_trace(last, "HOME", (int)rows.size());
    return true;
}

// --------------------------------------------------------------- album_of etc.

bool next_seed(const std::string &video_id, Row &seed, WebReply &reply,
               std::string &error)
{
    if (!web_call("next", "\"videoId\":\"" + escape(video_id) + "\"", reply, error))
        return false;
    const jsonv::Value *panel = find_renderer(reply.root, "playlistPanelRenderer");
    if (!panel)
    {
        error = "No watch playlist";
        return false;
    }
    const jsonv::Value &contents = (*panel)["contents"];
    for (std::size_t i = 0; i < contents.size(); ++i)
    {
        const jsonv::Value &r = contents.at(i)["playlistPanelVideoRenderer"];
        if (r.is_null())
            continue;
        if (r.str("videoId") != video_id)
            continue;
        seed.id = video_id;
        seed.title = trim(runs_text(r["title"]["runs"]));
        const std::vector<Part> byline = runs_parts(r["longBylineText"]["runs"]);
        if (!byline.empty())
        {
            seed.artist = byline[0].text;
            seed.artist_id = byline[0].browse_id;
        }
        seed.duration = parse_duration(trim(runs_text(r["lengthText"]["runs"])));
        return true;
    }
    // Fall back to the first queue entry (the seed is normally first).
    if (contents.size() > 0)
    {
        const jsonv::Value &r = contents.at(0)["playlistPanelVideoRenderer"];
        seed.id = r.str("videoId");
        seed.title = trim(runs_text(r["title"]["runs"]));
        const std::vector<Part> byline = runs_parts(r["longBylineText"]["runs"]);
        if (!byline.empty())
        {
            seed.artist = byline[0].text;
            seed.artist_id = byline[0].browse_id;
        }
        return !seed.title.empty();
    }
    error = "No watch playlist";
    return false;
}

// Song search used by album_of/artist_of: the subtitle runs of a search row
// carry the album browse id (MPREb...) and the artist browse id (UC...).
bool search_song_rows(const std::string &query, std::vector<Row> &rows,
                      ResolveTrace &trace, std::string &error)
{
    WebReply reply;
    if (!web_call("search",
                  "\"query\":\"" + escape(query) +
                      "\",\"params\":\"EgWKAQIIAWoMEA4QChADEAQQCRAF\"",
                  reply, error))
        return false;
    std::vector<const jsonv::Value *> renderers;
    collect_all(reply.root, "musicResponsiveListItemRenderer", renderers);
    util::append_tracks(renderers, rows, 20);
    trace = reply.trace;
    finish_trace(reply, "SEARCH", (int)rows.size());
    return !rows.empty();
}

bool artist_of(const std::string &video_id, std::string &json, std::string &error)
{
    if (video_id.size() != 11)
    {
        error = "Missing video_id";
        return false;
    }
    WebReply reply;
    Row seed;
    std::string next_error;
    if (next_seed(video_id, seed, reply, next_error) && !seed.artist_id.empty())
    {
        json = "{\"artist_id\":\"" + escape(seed.artist_id) + "\",\"artist_name\":\"" +
               escape(seed.artist) + "\"}";
        finish_trace(reply, "NEXT", 1);
        return true;
    }
    Row found;
    ResolveTrace trace;
    std::string trace_error;
    const std::string query = trim(seed.title + " " + seed.artist);
    std::vector<Row> rows;
    if (!query.empty() && search_song_rows(query, rows, trace, trace_error))
        for (const Row &row : rows)
            if (row.id == video_id && !row.artist_id.empty())
            {
                json = "{\"artist_id\":\"" + escape(row.artist_id) +
                       "\",\"artist_name\":\"" + escape(row.artist) + "\"}";
                return true;
            }
    error = "No artist found for this track.";
    return false;
}

bool album_of(const std::string &video_id, std::string &json, std::string &error)
{
    if (video_id.size() != 11)
    {
        error = "Missing video_id";
        return false;
    }
    WebReply reply;
    Row seed;
    std::string next_error;
    next_seed(video_id, seed, reply, next_error);

    Row found;
    ResolveTrace trace;
    std::string trace_error;
    const std::string query = trim(seed.title + " " + seed.artist);
    std::vector<Row> rows;
    if (!query.empty() && search_song_rows(query, rows, trace, trace_error))
    {
        for (const Row &row : rows)
            if (row.id == video_id && !row.album_id.empty())
            {
                json = "{\"album_id\":\"" + escape(row.album_id) +
                       "\",\"album_name\":\"" + escape(row.album) + "\"}";
                return true;
            }
        for (const Row &row : rows)
            if (!row.album_id.empty())
            {
                json = "{\"album_id\":\"" + escape(row.album_id) +
                       "\",\"album_name\":\"" + escape(row.album) + "\"}";
                return true;
            }
    }
    error = "Could not identify the album.";
    return false;
}

// ----------------------------------------------------------------- search_all

std::string search_top(const jsonv::Value &root)
{
    const jsonv::Value *card = find_renderer(root, "musicCardShelfRenderer");
    if (!card)
        return {};
    const std::string title = trim(runs_text((*card)["title"]["runs"]));
    if (title.empty())
        return {};
    const std::vector<Part> parts = runs_parts((*card)["subtitle"]["runs"]);
    std::string kind;
    for (const Part &p : parts)
        if (p.text == "Song" || p.text == "Video" || p.text == "Album" ||
            p.text == "Artist" || p.text == "Playlist" || p.text == "EP" ||
            p.text == "Single")
        {
            kind = p.text;
            break;
        }
    for (char &c : kind)
        c = (char)std::tolower((unsigned char)c);
    if (kind.empty())
        return {};

    const jsonv::Value &title_run = (*card)["title"]["runs"].at(0);
    std::string video_id = title_run["navigationEndpoint"]["watchEndpoint"].str("videoId");
    std::string browse_id = title_run["navigationEndpoint"]["browseEndpoint"].str("browseId");
    if (video_id.empty() && browse_id.empty())
    {
        video_id = (*card)["onTap"]["watchEndpoint"].str("videoId");
        browse_id = (*card)["onTap"]["browseEndpoint"].str("browseId");
    }
    std::string artist, artist_id, duration, year, subscribers;
    int duration_sec = 0;
    for (const Part &p : parts)
    {
        if (p.browse_id.rfind("UC", 0) == 0 && artist.empty())
        {
            artist = p.text;
            artist_id = p.browse_id;
        }
        if (looks_duration(p.text))
        {
            duration = p.text;
            duration_sec = parse_duration(p.text);
        }
        if (p.text.size() == 4 &&
            std::all_of(p.text.begin(), p.text.end(),
                        [](char c) { return c >= '0' && c <= '9'; }))
            year = p.text;
        if (p.text.find("audience") != std::string::npos ||
            p.text.find("subscriber") != std::string::npos)
            subscribers = p.text;
    }
    if ((kind == "song" || kind == "video") && video_id.empty())
        return {};
    if ((kind == "album" || kind == "artist") && browse_id.empty())
        return {};
    const std::string id = video_id.empty() ? browse_id : video_id;
    const std::string thumb = util::renderer_thumb(*card);
    const bool is_artist = kind == "artist";
    const bool is_album = kind == "album";
    return "{\"kind\":\"" + kind + "\",\"video\":" +
           (kind == "video" ? "true" : "false") + ",\"id\":\"" + escape(id) +
           "\",\"title\":\"" + escape(title) + "\",\"artist\":\"" +
           escape(is_artist ? title : artist) + "\",\"artist_id\":\"" +
           escape(is_artist ? id : artist_id) + "\",\"album\":\"" +
           escape(is_album ? title : "") + "\",\"album_id\":\"" +
           escape(is_album ? id : "") + "\",\"year\":\"" + escape(year) +
           "\",\"duration\":\"" + escape(duration) + "\",\"duration_sec\":" +
           std::to_string(duration_sec) + ",\"subscribers\":\"" +
           escape(subscribers) + "\",\"thumbnail_url\":\"" + escape(thumb) + "\"}";
}

bool search_all(const std::string &query, int limit, std::string &json, std::string &error)
{
    if (query.empty())
    {
        error = "Missing query";
        return false;
    }
    // One default search carries the Top result card plus the album/artist
    // shelves; a second, songs-filtered request fills the song list.
    WebReply reply;
    std::string search_error;
    std::string top;
    std::string albums_json = "[]";
    std::string artists_json = "[]";
    if (web_call("search", "\"query\":\"" + escape(query) + "\"", reply, search_error))
    {
        top = search_top(reply.root);
        std::string albums = "[";
        std::string artists = "[";
        bool first_album = true, first_artist = true;
        std::vector<std::string> seen_albums, seen_artists;
        std::vector<const jsonv::Value *> renderers;
        collect_all(reply.root, "musicResponsiveListItemRenderer", renderers);
        for (const jsonv::Value *r : renderers)
        {
            const std::string title = trim(runs_text(
                (*r)["flexColumns"].at(0)["musicResponsiveListItemFlexColumnRenderer"]
                    ["text"]["runs"]));
            if (title.empty())
                continue;
            std::string browse =
                trim((*r)["navigationEndpoint"]["browseEndpoint"].str("browseId"));
            const std::vector<Part> parts = runs_parts(
                (*r)["flexColumns"].at(1)["musicResponsiveListItemFlexColumnRenderer"]
                    ["text"]["runs"]);
            const bool is_album_kind =
                !parts.empty() && (parts[0].text == "Album" || parts[0].text == "Single" ||
                                   parts[0].text == "EP");
            const bool is_artist_kind = !parts.empty() && parts[0].text == "Artist";
            if (browse.empty() || (!is_album_kind && !is_artist_kind))
                continue;
            const std::string thumb = util::renderer_thumb(*r);
            if (browse.rfind("MPREb", 0) == 0)
            {
                if (std::find(seen_albums.begin(), seen_albums.end(), browse) !=
                    seen_albums.end())
                    continue;
                seen_albums.push_back(browse);
                std::string artist, year;
                for (const Part &p : parts)
                {
                    if (p.browse_id.rfind("UC", 0) == 0 && artist.empty())
                        artist = p.text;
                    if (p.text.size() == 4 &&
                        std::all_of(p.text.begin(), p.text.end(),
                                    [](char c) { return c >= '0' && c <= '9'; }))
                        year = p.text;
                }
                if (!first_album)
                    albums += ",";
                first_album = false;
                albums += "{\"browse_id\":\"" + escape(browse) + "\",\"title\":\"" +
                          escape(title) + "\",\"artist\":\"" + escape(artist) +
                          "\",\"year\":\"" + escape(year) + "\",\"thumbnail_url\":\"" +
                          escape(thumb) + "\",\"track_count\":0}";
            }
            else if (browse.rfind("UC", 0) == 0)
            {
                if (std::find(seen_artists.begin(), seen_artists.end(), browse) !=
                    seen_artists.end())
                    continue;
                seen_artists.push_back(browse);
                std::string subscribers;
                if (parts.size() > 1)
                    subscribers = parts[1].text;
                if (!first_artist)
                    artists += ",";
                first_artist = false;
                artists += "{\"browse_id\":\"" + escape(browse) + "\",\"name\":\"" +
                           escape(title) + "\",\"thumbnail_url\":\"" +
                           escape(thumb) + "\",\"subscribers\":\"" +
                           escape(subscribers) + "\"}";
            }
        }
        albums += "]";
        artists += "]";
        albums_json = albums;
        artists_json = artists;
        finish_trace(reply, "SEARCH", (int)renderers.size());
    }

    std::string songs_json;
    std::string songs_error;
    const bool songs_ok = ::native::search(query, limit, songs_json, songs_error);
    if (!songs_ok)
        songs_json = "[]";
    if (top.empty() && !songs_ok && albums_json == "[]" && artists_json == "[]")
    {
        error = "Search failed";
        return false;
    }
    json = "{\"top\":" + (top.empty() ? std::string("null") : top) + ",\"songs\":" +
           songs_json + ",\"albums\":" + albums_json + ",\"artists\":" + artists_json +
           "}";
    return true;
}

// ---------------------------------------------------------- library playlists

bool library_playlists(std::string &json, std::string &error)
{
    if (!load_auth().loaded)
    {
        error = "Sign in to YouTube Music to see your playlists";
        return false;
    }
    WebReply reply;
    if (!web_call("browse", "\"browseId\":\"FEmusic_liked_playlists\"", reply, error))
        return false;

    std::vector<const jsonv::Value *> rows;
    collect_all(reply.root, "musicTwoRowItemRenderer", rows);
    collect_all(reply.root, "musicResponsiveListItemRenderer", rows);

    std::string out = "[";
    bool first = true;
    for (const jsonv::Value *r : rows)
    {
        std::string browse =
            trim((*r)["navigationEndpoint"]["browseEndpoint"].str("browseId"));
        if (browse.rfind("VL", 0) == 0)
            browse = browse.substr(2);
        std::string title = trim(runs_text((*r)["title"]["runs"]));
        if (title.empty())
            title = trim(runs_text((*r)["flexColumns"].at(0)
                                       ["musicResponsiveListItemFlexColumnRenderer"]["text"]
                                       ["runs"]));
        if (browse.empty() || title.empty())
            continue;
        const std::string thumb = util::renderer_thumb(*r);
        std::string count =
            trim(runs_text((*r)["subtitle"]["runs"]));
        if (const jsonv::Value *fixed = &(*r)["fixedColumns"].at(0)["musicResponsiveListItemFixedColumnRenderer"]["text"]["runs"];
            !runs_text(*fixed).empty())
            count = trim(runs_text(*fixed));
        if (!first)
            out += ",";
        first = false;
        out += "{\"title\":\"" + escape(title) + "\",\"playlistId\":\"" + escape(browse) +
               "\",\"count\":\"" + escape(count) + "\",\"thumbnails\":[{\"url\":\"" +
               escape(thumb) + "\"}]}";
    }
    out += "]";
    json = out;
    finish_trace(reply, "LIBRARY", (int)rows.size());
    return true;
}


// --------------------------------------------------------------- account writes

bool parse_payload(const std::string &payload, jsonv::Value &root, std::string &error)
{
    std::string parse_error;
    if (!jsonv::Value::parse(payload.empty() ? "{}" : payload, root, parse_error) ||
        !root.is_object())
    {
        error = "Bad command payload";
        return false;
    }
    return true;
}

std::string id_array(const jsonv::Value &v)
{
    std::string out = "[";
    if (v.is_array())
        for (std::size_t i = 0; i < v.size(); ++i)
        {
            if (i)
                out += ",";
            out += "\"" + escape(v.at(i).as_string()) + "\"";
        }
    out += "]";
    return out;
}

bool write_call(const std::string &endpoint, const std::string &fields,
                std::string &json, std::string &error)
{
    if (!load_auth().loaded)
    {
        error = "Sign in to YouTube Music to change your library";
        return false;
    }
    WebReply reply;
    if (!web_call(endpoint, fields, reply, error))
        return false;
    const std::string status = reply.root.str("status");
    if (!status.empty() && status != "STATUS_SUCCEEDED" && status != "SUCCEEDED")
    {
        finish_trace(reply, "WRITE", 0);
        error = "YouTube rejected the change (" + status + ")";
        return false;
    }
    const std::string playlist_id = reply.root.str("playlistId");
    json = "{\"status\":\"ok\"";
    if (!playlist_id.empty())
        json += ",\"playlistId\":\"" + escape(playlist_id) + "\"";
    json += "}";
    finish_trace(reply, "WRITE", 1);
    return true;
}

bool rate_song(const std::string &payload, std::string &json, std::string &error)
{
    jsonv::Value root;
    if (!parse_payload(payload, root, error))
        return false;
    const std::string video_id = root.str("videoId");
    std::string rating = root.str("rating");
    if (rating.empty())
        rating = "LIKE";
    if (video_id.size() != 11)
    {
        error = "Missing videoId";
        return false;
    }
    std::string endpoint;
    if (rating == "LIKE")
        endpoint = "like/like";
    else if (rating == "DISLIKE")
        endpoint = "like/dislike";
    else if (rating == "INDIFFERENT")
        endpoint = "like/removelike";
    else
    {
        error = "Invalid rating: " + rating;
        return false;
    }
    return write_call(endpoint, "\"target\":{\"videoId\":\"" + escape(video_id) + "\"}",
                      json, error);
}

bool create_playlist(const std::string &payload, std::string &json, std::string &error)
{
    jsonv::Value root;
    if (!parse_payload(payload, root, error))
        return false;
    const std::string title = root.str("title");
    if (title.empty())
    {
        error = "Missing title";
        return false;
    }
    std::string privacy = root.str("privacyStatus");
    if (privacy.empty())
        privacy = "PRIVATE";
    std::string fields = "\"title\":\"" + escape(title) + "\",\"privacyStatus\":\"" +
                         escape(privacy) + "\"";
    const std::string description = root.str("description");
    if (!description.empty())
        fields += ",\"description\":\"" + escape(description) + "\"";
    const jsonv::Value &ids = root["videoIds"];
    if (ids.is_array() && ids.size() > 0)
        fields += ",\"videoIds\":" + id_array(ids);
    return write_call("playlist/create", fields, json, error);
}

bool delete_playlist(const std::string &payload, std::string &json, std::string &error)
{
    jsonv::Value root;
    if (!parse_payload(payload, root, error))
        return false;
    const std::string id = root.str("playlistId");
    if (id.empty())
    {
        error = "Missing playlistId";
        return false;
    }
    return write_call("playlist/delete", "\"playlistId\":\"" + escape(id) + "\"", json,
                      error);
}

bool edit_playlist(const std::string &payload, std::string &json, std::string &error)
{
    jsonv::Value root;
    if (!parse_payload(payload, root, error))
        return false;
    const std::string id = root.str("playlistId");
    if (id.empty())
    {
        error = "Missing playlistId";
        return false;
    }
    std::string actions;
    auto add_action = [&actions](const std::string &action) {
        if (!actions.empty())
            actions += ",";
        actions += action;
    };
    const std::string title = root.str("title");
    if (!title.empty())
        add_action("{\"action\":\"ACTION_SET_PLAYLIST_NAME\",\"playlistName\":\"" +
                   escape(title) + "\"}");
    const std::string description = root.str("description");
    if (!description.empty())
        add_action("{\"action\":\"ACTION_SET_PLAYLIST_DESCRIPTION\","
                   "\"playlistDescription\":\"" + escape(description) + "\"}");
    const std::string privacy = root.str("privacyStatus");
    if (!privacy.empty())
        add_action("{\"action\":\"ACTION_SET_PLAYLIST_PRIVACY\",\"playlistPrivacy\":\"" +
                   escape(privacy) + "\"}");
    const jsonv::Value &move = root["moveItem"];
    if (move.is_object())
    {
        const std::string set_video_id = move.str("setVideoId");
        const std::string successor = move.get("afterItemId").is_string()
                                          ? move.str("afterItemId")
                                          : std::string();
        if (!set_video_id.empty())
        {
            std::string action =
                "{\"action\":\"ACTION_MOVE_VIDEO_BEFORE\",\"setVideoId\":\"" +
                escape(set_video_id) + "\"";
            if (!successor.empty())
                action += ",\"movedSetVideoIdSuccessor\":\"" + escape(successor) + "\"";
            action += "}";
            add_action(action);
        }
    }
    if (actions.empty())
    {
        error = "Nothing to change";
        return false;
    }
    return write_call("browse/edit_playlist",
                      "\"playlistId\":\"" + escape(id) + "\",\"actions\":[" + actions + "]",
                      json, error);
}

bool add_playlist_items(const std::string &payload, std::string &json, std::string &error)
{
    jsonv::Value root;
    if (!parse_payload(payload, root, error))
        return false;
    const std::string id = root.str("playlistId");
    const jsonv::Value &ids = root["videoIds"];
    if (id.empty() || !ids.is_array() || ids.size() == 0)
    {
        error = "Missing playlistId or videoIds";
        return false;
    }
    std::string actions;
    std::vector<std::string> seen;
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        const std::string video = ids.at(i).as_string();
        if (video.size() != 11)
            continue;
        if (std::find(seen.begin(), seen.end(), video) != seen.end())
            continue;
        seen.push_back(video);
        if (!actions.empty())
            actions += ",";
        actions += "{\"action\":\"ACTION_ADD_VIDEO\",\"addedVideoId\":\"" +
                   escape(video) + "\"}";
    }
    if (actions.empty())
    {
        error = "No valid video ids";
        return false;
    }
    return write_call("browse/edit_playlist",
                      "\"playlistId\":\"" + escape(id) + "\",\"actions\":[" + actions + "]",
                      json, error);
}

bool remove_playlist_items(const std::string &payload, std::string &json,
                           std::string &error)
{
    jsonv::Value root;
    if (!parse_payload(payload, root, error))
        return false;
    const std::string id = root.str("playlistId");
    const jsonv::Value &videos = root["videos"];
    if (id.empty() || !videos.is_array() || videos.size() == 0)
    {
        error = "Missing playlistId or videos";
        return false;
    }
    std::string actions;
    for (std::size_t i = 0; i < videos.size(); ++i)
    {
        const jsonv::Value &v = videos.at(i);
        const std::string set_video_id = v.str("setVideoId");
        const std::string video = v.str("videoId");
        if (set_video_id.empty() || video.empty())
            continue;
        if (!actions.empty())
            actions += ",";
        actions += "{\"action\":\"ACTION_REMOVE_VIDEO\",\"setVideoId\":\"" +
                   escape(set_video_id) + "\",\"removedVideoId\":\"" + escape(video) +
                   "\"}";
    }
    if (actions.empty())
    {
        error = "No removable items";
        return false;
    }
    return write_call("browse/edit_playlist",
                      "\"playlistId\":\"" + escape(id) + "\",\"actions\":[" + actions + "]",
                      json, error);
}


// ---------------------------------------------------------------------- lyrics
//
// Unison/Better Lyrics word timing, enhanced LRC parsing and the translation
// flow follow ytm-rs (GPL-3.0); the implementation is C++.

namespace
{
std::string url_encode(const std::string &text)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string get_text(const std::string &url, long long *ms = nullptr)
{
    std::string jar;
    std::vector<HttpHeader> headers = {
        {"User-Agent", web_user_agent()},
        {"Accept", "application/json,text/plain,*/*"},
    };
    const HttpResult res = http_get(url, headers, jar);
    if (ms)
        *ms = res.ms;
    return res.ok ? res.body : std::string();
}

// "[mm:ss.xx] text" lines -> {"synced":bool,"source":...,"lines":[...]}.
std::string lrc_payload(const std::string &lrc, const std::string &source)
{
    struct Line
    {
        int t;
        std::string text;
    };
    std::vector<Line> lines;
    std::size_t pos = 0;
    while (pos < lrc.size())
    {
        std::size_t end = lrc.find('\n', pos);
        if (end == std::string::npos)
            end = lrc.size();
        std::string raw = lrc.substr(pos, end - pos);
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        const std::size_t open = raw.find('[');
        const std::size_t close = open == std::string::npos ? std::string::npos
                                                           : raw.find(']', open);
        if (open != std::string::npos && close != std::string::npos)
        {
            const std::string tag = raw.substr(open + 1, close - open - 1);
            const std::size_t colon = tag.find(':');
            if (colon != std::string::npos)
            {
                int minutes = std::atoi(tag.substr(0, colon).c_str());
                const std::string rest = tag.substr(colon + 1);
                const std::size_t dot = rest.find('.');
                int seconds = std::atoi(rest.substr(0, dot).c_str());
                const std::string text = trim(raw.substr(close + 1));
                if (!text.empty() && (minutes > 0 || seconds > 0))
                    lines.push_back({minutes * 60 + seconds, text});
            }
        }
        pos = end + 1;
    }
    bool synced = !lines.empty();
    std::string out = "{\"synced\":";
    out += synced ? "true" : "false";
    out += ",\"source\":\"" + escape(source) + "\",\"lines\":[";
    if (lines.empty())
    {
        // Plain text: one entry per non-empty line.
        bool first = true;
        std::size_t p = 0;
        while (p < lrc.size())
        {
            std::size_t e = lrc.find('\n', p);
            if (e == std::string::npos)
                e = lrc.size();
            std::string text = trim(lrc.substr(p, e - p));
            if (!text.empty())
            {
                if (!first)
                    out += ",";
                first = false;
                out += "{\"t\":0,\"text\":\"" + escape(text) + "\"}";
            }
            p = e + 1;
        }
    }
    else
    {
        std::stable_sort(lines.begin(), lines.end(),
                         [](const Line &a, const Line &b) { return a.t < b.t; });
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (i)
                out += ",";
            out += "{\"t\":" + std::to_string(lines[i].t) + ",\"text\":\"" +
                   escape(lines[i].text) + "\"}";
        }
    }
    out += "]}";
    return out;
}

// YouTube Music's timed lyrics, served by the public ANDROID_MUSIC browse.
std::string ytm_timed(const std::string &browse_id)
{
    if (browse_id.empty())
        return {};
    const std::string body =
        "{\"context\":{\"client\":{\"clientName\":\"ANDROID_MUSIC\","
        "\"clientVersion\":\"7.21.50\",\"androidSdkVersion\":34,"
        "\"osName\":\"Android\",\"osVersion\":\"14\",\"hl\":\"en\",\"gl\":\"US\","
        "\"utcOffsetMinutes\":0}},\"browseId\":\"" + escape(browse_id) + "\"}";
    std::vector<HttpHeader> headers = {
        {"User-Agent", "com.google.android.apps.youtube.music/7.21.50 (Linux; U; "
                       "Android 14) gzip"},
        {"Origin", kMusicOrigin},
    };
    std::string jar;
    const HttpResult res = http_post_json(
        "https://music.youtube.com/youtubei/v1/browse?alt=json", body, headers, jar);
    if (!res.ok)
        return {};
    jsonv::Value root;
    std::string parse_error;
    if (!jsonv::Value::parse(res.body, root, parse_error))
        return {};

    // timedLyricsData is an array of {lyricLine, cueRange:{startTimeMilliseconds}}.
    const jsonv::Value *timed = nullptr;
    std::vector<const jsonv::Value *> found;
    // collect_renderer looks for object members, so walk manually.
    std::function<void(const jsonv::Value &)> walk = [&](const jsonv::Value &v) {
        if (v.is_object())
        {
            const jsonv::Value &t = v["timedLyricsData"];
            if (t.is_array())
                timed = &t;
            for (const auto &kv : v.members())
                walk(kv.second);
        }
        else if (v.is_array())
            for (std::size_t i = 0; i < v.size(); ++i)
                walk(v.at(i));
    };
    walk(root);
    if (!timed || timed->size() == 0)
        return {};
    std::string source = find_string(root, "sourceMessage");
    if (source.rfind("source:", 0) == 0 || source.rfind("Source:", 0) == 0)
        source = trim(source.substr(7));
    if (source.empty())
        source = "YouTube";

    struct Line
    {
        long long ms;
        std::string text;
    };
    std::vector<Line> lines;
    for (std::size_t i = 0; i < timed->size(); ++i)
    {
        const jsonv::Value &item = timed->at(i);
        const std::string text = trim(item.str("lyricLine"));
        if (text.empty())
            continue;
        lines.push_back({item["cueRange"]["startTimeMilliseconds"].as_int(), text});
    }
    if (lines.empty())
        return {};
    const bool synced = std::any_of(lines.begin(), lines.end(),
                                    [](const Line &l) { return l.ms > 0; });
    if (synced)
        std::stable_sort(lines.begin(), lines.end(),
                         [](const Line &a, const Line &b) { return a.ms < b.ms; });
    std::string out = "{\"synced\":";
    out += synced ? "true" : "false";
    out += ",\"source\":\"" + escape(source) + "\",\"lines\":[";
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i)
            out += ",";
        out += "{\"t\":" + std::to_string(lines[i].ms / 1000) + ",\"text\":\"" +
               escape(lines[i].text) + "\"}";
    }
    out += "]}";
    return out;
}

std::string lrclib(const std::string &title, const std::string &artist)
{
    if (title.empty() || artist.empty())
        return {};
    const std::string base = "https://lrclib.net/api/get?artist_name=" +
                             url_encode(artist) + "&track_name=" + url_encode(title);
    std::string body = get_text(base);
    jsonv::Value root;
    std::string parse_error;
    if (!body.empty() && jsonv::Value::parse(body, root, parse_error) && root.is_object() &&
        !root.boolean("instrumental"))
    {
        const std::string synced = root.str("syncedLyrics");
        if (!synced.empty())
            return lrc_payload(synced, "LRCLIB");
        const std::string plain = root.str("plainLyrics");
        if (!plain.empty())
            return lrc_payload(plain, "LRCLIB");
    }
    // Search endpoint: pick the first synced hit, else the first plain one.
    body = get_text("https://lrclib.net/api/search?artist_name=" + url_encode(artist) +
                    "&track_name=" + url_encode(title));
    if (body.empty() || !jsonv::Value::parse(body, root, parse_error) || !root.is_array())
        return {};
    std::string plain_fallback;
    for (std::size_t i = 0; i < root.size(); ++i)
    {
        const jsonv::Value &hit = root.at(i);
        if (hit.boolean("instrumental"))
            continue;
        const std::string synced = hit.str("syncedLyrics");
        if (!synced.empty())
            return lrc_payload(synced, "LRCLIB");
        if (plain_fallback.empty())
        {
            const std::string plain = hit.str("plainLyrics");
            if (!plain.empty())
                plain_fallback = lrc_payload(plain, "LRCLIB");
        }
    }
    return plain_fallback;
}

std::string lyrics_ovh(const std::string &title, const std::string &artist)
{
    if (title.empty() || artist.empty())
        return {};
    const std::string body =
        get_text("https://api.lyrics.ovh/v1/" + url_encode(artist) + "/" +
                 url_encode(title));
    if (body.empty())
        return {};
    jsonv::Value root;
    std::string parse_error;
    if (!jsonv::Value::parse(body, root, parse_error) || !root.is_object())
        return {};
    const std::string text = root.str("lyrics");
    if (text.empty())
        return {};
    return lrc_payload(text, "lyrics.ovh");
}

// ------------------------------------------------------------------ Unison
// Unison (betterlyrics.org) returns TTML or enhanced LRC with word/syllable
// timings, plus a translation/romanization endpoint. This is the same source
// ytm-rs uses for its word-by-word view.

std::string xml_unescape(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();)
    {
        if (text[i] != '&')
        {
            out += text[i++];
            continue;
        }
        const std::size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12)
        {
            out += text[i++];
            continue;
        }
        const std::string ent = text.substr(i + 1, semi - i - 1);
        if (ent == "amp")
            out += '&';
        else if (ent == "lt")
            out += '<';
        else if (ent == "gt")
            out += '>';
        else if (ent == "quot")
            out += '"';
        else if (ent == "apos")
            out += '\'';
        else if (!ent.empty() && ent[0] == '#')
        {
            const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            const unsigned long cp = std::strtoul(ent.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
            if (cp < 0x80)
                out += (char)cp;
            else if (cp < 0x800)
            {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
            else
            {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }
        else
            out += text.substr(i, semi - i + 1);
        i = semi + 1;
    }
    return out;
}

int ttml_time_ms(const std::string &raw)
{
    const std::string text = trim(raw);
    if (text.empty())
        return 0;
    std::vector<double> parts;
    std::size_t pos = 0;
    while (pos <= text.size())
    {
        const std::size_t dot = text.find(':', pos);
        const std::string bit = text.substr(pos, dot == std::string::npos ? std::string::npos
                                                                         : dot - pos);
        parts.push_back(std::atof(bit.c_str()));
        if (dot == std::string::npos)
            break;
        pos = dot + 1;
    }
    if (parts.size() == 1)
        return (int)(parts[0] * 1000.0 + 0.5);
    if (parts.size() == 2)
        return (int)((parts[0] * 60.0 + parts[1]) * 1000.0 + 0.5);
    return (int)((parts[0] * 3600.0 + parts[1] * 60.0 + parts[2]) * 1000.0 + 0.5);
}

std::string attr_value(const std::string &tag, const std::string &name)
{
    const std::string key = name + "=\"";
    std::size_t p = tag.find(key);
    if (p == std::string::npos)
        return {};
    p += key.size();
    const std::size_t q = tag.find('"', p);
    return q == std::string::npos ? std::string() : tag.substr(p, q - p);
}

std::string strip_xml(const std::string &text)
{
    std::string out;
    bool in_tag = false;
    for (char c : text)
    {
        if (c == '<')
            in_tag = true;
        else if (c == '>')
            in_tag = false;
        else if (!in_tag)
            out += c;
    }
    return xml_unescape(out);
}

struct UWord
{
    int start_ms = 0;
    int end_ms = 0;
    std::string text;
};

struct RawLyricLine
{
    int start_ms = 0;
    int end_ms = 0;
    std::string text;
    std::vector<UWord> words;
    std::string translation;
    std::string romanization;
};

std::vector<RawLyricLine> parse_ttml(const std::string &xml)
{
    std::vector<RawLyricLine> out;
    std::size_t pos = 0;
    while ((pos = xml.find("<p", pos)) != std::string::npos)
    {
        const std::size_t tag_end = xml.find('>', pos);
        if (tag_end == std::string::npos)
            break;
        const std::string tag = xml.substr(pos, tag_end - pos + 1);
        const std::size_t close = xml.find("</p>", tag_end + 1);
        if (close == std::string::npos)
            break;
        const std::string body = xml.substr(tag_end + 1, close - tag_end - 1);
        RawLyricLine line;
        line.start_ms = ttml_time_ms(attr_value(tag, "begin"));
        line.end_ms = ttml_time_ms(attr_value(tag, "end"));
        std::size_t sp = 0;
        while ((sp = body.find("<span", sp)) != std::string::npos)
        {
            const std::size_t se = body.find('>', sp);
            if (se == std::string::npos)
                break;
            const std::string stag = body.substr(sp, se - sp + 1);
            const std::size_t sclose = body.find("</span>", se + 1);
            if (sclose == std::string::npos)
                break;
            const std::string inner = xml_unescape(body.substr(se + 1, sclose - se - 1));
            UWord w;
            w.start_ms = ttml_time_ms(attr_value(stag, "begin"));
            w.end_ms = ttml_time_ms(attr_value(stag, "end"));
            w.text = inner;
            if (!w.text.empty())
            {
                // Preserve the inter-word space TTML leaves between spans.
                const std::size_t after = body.find('<', sclose + 7);
                const std::string gap = body.substr(sclose + 7,
                                                    after == std::string::npos
                                                        ? body.size() - (sclose + 7)
                                                        : after - (sclose + 7));
                if (gap.find_first_of(" \t\n\r") != std::string::npos)
                    w.text += " ";
                line.words.push_back(std::move(w));
            }
            sp = sclose + 7;
        }
        line.text = trim(strip_xml(body));
        if (line.text.empty() && !line.words.empty())
        {
            for (const UWord &w : line.words)
                line.text += w.text;
            line.text = trim(line.text);
        }
        if (line.start_ms == 0 && !line.words.empty())
            line.start_ms = line.words.front().start_ms;
        if (line.end_ms == 0 && !line.words.empty())
            line.end_ms = line.words.back().end_ms;
        if (!line.text.empty() || !line.words.empty())
            out.push_back(std::move(line));
        pos = close + 4;
    }
    for (std::size_t i = 0; i + 1 < out.size(); ++i)
        if (out[i].end_ms <= out[i].start_ms)
            out[i].end_ms = out[i + 1].start_ms;
    return out;
}

std::vector<RawLyricLine> parse_rich_lrc(const std::string &lrc)
{
    std::vector<RawLyricLine> out;
    std::size_t pos = 0;
    while (pos < lrc.size())
    {
        std::size_t end = lrc.find('\n', pos);
        if (end == std::string::npos)
            end = lrc.size();
        std::string raw = lrc.substr(pos, end - pos);
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        pos = end + 1;
        if (raw.size() < 4 || raw[0] != '[')
            continue;
        const std::size_t close = raw.find(']');
        if (close == std::string::npos)
            continue;
        const std::string tag = raw.substr(1, close - 1);
        const std::size_t colon = tag.find(':');
        if (colon == std::string::npos)
            continue;
        int minutes = std::atoi(tag.substr(0, colon).c_str());
        const std::string rest = tag.substr(colon + 1);
        const std::size_t dot = rest.find('.');
        int seconds = std::atoi(rest.substr(0, dot).c_str());
        const int start = minutes * 60000 + seconds * 1000;
        std::string text = raw.substr(close + 1);
        RawLyricLine line;
        line.start_ms = start;
        std::size_t wp = 0;
        while ((wp = text.find('<', wp)) != std::string::npos)
        {
            const std::size_t wc = text.find('>', wp);
            if (wc == std::string::npos)
                break;
            const std::string stag = text.substr(wp + 1, wc - wp - 1);
            const std::size_t dot2 = stag.find('.');
            int wm = 0, ws = 0;
            const std::size_t colon2 = stag.find(':');
            if (colon2 != std::string::npos)
            {
                wm = std::atoi(stag.substr(0, colon2).c_str());
                ws = std::atoi(stag.substr(colon2 + 1, dot2 == std::string::npos
                                                             ? std::string::npos
                                                             : dot2 - colon2 - 1)
                                   .c_str());
            }
            const std::size_t next = text.find('<', wc + 1);
            std::string word = text.substr(wc + 1, next == std::string::npos
                                                     ? std::string::npos
                                                     : next - wc - 1);
            line.words.push_back({wm * 60000 + ws * 1000, 0, word});
            wp = wc + 1;
            if (next == std::string::npos)
                break;
        }
        for (std::size_t i = 0; i < line.words.size(); ++i)
        {
            if (line.words[i].end_ms <= line.words[i].start_ms)
                line.words[i].end_ms = i + 1 < line.words.size()
                                           ? line.words[i + 1].start_ms
                                           : line.start_ms + 4000;
        }
        if (line.words.empty())
            line.text = trim(text);
        else
        {
            for (const UWord &w : line.words)
                line.text += w.text;
            line.text = trim(line.text);
            line.start_ms = line.words.front().start_ms;
        }
        if (!line.text.empty())
            out.push_back(std::move(line));
    }
    for (std::size_t i = 0; i + 1 < out.size(); ++i)
        if (out[i].end_ms <= out[i].start_ms)
            out[i].end_ms = out[i + 1].start_ms;
    return out;
}

std::string clean_song_title(const std::string &title)
{
    std::string out = title;
    static const char *suffixes[] = {
        " (Official Audio)", " (Official Video)", " (Official Music Video)",
        " [Official Audio]", " [Official Video]", " (Lyrics)", " (Lyric Video)",
        " (Audio)", " (Visualizer)", " (Official Visualizer)"};
    for (const char *suffix : suffixes)
    {
        const std::string s(suffix);
        if (out.size() >= s.size() &&
            out.compare(out.size() - s.size(), s.size(), s) == 0)
        {
            out.erase(out.size() - s.size());
            break;
        }
    }
    return trim(out);
}

void unison_translate(std::vector<RawLyricLine> &lines, const std::string &language)
{
    std::vector<std::string> unique;
    std::vector<std::vector<std::size_t>> indices;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string text = trim(lines[i].text);
        if (text.empty())
            continue;
        std::size_t which = unique.size();
        for (std::size_t u = 0; u < unique.size(); ++u)
            if (unique[u] == text)
            {
                which = u;
                break;
            }
        if (which == unique.size())
        {
            unique.push_back(text);
            indices.push_back({});
        }
        indices[which].push_back(i);
    }
    if (unique.empty())
        return;
    std::string body = "{\"lines\":[";
    for (std::size_t i = 0; i < unique.size(); ++i)
    {
        if (i)
            body += ",";
        body += "\"" + escape(unique[i]) + "\"";
    }
    body += "],\"to\":\"en\"";
    if (!language.empty())
        body += ",\"from\":\"" + escape(language) + "\"";
    body += "}";
    std::vector<HttpHeader> headers = {
        {"User-Agent", web_user_agent()},
        {"Accept", "application/json"},
        {"Origin", "https://unison.betterlyrics.org"},
    };
    std::string jar;
    const HttpResult res = http_post_json("https://unison.betterlyrics.org/translate", body,
                                          headers, jar);
    if (!res.ok)
        return;
    jsonv::Value root;
    std::string parse_error;
    if (!jsonv::Value::parse(res.body, root, parse_error))
        return;
    const jsonv::Value &rows = root["lines"];
    for (std::size_t u = 0; u < indices.size() && u < rows.size(); ++u)
    {
        const jsonv::Value &row = rows.at(u);
        const std::string translation = row.str("translation");
        const std::string roman = row.str("romanization");
        for (std::size_t idx : indices[u])
        {
            if (!translation.empty())
                lines[idx].translation = translation;
            if (!roman.empty() && roman != unique[u])
                lines[idx].romanization = roman;
        }
    }
}

// Add translation/romanization to a line-level payload (YouTube timed, LRCLIB,
// lyrics.ovh). Unison's translate endpoint auto-detects the language, so this
// also gives English subtitles for providers that have no extras of their own.
std::string translate_payload(const std::string &payload)
{
    jsonv::Value root;
    std::string parse_error;
    if (payload.empty() || !jsonv::Value::parse(payload, root, parse_error) ||
        !root.is_object())
        return payload;
    const jsonv::Value &src = root["lines"];
    if (!src.is_array() || src.size() == 0)
        return payload;
    std::vector<RawLyricLine> lines;
    for (std::size_t i = 0; i < src.size(); ++i)
    {
        RawLyricLine l;
        l.start_ms = (int)src.at(i)["t"].as_int() * 1000;
        l.end_ms = (int)src.at(i)["end"].as_int() * 1000;
        l.text = src.at(i).str("text");
        lines.push_back(std::move(l));
    }
    const std::string language = root.str("language");
    const bool non_latin = std::any_of(lines.begin(), lines.end(),
                                       [](const RawLyricLine &l) {
                                           for (unsigned char c : l.text)
                                               if (c >= 0x80)
                                                   return true;
                                           return false;
                                       });
    if (language.empty() || language != "en" || non_latin)
        unison_translate(lines, language);

    std::string out = "{\"version\":2,\"synced\":";
    out += root.boolean("synced") ? "true" : "false";
    out += ",\"source\":\"" + escape(root.str("source")) + "\",\"language\":\"" +
           escape(language) + "\",\"lines\":[";
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i)
            out += ",";
        const RawLyricLine &l = lines[i];
        out += "{\"t\":" + std::to_string(l.start_ms / 1000);
        if (l.end_ms > 0)
            out += ",\"end\":" + std::to_string(l.end_ms / 1000);
        out += ",\"text\":\"" + escape(l.text) + "\"";
        if (!l.translation.empty())
            out += ",\"translation\":\"" + escape(l.translation) + "\"";
        if (!l.romanization.empty())
            out += ",\"roman\":\"" + escape(l.romanization) + "\"";
        out += "}";
    }
    out += "]}";
    return out;
}

std::string unison_lyrics(const std::string &video_id, const std::string &title,
                          const std::string &artist, int duration)
{
    if (video_id.empty() || title.empty())
        return {};
    std::string url = "https://unison.betterlyrics.org/lyrics?v=" + url_encode(video_id) +
                      "&song=" + url_encode(clean_song_title(title)) +
                      "&artist=" + url_encode(artist) +
                      "&duration=" + std::to_string(std::max(0, duration));
    std::string body = get_text(url);
    if (body.empty())
        return {};
    jsonv::Value root;
    std::string parse_error;
    if (!jsonv::Value::parse(body, root, parse_error))
        return {};
    const jsonv::Value &data = root["data"];
    if (!data.is_object())
        return {};
    const std::string format = data.str("format");
    const std::string raw = data.str("lyrics");
    if (raw.empty())
        return {};
    const std::string language = data.str("language");
    std::vector<RawLyricLine> lines;
    if (format == "ttml")
        lines = parse_ttml(raw);
    else if (format == "lrc")
        lines = parse_rich_lrc(raw);
    else
    {
        RawLyricLine line;
        line.text = trim(raw);
        if (!line.text.empty())
            lines.push_back(std::move(line));
    }
    if (lines.empty())
        return {};
    const bool synced = std::any_of(lines.begin(), lines.end(),
                                    [](const RawLyricLine &l) { return l.start_ms > 0; });
    const bool non_latin = std::any_of(lines.begin(), lines.end(),
                                       [](const RawLyricLine &l) {
                                           for (unsigned char c : l.text)
                                               if (c >= 0x80)
                                                   return true;
                                           return false;
                                       });
    if (synced && (language.empty() || language != "en" || non_latin))
        unison_translate(lines, language);
    std::string out = "{\"version\":2,\"synced\":";
    out += synced ? "true" : "false";
    out += ",\"source\":\"Unison\",\"language\":\"" + escape(language) + "\",\"lines\":[";
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i)
            out += ",";
        const RawLyricLine &l = lines[i];
        out += "{\"t\":" + std::to_string(l.start_ms / 1000);
        if (l.end_ms > 0)
            out += ",\"end\":" + std::to_string(l.end_ms / 1000);
        out += ",\"text\":\"" + escape(l.text) + "\"";
        if (!l.translation.empty())
            out += ",\"translation\":\"" + escape(l.translation) + "\"";
        if (!l.romanization.empty())
            out += ",\"roman\":\"" + escape(l.romanization) + "\"";
        if (!l.words.empty())
        {
            out += ",\"words\":[";
            for (std::size_t w = 0; w < l.words.size(); ++w)
            {
                if (w)
                    out += ",";
                out += "{\"s\":" + std::to_string(l.words[w].start_ms) + ",\"e\":" +
                       std::to_string(l.words[w].end_ms) + ",\"text\":\"" +
                       escape(l.words[w].text) + "\"}";
            }
            out += "]";
        }
        out += "}";
    }
    out += "]}";
    return out;
}

} // namespace

bool lyrics(const std::string &video_id, std::string &json, std::string &error)
{
    if (video_id.size() != 11)
    {
        error = "Missing video_id";
        return false;
    }
    // One next call gives the seed metadata and the lyrics browse id.
    Row seed;
    WebReply reply;
    std::string next_error;
    if (!next_seed(video_id, seed, reply, next_error))
    {
        error = next_error;
        return false;
    }
    std::string lyrics_browse_id;
    std::function<void(const jsonv::Value &)> walk = [&](const jsonv::Value &v) {
        if (!lyrics_browse_id.empty())
            return;
        if (v.is_object())
        {
            const jsonv::Value &ep = v["browseEndpoint"];
            if (ep.is_object() &&
                ep["browseEndpointContextSupportedConfigs"]
                  ["browseEndpointContextMusicConfig"]
                      .str("pageType") == "MUSIC_PAGE_TYPE_TRACK_LYRICS")
                lyrics_browse_id = ep.str("browseId");
            for (const auto &kv : v.members())
                walk(kv.second);
        }
        else if (v.is_array())
            for (std::size_t i = 0; i < v.size(); ++i)
                walk(v.at(i));
    };
    walk(reply.root);
    finish_trace(reply, "NEXT", 1);

    auto synced = [](const std::string &payload) {
        return payload.find("\"synced\":true") != std::string::npos;
    };
    std::string result = unison_lyrics(video_id, seed.title, seed.artist, seed.duration);
    if (synced(result))
    {
        json = result;
        return true;
    }
    std::string plain = result;
    result = ytm_timed(lyrics_browse_id);
    if (synced(result))
    {
        json = translate_payload(result);
        return true;
    }
    if (plain.empty())
        plain = result;
    result = lrclib(seed.title, seed.artist);
    if (synced(result))
    {
        json = translate_payload(result);
        return true;
    }
    if (plain.empty())
        plain = result;
    result = lyrics_ovh(seed.title, seed.artist);
    if (synced(result))
    {
        json = translate_payload(result);
        return true;
    }
    if (plain.empty())
        plain = result;
    if (plain.empty())
    {
        json = "{\"none\":true}";
        return true;
    }
    json = translate_payload(plain);
    return true;
}


// -------------------------------------------------------------------- prefetch

bool prefetch_urls(const std::vector<std::string> &video_ids, std::string &json,
                   std::string &error)
{
    std::vector<std::string> ids;
    for (const std::string &id : video_ids)
        if (id.size() == 11)
            ids.push_back(id);
    if (ids.empty())
    {
        error = "No video ids";
        return false;
    }
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> urls;
    std::atomic<std::size_t> next{0};
    const std::size_t workers = std::min<std::size_t>(4, ids.size());
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w)
        threads.emplace_back([&]() {
            for (;;)
            {
                const std::size_t i = next.fetch_add(1);
                if (i >= ids.size())
                    break;
                const StreamResult result = resolve_audio(ids[i]);
                if (!result.ok || result.url.empty())
                    continue;
                std::lock_guard<std::mutex> lock(mutex);
                urls.emplace_back(ids[i], result.url);
            }
        });
    for (std::thread &t : threads)
        t.join();
    if (urls.empty())
    {
        error = "No streams resolved";
        return false;
    }
    std::string out = "[";
    for (std::size_t i = 0; i < urls.size(); ++i)
    {
        if (i)
            out += ",";
        out += "{\"id\":\"" + escape(urls[i].first) + "\",\"url\":\"" +
               escape(urls[i].second) + "\"}";
    }
    out += "]";
    json = out;
    return true;
}

// ------------------------------------------------------------------ musicvideo

struct VideoMapEntry
{
    long long ts = 0;
    std::string json;
};
std::mutex g_video_map_mutex;
std::vector<std::pair<std::string, VideoMapEntry>> g_video_map;

bool musicvideo(const std::string &video_id, std::string &json, std::string &error)
{
    if (video_id.size() != 11)
    {
        error = "Invalid video id";
        return false;
    }
    const long long now = (long long)std::time(nullptr);
    {
        std::lock_guard<std::mutex> lock(g_video_map_mutex);
        for (const auto &entry : g_video_map)
            if (entry.first == video_id && now - entry.second.ts < 3600)
            {
                json = entry.second.json;
                return true;
            }
    }

    std::string meta_json, meta_error;
    if (!fetch_meta(video_id, meta_json, meta_error))
    {
        error = meta_error;
        return false;
    }
    jsonv::Value meta;
    std::string parse_error;
    if (!jsonv::Value::parse(meta_json, meta, parse_error))
    {
        error = "meta parse";
        return false;
    }
    std::string title = meta.str("title");
    const std::string artist = meta.str("artist");
    // videoDetails titles are often "Artist - Track (Official Audio)".
    if (!artist.empty() && title.rfind(artist + " - ", 0) == 0)
        title = title.substr(artist.size() + 3);
    const long long duration = meta["durationSec"].as_int();
    const std::string video_type = meta.str("videoType");

    std::string result_id = video_id;
    std::string result_title = title;
    std::string result_artist = artist;
    long long result_duration = duration;
    bool matched = true;
    bool self = true;

    if (!video_type.empty() && video_type != "MUSIC_VIDEO_TYPE_ATV")
    {
        // Already a video (official video, upload, podcast episode).
    }
    else
    {
        // Search the video shelf and score the hits, like the Python matcher.
        WebReply reply;
        std::string search_error;
        std::string query = title;
        if (!artist.empty())
            query = artist + " " + title;
        if (!query.empty() &&
            web_call("search",
                     "\"query\":\"" + escape(query) +
                         "\",\"params\":\"EgWKAQIQAWoMEA4QChADEAQQCRAF\"",
                     reply, search_error))
        {
            std::vector<const jsonv::Value *> renderers;
            collect_all(reply.root, "musicResponsiveListItemRenderer", renderers);
            std::vector<Row> rows;
            util::append_tracks(renderers, rows, 20);
            double best_score = 0.0;
            Row best;
            for (const Row &row : rows)
            {
                if (row.id == video_id || row.title.empty())
                    continue;
                const bool title_exact = row.title == title;
                const bool title_hit = title_exact ||
                                       row.title.find(title) != std::string::npos ||
                                       title.find(row.title) != std::string::npos;
                if (!title_hit)
                    continue;
                const bool artist_hit =
                    artist.empty() || row.artist.empty() ||
                    row.artist.find(artist) != std::string::npos ||
                    artist.find(row.artist) != std::string::npos ||
                    row.title.find(artist) != std::string::npos;
                const bool official =
                    row.title.find("Official") != std::string::npos ||
                    row.title.find("official") != std::string::npos;
                double score = title_exact ? 4.0 : 2.0;
                if (artist_hit)
                    score += 3.0;
                if (official)
                    score += 2.0;
                if (duration > 0 && row.duration > 0)
                {
                    double diff = (double)(row.duration - duration);
                    if (diff < 0)
                        diff = -diff;
                    score += std::max(0.0, 3.0 - diff / 15.0);
                }
                if (score > best_score && (artist_hit || official))
                {
                    best_score = score;
                    best = row;
                }
            }
            if (best_score >= 4.0 && !best.id.empty())
            {
                result_id = best.id;
                result_title = best.title.empty() ? title : best.title;
                result_artist = best.artist.empty() ? artist : best.artist;
                result_duration = best.duration > 0 ? best.duration : duration;
                matched = true;
                self = false;
            }
            else
                matched = false;
            finish_trace(reply, "SEARCH", (int)rows.size());
        }
    }

    json = "{\"ok\":true,\"videoId\":\"" + escape(result_id) + "\",\"title\":\"" +
           escape(result_title) + "\",\"artist\":\"" + escape(result_artist) +
           "\",\"durationSec\":" + std::to_string(result_duration) + ",\"matched\":" +
           (matched ? "true" : "false") + ",\"self\":" + (self ? "true" : "false") + "}";
    {
        std::lock_guard<std::mutex> lock(g_video_map_mutex);
        if (g_video_map.size() >= 64)
            g_video_map.erase(g_video_map.begin());
        g_video_map.emplace_back(video_id, VideoMapEntry{now, json});
    }
    return true;
}


// -------------------------------------------------------------------- podcasts

namespace
{
int parse_span_seconds(const std::string &text)
{
    int total = 0;
    int value = 0;
    bool has = false;
    std::string word;
    auto flush = [&]() {
        if (has)
        {
            if (word.rfind("h", 0) == 0)
                total += value * 3600;
            else if (word.rfind("m", 0) == 0)
                total += value * 60;
            else if (word.rfind("s", 0) == 0)
                total += value;
        }
        value = 0;
        has = false;
        word.clear();
    };
    for (char c : text)
    {
        if (c >= '0' && c <= '9')
        {
            value = value * 10 + (c - '0');
            has = true;
        }
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            word += (char)std::tolower((unsigned char)c);
        else if (!word.empty())
            flush();
    }
    flush();
    return total;
}

std::string show_json(const std::string &browse_id, const std::string &title,
                      const std::string &author, const std::string &thumb,
                      const std::string &description, bool saved)
{
    return "{\"browse_id\":\"" + escape(browse_id) + "\",\"title\":\"" + escape(title) +
           "\",\"author\":\"" + escape(author) + "\",\"thumbnail_url\":\"" +
           escape(thumb) + "\",\"description\":\"" + escape(description) +
           "\",\"saved\":" + (saved ? "true" : "false") + "}";
}

std::string episode_json(const std::string &video_id, const std::string &title,
                         const std::string &author, const std::string &thumb,
                         int duration, const std::string &date,
                         const std::string &description)
{
    return "{\"video_id\":\"" + escape(video_id) + "\",\"title\":\"" + escape(title) +
           "\",\"author\":\"" + escape(author) + "\",\"thumbnail_url\":\"" +
           escape(thumb) + "\",\"duration_sec\":" + std::to_string(duration) +
           ",\"date\":\"" + escape(date) + "\",\"description\":\"" +
           escape(description) + "\"}";
}

bool parse_show_row(const jsonv::Value &r, std::string &browse_id, std::string &title,
                    std::string &author, std::string &thumb)
{
    browse_id = trim(r["navigationEndpoint"]["browseEndpoint"].str("browseId"));
    title = trim(runs_text(r["flexColumns"].at(0)
                               ["musicResponsiveListItemFlexColumnRenderer"]["text"]["runs"]));
    if (title.empty())
        title = trim(runs_text(r["title"]["runs"]));
    author = trim(runs_text(r["flexColumns"].at(1)
                                ["musicResponsiveListItemFlexColumnRenderer"]["text"]["runs"]));
    if (author.empty())
        author = trim(runs_text(r["straplineTextOne"]["runs"]));
    thumb = util::renderer_thumb(r);
    return !browse_id.empty() && !title.empty();
}
} // namespace

bool podcasts_search(const std::string &query, std::string &json, std::string &error)
{
    if (query.empty())
    {
        error = "Missing query";
        return false;
    }
    WebReply reply;
    if (!web_call("search",
                  "\"query\":\"" + escape(query) +
                      "\",\"params\":\"EgWKAQJQAWoMEA4QChADEAQQCRAF\"",
                  reply, error))
        return false;
    std::vector<const jsonv::Value *> rows;
    collect_all(reply.root, "musicResponsiveListItemRenderer", rows);
    std::string out = "[";
    bool first = true;
    for (const jsonv::Value *r : rows)
    {
        std::string browse_id, title, author, thumb;
        if (!parse_show_row(*r, browse_id, title, author, thumb))
            continue;
        if (browse_id.rfind("MPSP", 0) != 0)
            continue;
        if (!first)
            out += ",";
        first = false;
        out += show_json(browse_id, title, author, thumb, "", false);
    }
    out += "]";
    json = out;
    finish_trace(reply, "PODCASTS", (int)rows.size());
    return true;
}

bool podcast(const std::string &browse_id, std::string &json, std::string &error)
{
    if (browse_id.empty())
    {
        error = "Missing podcast id";
        return false;
    }
    std::string id = browse_id.rfind("MPSP", 0) == 0 ? browse_id : "MPSP" + browse_id;
    WebReply reply;
    if (!web_call("browse", "\"browseId\":\"" + escape(id) + "\"", reply, error))
        return false;

    const jsonv::Value *header = find_renderer(reply.root, "musicResponsiveHeaderRenderer");
    std::string title, author, thumb, description;
    if (header)
    {
        title = header_title(*header);
        author = field_text(*header, "straplineTextOne");
        thumb = util::renderer_thumb(*header);
        description = trim(runs_text((*header)["description"]["runs"]));
    }
    if (const jsonv::Value *desc = find_renderer(reply.root, "musicDescriptionShelfRenderer"))
        description = trim(runs_text((*desc)["description"]["runs"]));

    std::vector<const jsonv::Value *> items;
    collect_all(reply.root, "musicMultiRowListItemRenderer", items);
    std::string episodes = "[";
    bool first = true;
    for (const jsonv::Value *item : items)
    {
        if (episodes.size() > 200000)
            break;
        const std::string video_id = (*item)["onTap"]["watchEndpoint"].str("videoId");
        const std::string ep_title = trim(runs_text((*item)["title"]["runs"]));
        if (video_id.size() != 11 || ep_title.empty())
            continue;
        const std::vector<Part> sub = runs_parts((*item)["subtitle"]["runs"]);
        std::string date;
        for (const Part &p : sub)
            if (p.text.find("ago") != std::string::npos ||
                (p.text.size() > 3 && p.text.find(',') != std::string::npos))
                date = p.text;
        const std::string duration_text = trim(runs_text(
            (*item)["playbackProgress"]["musicPlaybackProgressRenderer"]["durationText"]["runs"]));
        int duration = parse_span_seconds(duration_text);
        if (duration == 0)
            duration = parse_duration(trim(runs_text((*item)["lengthText"]["runs"])));
        const std::string ep_desc = trim(runs_text((*item)["description"]["runs"]));
        if (!first)
            episodes += ",";
        first = false;
        episodes += episode_json(video_id, ep_title, author, util::renderer_thumb(*item),
                                 duration, date, ep_desc);
    }
    episodes += "]";
    json = "{\"show\":" + show_json(id, title, author, thumb, description, false) +
           ",\"episodes\":" + episodes + "}";
    finish_trace(reply, "PODCAST", (int)items.size());
    return true;
}

bool podcasts_library(std::string &json, std::string &error)
{
    if (!load_auth().loaded)
    {
        error = "Sign in to YouTube Music to see your saved podcasts";
        return false;
    }
    WebReply reply;
    if (!web_call("browse", "\"browseId\":\"FEmusic_liked_podcasts\"", reply, error))
        return false;
    std::vector<const jsonv::Value *> rows;
    collect_all(reply.root, "musicResponsiveListItemRenderer", rows);
    std::string out = "[";
    bool first = true;
    for (const jsonv::Value *r : rows)
    {
        std::string browse_id, title, author, thumb;
        if (!parse_show_row(*r, browse_id, title, author, thumb))
            continue;
        if (browse_id.rfind("MPSP", 0) != 0)
            continue;
        if (!first)
            out += ",";
        first = false;
        out += show_json(browse_id, title, author, thumb, "", false);
    }
    out += "]";
    json = out;
    finish_trace(reply, "PODCASTS", (int)rows.size());
    return true;
}

bool episodes_later(std::string &json, std::string &error)
{
    if (!load_auth().loaded)
    {
        error = "Sign in to YouTube Music to see your saved episodes";
        return false;
    }
    WebReply reply;
    if (!web_call("browse", "\"browseId\":\"FEmusic_saved_episodes\"", reply, error))
        return false;
    std::vector<const jsonv::Value *> items;
    collect_all(reply.root, "musicMultiRowListItemRenderer", items);
    std::vector<const jsonv::Value *> rows;
    collect_all(reply.root, "musicResponsiveListItemRenderer", rows);
    std::string out = "[";
    bool first = true;
    for (const jsonv::Value *item : items)
    {
        const std::string video_id = (*item)["onTap"]["watchEndpoint"].str("videoId");
        const std::string title = trim(runs_text((*item)["title"]["runs"]));
        if (video_id.size() != 11 || title.empty())
            continue;
        const std::string duration_text = trim(runs_text(
            (*item)["playbackProgress"]["musicPlaybackProgressRenderer"]["durationText"]["runs"]));
        const std::vector<Part> sub = runs_parts((*item)["subtitle"]["runs"]);
        std::string date, author;
        for (const Part &p : sub)
        {
            if (p.text.find("ago") != std::string::npos)
                date = p.text;
            if (p.browse_id.rfind("UC", 0) == 0)
                author = p.text;
        }
        if (!first)
            out += ",";
        first = false;
        out += episode_json(video_id, title, author, util::renderer_thumb(*item),
                            parse_span_seconds(duration_text), date,
                            trim(runs_text((*item)["description"]["runs"])));
    }
    out += "]";
    json = out;
    finish_trace(reply, "EPISODES", (int)items.size());
    return true;
}

} // namespace native
