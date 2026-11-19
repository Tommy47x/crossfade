#pragma once

#include <string>

// Cross-platform update checker and Linux self-updater.
//
// Flow:
//   init(...)                  once, after settings are loaded
//   check_async(manual)        background fetch of version.txt
//   poll()                     every frame on the main thread; surfaces toasts
//   start_update()             toast/settings action: download + stage (Linux)
//                              or launch the one-click installer (Windows)
//   restart_and_update()       "Restart & update" toast action
//   apply_staged_at_startup()  early in app_run(); applies an ignored stage
//   finalize_pending_update()  just before the process exits; applies after an
//                              explicit restart request, then execs the new build
//
// Check/download state lives in <config>/update.json and the staged payload
// in <config>/update-staging/; neither belongs in settings.json (the periodic
// settings dirty-check would rewrite it constantly).
namespace updater
{

enum class State
{
    Idle,        // initialized, never checked
    Checking,    // version.txt fetch in flight
    Available,   // a newer release is known
    UpToDate,    // the running build is the latest release
    Downloading, // fetching/verifying/staging the release asset
    Ready,       // verified payload staged; restart or next launch applies it
    Updating,    // Windows installer launched
    Failed       // see Status::message
};

struct Status
{
    State state = State::Idle;
    std::string current;    // running version
    std::string latest;     // newest known release version
    std::string message;    // failure detail, empty otherwise
    std::string install;    // human-readable install kind for Settings
    long long last_checked = 0;   // unix seconds, 0 = never
    bool can_self_update = false; // download + replace files in place
    bool package_managed = false; // files owned by the distro/root
    bool staged = false;          // a payload is waiting to be applied
};

void init(void (*wake)(), void (*request_quit)(), bool auto_check);
void shutdown();

// `manual` ignores the daily throttle; automatic checks honour it.
void check_async(bool manual = false);
void set_auto_check(bool enabled);

Status status();
void poll();

// Start downloading (Linux) or launch the Windows installer.
void start_update();
// Apply the staged payload after a clean shutdown, then relaunch.
void restart_and_update();
// Throw away a staged payload and go back to "update available".
void discard_staged();
bool open_release_page();
std::string release_page_url();

// App integration (no-ops on Windows). Both may exec the new binary; they
// only return when nothing was applied or the relaunch failed.
void apply_staged_at_startup(int argc, char **argv, bool safe_mode);
void finalize_pending_update(int argc, char **argv);

} // namespace updater
