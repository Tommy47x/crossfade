#include "app_internal.h"

#include "ytdlp.h"
#include "search_top.h"
#include "local_audio.h"
#include "mpvplayer.h"
#include "crossfade.h"
#include "texture.h"
#include "ui.h"
#include "ui_notify.h"
#include "library.h"
#include "writeback.h"
#include "paths.h"
#include "discord_rpc.h"
#include "tasks.h"
#include "power.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

void cover_sweep_worker(std::vector<Track> tracks)
{
    for (const auto &t : tracks)
    {
        if (t.id.empty() || t.thumbnail_url.empty())
            continue;
        if (!thumbnail_is_cached(t.thumbnail_url))
            get_cached_thumbnail_path(t.thumbnail_url);
    }
    g_cover_sweep_running = false;
    g_state.repaint_requested = true;
}


void maybe_sweep_missing_covers()
{
    if (power::low_power())
        return;
    if (!g_state.downloaded_loaded || g_state.downloaded_tracks.empty())
        return;
    if (g_cover_sweep_running.exchange(true))
        return;
    tasks::spawn("cover-sweep", [tracks = g_state.downloaded_tracks]() { cover_sweep_worker(tracks); });
}


void background_download(const std::string &video_id)
{
    if (power::low_power())
        return; // low power: never download ahead of playback
    if (video_id.empty())
        return;
    if (is_local_track_id(video_id))
        return; // local files are already on disk
    if (!find_cached_audio_path(video_id).empty())
        return;
    {
        std::lock_guard<std::mutex> lock(g_download_mutex);
        if (!g_downloading.insert(video_id).second)
            return; // already running
        if (g_bg_download_count.load() >= kMaxBgDownloads)
        {
            g_downloading.erase(video_id);
            return; // retried by the next predownload pass
        }
    }
    g_bg_download_count.fetch_add(1);
    tasks::spawn("download", [video_id]()
                {
        std::string path = get_cached_audio_path(video_id);
        g_bg_download_count.fetch_sub(1);
        {
            std::lock_guard<std::mutex> lock(g_download_mutex);
            g_downloading.erase(video_id);
        }
        if (!path.empty())
            g_downloads_dirty = true; });
}


void load_recommendations(bool force, bool quiet)
{
    auto tracks = fetch_recommendations(force);
    BackendError err = take_backend_error();
    if (err && !quiet)
        ui_notify_backend_error(err);
    // Don't clobber a good list with an empty one (e.g. a refresh that hit
    // a transient backend error) -- keep what's on screen and surface the
    // error instead.
    if (!tracks.empty())
    {
        auto snapshot = std::make_shared<const std::vector<Track>>(std::move(tracks));
        std::atomic_store(&g_state.recommendations, snapshot);
        g_state.home_failed = false;
        g_state.home_error_msg.clear();
        // Publish the list first: the header spinner should stop as soon as
        // the new recommendations are on screen, not while we warm the first
        // few URLs in the background.
        g_state.home_refreshing = false;
        g_state.repaint_requested = true;
        prefetch_urls_for(*snapshot, 3);
    }
    else if (err)
    {
        // Nothing on screen (or the refresh failed): remember why, so the
        // Home tab can show an inline error + Retry instead of an empty list.
        g_state.home_failed = true;
        g_state.home_error_msg = backend_error_user_message(err);
    }
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_state.status_text = "Ready.";
    g_state.home_refreshing = false;
    g_state.backend_status = err;
    g_state.repaint_requested = true;
}


void artist_search_and_update(const std::string &query, int expect_id)
{
    if (query.empty())
    {
        std::atomic_store(&g_state.artist_search_results,
                          std::make_shared<const std::vector<ArtistSummary>>());
        g_state.artist_search_loading = false;
        return;
    }
    auto artists = fetch_artist_search(query);
    if (g_state.search_id == expect_id && g_state.current_tab == Tab::Artists)
    {
        std::atomic_store(&g_state.artist_search_results,
                          std::make_shared<const std::vector<ArtistSummary>>(std::move(artists)));
        g_state.artist_search_loading = false;
    }
    BackendError err = take_backend_error();
    if (err)
        ui_notify_backend_error(err);
    {
        std::lock_guard<std::mutex> lock(g_data_mutex);
        g_state.backend_status = err;
    }
    g_state.repaint_requested = true;
}


bool text_matches_query(const std::string &haystack, const std::string &needle)
{
    if (needle.empty())
        return true;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [](char a, char b) {
                           return std::tolower((unsigned char)a) ==
                                  std::tolower((unsigned char)b);
                       }) != haystack.end();
}


void merge_local_search_results(const std::string &query, SearchAllResult &results)
{
    std::unordered_set<std::string> seen;

    auto local_tracks = library::search_tracks(query, 60);
    std::vector<Track> tracks;
    tracks.reserve(local_tracks.size() + results.songs.size());
    for (auto &t : local_tracks)
        if (seen.insert(t.id).second)
            tracks.push_back(std::move(t));
    for (auto &t : results.songs)
        if (seen.insert(t.id).second)
            tracks.push_back(std::move(t));
    results.songs = std::move(tracks);

    std::unordered_set<std::string> seen_albums;
    std::vector<AlbumSummary> albums;
    for (const auto &a : library::load_saved_albums())
    {
        if (!text_matches_query(a.title, query) && !text_matches_query(a.artist, query))
            continue;
        AlbumSummary s;
        s.browse_id = a.browse_id;
        s.title = a.title;
        s.artist = a.artist;
        s.year = a.year;
        s.thumbnail_url = a.thumbnail_url;
        s.track_count = a.track_count;
        if (seen_albums.insert(s.browse_id).second)
            albums.push_back(std::move(s));
    }
    for (auto &a : results.albums)
        if (seen_albums.insert(a.browse_id).second)
            albums.push_back(std::move(a));
    results.albums = std::move(albums);

    std::unordered_set<std::string> seen_artists;
    std::vector<ArtistSummary> artists;
    for (const auto &a : library::load_saved_artists())
    {
        if (!text_matches_query(a.name, query))
            continue;
        ArtistSummary s;
        s.browse_id = a.browse_id;
        s.name = a.name;
        s.thumbnail_url = a.thumbnail_url;
        s.subscribers = a.subscribers;
        if (seen_artists.insert(s.browse_id).second)
            artists.push_back(std::move(s));
    }
    for (auto &a : results.artists)
        if (seen_artists.insert(a.browse_id).second)
            artists.push_back(std::move(a));
    results.artists = std::move(artists);
}


void search_all_and_update(const std::string &query, int expect_id)
{
    if (query.empty())
    {
        std::atomic_store(&g_state.search_results,
                          std::make_shared<const std::vector<Track>>());
        std::atomic_store(&g_state.search_album_results,
                          std::make_shared<const std::vector<AlbumSummary>>());
        std::atomic_store(&g_state.search_artist_results,
                          std::make_shared<const std::vector<ArtistSummary>>());
        g_state.search_loading = false;
        return;
    }

    SearchAllResult results;
    bool ok = fetch_search_all(query, results);
    if (!ok)
    {
        // Older/partially-updated backends may not know search_all yet.
        results.songs = fetch_search(query);
        ok = !results.songs.empty();
    }


    if (g_state.search_id == expect_id)
    {
        // Local library rows are authoritative and instant; put them first,
        // then append online results that aren't already present.
        merge_local_search_results(query, results);

        // Search "Top result" card: YouTube's intent match, validated
        // against these shelves and the local library (see search_top.cpp).
        SearchTopInputs top_in;
        top_in.query = query;
        top_in.server = results.top;
        top_in.songs = results.songs;
        top_in.albums = results.albums;
        top_in.artists = results.artists;
        top_in.saved_albums = library::load_saved_albums();
        top_in.saved_artists = library::load_saved_artists();
        top_in.local_tracks = library::search_tracks(query, 60);
        TopResult top = choose_search_top(top_in);

        // Warm the URLs of the visible results so clicking one starts fast.
        // Depth matches the rows a search page shows; warming runs on the
        // main backend channel and aborts as soon as search_id changes.
        prefetch_urls_for(results.songs, 10, expect_id);
        std::atomic_store(&g_state.search_results,
                          std::make_shared<const std::vector<Track>>(std::move(results.songs)));
        std::atomic_store(&g_state.search_album_results,
                          std::make_shared<const std::vector<AlbumSummary>>(std::move(results.albums)));
        std::atomic_store(&g_state.search_artist_results,
                          std::make_shared<const std::vector<ArtistSummary>>(std::move(results.artists)));
        std::atomic_store(&g_state.search_top,
                          std::make_shared<const TopResult>(std::move(top)));
        g_state.current_playlist_source = PlaylistSource::SEARCH;
        g_state.search_loading = false;
        // Success clears any previous failure state; a genuinely empty result
        // (no hits) keeps search_failed false so "no results" stays honest.
        if (ok)
        {
            g_state.search_failed = false;
            g_state.search_error_msg.clear();
        }
    }
    BackendError err = take_backend_error();
    if (err)
    {
        ui_notify_backend_error(err);
        if (g_state.search_id == expect_id)
        {
            g_state.search_failed = true;
            g_state.search_error_msg = backend_error_user_message(err);
            // A failed query has no card; don't leave the previous query's
            // top result above an error state.
            std::atomic_store(&g_state.search_top,
                              std::make_shared<const TopResult>());
        }
    }
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_state.backend_status = err;
    g_state.repaint_requested = true;
}

// The two-request search page devotes only a few slots to albums/artists, so
// the first time the user opens one of those chips we fetch that category's
// full shelf and merge it into what the summary already published. Merging
// (never replacing) keeps any rows that arrived while this was in flight.
void search_category_and_update(const std::string &query, int category, int expect_id)
{
    if (query.empty())
    {
        g_state.search_loading = false;
        return;
    }

    if (category == 1)
    {
        auto albums = fetch_search_albums(query);
        if (g_state.search_id == expect_id)
        {
            auto current = std::atomic_load(&g_state.search_album_results);
            std::vector<AlbumSummary> merged = current ? *current : std::vector<AlbumSummary>{};
            std::unordered_set<std::string> seen;
            for (const auto &a : merged)
                seen.insert(a.browse_id);
            for (auto &a : albums)
                if (seen.insert(a.browse_id).second)
                    merged.push_back(std::move(a));
            std::atomic_store(&g_state.search_album_results,
                              std::make_shared<const std::vector<AlbumSummary>>(std::move(merged)));
        }
    }
    else if (category == 2)
    {
        auto artists = fetch_artist_search(query);
        if (g_state.search_id == expect_id)
        {
            auto current = std::atomic_load(&g_state.search_artist_results);
            std::vector<ArtistSummary> merged = current ? *current : std::vector<ArtistSummary>{};
            std::unordered_set<std::string> seen;
            for (const auto &a : merged)
                seen.insert(a.browse_id);
            for (auto &a : artists)
                if (seen.insert(a.browse_id).second)
                    merged.push_back(std::move(a));
            std::atomic_store(&g_state.search_artist_results,
                              std::make_shared<const std::vector<ArtistSummary>>(std::move(merged)));
        }
    }

    BackendError err = take_backend_error();
    if (err)
        ui_notify_backend_error(err);
    if (g_state.search_id == expect_id)
        g_state.search_loading = false;
    {
        std::lock_guard<std::mutex> lock(g_data_mutex);
        g_state.backend_status = err;
    }
    g_state.repaint_requested = true;
}

void search_suggest_and_update(const std::string &query, int expect_id)
{
    SearchSuggestions sugg = fetch_search_suggestions(query);
    if (g_state.search_suggest_id != expect_id)
        return;
    std::atomic_store(&g_state.search_suggestions,
                      std::make_shared<const SearchSuggestions>(std::move(sugg)));
    g_state.search_suggest_loading = false;
    g_state.repaint_requested = true;
}

void load_lyrics_worker(std::string track_id, int expect_seq)
{
    Lyrics lx;
    if (!load_lyrics_cache(track_id, lx))
    {
        if (fetch_lyrics(track_id, lx))
            save_lyrics_cache(track_id, lx); // caches real lyrics and "none" results
        else
            lx.ok = false; // fetch failed: don't cache, retry on next play/open
    }

    // Discard if a newer request superseded this one.
    if (g_state.lyrics_seq.load() != expect_seq)
        return;

    auto view = std::make_shared<LyricsView>();
    view->track_id = std::move(track_id);
    view->lyrics = std::move(lx);
    std::atomic_store(&g_state.lyrics, std::shared_ptr<const LyricsView>(std::move(view)));
    g_state.lyrics_loading = false;
    g_state.repaint_requested = true;
}

