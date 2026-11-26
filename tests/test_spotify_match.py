#!/usr/bin/env python3
"""Regression tests for the Spotify -> YouTube Music confidence matcher.

A Spotify import used to take the first search hit, which is how karaoke
tracks, covers, sped-up edits and the right title by another artist ended up
in playlists. These checks pin the rules that replaced it.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "py"))

from cf_backend import core  # noqa: E402


def check(cond, what, failures):
    if not cond:
        print("FAIL:", what, file=sys.stderr)
        failures.append(what)


def spotify(title, artists, duration=200):
    return {"title": title, "subtitle": artists, "duration_sec": duration}


def hit(title, artist, duration=200):
    return {"title": title, "artist": artist, "duration_sec": duration}


def score(source, candidate):
    return core._spotify_match_score(source, candidate)


def test_exact(failures):
    s = score(spotify("Housewife", "Tate McRae"), hit("Housewife", "Tate McRae"))
    check(s > 0.9, "exact title+artist should score near 1 (got %.3f)" % s, failures)


def test_punctuation_and_feat(failures):
    source = spotify("Flower In A Hurricane (feat. Tame Impala, SZA)",
                     "Justice, Tame Impala")
    candidate = hit("Flower In A Hurricane", "Justice")
    check(score(source, candidate) > 0.85,
          "feat. parentheses must not break the match", failures)


def test_topic_channel(failures):
    source = spotify("Housewife", "Tate McRae")
    candidate = hit("Housewife", "Tate McRae - Topic")
    check(score(source, candidate) > 0.9, "'- Topic' channel suffix", failures)


def test_wrong_artist_rejected(failures):
    source = spotify("Housewife", "Tate McRae")
    check(score(source, hit("Housewife", "Some Cover Band")) == 0.0,
          "same title, different artist must be refused", failures)


def test_karaoke_rejected(failures):
    source = spotify("Housewife", "Tate McRae")
    check(score(source, hit("Housewife (Karaoke Version)", "Karaoke Hits")) == 0.0,
          "karaoke upload must be refused", failures)


def test_live_version_penalised(failures):
    source = spotify("Housewife", "Tate McRae")
    live = score(source, hit("Housewife (Live at the Forum)", "Tate McRae", 260))
    check(live == 0.0, "a live cut with a different duration is refused", failures)


def test_remix_needs_remix_in_source(failures):
    source = spotify("Housewife", "Tate McRae")
    check(score(source, hit("Housewife (Dance Remix)", "Tate McRae")) == 0.0,
          "an invented remix is refused", failures)
    remix_source = spotify("Housewife (Dance Remix)", "Tate McRae")
    remix = score(remix_source, hit("Housewife (Dance Remix)", "Tate McRae"))
    check(remix > 0.85, "a remix that the Spotify title names is allowed", failures)


def test_duration_guard(failures):
    source = spotify("Housewife", "Tate McRae", 208)
    check(score(source, hit("Housewife", "Tate McRae", 209)) > 0.9,
          "same performance within a second", failures)
    check(score(source, hit("Housewife", "Tate McRae", 1200)) == 0.0,
          "1 hour loop upload must be refused", failures)


def test_multi_artist_embed_subtitle(failures):
    source = spotify("Flower In A Hurricane",
                     "Justice,\u00a0Tame Impala,\u00a0SZA,\u00a0Grand Theft Auto VI")
    check(score(source, hit("Flower In A Hurricane", "Justice")) > 0.85,
          "embed artist list containing entities still matches the lead artist",
          failures)


def test_artist_only_agreement_required(failures):
    source = spotify("Intro", "The xx")
    check(score(source, hit("Intro", "The xx")) > 0.85,
          "leading 'The' is ignored on both sides", failures)


def main():
    failures = []
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn(failures)
    if failures:
        print("%d Spotify matcher check(s) failed" % len(failures), file=sys.stderr)
        return 1
    print("Spotify matcher: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
