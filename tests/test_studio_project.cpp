#include "studio_project.h"
#include "test_util.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main()
{
    using testutil::check;
    const auto root = testutil::make_config_dir("p100-studio-project-test");

    check(studio_project_id_for_beat("local:abc-123") == "beat_local_abc-123",
          "project id sanitised");

    SongProject p;
    p.id = studio_project_id_for_beat("local:abc");
    p.title = "Demo\tSong"; // tab must be escaped
    p.beat.id = "local:abc";
    p.beat.title = "Beat";
    p.beat.artist = "Artist";
    p.beat.url = "/tmp/beat.flac";
    p.beat.duration_sec = 123;
    p.zoom = 140.0f;
    p.following = false;
    p.snap = false;
    p.loop_a = 4.0;
    p.loop_b = 20.5;
    p.loop_enabled = true;

    Cue c1;
    c1.id = "cue1";
    c1.time = 12.5;
    c1.type = 1;
    c1.color = 3;
    c1.label = "Verse\nOne";
    c1.lyric_id = "lyr1";
    p.cues.push_back(c1);

    Cue c2;
    c2.id = "cue2";
    c2.time = 30.0;
    c2.type = 2;
    c2.color = 5;
    c2.label = "Hook";
    p.cues.push_back(c2);

    LyricLine l1;
    l1.id = "lyr1";
    l1.text = "First line";
    l1.cue_id = "cue1";
    p.lyrics.push_back(l1);

    LyricLine l2;
    l2.id = "lyr2";
    l2.text = "Second\tline"; // tab must be escaped
    p.lyrics.push_back(l2);

    check(save_song_project(p), "save project");
    check(std::filesystem::exists(studio_project_path(p.id)), "project file exists");

    SongProject loaded;
    check(load_song_project(p.id, loaded), "load project");
    check(loaded.title == p.title, "title round-trip (tab escaped)");
    check(loaded.beat.id == p.beat.id && loaded.beat.url == p.beat.url, "beat round-trip");
    check(std::fabs(loaded.zoom - 140.0f) < 0.01f && !loaded.following && !loaded.snap,
          "ui state round-trip");
    check(std::fabs(loaded.loop_a - 4.0) < 1e-9 && std::fabs(loaded.loop_b - 20.5) < 1e-9 &&
              loaded.loop_enabled,
          "loop round-trip");
    check(loaded.cues.size() == 2 && loaded.lyrics.size() == 2, "counts round-trip");
    check(loaded.cues[0].label == "Verse\nOne", "cue label newline round-trip");
    check(loaded.cues[0].lyric_id == "lyr1" && loaded.lyrics[0].cue_id == "cue1",
          "cue/lyric link round-trip");
    check(loaded.lyrics[1].text == "Second\tline", "lyric tab round-trip");
    check(std::fabs(loaded.cues[1].time - 30.0) < 1e-9, "cue time round-trip");

    SongProject latest;
    check(load_latest_song_project(latest) && latest.id == p.id, "load latest");

    {
        const auto ids = list_projects_for_beat(p.beat.id);
        check(ids.size() == 1 && ids[0].first == p.id, "list projects for beat");

        const std::string id2 = new_project_id(p.beat.id);
        check(id2.rfind(studio_project_id_for_beat(p.beat.id), 0) == 0,
              "new project id prefix");
        SongProject p2 = p;
        p2.id = id2;
        p2.title = "Second idea";
        check(save_song_project(p2), "save second project");
        check(list_projects_for_beat(p.beat.id).size() == 2, "two projects for beat");

        SongProject other = p;
        other.id = studio_project_id_for_beat("local:other");
        other.beat.id = "local:other";
        check(save_song_project(other), "save other-beat project");
        check(list_projects_for_beat(p.beat.id).size() == 2,
              "other beat not listed");
    }

    // Unknown keys must be ignored so older builds can open newer projects.
    {
        std::ofstream out(studio_project_path("forward"));
        out << "CFPROJECT 1\nid\tforward\ntitle\tForward\nfuture_key\twhatever\n";
    }
    SongProject fwd;
    check(load_song_project("forward", fwd) && fwd.title == "Forward",
          "unknown keys ignored");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    if (testutil::failures == 0)
        std::printf("studio_project: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
