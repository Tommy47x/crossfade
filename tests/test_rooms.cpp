#include "rooms.h"
#include "test_util.h"

#include <cstdio>
#include <string>
using testutil::check;

int main()
{
    testutil::make_config_dir("crossfade_test_rooms");
    rooms::init();

    Settings s;
    s.room_eq_enabled = true;
    s.room_eq_bands = {{52.0f, -6.5f, 4.0f}, {140.0f, 2.0f, 1.5f}, {8200.0f, -1.5f, 0.9f}};
    s.room_eq_preamp = -1.25f;
    s.room_eq_bass_db = 1.5f;
    s.room_eq_treble_db = -0.5f;
    s.room_eq_mic = "phone_generic";
    s.room_eq_target = "dynamic";
    s.room_eq_label = "Measured test";
    s.room_eq_residual_db = 0.8f;

    rooms::Profile p = rooms::from_settings(s, "Living room");
    p.id = "room_test1";
    p.device = "alsa_output.test";
    std::string err;
    check(rooms::save_profile(p, err), "save a room");
    check(rooms::list().size() == 1, "one room listed");
    check(rooms::match_device("alsa_output.test") == "room_test1", "device binding matches");
    check(rooms::match_device("alsa_output.other").empty(), "unbound device does not match");

    rooms::Profile loaded;
    check(rooms::load("room_test1", loaded), "load a room");
    check(loaded.name == "Living room", "room name round trip");
    check(loaded.bands.size() == 3, "band count round trip");
    check(loaded.bands[0].freq > 51.9f && loaded.bands[0].freq < 52.1f, "band frequency");
    check(loaded.bands[0].gain < -6.4f && loaded.bands[0].gain > -6.6f, "band gain");
    check(loaded.bass_db == 1.5f && loaded.treble_db == -0.5f, "tone round trip");
    check(loaded.target == "dynamic", "target round trip");

    Settings target;
    check(rooms::activate(target, "room_test1"), "activate a room");
    check(target.room_eq_enabled, "activation enables correction");
    check(target.room_eq_bands.size() == 3, "activation copies bands");
    check(target.room_eq_active_id == "room_test1", "activation records the id");
    check(target.room_eq_preamp < -1.24f, "activation copies preamp");

    rooms::deactivate(target);
    check(!target.room_eq_enabled && target.room_eq_bands.empty(), "deactivate clears");
    check(target.room_eq_active_id.empty(), "deactivate clears the id");

    // Saving the current settings under a fresh name activates the new one.
    check(rooms::save_current(s, "Desk", err), "save current");
    check(rooms::list().size() == 2, "two rooms listed");
    check(!s.room_eq_active_id.empty(), "save_current activates the new room");

    check(rooms::remove("room_test1"), "remove a room");
    check(rooms::list().size() == 1, "room removed from the list");

    if (testutil::failures == 0)
        std::printf("rooms: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
