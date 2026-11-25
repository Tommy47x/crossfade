#pragma once

// Shared helpers for the native command modules (browse, library, lyrics).
// These mirror the small parsing helpers in innertube.cpp; they live in a
// header so each module keeps its own translation unit without exporting
// internals from the stream resolver.

#include "json_value.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace native
{
namespace util
{
inline std::string escape(const std::string &s)
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

inline std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' ||
                     s[b - 1] == '\r'))
        --b;
    // A UTF-8 non-breaking space (C2 A0) is invisible but not whitespace.
    while (a + 1 < b && (unsigned char)s[a] == 0xC2 && (unsigned char)s[a + 1] == 0xA0)
        a += 2;
    while (b >= a + 2 && (unsigned char)s[b - 2] == 0xC2 && (unsigned char)s[b - 1] == 0xA0)
        b -= 2;
    return s.substr(a, b - a);
}

inline int parse_duration(const std::string &s)
{
    int total = 0;
    int part = 0;
    bool any = false;
    bool saw_colon = false;
    for (char c : s)
    {
        if (c >= '0' && c <= '9')
        {
            part = part * 10 + (c - '0');
            any = true;
        }
        else if (c == ':')
        {
            total = total * 60 + part;
            part = 0;
            saw_colon = true;
        }
        else if (saw_colon && c != ' ')
            break;
    }
    if (!any)
        return 0;
    if (!saw_colon)
        return part; // plain seconds ("338")
    return total * 60 + part;
}

inline bool looks_duration(const std::string &s)
{
    bool colon = false;
    bool digit = false;
    for (char c : s)
    {
        if (c == ':')
            colon = true;
        else if (c >= '0' && c <= '9')
            digit = true;
        else if (c != ' ')
            return false;
    }
    return colon && digit;
}

inline std::string runs_text(const jsonv::Value &runs)
{
    std::string out;
    if (!runs.is_array())
        return out;
    for (std::size_t i = 0; i < runs.size(); ++i)
        out += runs.at(i).str("text");
    return out;
}

struct Part
{
    std::string text;
    std::string browse_id;
    std::string watch_id;
};

inline std::vector<Part> runs_parts(const jsonv::Value &runs)
{
    std::vector<Part> out(1);
    if (!runs.is_array())
        return out;
    for (std::size_t i = 0; i < runs.size(); ++i)
    {
        const jsonv::Value &run = runs.at(i);
        const std::string text = run.str("text");
        if (text.find("\xE2\x80\xA2") != std::string::npos) // UTF-8 bullet
        {
            out.push_back(Part{});
            continue;
        }
        Part &p = out.back();
        p.text += text;
        const std::string browse = run.get("navigationEndpoint")
                                       .get("browseEndpoint")
                                       .str("browseId");
        if (p.browse_id.empty() && !browse.empty())
            p.browse_id = browse;
        const std::string watch =
            run.get("navigationEndpoint").get("watchEndpoint").str("videoId");
        if (p.watch_id.empty() && !watch.empty())
            p.watch_id = watch;
    }
    for (Part &p : out)
        p.text = trim(p.text);
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const Part &p) { return p.text.empty(); }),
              out.end());
    return out;
}

inline void collect_renderer(const jsonv::Value &node, const char *key,
                             std::vector<const jsonv::Value *> &out, int depth = 0)
{
    if (depth > 64)
        return;
    if (node.is_object())
    {
        const jsonv::Value &v = node[key];
        if (!v.is_null())
            out.push_back(&v);
        for (const auto &kv : node.members())
            collect_renderer(kv.second, key, out, depth + 1);
    }
    else if (node.is_array())
    {
        for (std::size_t i = 0; i < node.size(); ++i)
            collect_renderer(node.at(i), key, out, depth + 1);
    }
}

inline std::string thumb_from(const jsonv::Value &holder)
{
    const jsonv::Value &th = holder.get("thumbnails");
    if (!th.is_array())
        return {};
    std::string best;
    for (std::size_t i = 0; i < th.size(); ++i)
    {
        const std::string u = th.at(i).str("url");
        if (!u.empty())
            best = u;
    }
    return best;
}

inline std::string renderer_thumb(const jsonv::Value &r)
{
    std::string t = thumb_from(r.get("thumbnail").get("musicThumbnailRenderer").get("thumbnail"));
    if (!t.empty())
        return t;
    t = thumb_from(r.get("thumbnailRenderer")
                       .get("musicThumbnailRenderer")
                       .get("thumbnail"));
    if (!t.empty())
        return t;
    t = thumb_from(r.get("thumbnail"));
    return t;
}

// A musicResponsiveListItemRenderer row, flattened. Album-level fields
// (parent album id) are filled in by the caller.
struct Row
{
    std::string id;
    std::string title;
    std::string artist;
    std::string artist_id;
    std::string album;
    std::string album_id;
    std::string thumb;
    std::string kind;
    int duration = 0;
    int track_number = 0;
};

inline bool is_noise(const std::string &s)
{
    if (s.empty())
        return true;
    if (s == "Song" || s == "Video" || s == "Album" || s == "Single" || s == "EP" ||
        s == "Playlist" || s == "Podcast" || s == "Episode" || s == "Artist" ||
        s == "Top result" || s == "Podcasts")
        return true;
    if (s.find(" plays") != std::string::npos ||
        s.find(" views") != std::string::npos ||
        s.find("audience") != std::string::npos ||
        s.find("subscriber") != std::string::npos ||
        s.find("monthly") != std::string::npos)
        return true;
    // "1.2B plays", "30M views", "2026", "13 songs".
    bool only_meta = true;
    for (char c : s)
    {
        if (c >= '0' && c <= '9')
            continue;
        if (c == '.' || c == ',' || c == ' ')
            continue;
        only_meta = false;
        break;
    }
    return only_meta;
}

inline bool parse_row(const jsonv::Value &r, Row &t)
{
    t.title = trim(runs_text(r["flexColumns"].at(0)["musicResponsiveListItemFlexColumnRenderer"]
                                 ["text"]["runs"]));
    if (t.title.empty())
        return false;

    t.id = trim(r.get("playlistItemData").str("videoId"));
    if (t.id.empty())
        t.id = trim(r["navigationEndpoint"]["watchEndpoint"].str("videoId"));
    if (t.id.empty())
        t.id = trim(r["flexColumns"].at(0)["musicResponsiveListItemFlexColumnRenderer"]["text"]
                        ["runs"].at(0)["navigationEndpoint"]["watchEndpoint"].str("videoId"));

    t.thumb = renderer_thumb(r);

    std::vector<Part> parts;
    const jsonv::Value &cols = r["flexColumns"];
    for (std::size_t i = 1; i < cols.size(); ++i)
    {
        std::vector<Part> more =
            runs_parts(cols.at(i)["musicResponsiveListItemFlexColumnRenderer"]["text"]["runs"]);
        parts.insert(parts.end(), more.begin(), more.end());
    }
    const jsonv::Value &fixed = r["fixedColumns"].at(0)["musicResponsiveListItemFixedColumnRenderer"]
                                    ["text"]["runs"];
    std::string fixed_text = trim(runs_text(fixed));
    if (looks_duration(fixed_text))
        t.duration = parse_duration(fixed_text);

    bool artist_set = false;
    for (const Part &p : parts)
    {
        if (t.id.empty() && p.watch_id.size() == 11)
            t.id = p.watch_id;
        if (p.browse_id.rfind("MPREb", 0) == 0)
        {
            if (t.album_id.empty())
            {
                t.album_id = p.browse_id;
                t.album = p.text;
            }
            continue;
        }
        if (p.browse_id.rfind("UC", 0) == 0)
        {
            if (!artist_set)
            {
                artist_set = true;
                t.artist = p.text;
                t.artist_id = p.browse_id;
            }
            continue;
        }
        if (looks_duration(p.text))
        {
            if (t.duration == 0)
                t.duration = parse_duration(p.text);
            continue;
        }
        if (is_noise(p.text))
            continue;
        if (!artist_set)
        {
            artist_set = true;
            t.artist = p.text;
            continue;
        }
        if (t.album.empty())
            t.album = p.text;
    }
    if (t.album_id.empty())
    {
        // Some rows put the album under a plain column without a nav endpoint;
        // the last non-noise flex column is the album in that layout.
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
            if (!is_noise(it->text) && looks_duration(it->text) == false &&
                it->text != t.artist)
            {
                if (t.album.empty())
                    t.album = it->text;
                break;
            }
    }
    const std::string idx =
        trim(runs_text(r.get("index").get("runs")));
    if (!idx.empty())
        t.track_number = parse_duration(idx);
    if (t.track_number == 0)
        t.track_number = (int)r["index"].as_int();
    return !t.id.empty();
}

inline std::string track_json(const Row &t, const std::string &album_override = {},
                              const std::string &album_id_override = {})
{
    std::string out = "{\"id\":\"" + escape(t.id) + "\",\"title\":\"" + escape(t.title) +
                      "\",\"artist\":\"" + escape(t.artist) + "\",\"artist_id\":\"" +
                      escape(t.artist_id) + "\",\"album\":\"" +
                      escape(album_override.empty() ? t.album : album_override) +
                      "\",\"album_id\":\"" +
                      escape(album_id_override.empty() ? t.album_id : album_id_override) +
                      "\",\"track_number\":" + std::to_string(t.track_number) +
                      ",\"thumbnail_url\":\"" + escape(t.thumb) +
                      "\",\"duration_sec\":" + std::to_string(t.duration) +
                      ",\"url\":\"https://www.youtube.com/watch?v=" + escape(t.id) + "\"}";
    return out;
}

inline std::string row_array(const std::vector<Row> &rows)
{
    std::string out = "[";
    bool first = true;
    for (const Row &r : rows)
    {
        if (!first)
            out += ",";
        first = false;
        out += track_json(r);
    }
    out += "]";
    return out;
}

inline void append_tracks(const std::vector<const jsonv::Value *> &renderers,
                          std::vector<Row> &out, std::size_t cap)
{
    std::vector<std::string> seen;
    for (const jsonv::Value *r : renderers)
    {
        if (out.size() >= cap)
            break;
        Row t;
        if (!parse_row(*r, t))
            continue;
        if (std::find(seen.begin(), seen.end(), t.id) != seen.end())
            continue;
        seen.push_back(t.id);
        out.push_back(std::move(t));
    }
}

} // namespace util
} // namespace native
