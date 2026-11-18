#include "search_top.h"

#include <cctype>
#include <cstdio>

namespace
{

std::string format_seconds(int total)
{
    if (total <= 0)
        return "";
    char buf[32];
    if (total >= 3600)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", total / 3600, (total / 60) % 60,
                      total % 60);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}

TopResult song_card(const Track &t)
{
    TopResult c;
    c.kind = TopKind::Song;
    c.id = t.id;
    c.title = t.title;
    c.artist = t.artist;
    c.artist_id = t.artist_id;
    c.album = t.album;
    c.album_id = t.album_id;
    c.thumbnail_url = t.thumbnail_url;
    c.duration_sec = t.duration_sec;
    c.duration = format_seconds(t.duration_sec);
    return c;
}

TopResult album_card(const AlbumSummary &a)
{
    TopResult c;
    c.kind = TopKind::Album;
    c.id = a.browse_id;
    c.title = a.title;
    c.artist = a.artist;
    c.year = a.year;
    c.thumbnail_url = a.thumbnail_url;
    c.track_count = a.track_count;
    return c;
}

TopResult album_card(const SavedAlbum &a)
{
    TopResult c;
    c.kind = TopKind::Album;
    c.id = a.browse_id;
    c.title = a.title;
    c.artist = a.artist;
    c.year = a.year;
    c.thumbnail_url = a.thumbnail_url;
    c.track_count = a.track_count;
    return c;
}

TopResult artist_card(const ArtistSummary &a)
{
    TopResult c;
    c.kind = TopKind::Artist;
    c.id = a.browse_id;
    c.artist_id = a.browse_id;
    c.title = a.name;
    c.subscribers = a.subscribers;
    c.thumbnail_url = a.thumbnail_url;
    return c;
}

TopResult artist_card(const SavedArtist &a)
{
    TopResult c;
    c.kind = TopKind::Artist;
    c.id = a.browse_id;
    c.artist_id = a.browse_id;
    c.title = a.name;
    c.subscribers = a.subscribers;
    c.thumbnail_url = a.thumbnail_url;
    return c;
}

std::string normalize_lower(const std::string &text)
{
    std::string out = text;
    for (char &ch : out)
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
    return out;
}

// True when the artist name is backed by any shelf row or by the library.
// Used to tell an original from a cover/alternate upload: YouTube's card for
// a song query is sometimes a cover, while the artist's own album sits in the
// albums shelf of the very same response.
bool artist_corroborated(const std::string &artist, const SearchTopInputs &in)
{
    const std::string want = search_top_normalize(artist);
    if (want.empty())
        return false;
    for (const AlbumSummary &a : in.albums)
        if (search_top_normalize(a.artist) == want)
            return true;
    for (const ArtistSummary &a : in.artists)
        if (search_top_normalize(a.name) == want)
            return true;
    for (const SavedArtist &a : in.saved_artists)
        if (search_top_normalize(a.name) == want)
            return true;
    for (const Track &t : in.local_tracks)
        if (search_top_normalize(t.artist) == want)
            return true;
    return false;
}

void enrich_top(TopResult &card, const SearchTopInputs &in)
{
    if (card.kind == TopKind::Song)
    {
        for (const Track &t : in.songs)
        {
            if (t.id != card.id)
                continue;
            if (card.title.empty())
                card.title = t.title;
            if (card.artist.empty())
                card.artist = t.artist;
            if (card.artist_id.empty())
                card.artist_id = t.artist_id;
            if (card.album.empty())
                card.album = t.album;
            if (card.album_id.empty())
                card.album_id = t.album_id;
            if (card.thumbnail_url.empty())
                card.thumbnail_url = t.thumbnail_url;
            if (card.duration_sec == 0)
                card.duration_sec = t.duration_sec;
            if (card.duration.empty())
                card.duration = format_seconds(card.duration_sec);
            break;
        }
    }
    else if (card.kind == TopKind::Album)
    {
        for (const AlbumSummary &a : in.albums)
        {
            if (a.browse_id != card.id)
                continue;
            if (card.title.empty())
                card.title = a.title;
            if (card.artist.empty())
                card.artist = a.artist;
            if (card.year.empty())
                card.year = a.year;
            if (card.thumbnail_url.empty())
                card.thumbnail_url = a.thumbnail_url;
            if (card.track_count == 0)
                card.track_count = a.track_count;
            break;
        }
    }
    else if (card.kind == TopKind::Artist)
    {
        for (const ArtistSummary &a : in.artists)
        {
            if (a.browse_id != card.id)
                continue;
            if (card.title.empty())
                card.title = a.name;
            if (card.subscribers.empty())
                card.subscribers = a.subscribers;
            if (card.thumbnail_url.empty())
                card.thumbnail_url = a.thumbnail_url;
            break;
        }
    }
}

// Exact local-library matches outrank the server: saving an album, following
// an artist or liking a song is the strongest statement of what a query
// means to this user.
bool local_card(const SearchTopInputs &in, TopResult &out)
{
    const std::string q = search_top_normalize(in.query);
    if (q.empty())
        return false;
    for (const Track &t : in.local_tracks)
    {
        if (t.id.empty() || search_top_normalize(t.title) != q)
            continue;
        out = song_card(t);
        return true;
    }
    for (const SavedAlbum &a : in.saved_albums)
    {
        if (a.browse_id.empty() || search_top_normalize(a.title) != q)
            continue;
        out = album_card(a);
        return true;
    }
    for (const SavedArtist &a : in.saved_artists)
    {
        if (a.browse_id.empty() || search_top_normalize(a.name) != q)
            continue;
        out = artist_card(a);
        return true;
    }
    return false;
}

// A song card whose title is the query but whose artist nothing else in the
// response (or the library) backs is likely a cover. Prefer the first
// exact-title song with a corroborated artist -- the filtered song shelf is
// relevance-ranked, so that is the canonical release.
void demote_alternate(TopResult &card, const SearchTopInputs &in)
{
    if (card.kind != TopKind::Song)
        return;
    const std::string q = search_top_normalize(in.query);
    if (q.empty() || search_top_normalize(card.title) != q)
        return;
    if (artist_corroborated(card.artist, in))
        return;
    for (const Track &t : in.songs)
    {
        if (t.id.empty() || search_top_normalize(t.title) != q)
            continue;
        if (!artist_corroborated(t.artist, in))
            continue;
        card = song_card(t);
        return;
    }
}

// No server card: only confident exact matches, never "just the first row".
TopResult synthesize_top(const SearchTopInputs &in)
{
    const std::string q = search_top_normalize(in.query);
    if (q.empty())
        return TopResult{};
    for (const Track &t : in.songs)
        if (!t.id.empty() && search_top_normalize(t.title) == q)
            return song_card(t);
    for (const ArtistSummary &a : in.artists)
        if (!a.browse_id.empty() && search_top_normalize(a.name) == q)
            return artist_card(a);
    for (const AlbumSummary &a : in.albums)
        if (!a.browse_id.empty() && search_top_normalize(a.title) == q)
            return album_card(a);
    // "artist name plus extra words" ("radiohead live") -> their artist page.
    for (const ArtistSummary &a : in.artists)
    {
        const std::string name = search_top_normalize(a.name);
        if (name.size() < 4 || search_top_normalize(a.name) == q)
            continue;
        if (q.rfind(name + " ", 0) == 0 || q.find(" " + name) != std::string::npos)
            return artist_card(a);
    }
    return TopResult{};
}

} // namespace

std::string search_top_normalize(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    int paren = 0;
    int bracket = 0;
    bool pending_space = false;
    for (char ch : text)
    {
        if (ch == '(')
        {
            ++paren;
            continue;
        }
        if (ch == ')')
        {
            if (paren > 0)
                --paren;
            continue;
        }
        if (ch == '[')
        {
            ++bracket;
            continue;
        }
        if (ch == ']')
        {
            if (bracket > 0)
                --bracket;
            continue;
        }
        if (paren > 0 || bracket > 0)
            continue;
        const unsigned char c = (unsigned char)ch;
        if (c >= 128 || std::isalnum(c))
        {
            if (pending_space && !out.empty())
                out.push_back(' ');
            pending_space = false;
            out.push_back(c >= 128 ? ch : (char)std::tolower(c));
        }
        else
        {
            pending_space = true;
        }
    }
    return out;
}

bool search_top_is_url(const std::string &query)
{
    const std::string q = normalize_lower(query);
    if (q.rfind("http://", 0) == 0 || q.rfind("https://", 0) == 0)
        return true;
    return q.find("youtu.be/") != std::string::npos ||
           q.find("youtube.com/") != std::string::npos;
}

TopResult choose_search_top(const SearchTopInputs &in)
{
    if (search_top_is_url(in.query))
        return TopResult{};
    if (search_top_normalize(in.query).empty())
        return TopResult{};

    TopResult card = in.server;
    enrich_top(card, in);

    TopResult local;
    if (local_card(in, local))
        return local;

    if (card.kind == TopKind::None)
        return synthesize_top(in);

    demote_alternate(card, in);
    return card;
}
