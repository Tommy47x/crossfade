#pragma once

#include <string>

struct PartyServerConfig
{
    std::string bind_ip;   // specific LAN address (or 0.0.0.0 fallback)
    std::string public_ip; // address shown in the QR / allowed in Host
    std::string public_host; // tunnel hostname (e.g. x.trycloudflare.com)
    int port = 0;          // 0 = try the configured range; updated on success
};

bool party_server_start(const PartyServerConfig &config, std::string &error);
void party_server_stop();
bool party_server_running();
int party_server_port();

// Update the tunnel hostname accepted in the Host header. Empty clears it.
// Safe to call while the server is running; used when the public URL arrives
// asynchronously (and when the tunnel goes away).
void party_server_set_public_host(const std::string &host);
