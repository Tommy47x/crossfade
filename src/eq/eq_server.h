#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Tiny LAN HTTP(S) server used only while calibrating. It serves the phone
// page, hands the host a "start the sweep" signal, and accepts raw 16-bit
// mono PCM uploads. All calls are safe from any thread.
namespace eqserver
{
struct Config
{
    std::string bind_ip;  // specific LAN address, or "0.0.0.0"
    int port = 8585;      // first port to try
    std::string token;    // page/method guard, generated per session
    std::string title = "crossfade";
    double total_seconds = 8.0; // lead-in + sweep + tail, sent to the page
    int sample_rate = 48000;
    bool prefer_tls = true;
};

bool start(const Config &cfg, std::string &error);
void stop();
bool running();
bool secure();
int port();
std::string url(); // full page URL (empty when stopped)

// Host -> phone status line shown while the phone waits for analysis.
void set_status(const std::string &state, const std::string &message);

// Wakes the main loop when the phone signals readiness (called from an HTTP
// worker thread; must be cheap and thread-safe).
void set_wake(void (*fn)());

// Phone -> host mailbox.
void note_ready();
bool consume_ready(); // true once per note_ready()
bool take_upload(std::vector<std::int16_t> &pcm, int &rate);
} // namespace eqserver
