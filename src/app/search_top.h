#pragma once

#include "types.h"
#include "ytdlp.h"

#include <string>
#include <vector>

// Inputs for the search "Top result" card. `server` is YouTube Music's own
// intent match (kind == None when the response carried no card); the shelf
// lists and the local library are used to validate it, enrich it, override
// it when the server picked a cover/alternate version, or synthesize a card
// when the server returned none.
struct SearchTopInputs
{
    std::string query;
    TopResult server;
    std::vector<Track> songs;
    std::vector<AlbumSummary> albums;
    std::vector<ArtistSummary> artists;
    std::vector<SavedAlbum> saved_albums;
    std::vector<SavedArtist> saved_artists;
    std::vector<Track> local_tracks;
};

// Case/punctuation/bracket-insensitive form used for matching. Parenthetical
// suffixes are dropped so "Creep (Acoustic)" and "creep" compare equal.
std::string search_top_normalize(const std::string &text);

// True for pasted YouTube URLs; those are navigation, not search intent.
bool search_top_is_url(const std::string &query);

// Policy (see docs in the .cpp):
//  1. start from the server card,
//  2. validate/enrich it against the shelves,
//  3. let exact local-library matches override it,
//  4. demote cover/alternate versions when a corroborated original exists,
//  5. synthesize a card when the server returned none,
//  6. return kind == None when nothing matches confidently.
TopResult choose_search_top(const SearchTopInputs &in);
