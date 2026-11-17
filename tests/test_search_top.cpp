#include "search_top.h"
#include "test_util.h"
using testutil::check;

#include <string>

namespace
{

Track song(const std::string &id, const std::string &title, const std::string &artist,
           int duration_sec = 0)
{
    Track t;
    t.id = id;
    t.title = title;
    t.artist = artist;
    t.duration_sec = duration_sec;
    return t;
}

AlbumSummary album(const std::string &id, const std::string &title, const std::string &artist,
                   const std::string &year = "")
{
    AlbumSummary a;
    a.browse_id = id;
    a.title = title;
    a.artist = artist;
    a.year = year;
    return a;
}

ArtistSummary artist(const std::string &id, const std::string &name)
{
    ArtistSummary a;
    a.browse_id = id;
    a.name = name;
    return a;
}

TopResult server_song(const std::string &id, const std::string &title, const std::string &artist)
{
    TopResult t;
    t.kind = TopKind::Song;
    t.id = id;
    t.title = title;
    t.artist = artist;
    return t;
}

} // namespace

int main()
{
    check(search_top_normalize("Creep (Acoustic)") == "creep", "normalize drops parentheses");
    check(search_top_normalize("  Radiohead!!  ") == "radiohead", "normalize trims punctuation");
    check(search_top_normalize("The Bends [Remaster]") == "the bends", "normalize drops brackets");
    check(search_top_normalize("Sigur Rós") == "sigur rós", "normalize keeps non-ascii");

    check(search_top_is_url("https://music.youtube.com/watch?v=x"), "url https");
    check(search_top_is_url("youtu.be/abc"), "url short link");
    check(!search_top_is_url("radiohead"), "plain query is not a url");

    // Server card for an artist name is trusted as-is.
    {
        SearchTopInputs in;
        in.query = "radiohead";
        in.server.kind = TopKind::Artist;
        in.server.id = "UCr_iyUANcn9OX_yy9piYoLw";
        in.server.title = "Radiohead";
        in.artists.push_back(artist("UCr_iyUANcn9OX_yy9piYoLw", "Radiohead"));
        TopResult out = choose_search_top(in);
        check(out.kind == TopKind::Artist, "artist card kept");
        check(out.title == "Radiohead", "artist card title");
    }

    // Album name -> album card, enriched with the shelf's year.
    {
        SearchTopInputs in;
        in.query = "ok computer";
        in.server.kind = TopKind::Album;
        in.server.id = "MPREb_yXhSI4FCUo6";
        in.server.title = "OK Computer";
        in.albums.push_back(album("MPREb_yXhSI4FCUo6", "OK Computer", "Radiohead", "1997"));
        TopResult out = choose_search_top(in);
        check(out.kind == TopKind::Album, "album card kept");
        check(out.year == "1997", "album card enriched with year");
    }

    // A same-id song row fills in fields the card left empty.
    {
        SearchTopInputs in;
        in.query = "karma police";
        in.server = server_song("karma", "Karma Police", "Radiohead");
        Track row = song("karma", "Karma Police", "Radiohead", 261);
        row.album = "OK Computer";
        in.songs.push_back(row);
        in.albums.push_back(album("okc", "OK Computer", "Radiohead"));
        TopResult out = choose_search_top(in);
        check(out.duration_sec == 261, "song card enriched with duration");
        check(out.duration == "4:21", "song card formats duration");
        check(out.album == "OK Computer", "song card enriched with album");
    }

    // Local library wins on an exact match: the liked version beats a cover.
    {
        SearchTopInputs in;
        in.query = "creep";
        in.server = server_song("dwight", "Creep (Soul Blues Version)", "Dwight Evans");
        in.local_tracks.push_back(song("radiohead-creep", "Creep", "Radiohead", 239));
        TopResult out = choose_search_top(in);
        check(out.kind == TopKind::Song, "local song card kind");
        check(out.id == "radiohead-creep", "liked version wins");
        check(out.artist == "Radiohead", "liked version artist");
    }

    // Saved album exact match beats the server's artist/song guess.
    {
        SearchTopInputs in;
        in.query = "the bends";
        in.server = server_song("random", "The Bends", "Some Cover Band");
        in.saved_albums.push_back(
            {"MB_bends", "The Bends", "Radiohead", "1995", "", "", 12});
        in.albums.push_back(album("MB_bends", "The Bends", "Radiohead", "1995"));
        TopResult out = choose_search_top(in);
        check(out.kind == TopKind::Album, "saved album overrides server");
        check(out.title == "The Bends", "saved album title");
        check(out.track_count == 12, "saved album track count");
    }

    // Saved artist exact match.
    {
        SearchTopInputs in;
        in.query = "portishead";
        in.saved_artists.push_back({"UCportis", "Portishead", "", "8M", ""});
        TopResult out = choose_search_top(in);
        check(out.kind == TopKind::Artist && out.id == "UCportis", "saved artist card");
    }

    // Cover guard: no library match, but the album shelf corroborates the
    // original artist and the song shelf carries it -> swap the card.
    {
        SearchTopInputs in;
        in.query = "creep";
        in.server = server_song("dwight", "Creep", "Dwight Evans");
        in.albums.push_back(album("creep-single", "Creep", "Radiohead", "1992"));
        in.songs.push_back(song("cover", "Creep", "Dwight Evans"));
        in.songs.push_back(song("canon", "Creep", "Radiohead", 239));
        TopResult out = choose_search_top(in);
        check(out.id == "canon", "cover demoted for corroborated original");
        check(out.artist == "Radiohead", "original artist");
    }

    // Cover guard does not swap when nothing corroborates a competing artist.
    {
        SearchTopInputs in;
        in.query = "creep";
        in.server = server_song("dwight", "Creep", "Dwight Evans");
        in.songs.push_back(song("other", "Creep", "Some Other Band"));
        TopResult out = choose_search_top(in);
        check(out.id == "dwight", "unverified cover card kept when no better match");
    }

    // No server card: exact title match, not the first row.
    {
        SearchTopInputs in;
        in.query = "weird fishes";
        in.songs.push_back(song("a", "Weird Fishes / Arpeggi (Live)", "Radiohead"));
        in.songs.push_back(song("b", "Weird Fishes", "Radiohead", 238));
        TopResult out = choose_search_top(in);
        check(out.kind == TopKind::Song && out.id == "b", "synthesized exact song match");
    }

    // No server card, no confident match -> no card at all.
    {
        SearchTopInputs in;
        in.query = "zzzzzz";
        in.songs.push_back(song("a", "Something Else", "Nobody"));
        in.albums.push_back(album("b", "Another Album", "Nobody"));
        TopResult out = choose_search_top(in);
        check(out.kind == TopKind::None, "no confident card");
    }

    // Pasted URLs never produce a card.
    {
        SearchTopInputs in;
        in.query = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
        in.server = server_song("dQw4w9WgXcQ", "Never Gonna Give You Up", "Rick Astley");
        TopResult out = choose_search_top(in);
        check(out.kind == TopKind::None, "url query has no card");
    }

    // "artist + extra words" synthesizes the artist card when the server
    // returned nothing.
    {
        SearchTopInputs in;
        in.query = "radiohead live";
        in.artists.push_back(artist("UCr", "Radiohead"));
        TopResult out = choose_search_top(in);
        check(out.kind == TopKind::Artist && out.id == "UCr", "artist name in query");
    }

    return 0;
}
