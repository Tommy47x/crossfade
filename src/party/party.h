#pragma once

#include <string>
#include <vector>

struct AppState;

// ----------------------------------------------------------------------
// Party mode — public surface shared by the host UI, the HTTP server and
// the main loop. Everything in here is called on the UI/main thread except
// the party_api::* functions, which are safe from HTTP worker threads.
// ----------------------------------------------------------------------

struct PartySettings
{
    std::string name = "crossfade party";
    int port = 8484;
    std::string interface_ip; // empty = auto (best private IPv4)
    // "public" reaches guests through a Cloudflare Quick Tunnel (works from any
    // network); "lan" keeps the link on the local network.
    std::string reach = "public";
    std::string tunnel_path; // optional cloudflared override ("" = auto)
    bool require_approval = false;
    int max_per_guest = 3;     // 0 = unlimited
    int cooldown_sec = 5;
    int max_video_minutes = 10; // 0 = unlimited
    int max_queue = 100;
    bool allow_search = true;
    bool allow_duplicates = false;
    bool show_names = true;
    // Guests may play/pause and skip from the party page. Off by default so a
    // host keeps playback control unless they explicitly hand some of it out.
    bool allow_controls = false;

    bool public_reach() const { return reach != "lan"; }

    bool operator==(const PartySettings &o) const
    {
        return name == o.name && port == o.port && interface_ip == o.interface_ip &&
               reach == o.reach && tunnel_path == o.tunnel_path &&
               require_approval == o.require_approval && max_per_guest == o.max_per_guest &&
               cooldown_sec == o.cooldown_sec && max_video_minutes == o.max_video_minutes &&
               max_queue == o.max_queue && allow_search == o.allow_search &&
               allow_duplicates == o.allow_duplicates && show_names == o.show_names &&
               allow_controls == o.allow_controls;
    }
    bool operator!=(const PartySettings &o) const { return !(*this == o); }
};

enum class PartyRunState
{
    Stopped,
    Running,
    Error,
};

struct PartyInterfaceInfo
{
    std::string name;
    std::string ip;
    std::string label;
    bool is_private = false;
    bool is_preferred = false;
    bool is_secondary = false;
};

struct PartyPendingRequest
{
    std::string request_id;
    std::string id;
    std::string title;
    std::string artist;
    std::string by;
    int duration_sec = 0;
};

enum class PartyTunnelState
{
    Off,      // LAN-only party
    Starting, // public link requested; waiting for cloudflared
    Live,     // public URL is ready
    Error,    // tunnel failed; the LAN link remains usable
};

struct PartyStatus
{
    PartyRunState state = PartyRunState::Stopped;
    std::string url; // primary link (public when live, otherwise LAN)
    std::string lan_url;
    std::string ip;
    int port = 0;
    std::string error;
    bool public_reach = false;
    PartyTunnelState tunnel_state = PartyTunnelState::Off;
    std::string tunnel_error;
    int guests = 0;
    int requests_added = 0;
};

// ---- Host-side lifecycle (main thread) ----
void party_init();
void party_set_wake(void (*fn)());
bool party_start(const PartySettings &settings, std::string &error);
void party_stop();
void party_tick(AppState &state, double position_sec, double duration_sec, bool paused);

PartyStatus party_status();
const PartySettings &party_active_settings();
bool party_running();

std::vector<PartyInterfaceInfo> party_interfaces();
std::vector<PartyPendingRequest> party_pending_requests();
void party_approve(const std::string &request_id);
void party_reject(const std::string &request_id);
void party_approve_all();


// ---- HTTP worker-thread surface ----
namespace party_api
{
struct AddResult
{
    int http = 503;
    std::string status;
    std::string error;
    std::string message;
    int retry_after = 0;
    std::string item_json; // one serialized queue item when queued
};

// Join token -> (new) guest id. The token is compared in constant time.
bool join(const std::string &token, std::string &guest_id, bool &is_new);
bool guest_exists(const std::string &guest_id);

// Full state JSON, or a tiny unchanged reply when `since` matches.
std::string state_json(const std::string &guest_id, const std::string &since);

AddResult add(const std::string &guest_id, const std::string &input);
AddResult undo(const std::string &guest_id, const std::string &video_id);
// Move a song that is already in Up Next to the front of the queue.
AddResult bump(const std::string &guest_id, const std::string &video_id);
// Guest media controls: "play", "pause", "next" or "prev". Only accepted
// while the host has guest controls enabled.
AddResult control(const std::string &guest_id, const std::string &action);
bool set_name(const std::string &guest_id, const std::string &name);

// Search (main backend channel, rate-limited per guest). Returns a JSON
// array string on success.
bool search(const std::string &guest_id, const std::string &query,
            std::string &json, int &http, int &retry_after);

// Local artwork path for the validated 11-char id, or "" when not cached.
std::string art_path(const std::string &video_id);

std::string page_html();
} // namespace party_api
