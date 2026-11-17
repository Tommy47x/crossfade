#pragma once

#include "ytdlp.h"

#include <string>
#include <utility>
#include <vector>

// ----------------------------------------------------------------------
// Songwriting project.
//
// One project belongs to one beat. Lyrics and cues are linked 1:1: the cue
// owns the time, the lyric line owns the link, so moving a cue automatically
// moves the line it is tied to (there is no second copy of the time to drift).
//
// Persisted as a small tab-separated text file under
// <config>/studio/projects/<id>.cfproj (see studio_project.cpp). The format is
// deliberately dependency-free and forwards-compatible: unknown keys are
// ignored on load.
// ----------------------------------------------------------------------

struct LyricLine
{
    std::string id;
    std::string text;
    std::string cue_id; // optional link to a Cue
};

struct Cue
{
    std::string id;
    double time = 0.0;
    int type = 0;  // 0 marker, 1 section, 2 note
    int color = 0; // palette index
    std::string label;
    std::string lyric_id; // optional 1:1 link to a LyricLine
};

struct SongProject
{
    std::string id;
    std::string title;
    Track beat;
    std::vector<LyricLine> lyrics;
    std::vector<Cue> cues;
    float zoom = 90.0f;
    bool following = true;
    bool snap = true;
    // A-B loop region (seconds). -1 = unset.
    double loop_a = -1.0;
    double loop_b = -1.0;
    bool loop_enabled = false;
};

// Stable project id for a beat (one project per beat). Sanitised to a safe
// filename component.
std::string studio_project_id_for_beat(const std::string &beat_id);
std::string studio_project_path(const std::string &project_id);

bool load_song_project(const std::string &project_id, SongProject &out);
bool save_song_project(const SongProject &project);
// Most recently modified project, for restoring the last session's work.
bool load_latest_song_project(SongProject &out);

// All projects that belong to one beat, as (id, title) pairs, base project
// first. Projects are matched by the beat id stored inside the file.
std::vector<std::pair<std::string, std::string>> list_projects_for_beat(
    const std::string &beat_id);
// A fresh project id for an additional idea on the same beat.
std::string new_project_id(const std::string &beat_id);

// Unique id with a short readable prefix (cues, lyric lines).
std::string new_studio_id(const char *prefix);
