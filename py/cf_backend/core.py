"""Shared command loop and YouTube Music helpers for both auth backends.

The entry points (p100-backend.py and p100-backend-oauth.py) only differ in
how they create the YTMusic session. Everything below is shared so a command
cannot work in one backend and silently fail in the other.
"""

import base64
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import html as _html
from concurrent.futures import ThreadPoolExecutor

YTDLP = os.environ.get('P100_YTDLP') or 'yt-dlp'

# crossfade is a GUI app on Windows: child console processes must not get a
# console window of their own.
_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0) if sys.platform == "win32" else 0

# video_id -> (resolved_at, url); avoids re-running yt-dlp for repeat plays.
_STREAM_TTL = 7200  # googlevideo URLs are valid ~6h; probed before caching
_stream_cache = {}
_stream_lock = threading.Lock()

# Resolved URLs are shared with the other backend processes (play, prefetch)
# through a small on-disk cache: a URL warmed by one channel is usable by
# another, and repeat plays across restarts skip extraction entirely.
_STREAM_DISK_MAX = 400
_stream_disk_loaded = False
_stream_save_lock = threading.Lock()


def _stream_disk_path():
    return os.path.join(_config_dir(), 'stream_urls.json')


def _load_stream_disk():
    global _stream_disk_loaded
    if _stream_disk_loaded:
        return
    _stream_disk_loaded = True
    try:
        with open(_stream_disk_path(), encoding='utf-8') as f:
            data = json.load(f)
    except Exception:
        return
    if not isinstance(data, dict):
        return
    now = time.time()
    with _stream_lock:
        for vid, entry in data.items():
            if not isinstance(entry, list) or len(entry) != 2:
                continue
            ts, url = entry
            if (isinstance(ts, (int, float)) and isinstance(url, str) and url
                    and now - ts < _STREAM_TTL and vid not in _stream_cache):
                _stream_cache[vid] = (float(ts), url)


def _stream_disk_lookup(video_id):
    """Re-read the shared cache: another channel may have warmed this id."""
    try:
        with open(_stream_disk_path(), encoding='utf-8') as f:
            data = json.load(f)
    except Exception:
        return None
    entry = data.get(video_id) if isinstance(data, dict) else None
    if not (isinstance(entry, list) and len(entry) == 2):
        return None
    ts, url = entry
    if not (isinstance(ts, (int, float)) and isinstance(url, str) and url):
        return None
    if time.time() - ts >= _STREAM_TTL:
        return None
    with _stream_lock:
        _stream_cache.setdefault(video_id, (float(ts), url))
    return url


def _save_stream_disk():
    now = time.time()
    with _stream_lock:
        items = [(vid, entry) for vid, entry in _stream_cache.items()
                 if now - entry[0] < _STREAM_TTL]
    items.sort(key=lambda kv: kv[1][0], reverse=True)
    payload = {vid: [entry[0], entry[1]] for vid, entry in items[:_STREAM_DISK_MAX]}
    tmp = '%s.%d.tmp' % (_stream_disk_path(), os.getpid())
    with _stream_save_lock:
        try:
            os.makedirs(os.path.dirname(tmp), exist_ok=True)
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(payload, f, separators=(',', ':'))
            os.replace(tmp, _stream_disk_path())
        except OSError:
            pass


def ensure_config_dir(path):
    """Create the config directory owner-only (0700)."""
    os.makedirs(path, mode=0o700, exist_ok=True)
    try:
        os.chmod(path, 0o700)
    except OSError:
        pass


def secure_auth_file(path):
    """Best-effort 0600 for cookie/OAuth token files."""
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass


def _album_fields(track):
    """Return (album_name, album_browse_id) from a ytmusicapi item.

    Search results carry album as a dict ({'name', 'id'}), while tracks
    inside an album carry it as a plain string (the id is the parent album).
    """
    album = track.get('album')
    if isinstance(album, dict):
        return album.get('name') or '', album.get('id') or ''
    if isinstance(album, str):
        return album, ''
    return '', ''


def _duration_sec(track):
    """Parse ytmusicapi's various duration representations into seconds."""
    secs = track.get('duration_seconds')
    if isinstance(secs, (int, float)) and secs > 0:
        return int(secs)
    dur = track.get('duration')
    if isinstance(dur, (int, float)) and dur > 0:
        return int(dur)
    if isinstance(dur, str) and dur:
        try:
            parts = [int(p) for p in dur.split(':')]
        except ValueError:
            return 0
        total = 0
        for p in parts:
            total = total * 60 + p
        return total
    return 0


def _artist_id(track):
    """First artist/channel id from a ytmusicapi track item, if present."""
    artists = track.get('artists') or []
    if artists and isinstance(artists[0], dict):
        return artists[0].get('id') or artists[0].get('browseId') or ''
    if artists and isinstance(artists[0], str):
        return ''
    return ''


# Error kinds sent alongside {'error': ...} so the C++ UI can react
# appropriately (sign-in banner vs. offline message vs. retry) without
# substring-guessing the message text.
_AUTH_MARKERS = ('auth', 'cookie', 'oauth', 'browser.json', 'forbidden',
                 'unauthorized', '401', '403', 'sign in', 'sign-in', 'logged')
_NETWORK_MARKERS = ('name resolution', 'name or service not known',
                    'temporary failure', 'network is unreachable',
                    'connection refused', 'connection reset', 'connection aborted',
                    'timed out', 'timeout', 'getaddrinfo failed',
                    'max retries exceeded with url', 'connectionerror',
                    'read error', 'httpsconnectionpool', 'failed to resolve',
                    'network')


def _classify_error(msg):
    m = (msg or '').lower()
    if any(k in m for k in _AUTH_MARKERS):
        return 'auth'
    if any(k in m for k in _NETWORK_MARKERS):
        return 'network'
    return 'backend'


def _err(msg, kind=None):
    """Standard error payload; kind defaults to a message-based guess."""
    return {'error': msg, 'kind': kind or _classify_error(msg)}


def _thumb_url(item, size=0):
    """Largest thumbnail URL for a ytmusicapi item.

    YouTube Music only advertises 120px art for search cards, but its image
    CDN serves the same asset at any requested width. `size` rewrites those
    Google-hosted URLs so a hero-sized card isn't upscaled from 120px.
    """
    thumbs = item.get('thumbnails') or item.get('thumbnail') or []
    if thumbs and isinstance(thumbs[-1], dict):
        url = thumbs[-1].get('url', '')
        if size and 'googleusercontent.com' in url:
            url = re.sub(r'=w\d+-h\d+', '=w%d-h%d' % (size, size), url)
        return url
    return ''


def track_to_dict(track):
    video_id = track.get('videoId') or track.get('id')
    title = track.get('title', 'Unknown')
    artists = track.get('artists', [])
    if isinstance(artists, list) and artists and isinstance(artists[0], dict):
        artist_name = ', '.join(a['name'] for a in artists)
    else:
        artist_name = str(artists[0]) if artists else ''
    # Search/home items use "thumbnails"; radio/watch-playlist items use the
    # singular "thumbnail" key. _thumb_url accepts both.
    thumb_url = _thumb_url(track)
    album_name, album_id = _album_fields(track)
    return {
        'id': video_id,
        'title': title,
        'artist': artist_name,
        'artist_id': _artist_id(track),
        'album': album_name,
        'album_id': album_id,
        'track_number': track.get('trackNumber') or 0,
        'thumbnail_url': thumb_url,
        'duration_sec': _duration_sec(track),
        'url': f'https://www.youtube.com/watch?v={video_id}'
    }


def get_track_meta(yt, video_id):
    """Metadata-only lookup for party mode. Never resolves a stream."""
    vid = (video_id or '').strip()
    if not re.fullmatch(r'[A-Za-z0-9_-]{11}', vid):
        return _err('Invalid video id', kind='backend')
    details = {}
    try:
        song = yt.get_song(vid)
        details = (song or {}).get('videoDetails') or {}
    except Exception:
        details = {}
    if not details:
        try:
            for item in yt.search(vid, filter='songs', limit=5) or []:
                if (item.get('videoId') or item.get('id') or '') == vid:
                    d = track_to_dict(item)
                    return {
                        'ok': True,
                        'id': vid,
                        'title': d['title'],
                        'artist': d['artist'],
                        'durationSec': d['duration_sec'],
                        'thumbUrl': d['thumbnail_url'],
                    }
        except Exception as e:
            return _err(str(e))
        return _err('Video not found', kind='not_found')
    title = details.get('title') or ''
    artist = details.get('author') or ''
    try:
        length = int(details.get('lengthSeconds') or 0)
    except (TypeError, ValueError):
        length = 0
    thumbs = ((details.get('thumbnail') or {}).get('thumbnails') or [])
    thumb = thumbs[-1].get('url', '') if thumbs else f'https://i.ytimg.com/vi/{vid}/hqdefault.jpg'
    if not title:
        return _err('Video not found', kind='not_found')
    if details.get('isLive'):
        return _err('Live streams are not supported', kind='backend')
    return {
        'ok': True,
        'id': vid,
        'title': title,
        'artist': artist,
        'durationSec': length,
        'thumbUrl': thumb,
    }


def get_album_data(yt, browse_id):
    """Fetch an album header + its ordered track list by browse id (MPRE...)."""
    if not browse_id:
        return {'error': 'Missing album id'}
    try:
        album = yt.get_album(browse_id)
    except Exception as e:
        return {'error': f'Failed to fetch album: {str(e)}'}
    if not album:
        return {'error': 'Album not found'}

    artists = album.get('artists') or []
    if artists and isinstance(artists[0], dict):
        artist_name = ', '.join(a.get('name', '') for a in artists)
    elif artists:
        artist_name = ', '.join(str(a) for a in artists)
    else:
        artist_name = ''

    thumbs = album.get('thumbnails') or []
    thumb_url = ''
    if thumbs and isinstance(thumbs[-1], dict):
        thumb_url = thumbs[-1].get('url', '')

    album_title = album.get('title') or 'Unknown Album'
    tracks = []
    for item in album.get('tracks') or []:
        if not isinstance(item, dict):
            continue
        if not (item.get('videoId') or item.get('id')):
            continue
        try:
            t = track_to_dict(item)
        except Exception:
            continue
        # Album items don't repeat the parent browse id; add it so the UI
        # can offer "view album" on every row without another lookup.
        t['album'] = t.get('album') or album_title
        t['album_id'] = browse_id
        if not t.get('track_number'):
            t['track_number'] = len(tracks) + 1
        tracks.append(t)

    if not tracks:
        return {'error': 'Album has no playable tracks.'}

    year = album.get('year') or ''
    return {
        'browse_id': browse_id,
        'title': album_title,
        'artist': artist_name,
        'year': str(year),
        'track_count': album.get('trackCount') or len(tracks),
        'duration': album.get('duration') or '',
        'description': album.get('description') or '',
        'thumbnail_url': thumb_url,
        'audio_playlist_id': album.get('audioPlaylistId') or '',
        'tracks': tracks,
    }


def album_for_track(yt, video_id):
    """Resolve the album browse id for a song, when it isn't already known.

    Home/radio items don't always carry album metadata, so fall back to a
    song search and match the exact video id (or the same title+artist).
    """
    if not video_id:
        return {'error': 'Missing video_id'}
    title = artist = ''
    try:
        watch = yt.get_watch_playlist(videoId=video_id, limit=1) or {}
        seed = (watch.get('tracks') or [None])[0] or {}
        title = seed.get('title') or ''
        artists = seed.get('artists') or []
        if artists and isinstance(artists[0], dict):
            artist = artists[0].get('name') or ''
        name, album_id = _album_fields(seed)
        if album_id:
            return {'album_id': album_id, 'album_name': name}
    except Exception:
        pass

    if not title:
        return {'error': 'Could not identify the track.'}
    try:
        results = yt.search((title + ' ' + artist).strip(), filter='songs', limit=15)
    except Exception as e:
        return {'error': f'Album lookup failed: {str(e)}'}

    for r in results:
        if not isinstance(r, dict):
            continue
        name, album_id = _album_fields(r)
        if not album_id:
            continue
        if r.get('videoId') == video_id:
            return {'album_id': album_id, 'album_name': name}
    # No exact video id: accept an identical (normalised) title + artist.
    for r in results:
        if not isinstance(r, dict):
            continue
        name, album_id = _album_fields(r)
        if not album_id or _norm(r.get('title')) != _norm(title):
            continue
        cand_artists = r.get('artists') or []
        cand_artist = cand_artists[0].get('name', '') if cand_artists and isinstance(cand_artists[0], dict) else ''
        if not artist or _norm(artist) in _norm(cand_artist):
            return {'album_id': album_id, 'album_name': name}
    return {'error': 'No album found for this track.'}


def get_artist_data(yt, channel_id):
    """Fetch an artist header, top songs and top albums/singles."""
    if not channel_id:
        return {'error': 'Missing artist id'}
    try:
        artist = yt.get_artist(channel_id)
    except Exception as e:
        return {'error': f'Failed to fetch artist: {str(e)}'}
    if not artist:
        return {'error': 'Artist not found'}

    name = artist.get('name') or 'Unknown Artist'
    description = artist.get('description') or ''

    songs = []
    song_sec = artist.get('songs') or {}
    songs_browse_id = song_sec.get('browseId') or ''
    for item in song_sec.get('results') or []:
        if not isinstance(item, dict):
            continue
        if not (item.get('videoId') or item.get('id')):
            continue
        try:
            songs.append(track_to_dict(item))
        except Exception:
            continue

    albums = []
    seen = set()
    for key, default_type in (('albums', 'Album'), ('singles', 'Single')):
        section = artist.get(key) or {}
        for item in section.get('results') or []:
            if not isinstance(item, dict):
                continue
            browse_id = item.get('browseId') or ''
            if not browse_id or browse_id in seen:
                continue
            seen.add(browse_id)
            albums.append({
                'browse_id': browse_id,
                'title': item.get('title') or 'Unknown Album',
                'year': str(item.get('year') or ''),
                'thumbnail_url': _thumb_url(item),
                'type': item.get('type') or default_type,
            })

    return {
        'browse_id': channel_id,
        'name': name,
        'description': description,
        'subscribers': artist.get('subscribers') or '',
        'monthly_listeners': artist.get('monthlyListeners') or '',
        'thumbnail_url': _thumb_url(artist),
        'songs': songs,
        'songs_browse_id': songs_browse_id,
        'albums': albums,
    }


_ARTIST_SONGS_CACHE = {}


def get_artist_songs(yt, browse_id, offset, count):
    """Fetch a slice of the artist's full songs shelf.

    The artist header only returns a handful of top songs. Its songs browseId
    is a playlist-like endpoint with the full list; cache that list in the
    backend so consecutive "load more" clicks are cheap.
    """
    if not browse_id:
        return {'error': 'Missing songs browse id'}
    cached = _ARTIST_SONGS_CACHE.get(browse_id)
    if cached is None:
        try:
            playlist = yt.get_playlist(browse_id, limit=None)
        except Exception as e:
            return {'error': f'Failed to fetch artist songs: {str(e)}'}
        tracks = []
        for item in playlist.get('tracks') or []:
            if not isinstance(item, dict):
                continue
            if not (item.get('videoId') or item.get('id')):
                continue
            try:
                tracks.append(track_to_dict(item))
            except Exception:
                continue
        if len(_ARTIST_SONGS_CACHE) >= 8:
            _ARTIST_SONGS_CACHE.clear()
        _ARTIST_SONGS_CACHE[browse_id] = tracks
        cached = tracks
    offset = max(0, int(offset))
    count = max(0, int(count))
    return cached[offset:offset + count]


def artist_for_track(yt, video_id):
    """Resolve the artist channel id for a song."""
    if not video_id:
        return {'error': 'Missing video_id'}
    seed = {}
    try:
        watch = yt.get_watch_playlist(videoId=video_id, limit=1) or {}
        seed = (watch.get('tracks') or [None])[0] or {}
        artists = seed.get('artists') or []
        if artists and isinstance(artists[0], dict):
            aid = artists[0].get('id') or artists[0].get('browseId') or ''
            if aid:
                return {'artist_id': aid, 'artist_name': artists[0].get('name') or ''}
    except Exception:
        pass

    title = seed.get('title') or ''
    artist_name = seed.get('artists', [{}])[0].get('name', '') if seed.get('artists') else ''
    if not title:
        return {'error': 'Could not identify the track.'}
    try:
        results = yt.search((title + ' ' + artist_name).strip(), filter='songs', limit=10)
    except Exception as e:
        return {'error': f'Artist lookup failed: {str(e)}'}
    for r in results:
        if not isinstance(r, dict) or r.get('videoId') != video_id:
            continue
        artists = r.get('artists') or []
        if artists and isinstance(artists[0], dict):
            aid = artists[0].get('id') or ''
            if aid:
                return {'artist_id': aid, 'artist_name': artists[0].get('name') or ''}
    return {'error': 'No artist found for this track.'}


def search_artists(yt, query, limit=20):
    """Search YouTube Music artists."""
    if not query:
        return {'error': 'Missing query'}
    try:
        results = yt.search(query, filter='artists', limit=limit)
    except Exception as e:
        return {'error': f'Artist search failed: {str(e)}'}

    out = []
    seen = set()
    for r in results or []:
        if not isinstance(r, dict):
            continue
        browse_id = r.get('browseId') or ''
        if not browse_id or browse_id in seen:
            continue
        seen.add(browse_id)
        out.append({
            'browse_id': browse_id,
            'name': r.get('artist') or r.get('title') or 'Unknown Artist',
            'thumbnail_url': _thumb_url(r),
            'subscribers': r.get('subscribers') or '',
        })
    return out


def search_albums(yt, query, limit=20):
    if not query:
        return {'error': 'Missing query'}
    try:
        results = yt.search(query, filter='albums', limit=limit)
    except Exception as e:
        return {'error': f'Album search failed: {str(e)}'}

    out = []
    seen = set()
    for r in results or []:
        if not isinstance(r, dict):
            continue
        browse_id = r.get('browseId') or ''
        if not browse_id or browse_id in seen:
            continue
        seen.add(browse_id)
        artists = r.get('artists') or []
        if isinstance(artists, list) and artists and isinstance(artists[0], dict):
            artist_name = ', '.join(a.get('name', '') for a in artists)
        elif artists:
            artist_name = ', '.join(str(a) for a in artists)
        else:
            artist_name = ''
        out.append({
            'browse_id': browse_id,
            'title': r.get('title') or 'Unknown Album',
            'artist': artist_name,
            'year': str(r.get('year') or ''),
            'thumbnail_url': _thumb_url(r),
            'track_count': r.get('trackCount') or 0,
        })
    return out


def _artist_name(item):
    artists = item.get('artists') or []
    if artists and isinstance(artists[0], dict):
        return ', '.join(a.get('name') or '' for a in artists).strip(', ')
    return item.get('artist') or item.get('title') or ''


def _top_card(item):
    """Normalise YouTube Music's own "Top result" card for the UI.

    The server resolves query intent (artist name -> artist card, album name
    -> album card, track -> song card); this only reshapes the fields. The
    validation/override policy lives in C++, next to the local library data.
    """
    kind = (item.get('resultType') or '').lower()
    if kind not in ('song', 'video', 'album', 'artist'):
        return None
    artists = [a for a in (item.get('artists') or []) if isinstance(a, dict)]
    artist_name = _artist_name(item)
    artist_id = artists[0].get('id') or '' if artists else ''
    title = item.get('title') or ''
    if kind == 'artist':
        title = artist_name or title
    if not title:
        return None
    if kind in ('song', 'video'):
        card_id = item.get('videoId') or ''
    elif kind == 'album':
        card_id = item.get('browseId') or ''
    else:
        card_id = artist_id
    if not card_id:
        return None
    album_name, album_id = _album_fields(item)
    return {
        'kind': kind,
        'id': card_id,
        'title': title,
        'artist': '' if kind == 'artist' else artist_name,
        'artist_id': artist_id,
        'album': album_name,
        'album_id': album_id,
        'year': str(item.get('year') or ''),
        'duration': str(item.get('duration') or ''),
        'duration_sec': _duration_sec(item),
        'subscribers': str(item.get('subscribers') or ''),
        # Search cards advertise 120px art; the CDN serves hero size.
        'thumbnail_url': _thumb_url(item, size=544),
    }


def _suggestion_runs_text(runs):
    return ''.join((r.get('text') or '') for r in (runs or []) if isinstance(r, dict))


def get_search_suggestions(yt, query):
    """Typeahead suggestions + the music items YouTube recommends for them.

    The endpoint's first section is query suggestions (history entries when the
    query repeats something the account searched before); the second is a small
    set of songs/albums/artists. Playlists and podcasts are dropped: the
    suggestion UI only navigates entities the app can open."""
    try:
        raw = yt._send_request('music/get_search_suggestions', {'input': query or ''})
    except Exception as e:
        return _err(f'Suggestions failed: {e}')

    queries, items = [], []
    for section in raw.get('contents') or []:
        contents = ((section.get('searchSuggestionsSectionRenderer') or {})
                    .get('contents')) or []
        for entry in contents:
            if not isinstance(entry, dict):
                continue
            renderer = (entry.get('searchSuggestionRenderer') or
                        entry.get('historySuggestionRenderer'))
            if renderer:
                text = _suggestion_runs_text((renderer.get('suggestion') or {}).get('runs'))
                if text:
                    queries.append({
                        'text': text,
                        'history': 'historySuggestionRenderer' in entry,
                    })
                continue

            r = entry.get('musicResponsiveListItemRenderer')
            if not isinstance(r, dict):
                continue
            cols = []
            for c in r.get('flexColumns') or []:
                runs = (((c.get('musicResponsiveListItemFlexColumnRenderer') or {})
                         .get('text') or {}).get('runs')) or []
                cols.append(_suggestion_runs_text(runs))
            title = cols[0] if cols else ''
            meta = cols[1] if len(cols) > 1 else ''
            extra = cols[2] if len(cols) > 2 else ''
            parts = [p.strip() for p in meta.split('•')] if meta else []
            kind = ''
            if parts and parts[0].lower() in (
                    'song', 'video', 'album', 'artist', 'playlist', 'podcast', 'episode'):
                kind = parts[0].lower()
                parts = parts[1:]
            elif 'monthly audience' in meta.lower() or 'subscriber' in meta.lower():
                kind = 'artist'
            elif extra:
                kind = 'song'
            if not title or kind not in ('song', 'video', 'album', 'artist'):
                continue

            nav = r.get('navigationEndpoint') or {}
            pid = r.get('playlistItemData') or {}
            video_id = pid.get('videoId') or (nav.get('watchEndpoint') or {}).get('videoId') or ''
            browse_id = (nav.get('browseEndpoint') or {}).get('browseId') or ''
            item_id = video_id if kind in ('song', 'video') else browse_id
            if not item_id:
                continue

            artist = ''
            if parts:
                artist = parts[0]
            elif kind == 'artist':
                artist = title
            # "228M plays" is metadata noise in a compact dropdown.
            cleaned = [p for p in parts if p and not re.fullmatch(r'[\d.,]+\s*[KMB]?\s*plays', p)]
            subtitle_parts = cleaned[:1]
            if kind in ('song', 'video') and extra:
                subtitle_parts.append(extra)
            elif kind in ('album', 'artist') and len(cleaned) > 1:
                subtitle_parts.append(cleaned[1])
            if kind == 'artist':
                subtitle_parts = [meta] if meta else []
            thumbs = (((r.get('thumbnail') or {}).get('musicThumbnailRenderer') or {})
                      .get('thumbnail') or {}).get('thumbnails') or []
            thumb = thumbs[-1].get('url', '') if thumbs and isinstance(thumbs[-1], dict) else ''
            items.append({
                'kind': kind,
                'id': item_id,
                'title': title,
                'artist': artist,
                'album': extra if kind in ('song', 'video') else (title if kind == 'album' else ''),
                'subtitle': ' · '.join(p for p in subtitle_parts if p),
                'thumbnail_url': thumb,
            })

    return {'queries': queries, 'items': items}


def search_all(yt, query, limit=20):
    """Fetch one search page in two parallel requests.

    1. the default (unfiltered) search -- it carries YouTube Music's own
       "Top result" card plus the album/artist/playlist/video shelves;
    2. the song-filtered search -- the default page only devotes 5-10 slots
       to songs because podcasts and episodes share the page.

    Both run concurrently, so first paint costs the same as a single call.
    Either request may fail without taking the whole search down."""
    if not query:
        return {'error': 'Missing query'}

    def _run(fn):
        try:
            return fn() or [], None
        except Exception as e:
            return [], str(e)

    with ThreadPoolExecutor(max_workers=2) as pool:
        fut_summary = pool.submit(_run, lambda: yt.search(query, limit=40))
        fut_songs = pool.submit(_run, lambda: yt.search(query, filter='songs', limit=limit))
        summary, summary_err = fut_summary.result()
        songs, songs_err = fut_songs.result()

    if not summary and not songs and (summary_err or songs_err):
        return _err(songs_err or summary_err or 'Search failed')

    top = None
    albums, artists = [], []
    seen_albums, seen_artists = set(), set()
    for item in summary:
        if not isinstance(item, dict):
            continue
        category = item.get('category')
        if category == 'More from YouTube':
            continue  # the top card's related strip; the song shelf covers it
        if category == 'Top result':
            if top is None:
                top = _top_card(item)
            continue
        result_type = (item.get('resultType') or '').lower()
        if result_type == 'album':
            browse_id = item.get('browseId') or ''
            if not browse_id or browse_id in seen_albums:
                continue
            seen_albums.add(browse_id)
            albums.append({
                'browse_id': browse_id,
                'title': item.get('title') or 'Unknown Album',
                'artist': _artist_name(item),
                'year': str(item.get('year') or ''),
                'thumbnail_url': _thumb_url(item),
                'track_count': item.get('trackCount') or 0,
            })
        elif result_type == 'artist':
            name = _artist_name(item)
            item_artists = item.get('artists') or []
            browse_id = item.get('browseId') or ''
            if not browse_id and item_artists and isinstance(item_artists[0], dict):
                browse_id = item_artists[0].get('id') or ''
            if not browse_id or not name or browse_id in seen_artists:
                continue
            seen_artists.add(browse_id)
            artists.append({
                'browse_id': browse_id,
                'name': name,
                'thumbnail_url': _thumb_url(item),
                'subscribers': str(item.get('subscribers') or ''),
            })

    tracks, seen_songs = [], set()
    extra = [i for i in summary
             if isinstance(i, dict) and
             (i.get('resultType') or '').lower() in ('song', 'video')]
    for item in list(songs) + extra:
        if not isinstance(item, dict):
            continue
        video_id = item.get('videoId') or item.get('id')
        if not video_id or video_id in seen_songs or not item.get('title'):
            continue
        seen_songs.add(video_id)
        try:
            tracks.append(track_to_dict(item))
        except Exception:
            continue

    return {'top': top, 'songs': tracks, 'albums': albums, 'artists': artists}


def _duration_seconds(value):
    """Normalise YT Music's human duration ("1 hr 25 min", "45 min") or seconds."""
    if isinstance(value, (int, float)):
        return int(value)
    text = str(value or "").strip().lower()
    if not text:
        return 0
    m = re.fullmatch(r"(\d+):(\d{1,2})(?::(\d{1,2}))?", text)
    if m:
        parts = [int(x) for x in m.groups() if x is not None]
        if len(parts) == 2:
            return parts[0] * 60 + parts[1]
        return parts[0] * 3600 + parts[1] * 60 + parts[2]
    total = 0
    for amount, unit in re.findall(r"(\d+)\s*(hr|hour|min|minute|sec|second)", text):
        n = int(amount)
        if unit.startswith("h"):
            total += n * 3600
        elif unit.startswith("m"):
            total += n * 60
        else:
            total += n
    return total


def _podcast_show(item):
    channel = item.get("channel") or item.get("author") or {}
    if not isinstance(channel, dict):
        channel = {}
    return {
        "browse_id": item.get("browseId") or item.get("podcastId") or "",
        "title": item.get("title") or item.get("name") or "",
        "author": channel.get("name") or item.get("author_name") or "",
        "thumbnail_url": _thumb_url(item),
        "description": item.get("description") or "",
        "saved": bool(item.get("saved")),
    }


def _podcast_episode(item, show=None):
    show = show or {}
    return {
        "video_id": item.get("videoId") or item.get("id") or "",
        "title": item.get("title") or "",
        "author": show.get("author") or item.get("author") or "",
        "thumbnail_url": _thumb_url(item) or show.get("thumbnail_url") or "",
        "duration_sec": _duration_seconds(item.get("duration") or item.get("duration_seconds")),
        "date": item.get("date") or "",
        "description": item.get("description") or "",
    }


def get_podcast_library(yt, limit=100):
    try:
        items = yt.get_library_podcasts(limit=limit)
    except Exception as e:
        return _err(f"Could not load podcasts: {e}")
    return [_podcast_show(x) for x in items if (x.get("title") or "").lower() != "episodes for later"]


def search_podcasts(yt, query, limit=20):
    try:
        items = yt.search(query, filter="podcasts", limit=limit)
    except Exception as e:
        return _err(f"Podcast search failed: {e}")
    return [_podcast_show(x) for x in items]


def get_podcast_details(yt, browse_id, limit=100):
    if not browse_id:
        return {"error": "Missing podcast id"}
    try:
        data = yt.get_podcast(browse_id, limit=limit)
    except Exception as e:
        return _err(f"Could not load podcast: {e}")
    show = _podcast_show(data)
    if not show.get("browse_id"):
        show["browse_id"] = browse_id
    show["saved"] = bool(data.get("saved"))
    episodes = [_podcast_episode(x, show) for x in (data.get("episodes") or [])]
    return {"show": show, "episodes": episodes}


def _spotify_id(link):
    link = (link or "").strip()
    m = re.search(r"(?:playlist[/:])([A-Za-z0-9]+)", link)
    if m:
        return m.group(1)
    if re.fullmatch(r"[A-Za-z0-9]{16,}", link):
        return link
    return ""


def _spotify_cookie():
    path = os.path.join(
        os.environ.get("P100_CONFIG_DIR")
        or os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "crossfade"),
        "spotify.json")
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f).get("sp_dc") or None
    except Exception:
        return None


SPOTIFY_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36")
SPOTIFY_BOOTSTRAP = "37i9dQZF1DXcBWIGoYBM5M"
SPOTIFY_OPS = ("fetchPlaylist", "fetchLibraryTracks", "libraryV3", "profileAttributes")


def _config_dir():
    return (os.environ.get("P100_CONFIG_DIR")
            or os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"),
                            "crossfade"))


def _spotify_progress_path():
    return os.path.join(_config_dir(), "spotify_import_progress.json")


def _spotify_progress(phase, done, total, name):
    path = _spotify_progress_path()
    tmp = path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"phase": phase, "done": int(done), "total": int(total),
                       "name": name or ""}, f, ensure_ascii=False,
                      separators=(",", ":"))
        os.replace(tmp, path)
    except OSError:
        pass


def _spotify_get(url, headers=None, timeout=30):
    req = urllib.request.Request(url, headers=headers or {})
    return urllib.request.urlopen(req, timeout=timeout).read()


def _spotify_token(playlist_id=None):
    """The web player's user-scoped token, read from a playlist embed page.

    The embed page includes `settings.session.accessToken`. Unlike
    /api/token (which Spotify blocks for many networks), this works with just
    the sp_dc cookie and is valid for the signed-in account.
    """
    headers = {"User-Agent": SPOTIFY_UA, "Accept-Language": "en"}
    sp_dc = _spotify_cookie()
    if sp_dc:
        headers["Cookie"] = f"sp_dc={sp_dc}"
    pid = playlist_id or SPOTIFY_BOOTSTRAP
    page = _spotify_get(f"https://open.spotify.com/embed/playlist/{pid}",
                        headers).decode("utf-8", "replace")
    m = re.search(r'<script id="__NEXT_DATA__" type="application/json">(.*?)</script>',
                  page, re.S)
    if not m:
        raise RuntimeError("Spotify did not return a playlist page")
    data = json.loads(m.group(1))
    session = (data.get("props", {}).get("pageProps", {}).get("state", {})
               .get("settings", {}).get("session") or {})
    token = session.get("accessToken")
    if not token:
        raise RuntimeError("Spotify did not return an access token")
    return token, bool(session.get("isAnonymous"))


def _spotify_hashes():
    """Persisted-query hashes from the web-player bundle, cached per bundle."""
    cache = os.path.join(_config_dir(), "spotify_queries.json")
    try:
        with open(cache, encoding="utf-8") as f:
            saved = json.load(f)
    except Exception:
        saved = {}
    html = _spotify_get("https://open.spotify.com", {"User-Agent": SPOTIFY_UA}).decode(
        "utf-8", "replace")
    m = re.search(r'<script[^>]+src="([^"]+)"', html)
    bundle = None
    for src in re.findall(r'<script[^>]+src="([^"]+)"', html):
        if "web-player/web-player" in src and src.endswith(".js"):
            bundle = src
            break
    if not bundle:
        raise RuntimeError("Spotify's web-player bundle was not found")
    if saved.get("bundle") == bundle and saved.get("ops", {}).get("fetchPlaylist"):
        return saved["ops"]
    js = _spotify_get(bundle, {"User-Agent": SPOTIFY_UA}).decode("utf-8", "replace")
    ops = {}
    for op in SPOTIFY_OPS:
        hit = re.search(r'"%s","query","([0-9a-f]{64})"' % re.escape(op), js)
        if hit:
            ops[op] = hit.group(1)
    if "fetchPlaylist" not in ops:
        raise RuntimeError("Spotify's playlist query was not found")
    try:
        with open(cache, "w", encoding="utf-8") as f:
            json.dump({"bundle": bundle, "ops": ops}, f)
    except OSError:
        pass
    return ops


def _spotify_query(token, operation, variables):
    ops = _spotify_hashes()
    h = ops.get(operation)
    if not h:
        raise RuntimeError(f"Spotify's {operation} query is unavailable")
    ext = json.dumps({"persistedQuery": {"version": 1, "sha256Hash": h}},
                     separators=(",", ":"))
    params = urllib.parse.urlencode({
        "operationName": operation,
        "variables": json.dumps(variables, separators=(",", ":")),
        "extensions": ext,
    })
    data = json.loads(_spotify_get(
        "https://api-partner.spotify.com/pathfinder/v1/query?" + params,
        {"Authorization": f"Bearer {token}", "User-Agent": SPOTIFY_UA,
         "Accept": "application/json"}))
    if data.get("errors"):
        raise RuntimeError(str(data["errors"][0].get("message") or data["errors"][0]))
    return data.get("data") or {}


def _spotify_image(obj):
    """Pick the smallest usable image URL from Spotify's nested image shapes."""
    if isinstance(obj, dict):
        if isinstance(obj.get("sources"), list):
            sources = [x for x in obj["sources"] if isinstance(x, dict) and x.get("url")]
            sources.sort(key=lambda x: x.get("width") or 0)
            chosen = next((x for x in sources if (x.get("width") or 0) >= 60), None)
            if chosen or sources:
                return (chosen or sources[0])["url"]
        for key in ("image", "images", "coverArt"):
            if key in obj:
                found = _spotify_image(obj[key])
                if found:
                    return found
        for value in obj.values():
            if isinstance(value, (dict, list)):
                found = _spotify_image(value)
                if found:
                    return found
    elif isinstance(obj, list):
        for value in obj:
            found = _spotify_image(value)
            if found:
                return found
    return ""


def _spotify_library():
    token, anonymous = _spotify_token()
    if anonymous:
        raise RuntimeError("Sign in to Spotify to browse your playlists")
    out = []
    offset = 0
    total = 0
    while True:
        data = _spotify_query(token, "libraryV3", {
            "filters": ["Playlists"], "order": None, "textFilter": None,
            "features": ["LIKED_SONGS"], "limit": 50, "offset": offset,
            "flatten": True, "folderUri": None, "includeFoldersWhenFlattening": True,
        })
        lib = ((data.get("me") or {}).get("libraryV3") or {})
        page = lib.get("items") or []
        total = int(lib.get("totalCount") or 0)
        for raw in page:
            wrapper = raw.get("item") or raw
            node = wrapper.get("data") if isinstance(wrapper.get("data"), dict) else wrapper
            uri = (node.get("uri") or node.get("_uri")
                   or wrapper.get("uri") or wrapper.get("_uri") or "")
            name = (node.get("name") or wrapper.get("name") or "").strip()
            if uri.endswith(":collection") or name == "Liked Songs":
                out.append({"id": "liked", "kind": "liked", "name": "Liked Songs",
                            "owner": "", "count": int(node.get("count") or 0),
                            "art": _spotify_image(node.get("image"))})
            elif uri.startswith("spotify:playlist:"):
                pid = uri.rsplit(":", 1)[-1]
                owner = ((node.get("ownerV2") or {}).get("data") or {}).get("name") or ""
                count = int((node.get("content") or {}).get("totalCount")
                            or node.get("count") or 0)
                out.append({"id": pid, "kind": "playlist", "name": name,
                            "owner": owner, "count": count,
                            "art": _spotify_image(node.get("images") or node.get("image"))})
        offset += len(page)
        if not page or offset >= total:
            break
    # Liked Songs first, then the library order.
    out.sort(key=lambda x: 0 if x["kind"] == "liked" else 1)
    return out


def _spotify_liked_items(token):
    items = []
    offset = 0
    total = 0
    while True:
        data = _spotify_query(token, "fetchLibraryTracks",
                              {"offset": offset, "limit": 50})
        tracks = ((data.get("me") or {}).get("library") or {}).get("tracks") or {}
        page = tracks.get("items") or []
        total = int(tracks.get("totalCount") or 0)
        items.extend(page)
        offset += len(page)
        if not page or offset >= total:
            break
    return items


def _spotify_playlist_items(token, playlist_id):
    items = []
    offset = 0
    total = 0
    name = ""
    while True:
        data = _spotify_query(token, "fetchPlaylist", {
            "uri": f"spotify:playlist:{playlist_id}", "offset": offset,
            "limit": 343, "enableWatchFeedEntrypoint": False,
        })
        playlist = data.get("playlistV2") or {}
        if not playlist or playlist.get("__typename") == "NotFound":
            raise RuntimeError("Spotify could not find that playlist")
        name = name or (playlist.get("name") or "Spotify playlist")
        content = playlist.get("content") or {}
        page = content.get("items") or []
        total = int(content.get("totalCount") or 0)
        items.extend(page)
        offset += len(page)
        if not page or offset >= total:
            break
    return name, items


def _spotify_track(item):
    track = item.get("itemV2") or item.get("item") or item.get("track") or item
    if isinstance(track, dict) and isinstance(track.get("data"), dict):
        track = track["data"]
    if not isinstance(track, dict):
        return None
    title = (track.get("name") or "").strip()
    artists = []
    raw_artists = track.get("artists")
    if isinstance(raw_artists, dict):
        raw_artists = raw_artists.get("items") or []
    if isinstance(raw_artists, list):
        for artist in raw_artists:
            if not isinstance(artist, dict):
                continue
            name = ((artist.get("profile") or {}).get("name")
                    or artist.get("name") or "")
            if name:
                artists.append(name)
    if not artists:
        artists = [x.strip() for x in (track.get("artistName") or "").split(",") if x.strip()]
    if not title or not artists:
        return None
    duration = 0
    for key in ("trackDuration", "duration"):
        value = track.get(key)
        if isinstance(value, dict) and value.get("totalMilliseconds"):
            duration = int(value["totalMilliseconds"]) // 1000
            break
    return {
        "title": title,
        "subtitle": ", ".join(artists),
        "duration_sec": duration,
        "thumbnail_url": _spotify_image((track.get("albumOfTrack") or {}).get("coverArt")),
    }


def _spotify_sources(link):
    """Return (kind, id_or_None, display_name)."""
    link = (link or "").strip()
    if link.lower() in ("liked", "collection", "spotify:collection:tracks") or \
            link.endswith("/collection/tracks"):
        return "liked", None, "Liked Songs"
    pid = _spotify_id(link)
    if not pid:
        raise RuntimeError("Paste a Spotify playlist link or ID")
    return "playlist", pid, ""


def _spotify_entity(link):
    playlist_id = _spotify_id(link)
    if not playlist_id:
        raise RuntimeError("Paste a Spotify playlist link or ID")
    headers = {
        "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                      "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36",
        "Accept-Language": "en-US,en;q=0.9",
    }
    sp_dc = _spotify_cookie()
    if sp_dc:
        headers["Cookie"] = f"sp_dc={sp_dc}"
    last = None
    for url in (f"https://open.spotify.com/embed/playlist/{playlist_id}",
                f"https://open.spotify.com/playlist/{playlist_id}"):
        try:
            req = urllib.request.Request(url, headers=headers)
            page = urllib.request.urlopen(req, timeout=20).read().decode("utf-8", "replace")
            m = re.search(r'<script id="__NEXT_DATA__" type="application/json">(.*?)</script>',
                          page, re.S)
            if not m:
                continue
            data = json.loads(m.group(1))
            entity = (data.get("props", {}).get("pageProps", {}).get("state", {})
                      .get("data", {}).get("entity"))
            if not entity:
                continue
            tracks = entity.get("trackList") or []
            if not tracks and "collection/tracks" not in link:
                continue
            return entity.get("title") or entity.get("name") or "Spotify playlist", tracks
        except Exception as e:
            last = e
    raise RuntimeError(f"Could not read the Spotify playlist: {last or 'no track data'}")


def import_spotify(yt, link):
    _spotify_progress("loading", 0, 0, "")
    try:
        kind, pid, display = _spotify_sources(link)
        token, anonymous = _spotify_token(pid)
        if kind == "liked":
            if anonymous:
                raise RuntimeError("Sign in to Spotify to import Liked Songs")
            name = "Liked Songs"
            items = _spotify_liked_items(token)
        else:
            name, items = _spotify_playlist_items(token, pid)
        parsed = [x for x in (_spotify_track(i) for i in items) if x]
    except Exception as e:
        # Last-resort public embed reader (100 tracks, no sign-in needed).
        try:
            name, embed_items = _spotify_entity(link)
            parsed = [{"title": (x.get("title") or "").strip(),
                       "subtitle": (x.get("subtitle") or "").strip(),
                       "duration_sec": 0, "thumbnail_url": ""}
                      for x in embed_items if (x.get("title") or "").strip()]
        except Exception:
            return _err(str(e))

    total = len(parsed)
    if not total:
        return _err("That Spotify playlist has no playable songs")
    if len(parsed) < len(items):
        _spotify_progress("matching", 0, total, name)
    else:
        _spotify_progress("matching", 0, total, name)

    def match(item):
        query = f"{item['title']} {item['subtitle']}".strip()
        try:
            found = search_tracks(yt, query, limit=3)
        except Exception:
            return None
        if isinstance(found, dict) and "error" in found:
            return None
        return found[0] if found else None

    tracks = [None] * total
    done = 0
    workers = 4 if total >= 8 else 1
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for index, found in enumerate(pool.map(match, parsed)):
            tracks[index] = found
            done += 1
            if done % 5 == 0 or done == total:
                _spotify_progress("matching", done, total, name)

    matched = [t for t in tracks if t]
    _spotify_progress("done", total, total, name)
    if not matched:
        return _err("No Spotify tracks could be matched on YouTube Music")
    return {"name": name, "tracks": matched, "skipped": total - len(matched),
            "total": total}

def get_episodes_later(yt):
    try:
        data = yt.get_saved_episodes(limit=100)
    except Exception as e:
        return _err(f"Could not load episodes: {e}")
    return [_podcast_episode(x) for x in (data.get("tracks") or [])]


def check_logged_in(yt):
    """
    Sanity check: confirms the server actually sees us as authenticated,
    rather than silently falling back to anonymous/generic behavior.
    Returns (True, None) or (False, error_message).
    """
    try:
        response = yt._send_request("account/account_menu", {})
        tracking = response.get('responseContext', {}).get('serviceTrackingParams', [])
        for service in tracking:
            if service.get('service') == 'GFEEDBACK':
                for param in service.get('params', []):
                    if param.get('key') == 'logged_in':
                        return param.get('value') == '1', None
        return False, "Could not find logged_in flag in response"
    except Exception as e:
        return False, str(e)


def get_home_tracks(yt, limit=40):
    """
    Mirrors the YouTube Music homepage: returns tracks pulled from the
    same sections shown there (Quick picks, Mixed for you, etc.),
    rather than deriving a radio mix from listening history.
    """
    try:
        sections = yt.get_home(limit=limit)
    except Exception as e:
        return {'error': f"Failed to fetch home feed: {str(e)}"}

    if not sections:
        return {'error': 'Home feed returned no sections. Try again once your account has some listening activity.'}

    tracks = []
    seen_ids = set()
    for section in sections:
        if not isinstance(section, dict):
            continue
        for item in section.get('contents') or []:
            if not isinstance(item, dict):
                # Skip non-track entries (dividers, malformed items, etc.)
                continue
            video_id = item.get('videoId')
            # Skip non-track items (e.g. whole playlists/albums with no videoId)
            if not video_id or video_id in seen_ids:
                continue
            seen_ids.add(video_id)
            try:
                tracks.append(track_to_dict(item))
            except Exception:
                # Don't let one malformed track kill the whole batch
                continue
            if len(tracks) >= limit:
                return tracks

    if not tracks:
        return {'error': 'No individual tracks found in home feed sections (they may be playlists/albums instead of songs).'}

    return tracks


def search_tracks(yt, query, limit=20):
    """
    Search both the YouTube Music song catalogue and regular YouTube.

    The 'songs' filter only returns items indexed in the YTM catalogue, so
    YouTube-only uploads (UGC videos, lyric/live clips, unofficial releases)
    never show up there -- even though the home feed and radio do surface
    them. Merge both result sets, songs first, de-duplicated by videoId.
    """
    tracks = []
    seen_ids = set()

    def _collect(filt):
        # Returns (results, error) so the merge below can preserve the
        # original semantics: error only surfaces when nothing was collected.
        try:
            return yt.search(query, filter=filt, limit=limit), None
        except Exception as e:
            return None, str(e)

    with ThreadPoolExecutor(max_workers=2) as pool:
        fut_songs = pool.submit(_collect, 'songs')
        fut_videos = pool.submit(_collect, 'videos')
        for fut in (fut_songs, fut_videos):
            results, err = fut.result()
            if results is None:
                if not tracks and err:
                    return {'error': err}
                continue
            for r in results:
                video_id = r.get('videoId') or r.get('id')
                if not video_id or video_id in seen_ids or not r.get('title'):
                    continue
                seen_ids.add(video_id)
                try:
                    tracks.append(track_to_dict(r))
                except Exception:
                    continue
    return tracks


def get_radio_tracks(yt, video_id, limit=50):
    """
    Starts a "radio" queue seeded by a specific song - the same feature as
    right-clicking a track in the actual app and choosing "Start radio".
    """
    try:
        radio = yt.get_watch_playlist(videoId=video_id, radio=True, limit=limit)
    except Exception as e:
        return {'error': f"Failed to start radio: {str(e)}"}

    if not radio or 'tracks' not in radio:
        return {'error': 'No radio tracks returned for this song.'}

    tracks = []
    for track in radio['tracks']:
        if not isinstance(track, dict):
            continue
        if not track.get('videoId'):
            continue
        try:
            tracks.append(track_to_dict(track))
        except Exception:
            continue

    if not tracks:
        return {'error': 'Radio returned no playable tracks.'}

    return tracks


def get_lyrics(yt, video_id):
    """
    Fetch lyrics for a song. Metadata only -- no stream resolution, no
    downloads, so this also works for tracks that play from the local audio
    cache.

    Several providers are tried in order and the first SYNCED (timestamped)
    result wins; if none of them has timestamps, the first plain result is
    used. Providers:
      1. YouTube Music native timed lyrics (public mobile browse)
      2. LRCLIB (community; synced, then plain)
      3. NetEase Cloud Music (synced LRC)
      4. Kugou (synced LRC)
      5. lyrics.ovh (plain)
      6. YouTube Music native plain (LyricFind)
    """
    try:
        watch = yt.get_watch_playlist(videoId=video_id) or {}
    except Exception as e:
        return {'error': f"Failed to look up lyrics: {str(e)}"}

    lyrics_field = watch.get('lyrics')
    browse_id = lyrics_field if isinstance(lyrics_field, str) else (lyrics_field or {}).get('browseId')
    track_name, artist_name, album_name, duration_sec = _extract_meta(
        (watch.get('tracks') or [None])[0])

    providers = (
        lambda: _ytm_timed_lyrics(browse_id),
        lambda: _lrclib_lyrics(artist_name, track_name, album_name, duration_sec),
        lambda: _netease_lyrics(track_name, artist_name, duration_sec),
        lambda: _kugou_lyrics(track_name, artist_name, duration_sec),
        lambda: _lyrics_ovh(artist_name, track_name),
        lambda: _ytm_plain_lyrics(yt, browse_id),
    )

    plain_fallback = None
    for provider in providers:
        try:
            result = provider()
        except Exception:
            result = None
        if not result:
            continue
        if result.get('synced'):
            return result
        if plain_fallback is None:
            plain_fallback = result
    return plain_fallback or {'none': True}


def _extract_meta(meta):
    """Pull (title, artist, album, duration_sec) out of a watch-playlist track."""
    track_name = artist_name = album_name = None
    duration_sec = 0
    if isinstance(meta, dict):
        track_name = meta.get('title')
        artists = meta.get('artists') or []
        if artists and isinstance(artists[0], dict):
            artist_name = artists[0].get('name')
        album = meta.get('album')
        if isinstance(album, dict):
            album_name = album.get('name')
        elif isinstance(album, str):
            album_name = album
        dur = meta.get('duration')
        if isinstance(dur, str) and ':' in dur:
            parts = dur.split(':')
            try:
                duration_sec = int(parts[0]) * 60 + int(parts[1])
            except ValueError:
                duration_sec = 0
    return track_name, artist_name, album_name, duration_sec


# ---- HTTP / matching helpers -------------------------------------------

_BROWSER_UA = ('Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 '
               '(KHTML, like Gecko) Chrome/120.0 Safari/537.36')
_YTM_MOBILE_UA = ('com.google.android.apps.youtube.music/7.21.50 '
                  '(Linux; U; Android 14) gzip')


def _http_json(url, headers=None, data=None, timeout=10):
    req_headers = {'User-Agent': _BROWSER_UA}
    if headers:
        req_headers.update(headers)
    req = urllib.request.Request(url, data=data, headers=req_headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode('utf-8', 'replace'))
    except Exception:
        return None


def _find_key(obj, key):
    """Depth-first search for the first value stored under `key`."""
    stack = [obj]
    while stack:
        cur = stack.pop()
        if isinstance(cur, dict):
            if key in cur:
                return cur[key]
            stack.extend(cur.values())
        elif isinstance(cur, list):
            stack.extend(cur)
    return None


def _norm(s):
    """Normalise a title/artist for matching (drop case + punctuation)."""
    return re.sub(r'[\W_]+', '', (s or '').lower(), flags=re.UNICODE)


def _pick_song(candidates, track, artist, duration_sec, name_of, artists_of, dur_ms_of):
    """
    Choose the best search hit. Only candidates whose (normalised) title
    matches the wanted title are considered, so we never return lyrics for the
    wrong song. Among those, prefer an artist match and the closest duration.
    """
    want = _norm(track)
    if not want:
        return None
    best = None
    best_score = None
    for cand in candidates or []:
        try:
            name = _norm(name_of(cand))
            cand_artist = artists_of(cand)
            dur_ms = dur_ms_of(cand) or 0
        except Exception:
            continue
        if not name or (want not in name and name not in want):
            continue
        score = 0
        if name == want:
            score -= 1000
        if artist and _norm(artist) and _norm(artist) in _norm(cand_artist):
            score -= 200
        if duration_sec and dur_ms:
            score += abs(dur_ms / 1000.0 - duration_sec)
        if best_score is None or score < best_score:
            best_score = score
            best = cand
    return best


def _plain_result(source, text):
    lines = [{'t': 0, 'text': p}
             for p in (text or '').replace('\r\n', '\n').split('\n') if p.strip()]
    return {'synced': False, 'source': source, 'lines': lines} if lines else None


def _synced_from_lrc(source, lrc):
    lines = _parse_lrc(lrc or '')
    if not lines or not any(l['t'] > 0 for l in lines):
        return None
    return {'synced': True, 'source': source, 'lines': lines}


_LRC_ANY = re.compile(r'\[[^\]]*\]')


def _strip_lrc_tags(text):
    return _LRC_ANY.sub('', text or '').strip()


# ---- Providers ----------------------------------------------------------

def _ytm_timed_lyrics(browse_id):
    """
    YouTube Music's own timed lyrics. The browser-cookie auth path 400s on the
    mobile client, but an unauthenticated mobile browse of the lyrics page
    returns the public timed-lyrics payload.
    """
    if not browse_id:
        return None
    payload = json.dumps({
        'context': {'client': {
            'clientName': 'ANDROID_MUSIC', 'clientVersion': '7.21.50',
            'androidSdkVersion': 34, 'osName': 'Android', 'osVersion': '14',
            'hl': 'en', 'gl': 'US', 'utcOffsetMinutes': 0,
        }},
        'browseId': browse_id,
    }).encode('utf-8')
    data = _http_json('https://music.youtube.com/youtubei/v1/browse?alt=json',
                      headers={'Content-Type': 'application/json',
                               'User-Agent': _YTM_MOBILE_UA,
                               'Origin': 'https://music.youtube.com'},
                      data=payload)
    timed = _find_key(data or {}, 'timedLyricsData')
    if not timed:
        return None
    source = _find_key(data, 'sourceMessage') or 'YouTube'
    if isinstance(source, str) and source.lower().startswith('source:'):
        source = source.split(':', 1)[1].strip()

    synced_lines = []
    plain_lines = []
    for item in timed:
        if not isinstance(item, dict):
            continue
        text = item.get('lyricLine')
        if not text or not text.strip():
            continue
        try:
            start_ms = int((item.get('cueRange') or {}).get('startTimeMilliseconds') or 0)
        except (TypeError, ValueError):
            start_ms = 0
        plain_lines.append({'t': 0, 'text': text})
        synced_lines.append({'t': start_ms // 1000, '_ms': start_ms, 'text': text})
    if not plain_lines:
        return None
    if any(l['_ms'] > 0 for l in synced_lines):
        synced_lines.sort(key=lambda l: l['_ms'])
        return {'synced': True, 'source': source,
                'lines': [{'t': l['t'], 'text': l['text']} for l in synced_lines]}
    return {'synced': False, 'source': source, 'lines': plain_lines}


def _ytm_plain_lyrics(yt, browse_id):
    if not browse_id:
        return None
    try:
        data = yt.get_lyrics(browse_id, timestamps=False)
    except Exception:
        return None
    if data and data.get('lyrics'):
        return _plain_result(data.get('source') or 'LyricFind', data['lyrics'])
    return None


def _lrclib_lyrics(artist, track, album, duration_sec):
    if not (track and artist):
        return None
    data = _lrclib_lookup(artist, track, album, duration_sec)
    if not data or data.get('instrumental'):
        return None
    if data.get('syncedLyrics'):
        result = _synced_from_lrc('LRCLIB', data['syncedLyrics'])
        if result:
            return result
    if data.get('plainLyrics'):
        return _plain_result('LRCLIB', data['plainLyrics'])
    return None


def _netease_lyrics(track, artist, duration_sec):
    if not track:
        return None
    query = (track + ' ' + (artist or '')).strip()
    search = _http_json(
        'https://music.163.com/api/search/get?' +
        urllib.parse.urlencode({'s': query, 'type': 1, 'limit': 8}),
        headers={'Referer': 'https://music.163.com/'})
    songs = (((search or {}).get('result') or {}).get('songs')) or []
    song = _pick_song(
        songs, track, artist, duration_sec,
        name_of=lambda s: s.get('name'),
        artists_of=lambda s: ' '.join(a.get('name', '') for a in (s.get('artists') or [])),
        dur_ms_of=lambda s: s.get('duration') or 0)
    if not song or not song.get('id'):
        return None
    lyric = _http_json(
        'https://music.163.com/api/song/lyric?' +
        urllib.parse.urlencode({'id': song['id'], 'lv': 1, 'kv': 1, 'tv': -1}),
        headers={'Referer': 'https://music.163.com/'})
    lrc = ((lyric or {}).get('lrc') or {}).get('lyric') or ''
    return _synced_from_lrc('NetEase', lrc) or _plain_result('NetEase', _strip_lrc_tags(lrc))


def _kugou_lyrics(track, artist, duration_sec):
    if not track:
        return None
    query = (track + ' ' + (artist or '')).strip()
    search = _http_json(
        'https://songsearch.kugou.com/song_search_v2?' +
        urllib.parse.urlencode({'keyword': query, 'page': 1, 'pagesize': 8,
                                'platform': 'WebFilter', 'tag': 'em'}))
    songs = (((search or {}).get('data') or {}).get('lists')) or []
    song = _pick_song(
        songs, track, artist, duration_sec,
        name_of=lambda s: s.get('SongName'),
        artists_of=lambda s: s.get('SingerName'),
        dur_ms_of=lambda s: (s.get('Duration') or 0) * 1000)
    if not song:
        return None
    file_hash = song.get('FileHash') or song.get('HQFileHash')
    if not file_hash:
        return None
    cands = _http_json(
        'https://krcs.kugou.com/search?' +
        urllib.parse.urlencode({'ver': 1, 'man': 'yes', 'client': 'mobi',
                                'keyword': query, 'hash': file_hash}))
    cand_list = (cands or {}).get('candidates') or []
    if not cand_list:
        return None
    cand = cand_list[0]
    dl = _http_json(
        'https://krcs.kugou.com/download?' +
        urllib.parse.urlencode({'ver': 1, 'client': 'mobi', 'id': cand.get('id'),
                                'accesskey': cand.get('accesskey'),
                                'fmt': 'lrc', 'charset': 'utf8'}))
    content = (dl or {}).get('content')
    if not content:
        return None
    try:
        lrc = base64.b64decode(content).decode('utf-8', 'replace')
    except Exception:
        return None
    return _synced_from_lrc('Kugou', lrc) or _plain_result('Kugou', _strip_lrc_tags(lrc))


def _lyrics_ovh(artist, track):
    if not (track and artist):
        return None
    data = _http_json('https://api.lyrics.ovh/v1/' +
                      urllib.parse.quote(artist) + '/' + urllib.parse.quote(track))
    lyrics = (data or {}).get('lyrics')
    if not lyrics:
        return None
    return _plain_result('lyrics.ovh', lyrics)


_LRCLIB_UA = 'p100/0.1.0 (https://github.com/anomalyco/p100)'
_LRC_TAG = re.compile(r'\[(\d+):(\d+(?:\.\d+)?)\]')


def _lrclib_lookup(artist, track, album, duration_sec):
    """
    Query the LRCLIB exact-match endpoint. Tries with album+duration first
    (best match) and then drops them, since a wrong extra field yields a 404.
    Returns the parsed JSON dict, or None when nothing matched / on error.
    """
    def attempt(with_extra):
        params = {'artist_name': artist, 'track_name': track}
        if with_extra:
            if album:
                params['album_name'] = album
            if duration_sec:
                params['duration'] = str(duration_sec)
        url = 'https://lrclib.net/api/get?' + urllib.parse.urlencode(params)
        req = urllib.request.Request(url, headers={'User-Agent': _LRCLIB_UA})
        try:
            with urllib.request.urlopen(req, timeout=12) as resp:
                return json.loads(resp.read().decode('utf-8'))
        except Exception:
            return None

    return attempt(True) or attempt(False)


def _parse_lrc(text):
    """Parse LRC ("[mm:ss.xx] line") into [{'t': <sec int>, 'text': ...}]."""
    lines = []
    for raw in text.replace('\r\n', '\n').split('\n'):
        m = _LRC_TAG.search(raw)
        if not m:
            continue
        try:
            t = int(int(m.group(1)) * 60 + float(m.group(2)))
        except ValueError:
            continue
        lyric = _LRC_TAG.sub('', raw).strip()
        if lyric:
            lines.append({'t': t, 'text': lyric})
    lines.sort(key=lambda l: l['t'])
    return lines


# YouTube enforces per-client fetch checks (the `rqh=1` URL flag): a client
# can pass extraction while its URLs 403 on every fetch (seen with TVHTML and
# ANDROID_VR, Oct 2026), and which client wins extraction is effectively a
# dice roll. So every resolved URL is probed with mpv's exact initial request
# and dead ones trigger a forced re-extraction with the next client listed.
_PLAYER_CLIENT_FALLBACKS = (
    'visionos', 'ios', 'android_vr', 'tv', 'web_safari', 'mweb',
)


def _url_playable(url):
    """Mimic mpv's initial open (open-ended Range) to see if the URL really
    serves audio. One quick retry: googlevideo occasionally 403s a perfectly
    good URL for a moment (rate limiting), which must not discard it."""
    for attempt in range(2):
        try:
            req = urllib.request.Request(url, headers={
                'Range': 'bytes=0-',
                'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) '
                              'AppleWebKit/537.36 (KHTML, like Gecko) '
                              'Chrome/120.0.0.0 Safari/537.36',
            })
            with urllib.request.urlopen(req, timeout=8) as resp:
                return bool(resp.read(2048))
        except Exception:
            if attempt == 0:
                time.sleep(0.15)
    return False


_WARM_EXTRACTOR = os.environ.get('P100_WARM_EXTRACTOR') == '1'

# In-process extraction (P100_WARM_EXTRACTOR=1): run yt-dlp as a module so a
# resolve skips the per-subprocess Python interpreter startup (~1-3 s). The
# module is imported lazily; any failure falls back to the subprocess path.
_ydl_cls = None
_ydl_tried = False
_ydl_local = threading.local()


def _thread_ydl(client_arg, browser, opts):
    """Per-thread persistent YoutubeDL instance (yt-dlp caches player JS)."""
    cache = getattr(_ydl_local, 'instances', None)
    if cache is None:
        cache = {}
        _ydl_local.instances = cache
    key = (client_arg, browser)
    ydl = cache.get(key)
    if ydl is None:
        ydl = _ydl_cls(opts)
        cache[key] = ydl
    return ydl


def _in_process_extract(video_id, client_arg, browser):
    """One in-process yt-dlp extraction; returns (url, error)."""
    global _ydl_cls, _ydl_tried
    if not _ydl_tried:
        _ydl_tried = True
        try:
            from yt_dlp import YoutubeDL
            _ydl_cls = YoutubeDL
        except Exception:
            _ydl_cls = None
    if _ydl_cls is None:
        return None, 'yt_dlp module unavailable'
    opts = {
        'format': 'bestaudio/best',
        'quiet': True,
        'no_warnings': True,
        'skip_download': True,
        'noplaylist': True,
    }
    if client_arg:
        opts['extractor_args'] = {'youtube': {'player_client': [client_arg]}}
    if browser:
        # yt-dlp expects (browser, profile, keyring, container); a bare string
        # is unpacked character-by-character ("firefox" -> 7 args).
        opts['cookiesfrombrowser'] = (browser,)
    # The primary client instance is kept per thread: yt-dlp caches the player
    # JS and its signature solver on the instance, which is worth ~1 s per
    # subsequent resolve. Fallback clients stay ephemeral (they are rare and
    # each instance carries its own player-JS cache).
    persistent = client_arg in (None, 'web_music', 'visionos')
    try:
        ydl = _thread_ydl(client_arg, browser, opts) if persistent else _ydl_cls(opts)
        try:
            info = ydl.extract_info(
                f'https://www.youtube.com/watch?v={video_id}', download=False)
        finally:
            if not persistent:
                ydl.close()
    except Exception as e:
        return None, str(e)
    if not info:
        return None, 'yt-dlp module returned no info'
    url = info.get('url')
    if not url:
        # The requested format lives in the formats list; prefer audio-only.
        best = None
        for f in info.get('formats') or []:
            if not f.get('url') or f.get('vcodec') not in (None, 'none'):
                continue
            if best is None or (f.get('abr') or 0) > (best.get('abr') or 0):
                best = f
        url = best.get('url') if best else None
    if not url:
        return None, 'yt-dlp module returned no URL'
    return url, None


def _extract_stream_url(video_id, client_arg, browser):
    """Resolve a stream URL. With the warm extractor enabled the in-process
    module path is tried first and the original yt-dlp subprocess (with its
    cookie variant) stays as the fallback."""
    if _WARM_EXTRACTOR:
        url, err = _in_process_extract(video_id, client_arg, browser)
        if url:
            return url, None
        last_error = err
    else:
        last_error = "yt-dlp returned no URL"
    cmd = [YTDLP, "-f", "bestaudio", "-g", "--no-warnings"]
    if client_arg:
        cmd += ["--extractor-args", f"youtube:player_client={client_arg}"]
    cmd.append(f"https://www.youtube.com/watch?v={video_id}")
    attempts = [cmd]
    if browser:
        attempts.append(cmd[:3] + ["--cookies-from-browser", browser] + cmd[3:])
    for attempt in attempts:
        try:
            # Bounded: this runs on the single command thread, so a child that
            # hangs (cookie/keyring prompt, stuck network) would wedge every
            # later command long after the C++ side stopped waiting.
            url = subprocess.check_output(attempt, text=True, timeout=30,
                                          creationflags=_NO_WINDOW).strip()
            if url:
                return url, None
        except Exception as e:
            last_error = str(e)
    return None, last_error


# Client that last produced a playable URL this session (parsed from the
# `c=` parameter of a probe-verified googlevideo URL). Tried first on every
# resolve; None means fall through to yt-dlp's default order.
_warm_client = None


def _client_from_url(url):
    """Map a resolved URL's `c=` parameter back to a yt-dlp player_client
    name, or None when it isn't one we force ourselves."""
    m = re.search(r'[?&]c=([A-Za-z_]+)', url)
    if m and m.group(1).lower() in _PLAYER_CLIENT_FALLBACKS:
        return m.group(1).lower()
    return None


def get_stream_url(video_id, fresh=False):
    # Resolved googlevideo URLs stay valid for hours; reuse one for repeat
    # plays (and the player's prefetch) instead of re-running extraction.
    # `fresh` bypasses the cache after a player-side load failure.
    if fresh:
        with _stream_lock:
            _stream_cache.pop(video_id, None)
    else:
        _load_stream_disk()
        with _stream_lock:
            cached = _stream_cache.get(video_id)
        if cached and time.time() - cached[0] < _STREAM_TTL:
            return {'url': cached[1]}
        # Another backend channel may have resolved this id after we loaded.
        url = _stream_disk_lookup(video_id)
        if url:
            return {'url': url}

    browser = os.environ.get('P100_BROWSER')
    last_error = "yt-dlp returned no URL"
    last_url = None
    # Warm client first: the one that last produced a playable URL this
    # session. After that: `visionos` and `web_music` are the profiles whose
    # URLs reliably survive both the probe and mpv (measured 1.4-2.1 s vs
    # ~3 s for the default order, whose URL is usually rejected at fetch).
    global _warm_client
    client_args = []
    if _warm_client:
        client_args.append(_warm_client)
    for c in ('visionos', 'web_music', None) + _PLAYER_CLIENT_FALLBACKS:
        if c not in client_args:
            client_args.append(c)
    for client_arg in client_args:
        url, err = _extract_stream_url(video_id, client_arg, browser)
        if not url:
            last_error = err
            continue
        if _url_playable(url):
            with _stream_lock:
                _stream_cache[video_id] = (time.time(), url)
            _warm_client = _client_from_url(url) or client_arg
            _save_stream_disk()
            return {'url': url}
        last_url = url
        last_error = ("resolved URL rejected at fetch "
                      f"(client: {client_arg or 'yt-dlp default'})")
    # Every client was rejected (e.g. googlevideo rate-limiting all probes).
    # Hand over the last extraction anyway rather than erroring out: mpv may
    # still accept it, and if not, the player's fresh-resolve retry fires.
    if last_url:
        return {'url': last_url}
    return {'error': last_error}


_prefetch_pool = None
_prefetch_pool_lock = threading.Lock()


def _get_prefetch_pool():
    """Shared pool so its threads (and their warm YoutubeDL instances) live
    across prefetch commands instead of being rebuilt per batch."""
    global _prefetch_pool
    with _prefetch_pool_lock:
        if _prefetch_pool is None:
            _prefetch_pool = ThreadPoolExecutor(max_workers=4,
                                                thread_name_prefix='cf-prefetch')
        return _prefetch_pool


def prefetch_stream_urls(video_ids):
    """Resolve several ids at once (speculative prefetch).

    The command pipe answers one command at a time, so the parallelism has to
    live here: four worker threads, each with its own persistent YoutubeDL
    instance, turn a 10-row prefetch from ~30 s into a few seconds -- and the
    first rows are ready in under three."""
    ids = [v for v in video_ids if re.fullmatch(r'[A-Za-z0-9_-]{11}', v or '')]
    if not ids:
        return {'urls': []}

    def one(vid):
        try:
            result = get_stream_url(vid)
        except Exception:
            return None
        url = result.get('url') if isinstance(result, dict) else None
        return {'id': vid, 'url': url} if url else None

    rows = [r for r in _get_prefetch_pool().map(one, ids) if r]
    return {'urls': rows}


# Repeat/back-navigation searches hit YTM every time; a small TTL cache makes
# them instant. Errors are never cached (a transient failure must retry).
_SEARCH_TTL = 900  # seconds
_SEARCH_CACHE_MAX = 100
_search_cache = {}


def _cached_search(query, fn, scope=''):
    key = (scope, (query or '').strip().lower())
    if not key[1]:
        return fn()
    hit = _search_cache.get(key)
    now = time.time()
    if hit and now - hit[0] < _SEARCH_TTL:
        return hit[1]
    result = fn()
    if not (isinstance(result, dict) and 'error' in result):
        _search_cache[key] = (now, result)
        if len(_search_cache) > _SEARCH_CACHE_MAX:
            for k, _ in sorted(_search_cache.items(), key=lambda kv: kv[1][0])[:len(_search_cache) - _SEARCH_CACHE_MAX]:
                _search_cache.pop(k, None)
    return result


def _json_arg(arg):
    """Parse a JSON object argument (the whole rest of the command line)."""
    if not arg:
        return None
    try:
        obj = json.loads(arg)
    except Exception:
        return None
    return obj if isinstance(obj, dict) else None


# ---------------------------------------------------------------------------
# Studio: waveform + beat analysis
#
# ffmpeg decodes the file to mono float PCM; the waveform is a multi-resolution
# peak pyramid (so zooming never re-decodes) and beats come from a dependency-
# free spectral-flux-style onset detector + tempo autocorrelation. Results are
# written as a compact text file (see fetch_studio_analysis in ytdlp.cpp) so the
# C++ side needs no JSON array parser.
# ---------------------------------------------------------------------------

_STUDIO_SR = 11025


def _read_mono_pcm(path):
    ffmpeg = os.environ.get('P100_FFMPEG') or 'ffmpeg'
    cmd = [ffmpeg, '-v', 'error', '-i', path, '-t', '1800',
           '-f', 'f32le', '-ac', '1', '-ar', str(_STUDIO_SR), '-']
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=180, creationflags=_NO_WINDOW)
    except FileNotFoundError:
        return None, 'ffmpeg not found (install ffmpeg for waveform and beat analysis)'
    except Exception as e:
        return None, 'ffmpeg failed: ' + str(e)
    if proc.returncode != 0 or not proc.stdout:
        msg = (proc.stderr or b'').decode('utf-8', 'replace').strip()
        return None, 'Could not decode audio' + (': ' + msg if msg else '')
    raw = proc.stdout
    raw = raw[:len(raw) - (len(raw) % 4)]
    from array import array
    samples = array('f')
    samples.frombytes(raw)
    if sys.byteorder != 'little':
        samples.byteswap()
    return samples, None


def _wave_pyramid(samples, levels):
    n = len(samples)
    out = []
    for target in levels:
        buckets = min(target, n) if n else 0
        if buckets <= 0:
            out.append((0, [], []))
            continue
        step = n / float(buckets)
        mins, maxs = [], []
        for b in range(buckets):
            a = int(b * step)
            c = int((b + 1) * step)
            if c <= a:
                c = min(n, a + 1)
            chunk = samples[a:c]
            if chunk:
                mins.append(min(chunk))
                maxs.append(max(chunk))
            else:
                mins.append(0.0)
                maxs.append(0.0)
        out.append((buckets, mins, maxs))
    return out


def _detect_beats(samples, sample_rate):
    n = len(samples)
    hop = 256
    frames = n // hop
    if frames < 8:
        return 0.0, 0.0, [], []

    energy = [0.0] * frames
    for i in range(frames):
        a = i * hop
        s = 0.0
        for v in samples[a:a + hop]:
            s += v * v
        energy[i] = s

    onset = [0.0] * frames
    for i in range(1, frames):
        d = energy[i] - energy[i - 1]
        onset[i] = d if d > 0.0 else 0.0

    win = max(3, int(0.5 * sample_rate / hop))
    peaks = []
    last_t = -999.0
    for i in range(1, frames - 1):
        lo = max(0, i - win)
        hi = min(frames, i + win + 1)
        m = sum(onset[lo:hi]) / float(max(1, hi - lo))
        if onset[i] > m * 1.4 and onset[i] >= onset[i - 1] and onset[i] >= onset[i + 1]:
            t = i * hop / float(sample_rate)
            if t - last_t >= 0.12:
                peaks.append(t)
                last_t = t

    if len(peaks) < 4:
        return 0.0, 0.0, peaks, []

    min_lag = max(1, int(0.30 * sample_rate / hop))
    max_lag = max(min_lag + 1, int(1.00 * sample_rate / hop))
    best_lag, best_score = 0, 0.0
    for lag in range(min_lag, max_lag + 1):
        s = 0.0
        for i in range(lag, frames):
            s += onset[i] * onset[i - lag]
        if s > best_score:
            best_score, best_lag = s, lag
    if best_lag <= 0:
        return 0.0, 0.0, peaks, []

    # Autocorrelation favours the half-tempo (a click every beat also lines up
    # every other beat). If the octave-up lag correlates strongly too, prefer
    # it so the grid is twice as fine.
    if best_lag > 2:
        lo = best_lag // 2
        hi = best_lag - lo
        best_half, best_half_score = 0, 0.0
        for half in ({lo, hi}):
            if half <= 0:
                continue
            sh = 0.0
            for i in range(half, frames):
                sh += onset[i] * onset[i - half]
            if sh > best_half_score:
                best_half, best_half_score = half, sh
        if best_half and best_half_score >= best_score * 0.55:
            best_lag, best_score = best_half, best_half_score

    period = best_lag * hop / float(sample_rate)
    bpm = 60.0 / period
    while bpm < 60.0:
        bpm *= 2.0
        period /= 2.0
    while bpm > 200.0:
        bpm /= 2.0
        period *= 2.0
    total = sum(o * o for o in onset) + 1e-9
    confidence = max(0.0, min(1.0, best_score / total))

    duration = n / float(sample_rate)
    steps = 24
    best_phase, best_phase_score = 0.0, -1.0
    for k in range(steps):
        phase = period * k / float(steps)
        s = 0.0
        t = phase
        while t < duration:
            fi = int(t * sample_rate / hop)
            if 0 <= fi < frames:
                s += onset[fi]
            t += period
        if s > best_phase_score:
            best_phase_score, best_phase = s, phase

    beats = []
    t = best_phase
    while t < duration:
        beats.append(round(t, 3))
        t += period
    return bpm, confidence, beats, beats[::4]


def _write_wave_cache(path, duration, bpm, confidence, beats, downbeats, pyramid):
    tmp = path + '.tmp'
    try:
        parent = os.path.dirname(path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(tmp, 'w', encoding='utf-8') as f:
            f.write('CFWAVE 1\n')
            f.write('duration %.6f\n' % duration)
            f.write('bpm %.4f\n' % bpm)
            f.write('confidence %.4f\n' % confidence)
            f.write('beats ' + ','.join('%.3f' % x for x in beats) + '\n')
            f.write('downbeats ' + ','.join('%.3f' % x for x in downbeats) + '\n')
            for buckets, mins, maxs in pyramid:
                f.write('wave %d\n' % buckets)
                for i in range(buckets):
                    f.write('%.5f,%.5f\n' % (mins[i], maxs[i]))
        os.replace(tmp, path)
        return True
    except Exception:
        try:
            os.remove(tmp)
        except OSError:
            pass
        return False


def analyze_audio(payload):
    payload = payload or {}
    path = payload.get('path') or ''
    cache = payload.get('cache') or ''
    if not path or not cache:
        return _err('Missing path/cache', kind='backend')
    if not os.path.isfile(path):
        return _err('Audio file not found', kind='not_found')
    if os.path.isfile(cache) and os.path.getsize(cache) > 0:
        return {'ok': True, 'cache': cache, 'cached': True}

    samples, err = _read_mono_pcm(path)
    if err:
        return _err(err, kind='backend')
    n = len(samples)
    if n < 2048:
        return _err('Audio file is too short to analyse', kind='backend')

    duration = n / float(_STUDIO_SR)
    pyramid = _wave_pyramid(samples, (64, 256, 1024, 4096, 16384))
    bpm, confidence, beats, downbeats = _detect_beats(samples, _STUDIO_SR)
    if not _write_wave_cache(cache, duration, bpm, confidence, beats, downbeats,
                             pyramid):
        return _err('Could not write the analysis cache', kind='backend')
    return {'ok': True, 'cache': cache, 'duration': duration, 'bpm': bpm,
            'confidence': confidence, 'beats': len(beats)}


def run_command_loop(yt, stdin=None, stdout=None):
    """Read newline-delimited commands from stdin until EOF/quit.

    Every reply is one JSON object per line. Errors are stamped with a
    machine-readable kind so the C++ UI can distinguish auth, network and
    backend failures without parsing the message text.
    """
    stdin = stdin if stdin is not None else sys.stdin
    stdout = stdout if stdout is not None else sys.stdout
    # The C++ side speaks UTF-8. Reconfigure our own pipes as well, so a
    # backend started without PYTHONIOENCODING (manual run, unusual locale)
    # still decodes queries and encodes titles correctly.
    for stream in (stdin, stdout):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            try:
                reconfigure(encoding="utf-8")
            except (ValueError, OSError):
                pass
    for line in stdin:
        line = line.strip()
        if not line:
            continue
        parts = line.split(' ', 1)
        cmd = parts[0]
        arg = parts[1] if len(parts) > 1 else None

        try:
            if cmd == 'recommendations':
                limit = 40
                if arg:
                    try:
                        limit = int(arg)
                    except ValueError:
                        limit = 40
                result = get_home_tracks(yt, limit=limit)
            elif cmd == 'search':
                result = (_cached_search(arg, lambda: search_tracks(yt, arg), 'search')
                          if arg else {'error': 'Missing query'})
            elif cmd == 'suggest':
                result = get_search_suggestions(yt, arg or '')
            elif cmd == 'search_all':
                result = (_cached_search(arg, lambda: search_all(yt, arg), 'search_all')
                          if arg else {'error': 'Missing query'})
            elif cmd == 'album_search':
                result = (_cached_search(arg, lambda: search_albums(yt, arg), 'album_search')
                          if arg else {'error': 'Missing query'})
            elif cmd == 'radio':
                result = get_radio_tracks(yt, arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'album':
                result = get_album_data(yt, arg) if arg else {'error': 'Missing album_id'}
            elif cmd == 'album_of':
                result = album_for_track(yt, arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'artist':
                result = get_artist_data(yt, arg) if arg else {'error': 'Missing artist_id'}
            elif cmd == 'artist_songs':
                try:
                    bits = (arg or '').split()
                    if len(bits) != 3:
                        result = {'error': 'Usage: artist_songs <browse_id> <offset> <count>'}
                    else:
                        result = get_artist_songs(yt, bits[0], int(bits[1]), int(bits[2]))
                except ValueError:
                    result = {'error': 'Bad artist_songs offset/count'}
            elif cmd == 'artist_of':
                result = artist_for_track(yt, arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'artist_search':
                result = (_cached_search(arg, lambda: search_artists(yt, arg), 'artist_search')
                          if arg else {'error': 'Missing query'})
            elif cmd == 'play':
                result = get_stream_url(arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'prefetch':
                result = prefetch_stream_urls((arg or '').split())
            elif cmd == 'play_fresh':
                result = get_stream_url(arg, fresh=True) if arg else {'error': 'Missing video_id'}
            elif cmd == 'lyrics':
                result = get_lyrics(yt, arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'analyze':
                result = analyze_audio(_json_arg(arg))
            elif cmd == 'meta':
                result = get_track_meta(yt, arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'library_playlists':
                result = yt.get_library_playlists(limit=100)
            elif cmd == 'playlist':
                result = yt.get_playlist(arg, limit=1000) if arg else {'error': 'Missing playlist_id'}
            elif cmd == 'spotify_import':
                result = import_spotify(yt, arg) if arg else {'error': 'Missing Spotify playlist'}
            elif cmd == 'spotify_library':
                try:
                    result = _spotify_library()
                except Exception as e:
                    result = _err(str(e))
            elif cmd == 'podcasts_library':
                result = get_podcast_library(yt)
            elif cmd == 'podcasts_search':
                result = (_cached_search(arg, lambda: search_podcasts(yt, arg), 'podcasts_search')
                          if arg else {'error': 'Missing query'})
            elif cmd == 'podcast':
                result = get_podcast_details(yt, arg) if arg else {'error': 'Missing podcast_id'}
            elif cmd == 'episodes_later':
                result = get_episodes_later(yt)
            elif cmd == 'rate_song':
                payload = _json_arg(arg) or {}
                vid = payload.get('videoId')
                rating = payload.get('rating', 'LIKE')
                if not vid:
                    result = {'error': 'Missing videoId'}
                elif rating not in ('LIKE', 'DISLIKE', 'INDIFFERENT'):
                    result = {'error': f'Invalid rating: {rating}'}
                else:
                    yt.rate_song(vid, rating)
                    result = {'status': 'ok'}
            elif cmd == 'create_playlist':
                payload = _json_arg(arg) or {}
                title = payload.get('title')
                if not title:
                    result = {'error': 'Missing title'}
                else:
                    pid = yt.create_playlist(
                        title=title,
                        description=payload.get('description'),
                        privacy_status=payload.get('privacyStatus', 'PRIVATE'),
                        video_ids=payload.get('videoIds'))
                    result = {'status': 'ok', 'playlistId': pid}
            elif cmd == 'delete_playlist':
                payload = _json_arg(arg) or {}
                pid = payload.get('playlistId')
                if not pid:
                    result = {'error': 'Missing playlistId'}
                else:
                    yt.delete_playlist(pid)
                    result = {'status': 'ok'}
            elif cmd == 'edit_playlist':
                payload = _json_arg(arg) or {}
                pid = payload.get('playlistId')
                if not pid:
                    result = {'error': 'Missing playlistId'}
                else:
                    kwargs = {}
                    for key in ('title', 'description', 'privacyStatus'):
                        if payload.get(key) is not None:
                            kwargs[key] = payload[key]
                    if payload.get('moveItem') is not None:
                        kwargs['moveItem'] = payload['moveItem']
                    yt.edit_playlist(pid, **kwargs)
                    result = {'status': 'ok'}
            elif cmd == 'add_playlist_items':
                payload = _json_arg(arg) or {}
                pid = payload.get('playlistId')
                video_ids = payload.get('videoIds')
                if not pid or not video_ids:
                    result = {'error': 'Missing playlistId or videoIds'}
                else:
                    yt.add_playlist_items(pid, videoIds=video_ids, duplicates=False)
                    result = {'status': 'ok'}
            elif cmd == 'remove_playlist_items':
                payload = _json_arg(arg) or {}
                pid = payload.get('playlistId')
                videos = payload.get('videos')
                if not pid or not videos:
                    result = {'error': 'Missing playlistId or videos'}
                else:
                    yt.remove_playlist_items(pid, videos)
                    result = {'status': 'ok'}
            elif cmd == 'quit':
                break
            else:
                result = {'error': f'Unknown command: {cmd}'}
        except Exception as e:
            result = {'error': f'Unhandled error in command {cmd!r}: {str(e)}'}

        if isinstance(result, dict) and 'error' in result and 'kind' not in result:
            result['kind'] = _classify_error(result['error'])

        print(json.dumps(result, ensure_ascii=False), file=stdout)
        stdout.flush()
