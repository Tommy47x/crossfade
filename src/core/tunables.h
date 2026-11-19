#pragma once

#include <string>
#include <vector>

// Developer tunables: the values that used to be hardcoded across playback,
// network, UI motion, party and the Python backend. Each one is registered
// here with a default, a safe range and the group it belongs to; the Settings
// -> Tunables tab renders them with a search box.
//
// Overrides live in <config>/tunables.json, so the Python backend can read the
// same file at startup (entries marked `restart` only take effect then).

enum class TunableKind
{
    Float,
    Int,
    Bool
};

struct TunableDef
{
    const char *key;
    const char *group;
    const char *label;
    const char *desc;
    float def;
    float min;
    float max;
    TunableKind kind;
    bool restart; // read once at startup (backend / process init)
};

const std::vector<TunableDef> &tunable_defs();

// Current value: the override if one is set, otherwise the default.
float tunable(const char *key);
int tunable_int(const char *key);
bool tunable_bool(const char *key);

void tunables_load();
void tunables_set(const char *key, float value);
void tunables_reset(const char *key);
void tunables_reset_all();
bool tunables_is_set(const char *key);
int tunables_changed_count();
bool tunables_dirty();
// Increments whenever an override changes; UI caches compare it.
unsigned tunables_version();
void tunables_save_if_dirty();
