#include "rooms.h"

#include "json_util.h"
#include "paths.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace rooms
{
namespace
{
std::string g_device;
std::string g_device_label;
std::string g_label_for;

std::string rooms_dir()
{
    return p100_config_path("rooms");
}

void ensure_dir()
{
    std::error_code ec;
    std::filesystem::create_directories(rooms_dir(), ec);
}

std::string room_path(const std::string &id)
{
    return rooms_dir() + "/" + id + ".json";
}

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    std::size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a]))
        ++a;
    return s.substr(a);
}

void parse_float_array(const std::string &json, const char *key, std::vector<double> &out)
{
    out.clear();
    const std::string needle = std::string("\"") + key + "\":";
    std::size_t at = json.find(needle);
    if (at == std::string::npos)
        return;
    at = json.find('[', at);
    if (at == std::string::npos)
        return;
    std::size_t pos = at + 1;
    while (pos < json.size() && out.size() < 512)
    {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ','))
            ++pos;
        if (pos >= json.size() || json[pos] == ']')
            break;
        char *end = nullptr;
        const double v = std::strtod(json.c_str() + pos, &end);
        if (end == json.c_str() + pos)
            break;
        out.push_back(v);
        pos = (std::size_t)(end - json.c_str());
    }
}

#ifndef P100_WINDOWS
// Runs a shell command and returns its first line (empty on failure). Only
// used for the two tiny pactl queries below, cached when the device changes.
std::string first_line(const char *command)
{
    FILE *p = popen(command, "r");
    if (!p)
        return "";
    char buf[1024];
    std::string out;
    if (std::fgets(buf, sizeof(buf), p))
        out = trim(buf);
    pclose(p);
    return out;
}

std::string sink_description(const std::string &sink)
{
    if (sink.empty())
        return "";
    FILE *p = popen("pactl list sinks 2>/dev/null", "r");
    if (!p)
        return "";
    char buf[1024];
    std::string current, description;
    while (std::fgets(buf, sizeof(buf), p))
    {
        std::string line = trim(buf);
        if (line.rfind("Name:", 0) == 0)
            current = trim(line.substr(5));
        else if (line.rfind("Description:", 0) == 0 && current == sink)
        {
            description = trim(line.substr(12));
            break;
        }
    }
    pclose(p);
    return description;
}

std::string query_device()
{
    std::string dev = first_line("pactl get-default-sink 2>/dev/null");
    if (dev.empty())
    {
        // Older pulseaudio/pactl: "Default Sink: name" in `pactl info`.
        FILE *p = popen("pactl info 2>/dev/null", "r");
        if (p)
        {
            char buf[1024];
            while (std::fgets(buf, sizeof(buf), p))
            {
                std::string line = trim(buf);
                if (line.rfind("Default Sink:", 0) == 0)
                {
                    dev = trim(line.substr(13));
                    break;
                }
            }
            pclose(p);
        }
    }
    return dev;
}
#endif

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

void init()
{
    ensure_dir();
}

std::string refresh_output_device()
{
#ifndef P100_WINDOWS
    const std::string dev = query_device();
    if (dev != g_device)
    {
        g_device = dev;
        g_label_for.clear();
    }
    if (!g_device.empty() && g_label_for != g_device)
    {
        g_device_label = sink_description(g_device);
        if (g_device_label.empty())
            g_device_label = g_device;
        g_label_for = g_device;
    }
#endif
    return g_device;
}

std::string output_device()
{
    return g_device;
}

std::string output_device_label()
{
    return g_device_label.empty() ? g_device : g_device_label;
}

std::vector<Profile> list()
{
    std::vector<Profile> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(rooms_dir(), ec);
    if (ec)
        return out;
    for (const auto &entry : it)
    {
        if (!entry.is_regular_file())
            continue;
        const std::string name = entry.path().filename().string();
        if (name.size() < 6 || name.substr(name.size() - 5) != ".json")
            continue;
        Profile p;
        if (load(name.substr(0, name.size() - 5), p))
            out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const Profile &a, const Profile &b) {
        if (a.name != b.name)
            return a.name < b.name;
        return a.saved_at < b.saved_at;
    });
    return out;
}

bool load(const std::string &id, Profile &out)
{
    std::ifstream in(room_path(id));
    if (!in.good())
        return false;
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.id = id;
    out.curve = RoomEqCurve{};
    out.name = jsonutil::get_string(json, "name", id);
    out.device = jsonutil::get_string(json, "device");
    out.preamp = (float)jsonutil::get_double(json, "preamp");
    out.bass_db = (float)jsonutil::get_double(json, "bass");
    out.treble_db = (float)jsonutil::get_double(json, "treble");
    out.mic = jsonutil::get_string(json, "mic");
    out.target = jsonutil::get_string(json, "target");
    out.label = jsonutil::get_string(json, "label");
    out.residual_db = jsonutil::get_double(json, "residual");
    out.saved_at = (long long)jsonutil::get_double(json, "saved");

    std::vector<double> f, g, q;
    parse_float_array(json, "freq", f);
    parse_float_array(json, "gain", g);
    parse_float_array(json, "q", q);
    out.bands.clear();
    for (std::size_t i = 0; i < f.size(); ++i)
    {
        EqBand b;
        b.freq = (float)std::max(20.0, std::min(20000.0, f[i]));
        b.gain = (float)std::max(-15.0, std::min(15.0, i < g.size() ? g[i] : 0.0));
        b.q = (float)std::max(0.1, std::min(10.0, i < q.size() ? q[i] : 1.0));
        out.bands.push_back(b);
    }

    RoomEqCurve &c = out.curve;
    std::vector<double> chz, cmeas, ctarg, ccorr, cvalid;
    parse_float_array(json, "curve_hz", chz);
    parse_float_array(json, "curve_measured", cmeas);
    parse_float_array(json, "curve_target", ctarg);
    parse_float_array(json, "curve_corrected", ccorr);
    parse_float_array(json, "curve_valid", cvalid);
    c.hz.assign(chz.begin(), chz.end());
    c.measured_db.assign(cmeas.begin(), cmeas.end());
    c.target_db.assign(ctarg.begin(), ctarg.end());
    c.corrected_db.assign(ccorr.begin(), ccorr.end());
    c.valid.reserve(cvalid.size());
    for (double v : cvalid)
        c.valid.push_back(v > 0.5 ? 1 : 0);
    c.mic = jsonutil::get_string(json, "curve_mic");
    c.residual_before_db = (float)jsonutil::get_double(json, "curve_before");
    c.residual_after_db = (float)jsonutil::get_double(json, "curve_after");
    c.correct_lo_hz = (float)jsonutil::get_double(json, "curve_lo");
    c.correct_hi_hz = (float)jsonutil::get_double(json, "curve_hi");
    c.content_hi_hz = (float)jsonutil::get_double(json, "curve_content_hi");
    c.max_bands = (int)std::lround(jsonutil::get_double(json, "curve_max_bands"));
    c.sample_rate = (int)std::lround(jsonutil::get_double(json, "curve_rate"));
    if (c.sample_rate <= 0)
        c.sample_rate = 48000;
    if (c.valid.size() != c.hz.size())
        c.valid.assign(c.hz.size(), 1);
    if (c.empty())
        c = RoomEqCurve{};
    return true;
}

bool save_profile(const Profile &profile, std::string &error)
{
    ensure_dir();
    Profile p = profile;
    if (p.id.empty())
        p.id = "room_" + std::to_string(now_ms());
    if (p.name.empty())
        p.name = "Room";
    if (p.saved_at == 0)
        p.saved_at = (long long)std::time(nullptr);

    std::ofstream out(room_path(p.id), std::ios::trunc);
    if (!out)
    {
        error = "Could not write " + room_path(p.id);
        return false;
    }
    out << "{\"name\":\"" << jsonutil::escape(p.name) << "\""
        << ",\"device\":\"" << jsonutil::escape(p.device) << "\""
        << ",\"mic\":\"" << jsonutil::escape(p.mic) << "\""
        << ",\"target\":\"" << jsonutil::escape(p.target) << "\""
        << ",\"label\":\"" << jsonutil::escape(p.label) << "\""
        << ",\"preamp\":" << p.preamp << ",\"bass\":" << p.bass_db
        << ",\"treble\":" << p.treble_db << ",\"residual\":" << p.residual_db
        << ",\"saved\":" << p.saved_at << ",\"freq\":[";
    for (std::size_t i = 0; i < p.bands.size(); ++i)
        out << (i ? "," : "") << p.bands[i].freq;
    out << "],\"gain\":[";
    for (std::size_t i = 0; i < p.bands.size(); ++i)
        out << (i ? "," : "") << p.bands[i].gain;
    out << "],\"q\":[";
    for (std::size_t i = 0; i < p.bands.size(); ++i)
        out << (i ? "," : "") << p.bands[i].q;
    out << "]";
    {
        auto write_floats = [&out](const char *key, const std::vector<float> &v) {
            out << ",\"" << key << "\":[";
            for (std::size_t i = 0; i < v.size(); ++i)
                out << (i ? "," : "") << v[i];
            out << "]";
        };
        const RoomEqCurve &c = p.curve;
        write_floats("curve_hz", c.hz);
        write_floats("curve_measured", c.measured_db);
        write_floats("curve_target", c.target_db);
        write_floats("curve_corrected", c.corrected_db);
        out << ",\"curve_valid\":[";
        for (std::size_t i = 0; i < c.valid.size(); ++i)
            out << (i ? "," : "") << (c.valid[i] ? 1 : 0);
        out << "]"
            << ",\"curve_mic\":\"" << jsonutil::escape(c.mic) << "\""
            << ",\"curve_before\":" << c.residual_before_db
            << ",\"curve_after\":" << c.residual_after_db
            << ",\"curve_lo\":" << c.correct_lo_hz
            << ",\"curve_hi\":" << c.correct_hi_hz
            << ",\"curve_content_hi\":" << c.content_hi_hz
            << ",\"curve_max_bands\":" << c.max_bands
            << ",\"curve_rate\":" << c.sample_rate;
    }
    out << "}";
    return (bool)out;
}

bool remove(const std::string &id)
{
    std::error_code ec;
    return std::filesystem::remove(room_path(id), ec);
}

Profile from_settings(const Settings &settings, const std::string &name)
{
    Profile p;
    p.name = name;
    p.device = g_device;
    p.bands = settings.room_eq_bands;
    p.preamp = settings.room_eq_preamp;
    p.bass_db = settings.room_eq_bass_db;
    p.treble_db = settings.room_eq_treble_db;
    p.mic = settings.room_eq_mic;
    p.target = settings.room_eq_target;
    p.label = settings.room_eq_label;
    p.residual_db = settings.room_eq_residual_db;
    p.curve = settings.room_eq_curve;
    return p;
}

bool save_current(Settings &settings, const std::string &name, std::string &error)
{
    Profile p = from_settings(settings, name);
    if (p.id.empty())
        p.id = "room_" + std::to_string(now_ms());
    if (!save_profile(p, error))
        return false;
    return activate(settings, p.id);
}

bool activate(Settings &s, const std::string &id)
{
    Profile p;
    if (!load(id, p))
        return false;
    s.room_eq_bands = p.bands;
    s.room_eq_preamp = p.preamp;
    s.room_eq_bass_db = p.bass_db;
    s.room_eq_treble_db = p.treble_db;
    s.room_eq_label = p.label;
    s.room_eq_residual_db = (float)p.residual_db;
    s.room_eq_active_id = p.id;
    s.room_eq_curve = p.curve;
    s.room_eq_enabled = !p.bands.empty();
    return true;
}

void deactivate(Settings &s)
{
    s.room_eq_bands.clear();
    s.room_eq_enabled = false;
    s.room_eq_active_id.clear();
    s.room_eq_curve = RoomEqCurve{};
}

std::string match_device(const std::string &device, const std::string &prefer)
{
    if (device.empty())
        return "";
    // Keep the room the user picked if it is bound to this device, so manual
    // selection is never overridden while the output stays the same.
    std::string best;
    long long best_saved = -1;
    for (const Profile &p : list())
    {
        if (p.device != device)
            continue;
        if (!prefer.empty() && p.id == prefer)
            return prefer;
        if (p.saved_at >= best_saved)
        {
            best_saved = p.saved_at;
            best = p.id;
        }
    }
    return best;
}
} // namespace rooms
