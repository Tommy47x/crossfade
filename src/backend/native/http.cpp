#include "http.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "httplib.h"

namespace native
{
namespace
{
struct Url
{
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path;
};

bool split_url(const std::string &url, Url &out)
{
    const std::size_t sep = url.find("://");
    if (sep == std::string::npos)
        return false;
    out.scheme = url.substr(0, sep);
    const std::size_t host_begin = sep + 3;
    const std::size_t path_begin = url.find('/', host_begin);
    std::string host_port =
        url.substr(host_begin, path_begin == std::string::npos ? std::string::npos
                                                               : path_begin - host_begin);
    out.path = path_begin == std::string::npos ? "/" : url.substr(path_begin);
    const std::size_t colon = host_port.find(':');
    if (colon != std::string::npos)
    {
        out.host = host_port.substr(0, colon);
        out.port = std::atoi(host_port.c_str() + colon + 1);
    }
    else
    {
        out.host = host_port;
        out.port = out.scheme == "https" ? 443 : 80;
    }
    return !out.host.empty();
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

// Merge "name=value; Path=..." into a single Cookie header.
void merge_cookie(std::string &jar, const std::string &set_cookie)
{
    const std::size_t semi = set_cookie.find(';');
    const std::string pair = set_cookie.substr(0, semi);
    const std::size_t eq = pair.find('=');
    if (eq == std::string::npos || eq == 0)
        return;
    const std::string name = pair.substr(0, eq) + "=";

    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < jar.size())
    {
        const std::size_t end = jar.find("; ", pos);
        parts.push_back(jar.substr(pos, end == std::string::npos ? std::string::npos
                                                                 : end - pos));
        if (end == std::string::npos)
            break;
        pos = end + 2;
    }
    bool replaced = false;
    for (std::string &p : parts)
        if (p.compare(0, name.size(), name) == 0)
        {
            p = pair;
            replaced = true;
        }
    if (!replaced)
        parts.push_back(pair);
    jar.clear();
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
            jar += "; ";
        jar += parts[i];
    }
}

httplib::Headers to_headers(const std::vector<HttpHeader> &headers,
                            const std::string &cookie_jar)
{
    httplib::Headers out;
    for (const HttpHeader &h : headers)
        out.emplace(h.name, h.value);
    if (!cookie_jar.empty())
        out.emplace("Cookie", cookie_jar);
    return out;
}

void collect_cookies(const httplib::Result &res, std::string &cookie_jar)
{
    if (!res)
        return;
    for (const auto &h : res->headers)
        if (lower(h.first) == "set-cookie")
            merge_cookie(cookie_jar, h.second);
}

// One Client per scheme+host per thread; httplib reuses the socket while the
// server keeps the connection alive.
httplib::Client *client_for(const Url &u)
{
    thread_local std::map<std::string, std::unique_ptr<httplib::Client>> cache;
    const std::string key = u.scheme + "://" + u.host + ":" + std::to_string(u.port);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second.get();
#if defined(P100_HAVE_OPENSSL)
    auto cli = std::make_unique<httplib::Client>(
        (u.scheme == "https" ? "https://" : "http://") + u.host + ":" +
        std::to_string(u.port));
#else
    if (u.scheme == "https")
        return nullptr;
    auto cli = std::make_unique<httplib::Client>("http://" + u.host + ":" +
                                                 std::to_string(u.port));
#endif
    cli->set_connection_timeout(10, 0);
    cli->set_read_timeout(25, 0);
    cli->set_write_timeout(25, 0);
    cli->set_follow_location(true);
    cli->set_keep_alive(true);
    httplib::Client *raw = cli.get();
    cache.emplace(key, std::move(cli));
    return raw;
}

HttpResult finish(const std::chrono::steady_clock::time_point &start,
                  const httplib::Result &res, const char *what)
{
    HttpResult out;
    out.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count();
    if (!res)
    {
        out.error = std::string(what) + " failed: " + httplib::to_string(res.error());
        return out;
    }
    out.ok = true;
    out.status = res->status;
    out.body = res->body;
    out.bytes = out.body.size();
    if (out.status >= 400)
    {
        out.ok = false;
        out.error = "HTTP " + std::to_string(out.status);
    }
    return out;
}
} // namespace

bool http_available()
{
#if defined(P100_HAVE_OPENSSL)
    return true;
#else
    return false;
#endif
}

HttpResult http_get(const std::string &url, const std::vector<HttpHeader> &headers,
                    std::string &cookie_jar)
{
    HttpResult out;
    Url u;
    if (!split_url(url, u))
    {
        out.error = "bad url";
        return out;
    }
    httplib::Client *cli = client_for(u);
    if (!cli)
    {
        out.error = "no TLS client in this build";
        return out;
    }
    const auto start = std::chrono::steady_clock::now();
    auto res = cli->Get(u.path.c_str(), to_headers(headers, cookie_jar));
    out = finish(start, res, "GET");
    collect_cookies(res, cookie_jar);
    return out;
}

HttpResult http_post_json(const std::string &url, const std::string &body,
                          const std::vector<HttpHeader> &headers,
                          std::string &cookie_jar)
{
    HttpResult out;
    Url u;
    if (!split_url(url, u))
    {
        out.error = "bad url";
        return out;
    }
    httplib::Client *cli = client_for(u);
    if (!cli)
    {
        out.error = "no TLS client in this build";
        return out;
    }
    httplib::Headers h = to_headers(headers, cookie_jar);
    h.emplace("Content-Type", "application/json");
    const auto start = std::chrono::steady_clock::now();
    auto res = cli->Post(u.path.c_str(), h, body, "application/json");
    out = finish(start, res, "POST");
    collect_cookies(res, cookie_jar);
    return out;
}
} // namespace native
