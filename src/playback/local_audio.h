#pragma once

#include "ytdlp.h"

#include <string>
#include <vector>

// Local audio files the user has added to the library.
//
// A local track is represented as a normal Track with a stable id of the form
// "local:<fnv1a-hex>" and `url` set to the absolute file path. The id prefix is
// the single source of truth for "this is a file on disk, not a YouTube video":
// stream resolution, downloading and stream recording all skip it.

// Stable id for an audio file path. Canonicalises the path first so the same
// file reached via a symlink or relative path maps to one id.
std::string local_track_id(const std::string &path);
bool is_local_track_id(const std::string &id);

// Build a Track from an audio file: probes tags/duration and falls back to the
// filename stem for the title. Returns false when the file is missing or
// cannot be read.
bool build_local_track(const std::string &path, Track &out);

// Open a native multi-file picker filtered to audio files. Returns canonical
// absolute paths. Empty when the user cancels or no picker is available (the
// caller should then offer the manual path field).
std::vector<std::string> pick_audio_files();

// Cache path for a beat's waveform/beat analysis (<config>/studio/analysis).
// `id` is sanitised so a "local:<hash>" or video id maps to a safe filename.
std::string studio_analysis_cache_path(const std::string &id);
