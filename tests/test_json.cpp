#include "ytdlp.h"
#include "test_util.h"
using testutil::check;

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

int main()
{
    {
        const std::string json =
            "[{\"id\":\"a1\",\"title\":\"A \\\"quoted\\\" song\",\"artist\":\"Art\\\\ist\","
            "\"album\":\"Alb\",\"duration_sec\":215,\"track_number\":3},"
            "{\"id\":\"b2\",\"title\":\"Ünïcode\",\"artist\":\"Ärtist\"}]";
        auto tracks = parse_track_array(json);
        check(tracks.size() == 2, "two tracks");
        check(tracks[0].id == "a1", "id");
        check(tracks[0].title == "A \"quoted\" song", "escaped quote");
        check(tracks[0].artist == "Art\\ist", "escaped backslash");
        check(tracks[0].duration_sec == 215, "duration");
        check(tracks[0].track_number == 3, "track number");
        check(tracks[1].id == "b2", "second id");
        check(tracks[1].title == "Ünïcode", "unicode preserved");
    }

    {
        // Album caches used to persist the track's YouTube watch link in
        // thumbnail_url, and the thumbnails fallback grabbed the object's own
        // "url". Neither may come through as artwork.
        const std::string cached =
            "[{\"id\":\"a1\",\"title\":\"A\",\"album\":\"Alb\","
            "\"thumbnail_url\":\"https://www.youtube.com/watch?v=a1\","
            "\"url\":\"https://www.youtube.com/watch?v=a1\"}]";
        auto tracks = parse_track_array(cached);
        check(tracks.size() == 1, "cache track parses");
        check(tracks[0].thumbnail_url.empty(), "watch link rejected as thumbnail");

        const std::string shelf =
            "[{\"id\":\"a1\",\"title\":\"A\","
            "\"thumbnails\":[{\"url\":\"https://i.ytimg.com/vi/a1/hqdefault.jpg\"}],"
            "\"url\":\"https://www.youtube.com/watch?v=a1\"}]";
        tracks = parse_track_array(shelf);
        check(tracks.size() == 1, "shelf track parses");
        check(tracks[0].thumbnail_url == "https://i.ytimg.com/vi/a1/hqdefault.jpg",
              "thumbnails array still provides art");
    }

    {
        const std::string json =
            "[{\"id\":\"a\",\"title\":\"A\",\"meta\":{\"nested\":[1,2,3]}},"
            "{\"id\":\"b\",\"title\":\"B\"}]";
        auto tracks = parse_track_array(json);
        check(tracks.size() == 2, "nested object array");
        check(tracks[0].id == "a" && tracks[1].id == "b", "nested ids");
    }

    {
        auto tracks = parse_track_array("[{\"id\":\"a\",\"title\":\"A\"");
        check(tracks.size() <= 1, "truncated array does not crash");
    }

    {
        auto tracks = parse_track_array("[{\"videoId\":\"v1\",\"title\":\"V\"}]");
        check(tracks.size() == 1 && tracks[0].id == "v1", "videoId fallback");
    }

    {
        auto tracks = parse_track_array("{\"id\":\"a\",\"title\":\"A\"}");
        check(tracks.size() == 1, "object input still parses");
    }

    {
        auto tracks = parse_track_array("[{\"id\":\"\",\"title\":\"No id\"},"
                                        "{\"id\":\"x\",\"title\":\"\"}]");
        check(tracks.empty(), "empty id/title skipped");
    }

    {
        Lyrics lx;
        const std::string json =
            "{\"ok\":true,\"synced\":true,\"source\":\"LRCLIB\",\"lines\":["
            "{\"t\":1000,\"text\":\"  Hello   world  \"},"
            "{\"t\":2500,\"text\":\"Привет\"}]}";
        check(parse_lyrics_json(json, lx), "lyrics parse");
        check(lx.ok && lx.synced, "lyrics flags");
        check(lx.source == "LRCLIB", "lyrics source");
        check(lx.lines.size() == 2, "lyrics line count");
        check(lx.times.size() == 2 && lx.times[0] == 1000, "lyrics timestamps");
        check(lx.lines[0] == "  Hello   world  ", "lyrics text preserved");
        check(lx.lines[0].find("Hello") != std::string::npos, "lyrics text present");
        check(lx.lines[1] == "Привет", "lyrics unicode");
    }

    {
        Lyrics lx;
        check(parse_lyrics_json("{\"none\":true}", lx), "lyrics none parses");
        check(lx.none, "lyrics none flag");
        check(lx.lines.empty(), "lyrics none has no lines");
    }

    {
        Lyrics lx;
        check(!parse_lyrics_json("{\"error\":\"boom\"}", lx), "lyrics error rejected");
        check(!parse_lyrics_json("", lx), "empty lyrics rejected");
    }

    {
        Lyrics lx;
        check(parse_lyrics_json("{\"synced\":false,\"lines\":[{\"t\":0,\"text\":\"plain\"}]}",
                                lx),
              "plain lyrics parse");
        check(!lx.synced && lx.lines.size() == 1 && lx.lines[0] == "plain",
              "plain lyrics fields");
    }

    {
        // Deeply nested input is bounded and cannot make the scanner run away.
        std::string deep = "{\"id\":\"x\",\"title\":\"X\",\"a\":";
        for (int i = 0; i < 300; ++i)
            deep += "{\"a\":";
        deep += "1";
        auto tracks = parse_track_array(deep);
        check(tracks.empty(), "deep nesting bounded");
    }

    {
        // Oversized backend replies are rejected before scanning.
        std::string huge(16u * 1024u * 1024u + 1u, 'x');
        check(parse_track_array(huge).empty(), "oversized input rejected");
        Lyrics lx;
        check(!parse_lyrics_json(huge, lx), "oversized lyrics rejected");
    }

    {
        Lyrics lx;
        check(parse_lyrics_json(
                  "{\"synced\":true,\"lines\":[{\"t\":1,\"text\":\"A：B\"}]}", lx),
              "full-width lyrics parse");
        check(lx.lines.size() == 1 && lx.lines[0] == "A: B",
              "full-width punctuation normalized");
    }

    {
        // Liked Music ("LM") replies have "id": null at the top level and a
        // nested author id; the parser must still find every track.
        const std::string lm =
            "{\"owned\": false, \"id\": null, \"privacy\": \"PUBLIC\", "
            "\"trackCount\": 2, \"title\": \"Liked Music\", "
            "\"author\": {\"name\": \"x\", \"id\": \"UC_author\"}, \"tracks\": ["
            "{\"videoId\":\"v1\",\"title\":\"One\",\"artists\":[{\"name\":\"A\",\"id\":\"UC1\"}]},"
            "{\"videoId\":\"v2\",\"title\":\"Two\",\"artists\":[{\"name\":\"B\",\"id\":\"UC2\"}]}]}";
        auto tracks = parse_youtube_playlist_tracks_response(lm);
        check(tracks.size() == 2, "LM-shaped playlist parses");
        check(tracks[0].id == "v1" && tracks[1].id == "v2", "LM track ids");
    }

    {
        // A "local:" id is a file on disk and must never be handed to yt-dlp
        // or the backend's stream resolver.
        check(get_cached_audio_path("local:test").empty(), "local id not downloaded");
        check(fetch_stream_url("local:test").empty(), "local id not stream-resolved");
        check(fetch_stream_url_fresh("local:test").empty(), "local id not fresh-resolved");
        check(fetch_stream_url_prefetch("local:test").empty(), "local id not prefetch-resolved");
    }

    {
        // Studio wave-cache format (written by the Python backend).
        const auto path = std::filesystem::temp_directory_path() / "p100-wave-test.cfwave";
        const std::string cache =
            "CFWAVE 1\n"
            "duration 8.000000\n"
            "bpm 117.4538\n"
            "confidence 0.5372\n"
            "beats 0.362,0.873,1.384\n"
            "downbeats 0.362\n"
            "wave 3\n"
            "-0.50,0.60\n"
            "-0.10,0.20\n"
            "-0.30,0.40\n"
            "wave 1\n"
            "-0.80,0.90\n";
        testutil::write_file(path, cache);

        StudioAnalysis a;
        check(load_studio_analysis_file(path.string(), a), "wave cache parses");
        check(a.ok, "wave cache ok");
        check(std::fabs(a.duration - 8.0) < 1e-6, "wave duration");
        check(std::fabs(a.bpm - 117.4538) < 1e-3, "wave bpm");
        check(a.beats.size() == 3 && std::fabs(a.beats[1] - 0.873) < 1e-6, "wave beats");
        check(a.downbeats.size() == 1, "wave downbeats");
        check(a.waves.size() == 2, "wave levels");
        check(a.waves[0].buckets == 3 && a.waves[0].mins.size() == 3, "wave buckets");
        check(std::fabs(a.waves[0].maxs[1] - 0.20f) < 1e-5, "wave max value");
        std::error_code ec;
        std::filesystem::remove(path, ec);

        StudioAnalysis missing;
        check(!load_studio_analysis_file(path.string() + ".nope", missing),
              "missing wave cache rejected");
    }

    {
        // ytmusicapi appends setVideoId after nested artists/album/thumbnails
        // objects. A nearest-brace scan used to return the thumbnail object
        // instead of the item, so playlist write-back never saw any items.
        const std::string playlist =
            "{\"id\":\"PL1\",\"title\":\"T\",\"author\":{\"name\":\"A\"},"
            "\"tracks\":["
            "{\"videoId\":\"a1\",\"title\":\"A\","
            "\"artists\":[{\"name\":\"X\",\"id\":\"UCx\"}],"
            "\"album\":{\"name\":\"Alb\",\"id\":\"MPREb\"},"
            "\"thumbnails\":[{\"url\":\"https://i.ytimg.com/a.jpg\",\"width\":60,\"height\":60}],"
            "\"isAvailable\":true,\"duration\":\"3:32\",\"duration_seconds\":212,"
            "\"setVideoId\":\"SV1\"},"
            "{\"videoId\":\"b2\",\"title\":\"B \\\"quoted\\\" }\","
            "\"thumbnails\":[],\"setVideoId\":\"SV2\"},"
            "{\"videoId\":\"c3\",\"title\":\"C\"}"
            "],\"related\":[{\"playlistId\":\"PL2\"}]}";
        auto items = parse_playlist_item_pairs(playlist);
        check(items.size() == 2, "playlist pairs parsed");
        check(items[0].first == "a1" && items[0].second == "SV1",
              "first playlist pair");
        check(items[1].first == "b2" && items[1].second == "SV2",
              "second playlist pair (brace inside string)");

        check(parse_playlist_item_pairs("{\"tracks\":null}").empty(),
              "non-array tracks ignored");
        check(parse_playlist_item_pairs("{\"tracks\":[]}").empty(),
              "empty tracks ignored");
        check(parse_playlist_item_pairs("{\"error\":\"nope\"}").empty(),
              "error response ignored");
    }

    {
        // A malformed or absurd playlist count must not throw: std::stoi used
        // to do exactly that inside a parser worker (uncaught throw = abort).
        const std::string resp =
            "{\"playlists\":["
            "{\"playlistId\":\"PL1\",\"title\":\"A\",\"count\":\"999999999999999999999\"},"
            "{\"playlistId\":\"PL2\",\"title\":\"B\",\"count\":\"abc\"},"
            "{\"playlistId\":\"PL3\",\"title\":\"C\",\"count\":42}"
            "]}";
        auto playlists = parse_youtube_playlists_response(resp);
        check(playlists.size() == 3, "youtube playlists parsed");
        check(playlists[0].count == 1000000000, "huge count clamped");
        check(playlists[1].count == 0, "non-numeric count becomes zero");
        check(playlists[2].count == 42, "numeric count parses");
    }

    if (testutil::failures == 0)
        std::printf("json: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
