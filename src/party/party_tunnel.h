#pragma once

#include <string>

// Cloudflare Quick Tunnel bridge for Party mode.
//
// The party HTTP server stays bound to the LAN address. When public access is
// enabled we spawn `cloudflared tunnel --url http://<lan-ip>:<port>` and read
// the assigned https://<random>.trycloudflare.com URL from its log. Guests then
// reach the same guest page through Cloudflare; no inbound port or account is
// needed.
//
// Everything here is main-thread friendly: start()/stop() block only for the
// process spawn/kill, and the URL arrives asynchronously through status().
namespace party_tunnel
{
enum class State
{
    Stopped,
    Starting,
    Live,
    Error,
};

struct Status
{
    State state = State::Stopped;
    std::string url;   // https://<random>.trycloudflare.com (no trailing slash)
    std::string host;  // <random>.trycloudflare.com
    std::string error; // user-facing reason when state == Error
};

// Resolve the cloudflared binary. `override_path` (Party settings) wins, then
// P100_CLOUDFLARED, then PATH and the usual install locations. Returns false
// and leaves `out_path` empty when it is not installed.
bool available(std::string &out_path, const std::string &override_path = "");

// Start a quick tunnel in front of http://<origin_ip>:<port>. Returns false
// only when the process cannot be spawned at all; later failures (no URL,
// process exit) arrive through status(). Any previous tunnel is stopped first.
bool start(const std::string &origin_ip, int port, const std::string &binary_path,
           std::string &error);

// Kill the tunnel process and reset status to Stopped. Idempotent.
void stop();

// Thread-safe snapshot. Poll it from the main loop; it is cheap.
Status status();

// Exposed for tests: extract the https://*.trycloudflare.com URL from one
// cloudflared log line (ANSI colour codes are tolerated), or "" when the line
// does not contain one.
std::string parse_url(const std::string &line);
} // namespace party_tunnel
