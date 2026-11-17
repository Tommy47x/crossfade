#include "local_audio.h"
#include "mpvplayer.h"
#include "paths.h"

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <system_error>

#ifndef P100_WINDOWS
#include <sys/wait.h>
#endif

namespace
{
// FNV-1a 64-bit: small, dependency-free, and stable across runs/platforms.
std::uint64_t fnv1a(const std::string &s)
{
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s)
    {
        h ^= (std::uint64_t)c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string canonical_path(const std::string &path)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : p.string();
}

struct PickerResult
{
    bool available = false; // the dialog tool exists
    std::vector<std::string> files;
};

PickerResult run_picker(const std::string &cmd)
{
    PickerResult result;
#ifdef P100_WINDOWS
    FILE *fp = _popen(cmd.c_str(), "r");
#else
    FILE *fp = popen(cmd.c_str(), "r");
#endif
    if (!fp)
        return result;

    char buf[4096];
    while (fgets(buf, (int)sizeof(buf), fp))
    {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty())
            result.files.push_back(line);
    }
#ifdef P100_WINDOWS
    const int rc = _pclose(fp);
#else
    const int rc = pclose(fp);
#endif
    // Exit code 127 means the shell couldn't find the tool. Any other
    // non-zero exit is a normal cancel (zenity/kdialog return 1).
#ifndef P100_WINDOWS
    result.available = !(WIFEXITED(rc) && WEXITSTATUS(rc) == 127);
#else
    result.available = rc == 0 || !result.files.empty();
#endif
    return result;
}
} // namespace

std::string local_track_id(const std::string &path)
{
    const std::string canon = canonical_path(path);
    char buf[32];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)fnv1a(canon));
    return std::string("local:") + buf;
}

bool is_local_track_id(const std::string &id)
{
    return id.rfind("local:", 0) == 0;
}

bool build_local_track(const std::string &path, Track &out)
{
    const std::string abs = canonical_path(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(abs, ec))
        return false;

    LocalAudioMeta meta;
    if (!probe_local_audio_metadata(abs, meta) || !meta.readable)
        return false;

    out = Track{};
    out.id = local_track_id(abs);
    out.url = abs;
    out.title = meta.title.empty() ? std::filesystem::path(abs).stem().string() : meta.title;
    out.artist = meta.artist;
    out.album = meta.album;
    out.duration_sec = (int)(meta.duration + 0.5);
    return true;
}

std::string studio_analysis_cache_path(const std::string &id)
{
    std::string key;
    key.reserve(id.size());
    for (unsigned char c : id)
        key += (std::isalnum(c) || c == '_' || c == '-') ? (char)c : '_';
    if (key.empty())
        key = "analysis";
    const std::string dir = p100_config_path("studio/analysis");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + "/" + key + ".cfwave";
}

std::vector<std::string> pick_audio_files()
{
#ifdef P100_WINDOWS
    // Windows: the Studio panel offers a manual path field instead of adding
    // an untested Win32 dialog. (The native SMTC/toast layer is unrelated.)
    return {};
#else
    static const char *kFilter =
        "*.mp3 *.m4a *.aac *.opus *.ogg *.oga *.flac *.wav *.webm";
    const std::string zenity =
        std::string("zenity --file-selection --multiple --separator='\\n' "
                    "--title='Add local songs' --file-filter='Audio files | ") +
        kFilter + "' 2>/dev/null";
    PickerResult z = run_picker(zenity);
    if (z.available)
        return z.files;

    const std::string kdialog =
        std::string("kdialog --getopenfilename \"$HOME\" 'Audio files (") + kFilter +
        ")' --multiple --separate-output 2>/dev/null";
    PickerResult k = run_picker(kdialog);
    if (k.available)
        return k.files;

    return {};
#endif
}
