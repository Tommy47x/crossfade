#include "studio_project.h"
#include "paths.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
// Field escaping: tab-separated records, so only tabs/newlines/backslashes
// need encoding. Everything else (including UTF-8) passes through.
std::string esc(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }
    return out;
}

std::string unesc(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            const char n = s[++i];
            if (n == 't')
                out += '\t';
            else if (n == 'n')
                out += '\n';
            else if (n == 'r')
                out += '\r';
            else
                out += n;
        }
        else
            out += s[i];
    }
    return out;
}

std::vector<std::string> split_tabs(const std::string &line)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;)
    {
        const size_t p = line.find('\t', start);
        if (p == std::string::npos)
        {
            out.push_back(line.substr(start));
            break;
        }
        out.push_back(line.substr(start, p - start));
        start = p + 1;
    }
    return out;
}
} // namespace

std::string studio_project_id_for_beat(const std::string &beat_id)
{
    std::string key;
    key.reserve(beat_id.size());
    for (unsigned char c : beat_id)
        key += (std::isalnum(c) || c == '_' || c == '-') ? (char)c : '_';
    if (key.empty())
        key = "untitled";
    return "beat_" + key;
}

std::string studio_project_path(const std::string &project_id)
{
    return p100_config_path("studio/projects/" + project_id + ".cfproj");
}

bool save_song_project(const SongProject &p)
{
    if (p.id.empty())
        return false;
    const std::string path = studio_project_path(p.id);
    const std::string tmp = path + ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::ofstream out(tmp, std::ios::binary);
    if (!out.good())
        return false;
    out << "CFPROJECT 1\n";
    out << "id\t" << esc(p.id) << "\n";
    out << "title\t" << esc(p.title) << "\n";
    out << "beat_id\t" << esc(p.beat.id) << "\n";
    out << "beat_title\t" << esc(p.beat.title) << "\n";
    out << "beat_artist\t" << esc(p.beat.artist) << "\n";
    out << "beat_artist_id\t" << esc(p.beat.artist_id) << "\n";
    out << "beat_album\t" << esc(p.beat.album) << "\n";
    out << "beat_album_id\t" << esc(p.beat.album_id) << "\n";
    out << "beat_thumb\t" << esc(p.beat.thumbnail_url) << "\n";
    out << "beat_url\t" << esc(p.beat.url) << "\n";
    out << "beat_duration\t" << p.beat.duration_sec << "\n";
    out << "zoom\t" << p.zoom << "\n";
    out << "follow\t" << (p.following ? 1 : 0) << "\n";
    out << "snap\t" << (p.snap ? 1 : 0) << "\n";
    out << "loop_a\t" << p.loop_a << "\n";
    out << "loop_b\t" << p.loop_b << "\n";
    out << "loop_enabled\t" << (p.loop_enabled ? 1 : 0) << "\n";
    for (const LyricLine &l : p.lyrics)
        out << "lyric\t" << esc(l.id) << "\t" << esc(l.cue_id) << "\t" << esc(l.text) << "\n";
    for (const Cue &c : p.cues)
        out << "cue\t" << esc(c.id) << "\t" << c.time << "\t" << c.type << "\t" << c.color
            << "\t" << esc(c.label) << "\t" << esc(c.lyric_id) << "\n";
    out.close();
    if (!out.good())
    {
        std::remove(tmp.c_str());
        return false;
    }

    std::filesystem::remove(path, ec); // Windows rename does not replace
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool load_song_project(const std::string &project_id, SongProject &out)
{
    std::ifstream in(studio_project_path(project_id), std::ios::binary);
    if (!in.good())
        return false;

    SongProject p;
    p.id = project_id;
    std::string line;
    if (!std::getline(in, line) || line.rfind("CFPROJECT", 0) != 0)
        return false;

    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const std::vector<std::string> f = split_tabs(line);
        const std::string &key = f[0];
        auto get = [&](size_t i) { return i < f.size() ? unesc(f[i]) : std::string(); };

        if (key == "id")
            p.id = get(1);
        else if (key == "title")
            p.title = get(1);
        else if (key == "beat_id")
            p.beat.id = get(1);
        else if (key == "beat_title")
            p.beat.title = get(1);
        else if (key == "beat_artist")
            p.beat.artist = get(1);
        else if (key == "beat_artist_id")
            p.beat.artist_id = get(1);
        else if (key == "beat_album")
            p.beat.album = get(1);
        else if (key == "beat_album_id")
            p.beat.album_id = get(1);
        else if (key == "beat_thumb")
            p.beat.thumbnail_url = get(1);
        else if (key == "beat_url")
            p.beat.url = get(1);
        else if (key == "beat_duration")
            p.beat.duration_sec = std::atoi(get(1).c_str());
        else if (key == "zoom")
            p.zoom = (float)std::atof(get(1).c_str());
        else if (key == "follow")
            p.following = get(1) != "0";
        else if (key == "snap")
            p.snap = get(1) != "0";
        else if (key == "loop_a")
            p.loop_a = std::atof(get(1).c_str());
        else if (key == "loop_b")
            p.loop_b = std::atof(get(1).c_str());
        else if (key == "loop_enabled")
            p.loop_enabled = get(1) != "0";
        else if (key == "lyric")
        {
            LyricLine l;
            l.id = get(1);
            l.cue_id = get(2);
            l.text = get(3);
            if (!l.id.empty())
                p.lyrics.push_back(std::move(l));
        }
        else if (key == "cue")
        {
            Cue c;
            c.id = get(1);
            c.time = std::atof(get(2).c_str());
            c.type = std::atoi(get(3).c_str());
            c.color = std::atoi(get(4).c_str());
            c.label = get(5);
            c.lyric_id = get(6);
            if (!c.id.empty())
                p.cues.push_back(std::move(c));
        }
    }
    out = std::move(p);
    return true;
}

bool load_latest_song_project(SongProject &out)
{
    namespace fs = std::filesystem;
    const fs::path dir = p100_config_path("studio/projects");
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return false;

    fs::path best;
    fs::file_time_type best_time{};
    for (const auto &entry : fs::directory_iterator(dir, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;
        if (entry.path().extension() != ".cfproj")
            continue;
        const auto t = entry.last_write_time(ec);
        if (best.empty() || t > best_time)
        {
            best = entry.path();
            best_time = t;
        }
    }
    if (best.empty())
        return false;
    return load_song_project(best.stem().string(), out);
}

std::vector<std::pair<std::string, std::string>> list_projects_for_beat(
    const std::string &beat_id)
{
    std::vector<std::pair<std::string, std::string>> out;
    if (beat_id.empty())
        return out;

    namespace fs = std::filesystem;
    const fs::path dir = p100_config_path("studio/projects");
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;

    for (const auto &entry : fs::directory_iterator(dir, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;
        if (entry.path().extension() != ".cfproj")
            continue;
        SongProject p;
        if (!load_song_project(entry.path().stem().string(), p))
            continue;
        // Match the beat stored in the file, not just the filename prefix:
        // two sanitised beat ids can share a prefix.
        if (p.beat.id != beat_id)
            continue;
        out.emplace_back(p.id, p.title);
    }
    std::sort(out.begin(), out.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    return out;
}

std::string new_project_id(const std::string &beat_id)
{
    static std::atomic<unsigned> counter{0};
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s_%llx%u", studio_project_id_for_beat(beat_id).c_str(),
                  (unsigned long long)us, counter.fetch_add(1));
    return buf;
}

std::string new_studio_id(const char *prefix)
{
    static std::atomic<unsigned> counter{0};
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s%llx%u", prefix ? prefix : "id",
                  (unsigned long long)us, counter.fetch_add(1));
    return buf;
}
