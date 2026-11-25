#pragma once

#include <string>
#include <vector>

// Small HTTP client for the native backend. One keep-alive connection per
// host per thread, cookies collected from Set-Cookie, timings returned for the
// debug page. TLS comes from the same OpenSSL build httplib already uses for
// the room-correction server; without it the native backend reports
// "unavailable" and the Python backend stays in use.
namespace native
{
struct HttpResult
{
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
    long long ms = 0;          // total request time
    long long connect_ms = 0;  // reserved for a future curl-multi backend
    std::size_t bytes = 0;
};

struct HttpHeader
{
    std::string name;
    std::string value;
};

// GET `url`. Cookies set by the response are merged into `cookie_jar`.
HttpResult http_get(const std::string &url, const std::vector<HttpHeader> &headers,
                    std::string &cookie_jar);

// POST a JSON body.
HttpResult http_post_json(const std::string &url, const std::string &body,
                          const std::vector<HttpHeader> &headers,
                          std::string &cookie_jar);

// True when the build has a TLS client (OpenSSL). The native resolver cannot
// talk to YouTube without it.
bool http_available();
} // namespace native
