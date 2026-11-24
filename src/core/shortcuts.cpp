#include "shortcuts.h"
#include "paths.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace
{

constexpr uint32_t kSignificantMods = SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT | SHORTCUT_MOD_ALT;

#define SC(id, group, label, key, mods) {id, group, label, key, mods}

const std::vector<ShortcutDef> &build_defs()
{
    static const std::vector<ShortcutDef> defs = {
        // ---- Playback ------------------------------------------------
        SC("play_pause", "Playback", "Play / pause", SDLK_SPACE, 0),
        SC("prev_track", "Playback", "Previous track", SDLK_LEFT, SHORTCUT_MOD_CTRL),
        SC("next_track", "Playback", "Next track", SDLK_RIGHT, SHORTCUT_MOD_CTRL),

        // ---- Seeking -------------------------------------------------
        SC("seek_back", "Seeking", "Seek back 5 s", SDLK_LEFT, 0),
        SC("seek_fwd", "Seeking", "Seek forward 5 s", SDLK_RIGHT, 0),
        SC("seek_back_long", "Seeking", "Seek back 20 s", SDLK_LEFT, SHORTCUT_MOD_SHIFT),
        SC("seek_fwd_long", "Seeking", "Seek forward 20 s", SDLK_RIGHT, SHORTCUT_MOD_SHIFT),

        // ---- Volume --------------------------------------------------
        SC("vol_down", "Volume", "Volume down 5%", SDLK_DOWN, 0),
        SC("vol_up", "Volume", "Volume up 5%", SDLK_UP, 0),
        SC("vol_down_10", "Volume", "Volume down 10%", SDLK_DOWN, SHORTCUT_MOD_SHIFT),
        SC("vol_up_10", "Volume", "Volume up 10%", SDLK_UP, SHORTCUT_MOD_SHIFT),

        // ---- Navigation ----------------------------------------------
        SC("focus_search", "Navigation", "Focus search", SDLK_SLASH, 0),
        SC("focus_search_alt", "Navigation", "Focus search (alt)", SDLK_f, SHORTCUT_MOD_CTRL),
        SC("back", "Navigation", "Back / clear search", SDLK_ESCAPE, 0),
        SC("scroll_top", "Navigation", "Scroll to top", SDLK_t, 0),
        SC("shortcut_help", "Navigation", "Shortcut help", SDLK_SLASH, SHORTCUT_MOD_SHIFT),
        SC("settings", "Navigation", "Open settings", SDLK_COMMA, SHORTCUT_MOD_CTRL),

        // ---- Panels --------------------------------------------------
        SC("toggle_lyrics", "Panels", "Toggle lyrics", SDLK_l, 0),
        SC("immersive_lyrics", "Panels", "Full-screen lyrics", SDLK_RETURN, 0),
        SC("toggle_video", "Panels", "Toggle music video", SDLK_v, 0),
        SC("video_theater", "Panels", "Video theater mode", SDLK_f, 0),
        SC("toggle_sidebar", "Panels", "Toggle sidebar", SDLK_s, 0),
        SC("queue", "Panels", "Open up-next queue", SDLK_q, 0),
        SC("party", "Panels", "Open party mode", SDLK_p, 0),
        SC("low_power", "Playback", "Toggle low power mode", SDLK_p, SHORTCUT_MOD_SHIFT),

        // ---- Developer -----------------------------------------------
        SC("backend_debug", "Developer", "Backend debug window", SDLK_F2, 0),
        SC("native_backend_debug", "Developer", "Native backend debug window", SDLK_F3, 0),
        SC("debug_demo", "Developer", "ImGui demo window", SDLK_d,
           SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT),
    };
    return defs;
}

#undef SC

struct Binding
{
    int key;
    uint32_t mods;
};

std::map<std::string, Binding> g_overrides;
bool g_loaded = false;
bool g_dirty = false;
unsigned g_version = 0;

const std::map<std::string, const ShortcutDef *> &index()
{
    static const std::map<std::string, const ShortcutDef *> idx = [] {
        std::map<std::string, const ShortcutDef *> out;
        for (const ShortcutDef &d : build_defs())
            out.emplace(d.id, &d);
        return out;
    }();
    return idx;
}

std::string file_path()
{
    return p100_config_path("shortcuts.json");
}

std::string upper_name(int key)
{
    const char *name = SDL_GetKeyName((SDL_Keycode)key);
    std::string out = name ? name : "?";
    if (out.size() == 1)
        out[0] = (char)std::toupper((unsigned char)out[0]);
    return out;
}

void load_file()
{
    g_overrides.clear();
    FILE *f = std::fopen(file_path().c_str(), "rb");
    if (!f)
        return;
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    std::fclose(f);

    // {"id": "keycode:mods", ...}
    size_t pos = 0;
    while ((pos = text.find('"', pos)) != std::string::npos)
    {
        const size_t key_end = text.find('"', pos + 1);
        if (key_end == std::string::npos)
            break;
        const std::string id = text.substr(pos + 1, key_end - pos - 1);
        const size_t v0 = text.find('"', key_end + 1);
        const size_t v1 = v0 == std::string::npos ? std::string::npos : text.find('"', v0 + 1);
        if (v0 == std::string::npos || v1 == std::string::npos)
            break;
        const std::string value = text.substr(v0 + 1, v1 - v0 - 1);
        pos = v1 + 1;
        const size_t colon = value.find(':');
        if (colon == std::string::npos || index().find(id) == index().end())
            continue;
        const int key = std::atoi(value.substr(0, colon).c_str());
        const uint32_t mods = (uint32_t)std::strtoul(value.substr(colon + 1).c_str(), nullptr, 10);
        if (key != 0)
            g_overrides[id] = Binding{key, mods & kSignificantMods};
    }
}

} // namespace

const std::vector<ShortcutDef> &shortcut_defs()
{
    return build_defs();
}

int shortcut_key(const char *id)
{
    if (!g_loaded)
    {
        load_file();
        g_loaded = true;
    }
    if (id)
    {
        auto it = g_overrides.find(id);
        if (it != g_overrides.end())
            return it->second.key;
    }
    auto def = index().find(id ? id : "");
    return def == index().end() ? 0 : def->second->key;
}

uint32_t shortcut_mods(const char *id)
{
    if (!g_loaded)
    {
        load_file();
        g_loaded = true;
    }
    if (id)
    {
        auto it = g_overrides.find(id);
        if (it != g_overrides.end())
            return it->second.mods;
    }
    auto def = index().find(id ? id : "");
    return def == index().end() ? 0 : def->second->mods;
}

bool shortcut_matches(const char *id, int key, uint32_t mods)
{
    const uint32_t want = shortcut_mods(id) & kSignificantMods;
    if (shortcut_key(id) != key)
        return false;
    return want == (mods & kSignificantMods);
}

bool shortcut_modifier_is(const char *id, uint32_t mods)
{
    return (shortcut_mods(id) & kSignificantMods) == (mods & kSignificantMods);
}

std::string shortcut_binding_label(int key, uint32_t mods)
{
    if (key == 0)
        return "Unbound";
    std::string out;
    if (mods & SHORTCUT_MOD_CTRL)
        out += "Ctrl+";
    if (mods & SHORTCUT_MOD_SHIFT)
        out += "Shift+";
    if (mods & SHORTCUT_MOD_ALT)
        out += "Alt+";
    out += upper_name(key);
    return out;
}

std::string shortcut_label(const char *id)
{
    return shortcut_binding_label(shortcut_key(id), shortcut_mods(id));
}

bool shortcut_is_custom(const char *id)
{
    return id && g_overrides.find(id) != g_overrides.end();
}

void shortcuts_set(const char *id, int key, uint32_t mods)
{
    if (!id || key == 0)
        return;
    auto def = index().find(id);
    if (def == index().end())
        return;
    const uint32_t clean = mods & kSignificantMods;
    if (key == def->second->key && clean == def->second->mods)
    {
        if (g_overrides.erase(id) > 0)
        {
            g_dirty = true;
            ++g_version;
        }
        return;
    }
    g_overrides[id] = Binding{key, clean};
    g_dirty = true;
    ++g_version;
}

void shortcuts_reset(const char *id)
{
    if (id && g_overrides.erase(id) > 0)
    {
        g_dirty = true;
        ++g_version;
    }
}

void shortcuts_reset_all()
{
    if (!g_overrides.empty())
    {
        g_overrides.clear();
        g_dirty = true;
        ++g_version;
    }
}

int shortcuts_custom_count()
{
    return (int)g_overrides.size();
}

unsigned shortcuts_version()
{
    return g_version;
}

void shortcuts_load()
{
    load_file();
    g_loaded = true;
    g_dirty = false;
}

bool shortcuts_dirty()
{
    return g_dirty;
}

void shortcuts_save_if_dirty()
{
    if (!g_dirty)
        return;
    g_dirty = false;
    if (!p100_ensure_config_dir())
        return;
    const std::string path = file_path();
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return;
    std::fprintf(f, "{\n");
    bool first = true;
    for (const ShortcutDef &d : build_defs())
    {
        auto it = g_overrides.find(d.id);
        if (it == g_overrides.end())
            continue;
        std::fprintf(f, "%s  \"%s\": \"%d:%u\"", first ? "" : ",\n", d.id, it->second.key,
                     (unsigned)it->second.mods);
        first = false;
    }
    std::fprintf(f, first ? "}\n" : "\n}\n");
    std::fclose(f);
    std::rename(tmp.c_str(), path.c_str());
}
