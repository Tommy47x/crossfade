#pragma once

#include <string>
#include <vector>

// Native YouTube Music browse commands. Each returns the exact JSON shape the
// Python backend produces, so the existing C++ parsers in src/net/ytdlp.cpp do
// not change. A false return (or an error string) tells the router to fall
// back to Python.
namespace native
{
bool suggest(const std::string &query, std::string &json, std::string &error);
bool album(const std::string &browse_id, std::string &json, std::string &error);
bool artist(const std::string &browse_id, std::string &json, std::string &error);
bool artist_songs(const std::string &browse_id, int offset, int count,
                  std::string &json, std::string &error);
bool playlist(const std::string &playlist_id, std::string &json,
              std::string &error);
bool radio(const std::string &video_id, int limit, std::string &json,
           std::string &error);
bool home(int limit, std::string &json, std::string &error);
// Next page of the personalized Home feed. `json` has the same shape as
// home(): {tracks:[...], continuation:"..."}.
bool home_more(const std::string &continuation, int limit, std::string &json,
               std::string &error);
bool album_of(const std::string &video_id, std::string &json, std::string &error);
bool artist_of(const std::string &video_id, std::string &json, std::string &error);
bool search_all(const std::string &query, int limit, std::string &json,
                std::string &error);
bool library_playlists(std::string &json, std::string &error);
bool lyrics(const std::string &video_id, std::string &json, std::string &error);

// Speculative prefetch: resolve several ids concurrently on the native path.
// Produces the Python reply's bare [{"id","url"}] array.
bool prefetch_urls(const std::vector<std::string> &video_ids, std::string &json,
                   std::string &error);

// Official music video for a song: the song's own upload when it is already a
// video, otherwise the best official match. Produces the Python reply shape
// (ok, videoId, title, artist, durationSec, matched, self).
bool musicvideo(const std::string &video_id, std::string &json, std::string &error);

bool podcasts_search(const std::string &query, std::string &json, std::string &error);
bool podcast(const std::string &browse_id, std::string &json, std::string &error);
bool podcasts_library(std::string &json, std::string &error);
bool episodes_later(std::string &json, std::string &error);

// Account writes. Each takes the raw JSON payload the C++ side already sends
// (the same one the Python backend receives) and returns {"status":"ok"} or
// {"status":"ok","playlistId":"..."} on success.
bool rate_song(const std::string &payload, std::string &json, std::string &error);
bool create_playlist(const std::string &payload, std::string &json,
                     std::string &error);
bool delete_playlist(const std::string &payload, std::string &json,
                     std::string &error);
bool edit_playlist(const std::string &payload, std::string &json,
                   std::string &error);
bool add_playlist_items(const std::string &payload, std::string &json,
                        std::string &error);
bool remove_playlist_items(const std::string &payload, std::string &json,
                           std::string &error);
} // namespace native
