#include "party_tunnel.h"
#include "test_util.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using testutil::check;

int main()
{
    check(party_tunnel::parse_url(
              "2026-01-01T00:00:00Z INF |  https://random-words-here.trycloudflare.com  |") ==
              "https://random-words-here.trycloudflare.com",
          "plain quick-tunnel line");
    check(party_tunnel::parse_url(
              "\x1b[32mhttps://abc-123.trycloudflare.com\x1b[0m") ==
              "https://abc-123.trycloudflare.com",
          "ANSI colours stripped");
    check(party_tunnel::parse_url("https://Mixed-Case.trycloudflare.com") ==
              "https://mixed-case.trycloudflare.com",
          "host lowercased");
    check(party_tunnel::parse_url("https://abc.trycloudflare.com/join?x=1") ==
              "https://abc.trycloudflare.com",
          "path ignored");
    check(party_tunnel::parse_url("https://abc.trycloudflare.com.") ==
              "https://abc.trycloudflare.com",
          "trailing dot trimmed");
    check(party_tunnel::parse_url("https://example.com") == "",
          "non-cloudflare host rejected");
    check(party_tunnel::parse_url("https://abc.trycloudflare.com.evil.test") == "",
          "suffix spoof rejected");
    check(party_tunnel::parse_url("INF no link on this line") == "",
          "line without a link");
    check(party_tunnel::parse_url(
              "INF https://abc.trycloudflare.com INF https://def.trycloudflare.com") ==
              "https://abc.trycloudflare.com",
          "first link wins");

    const party_tunnel::Status idle = party_tunnel::status();
    check(idle.state == party_tunnel::State::Stopped, "starts stopped");
    party_tunnel::stop(); // idempotent
    check(party_tunnel::status().state == party_tunnel::State::Stopped, "stop idempotent");

    // A configured path wins when it exists and is executable.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "p100-party-tunnel-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const std::filesystem::path fake = root / "cloudflared";
    testutil::write_file(fake, "#!/bin/sh\nexit 0\n");
#ifndef _WIN32
    ::chmod(fake.c_str(), 0755);
#endif
    std::string found;
    check(party_tunnel::available(found, fake.string()), "override path found");
    check(found == fake.string(), "override path returned");

#ifndef _WIN32
    // End-to-end plumbing: a fake cloudflared prints a quick-tunnel URL on
    // stderr and then stays alive like the real one. The reader must surface
    // the URL, and stop() must kill/reap the process and reset the state.
    const std::filesystem::path fake_live = root / "cloudflared-live";
    testutil::write_file(
        fake_live,
        "#!/bin/sh\n"
        "echo 'INF |  https://unit-test-abc.trycloudflare.com  |' >&2\n"
        "exec sleep 30\n");
    ::chmod(fake_live.c_str(), 0755);
    std::string start_error;
    check(party_tunnel::start("127.0.0.1", 65535, fake_live.string(), start_error),
          "fake tunnel starts");
    bool live = false;
    for (int i = 0; i < 60; ++i)
    {
        const party_tunnel::Status st = party_tunnel::status();
        if (st.state == party_tunnel::State::Live)
        {
            live = true;
            check(st.url == "https://unit-test-abc.trycloudflare.com",
                  "fake tunnel URL");
            check(st.host == "unit-test-abc.trycloudflare.com", "fake tunnel host");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(live, "fake tunnel becomes live");
    party_tunnel::stop();
    check(party_tunnel::status().state == party_tunnel::State::Stopped,
          "stop resets the tunnel after a live session");
#endif

    std::filesystem::remove_all(root, ec);
    if (testutil::failures == 0)
        std::printf("party_tunnel: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
