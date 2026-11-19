#include "updater.h"

#include "json_util.h"
#include "paths.h"
#include "tasks.h"
#include "ui_notify.h"
#include "update_apply.h"
#include "version.h"
#include "version_compare.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#ifdef P100_WINDOWS
#include "win_process.h"

#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace updater
{
namespace
{

constexpr const char *kVersionUrl =
    "https://github.com/XBoom31/crossfade/releases/latest/download/version.txt";
constexpr long long kCheckIntervalSec = 24 * 60 * 60;
constexpr long long kRetryIntervalSec = 60 * 60;

std::mutex g_mu;
Status g_status;
std::string g_notified;       // version whose "available" toast was shown
std::string g_staged_version; // from update-staging/state.json
void (*g_wake)() = nullptr;
void (*g_request_quit)() = nullptr;
bool g_auto_check = true;
bool g_shutting_down = false;
bool g_ready_toasted = false;
bool g_download_failed = false;
bool g_restart_requested = false;

std::mutex g_file_mu;

struct Stage
{
    int schema = 1;
    std::string version;
    std::string kind; // appimage | prefix | flat
    std::string asset;
    std::string root;
    int attempts = 0;
};

std::string current_version()
{
#ifdef CROSSFADE_VERSION
    return CROSSFADE_VERSION;
#else
    return "0.0.0";
#endif
}

std::string stage_dir() { return p100_config_path("update-staging"); }

#ifdef P100_WINDOWS
std::filesystem::path installed_updater()
{
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
        return {};
    std::filesystem::path p(buf);
    return p.parent_path() / L"install-crossfade.cmd";
}
#endif

std::string kind_name(update_apply::Kind kind)
{
    switch (kind)
    {
    case update_apply::Kind::AppImage:
        return "appimage";
    case update_apply::Kind::Prefix:
        return "prefix";
    case update_apply::Kind::FlatDir:
        return "flat";
    default:
        return "none";
    }
}

void refresh_target_locked()
{
    const update_apply::Target target = update_apply::detect_install();
    g_status.can_self_update = update_apply::self_update_supported(target);
    g_status.package_managed = false;
    switch (target.kind)
    {
    case update_apply::Kind::WindowsInstaller:
        g_status.install = "Windows installer";
        break;
    case update_apply::Kind::AppImage:
        g_status.install = "AppImage";
        break;
    case update_apply::Kind::Prefix:
        g_status.install = "User install";
        break;
    case update_apply::Kind::FlatDir:
        g_status.install = "Release tarball";
        break;
    default:
        g_status.install = "Read-only or development build";
        if (!target.exe.empty())
            g_status.package_managed = target.exe.rfind("/usr/", 0) == 0 ||
                                       target.exe.rfind("/opt/", 0) == 0;
        if (g_status.package_managed)
            g_status.install = "Package-managed";
        break;
    }
}

bool load_stage(Stage &s)
{
    const std::string json = update_apply::read_text_file(stage_dir() + "/state.json");
    if (json.empty())
        return false;
    s.schema = jsonutil::get_int(json, "schema", 1);
    s.version = jsonutil::get_string(json, "version");
    s.kind = jsonutil::get_string(json, "kind");
    s.asset = jsonutil::get_string(json, "asset");
    s.root = jsonutil::get_string(json, "root");
    s.attempts = jsonutil::get_int(json, "attempts", 0);
    return s.schema == 1 && !s.version.empty() && !s.asset.empty();
}

bool save_stage(const Stage &s)
{
    std::string json = "{";
    json += "\"schema\":1";
    json += ",\"version\":\"" + jsonutil::escape(s.version) + "\"";
    json += ",\"kind\":\"" + jsonutil::escape(s.kind) + "\"";
    json += ",\"asset\":\"" + jsonutil::escape(s.asset) + "\"";
    json += ",\"root\":\"" + jsonutil::escape(s.root) + "\"";
    json += ",\"attempts\":" + std::to_string(s.attempts);
    json += "}";
    return update_apply::write_text_file(stage_dir() + "/state.json", json);
}

void clear_stage_locked()
{
    g_staged_version.clear();
    g_status.staged = false;
    update_apply::remove_tree(stage_dir());
}

void clear_stage()
{
    std::lock_guard<std::mutex> lock(g_mu);
    clear_stage_locked();
}

void persist_check_state()
{
    std::string json;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        json = "{";
        json += "\"schema\":1";
        json += ",\"last_checked\":" + std::to_string(g_status.last_checked);
        json += ",\"latest\":\"" + jsonutil::escape(g_status.latest) + "\"";
        json += ",\"notified\":\"" + jsonutil::escape(g_notified) + "\"";
        json += "}";
    }
    std::lock_guard<std::mutex> file_lock(g_file_mu);
    const std::string path = p100_config_path("update.json");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return;
        out << json;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
        std::filesystem::remove(tmp, ec);
}

void load_check_state()
{
    const std::string json = update_apply::read_text_file(p100_config_path("update.json"));
    if (json.empty())
        return;
    std::lock_guard<std::mutex> lock(g_mu);
    g_status.last_checked = (long long)jsonutil::get_double(json, "last_checked", 0.0);
    g_status.latest = jsonutil::get_string(json, "latest");
    g_notified = jsonutil::get_string(json, "notified");
}

State resting_state_locked()
{
    if (!g_staged_version.empty() &&
        version_compare::newer(g_staged_version, current_version()))
        return State::Ready;
    if (!g_status.latest.empty())
        return version_compare::newer(g_status.latest, current_version()) ? State::Available
                                                                         : State::UpToDate;
    return State::Idle;
}

void settle_after_stage_locked()
{
    if (g_staged_version.empty())
        g_status.state = resting_state_locked();
}

bool fetch_version(std::string &out)
{
    out.clear();
#ifdef P100_WINDOWS
    const std::string cmd = std::string("curl -fsSL --connect-timeout 8 --max-time 20 \"") +
                            kVersionUrl + "\" 2>nul";
    FILE *pipe = winproc::popen_hidden(cmd, "rb");
    if (!pipe)
        return false;
    char buf[512];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, pipe);
    if (n > 0)
        out.assign(buf, n);
    const int rc = winproc::pclose_hidden(pipe);
    return rc == 0 && !out.empty();
#else
    const std::string cmd = std::string("curl -fsSL --connect-timeout 8 --max-time 20 \"") +
                            kVersionUrl + "\" 2>/dev/null";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return false;
    char buf[512];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, pipe);
    if (n > 0)
        out.assign(buf, n);
    const int rc = pclose(pipe);
    return rc == 0 && !out.empty();
#endif
}

void do_check()
{
    std::string body;
    const bool fetched = fetch_version(body);
    const std::string latest = version_compare::canonical(version_compare::trim(body));
    const bool parsed = !latest.empty();
    const long long now = update_apply::now_epoch();

    State state = State::Failed;
    std::string message = "Couldn't reach the update server.";
    if (fetched && parsed)
    {
        if (version_compare::newer(latest, current_version()))
        {
            state = State::Available;
            message.clear();
        }
        else
        {
            state = State::UpToDate;
            message.clear();
        }
    }
    else if (fetched)
    {
        message = "The update server returned an unreadable version.";
    }

    void (*wake)() = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_shutting_down)
            return;
        g_status.last_checked = now;
        g_status.state = state;
        g_status.message = message;
        if (parsed)
            g_status.latest = latest;
        wake = g_wake;
    }
    persist_check_state();
    if (wake)
        wake();
}

void fail_download(const std::string &message)
{
    void (*wake)() = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = State::Failed;
        g_status.message = message;
        g_download_failed = true;
        wake = g_wake;
    }
    {
        std::lock_guard<std::mutex> file_lock(g_file_mu);
        update_apply::remove_tree(stage_dir());
    }
    if (wake)
        wake();
}

void do_download()
{
    Status snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        snapshot = g_status;
    }

    const update_apply::Target target = update_apply::detect_install();
    if (!update_apply::self_update_supported(target))
    {
        fail_download("This copy of crossfade cannot update itself.");
        return;
    }
    const std::string version = version_compare::canonical(snapshot.latest);
    const std::string asset = update_apply::asset_name(target.kind, version);
    if (asset.empty() || !version_compare::newer(version, current_version()))
    {
        fail_download("There is no update to download.");
        return;
    }

    const std::string dir = stage_dir();
    update_apply::remove_tree(dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        fail_download("Could not create the update staging directory.");
        return;
    }
    update_apply::write_text_file(dir + "/.downloading", "1");

    std::string err;
    const std::string sums = dir + "/SHA256SUMS";
    if (!update_apply::download(update_apply::asset_url("SHA256SUMS"), sums, err))
    {
        fail_download(err);
        return;
    }
    const std::string file = dir + "/" + asset;
    if (!update_apply::download(update_apply::asset_url(asset), file, err))
    {
        fail_download(err);
        return;
    }
    if (!update_apply::verify(file, sums, asset, err))
    {
        fail_download(err);
        return;
    }
    if (target.kind != update_apply::Kind::AppImage)
    {
        if (!update_apply::extract(file, dir + "/extracted", version, err))
        {
            fail_download(err);
            return;
        }
        std::error_code remove_ec;
        std::filesystem::remove(file, remove_ec);
    }

    Stage stage;
    stage.version = snapshot.latest;
    stage.kind = kind_name(target.kind);
    stage.asset = asset;
    stage.root = target.root;
    if (!save_stage(stage))
    {
        fail_download("Could not save the update staging state.");
        return;
    }
    std::filesystem::remove(dir + "/.downloading", ec);

    void (*wake)() = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_shutting_down)
            return;
        g_staged_version = stage.version;
        g_status.staged = true;
        g_status.state = State::Ready;
        g_status.message.clear();
        g_ready_toasted = false;
        wake = g_wake;
    }
    if (wake)
        wake();
}

void open_release_action() { open_release_page(); }

void show_available_toast(const Status &s)
{
    if (s.can_self_update)
    {
#ifdef P100_WINDOWS
        ui_notify(UiNotifyType::Info, "crossfade update available",
                  "Version " + s.latest +
                      " is ready. The updater opens in its own window and will offer to "
                      "restart crossfade when it finishes.",
                  0.0f, "Update now", &start_update);
#else
        ui_notify(UiNotifyType::Info, "crossfade update available",
                  "Version " + s.latest +
                      " is available. crossfade downloads it from GitHub, verifies its "
                      "SHA-256, and asks before restarting.",
                  12.0f, "Update now", &start_update);
#endif
    }
    else
    {
        ui_notify(UiNotifyType::Info, "crossfade update available",
                  "Version " + s.latest +
                      " is available. This install is managed by your package manager, so "
                      "update it from the release page.",
                  12.0f, "Open release page", &open_release_action);
    }
}

void relaunch(const update_apply::Target &target, int argc, char **argv)
{
    std::string exe;
    switch (target.kind)
    {
    case update_apply::Kind::AppImage:
        exe = target.root;
        break;
    case update_apply::Kind::Prefix:
        exe = target.root + "/bin/crossfade";
        break;
    case update_apply::Kind::FlatDir:
        exe = target.root + "/crossfade";
        break;
    default:
        return;
    }
#ifdef P100_WINDOWS
    (void)argc;
    (void)argv;
    (void)exe;
#else
    std::vector<char *> args;
    args.reserve((size_t)(argc > 0 ? argc : 1));
    args.push_back(const_cast<char *>(exe.c_str()));
    for (int i = 1; i < argc; ++i)
        args.push_back(argv[i]);
    args.push_back(nullptr);
    execv(exe.c_str(), args.data());
    std::string message = "The new version was installed but could not be started (" + exe + ").";
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = State::Failed;
        g_status.message = message;
    }
#endif
}

void apply_and_relaunch(const Stage &stage, int argc, char **argv)
{
    const update_apply::Target target = update_apply::detect_install();
    if (!update_apply::self_update_supported(target))
    {
        clear_stage();
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = State::Failed;
        g_status.message = "This copy of crossfade cannot update itself.";
        return;
    }

    // Never trust the staged metadata for paths: rebuild the version and
    // asset name from the numeric version alone.
    const std::string version = version_compare::canonical(stage.version);
    const std::string asset = update_apply::asset_name(target.kind, version);
    if (version.empty() || asset.empty())
    {
        clear_stage();
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = State::Failed;
        g_status.message = "The staged update is not readable.";
        return;
    }

    std::string err;
    if (!update_apply::apply(target, stage_dir(), version, asset, err))
    {
        Stage retry = stage;
        ++retry.attempts;
        if (retry.attempts >= 2)
            clear_stage();
        else
        {
            save_stage(retry);
            std::lock_guard<std::mutex> lock(g_mu);
            g_staged_version = retry.version;
            g_status.staged = true;
        }
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = State::Failed;
        g_status.message = err;
        return;
    }

    clear_stage();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = State::UpToDate;
        g_status.latest = version;
        g_status.last_checked = update_apply::now_epoch();
        g_notified = version;
    }
    persist_check_state();
    relaunch(target, argc, argv);
}

} // namespace

void init(void (*wake)(), void (*request_quit)(), bool auto_check)
{
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_wake = wake;
        g_request_quit = request_quit;
        g_auto_check = auto_check;
        g_status.current = current_version();
        refresh_target_locked();
    }
    load_check_state();

    Stage stage;
    if (load_stage(stage))
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_staged_version = stage.version;
        g_status.staged = true;
        if (g_status.latest.empty() ||
            version_compare::newer(stage.version, g_status.latest))
            g_status.latest = stage.version;
    }
    else
    {
        // A crash or a cancelled download can leave a directory without a
        // finished stage state. Don't delete a sibling instance's in-flight
        // download: .part files mean it is still running elsewhere.
        const std::string dir = stage_dir();
        std::error_code ec;
        bool in_flight = std::filesystem::exists(dir + "/.downloading", ec);
        if (!in_flight && std::filesystem::is_directory(dir, ec))
        {
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                if (entry.path().extension() == ".part")
                {
                    in_flight = true;
                    break;
                }
            }
            if (!ec && !in_flight)
                update_apply::remove_tree(dir);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = resting_state_locked();
    }

    if (!auto_check)
        return;

    long long last_checked = 0;
    State state = State::Idle;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        last_checked = g_status.last_checked;
        state = g_status.state;
    }
    const long long now = update_apply::now_epoch();
    const long long interval = state == State::Failed ? kRetryIntervalSec : kCheckIntervalSec;
    if (state != State::Ready &&
        (last_checked <= 0 || now < last_checked || now - last_checked >= interval))
        check_async(false);
}

void shutdown()
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_shutting_down = true;
    g_wake = nullptr;
}

void check_async(bool manual)
{
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_shutting_down)
            return;
        if (g_status.state == State::Checking || g_status.state == State::Downloading ||
            g_status.state == State::Updating || g_status.state == State::Ready)
            return;
        if (!manual && !g_auto_check)
            return;
        if (!manual && g_status.last_checked > 0)
        {
            const long long now = update_apply::now_epoch();
            const long long interval =
                g_status.state == State::Failed ? kRetryIntervalSec : kCheckIntervalSec;
            if (now >= g_status.last_checked && now - g_status.last_checked < interval)
                return;
        }
        g_status.state = State::Checking;
        g_status.current = current_version();
        g_status.message.clear();
    }
    tasks::spawn("update-check", []() { do_check(); });
}

void set_auto_check(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_auto_check = enabled;
    }
    if (enabled)
        check_async(true);
}

Status status()
{
    std::lock_guard<std::mutex> lock(g_mu);
    Status s = g_status;
    if (s.current.empty())
        s.current = current_version();
    s.staged = !g_staged_version.empty();
    return s;
}

void poll()
{
    bool shutting_down = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        shutting_down = g_shutting_down;
    }
    if (shutting_down)
        return;

    Status s = status();
    bool show_available = false;
    bool show_ready = false;
    bool show_download_failure = false;
    std::string staged_version;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_status.state == State::Available && !g_status.latest.empty() &&
            g_notified != g_status.latest)
        {
            g_notified = g_status.latest;
            show_available = true;
        }
        if (g_status.state == State::Ready && !g_ready_toasted)
        {
            g_ready_toasted = true;
            staged_version = g_staged_version;
            show_ready = true;
        }
        if (g_status.state == State::Failed && g_download_failed)
        {
            g_download_failed = false;
            show_download_failure = true;
        }
    }

    if (show_available)
    {
        show_available_toast(s);
        persist_check_state();
    }
    if (show_ready)
    {
        ui_notify(UiNotifyType::Info, "Update ready",
                  "crossfade " + staged_version +
                      " is downloaded and verified. Restart to finish updating.",
                  0.0f, "Restart & update", &restart_and_update);
    }
    if (show_download_failure)
    {
        ui_notify(UiNotifyType::Error, "Update download failed", s.message, 8.0f, "Try again",
                  &start_update);
    }
}

void start_update()
{
#ifdef P100_WINDOWS
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_status.state == State::Downloading || g_status.state == State::Updating)
            return;
        g_status.state = State::Updating;
    }

    const std::filesystem::path local_cmd = installed_updater();

    std::wstring file = L"cmd.exe";
    std::wstring params;
    if (!local_cmd.empty() && std::filesystem::exists(local_cmd))
    {
        params = L"/c \"" + local_cmd.wstring() + L"\"";
    }
    else
    {
        // Older installs don't ship the updater next to the exe. Download the
        // current one-click cmd and let it do the normal release install.
        file = L"powershell.exe";
        params = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command \""
                 L"$u='https://github.com/XBoom31/crossfade/releases/latest/download/install-crossfade.cmd';"
                 L"$p=Join-Path $env:TEMP 'crossfade-update.cmd';"
                 L"Invoke-WebRequest -UseBasicParsing $u -OutFile $p;"
                 L"Start-Process cmd.exe -ArgumentList '/c',$p\"";
    }

    const HINSTANCE r = ShellExecuteW(nullptr, L"open", file.c_str(), params.c_str(), nullptr,
                                     SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = State::Failed;
        g_status.message = "The updater process could not be started.";
        g_download_failed = true;
    }
#else
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_shutting_down)
            return;
        if (g_status.state == State::Downloading || g_status.state == State::Ready)
            return;
        if (g_status.latest.empty() ||
            !version_compare::newer(g_status.latest, current_version()))
            return;
        if (!g_status.can_self_update)
        {
            // Package-managed/read-only install: fall back to the release page.
        }
        else
        {
            g_status.state = State::Downloading;
            g_status.message.clear();
            g_download_failed = false;
            tasks::spawn("update-download", []() { do_download(); });
            return;
        }
    }
    open_release_page();
#endif
}

void restart_and_update()
{
    void (*wake)() = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_status.state != State::Ready)
            return;
        g_restart_requested = true;
        wake = g_wake;
    }
    if (g_request_quit)
        g_request_quit();
    // The loop may be parked in SDL_WaitEventTimeout with an idle budget.
    if (wake)
        wake();
}

void discard_staged()
{
    clear_stage();
    std::lock_guard<std::mutex> lock(g_mu);
    g_status.staged = false;
    g_status.message.clear();
    settle_after_stage_locked();
}

bool open_release_page()
{
    const std::string url = release_page_url();
    return !url.empty() && SDL_OpenURL(url.c_str()) == 0;
}

std::string release_page_url() { return update_apply::release_page_url(); }

void apply_staged_at_startup(int argc, char **argv, bool safe_mode)
{
#ifndef P100_WINDOWS
    if (safe_mode)
        return;
    Stage stage;
    if (!load_stage(stage))
        return;
    if (!version_compare::newer(stage.version, current_version()))
    {
        clear_stage();
        return;
    }
    apply_and_relaunch(stage, argc, argv);
#else
    (void)argc;
    (void)argv;
    (void)safe_mode;
#endif
}

void finalize_pending_update(int argc, char **argv)
{
#ifndef P100_WINDOWS
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_restart_requested)
            return;
    }
    Stage stage;
    if (!load_stage(stage))
        return;
    apply_and_relaunch(stage, argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
}

} // namespace updater
