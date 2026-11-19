#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Keyboard shortcuts. Every action in the app is registered here with a
// default binding; overrides live in <config>/shortcuts.json and are edited in
// Settings -> Shortcuts. Bindings are stored as (SDL keycode, modifier mask).

enum ShortcutMods : uint32_t
{
    SHORTCUT_MOD_CTRL = 1u << 0,
    SHORTCUT_MOD_SHIFT = 1u << 1,
    SHORTCUT_MOD_ALT = 1u << 2
};

struct ShortcutDef
{
    const char *id;
    const char *group;
    const char *label;
    int key;       // SDL_Keycode value
    uint32_t mods; // ShortcutMods bits
};

const std::vector<ShortcutDef> &shortcut_defs();

// Current binding for an action (override or default).
int shortcut_key(const char *id);
uint32_t shortcut_mods(const char *id);

// Exact match against a key event. Only Ctrl/Shift/Alt are significant.
bool shortcut_matches(const char *id, int key, uint32_t mods);
bool shortcut_modifier_is(const char *id, uint32_t mods);

// "Ctrl+Shift+K" style label and the default binding's label.
std::string shortcut_label(const char *id);
std::string shortcut_binding_label(int key, uint32_t mods);

bool shortcut_is_custom(const char *id);
void shortcuts_set(const char *id, int key, uint32_t mods);
void shortcuts_reset(const char *id);
void shortcuts_reset_all();
int shortcuts_custom_count();
unsigned shortcuts_version();

void shortcuts_load();
bool shortcuts_dirty();
void shortcuts_save_if_dirty();
