#include "eq_server.h"

#include "eq_page.h"
#include "paths.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <sys/stat.h>

#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include "httplib.h"

#if defined(P100_HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

namespace eqserver
{
namespace
{
std::unique_ptr<httplib::Server> g_server;
std::thread g_listen;
std::mutex g_server_mutex;
Config g_cfg;
std::string g_url;
bool g_running = false;
bool g_secure = false;

std::mutex g_mail_mutex;
bool g_ready = false;
std::string g_page_state = "measuring";
std::string g_page_message = "Waiting for the sweep…";
std::vector<std::int16_t> g_pcm;
int g_pcm_rate = 0;
std::string g_user_agent;
void (*g_wake)() = nullptr;

std::string replace_all(std::string s, const std::string &from, const std::string &to)
{
    if (from.empty())
        return s;
    std::size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos)
    {
        s.replace(at, from.size(), to);
        at += to.size();
    }
    return s;
}

void json_escape(std::string &out, const std::string &in)
{
    out += '"';
    for (char c : in)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default:
            if ((unsigned char)c >= 0x20)
                out += c;
            break;
        }
    }
    out += '"';
}

std::string status_json()
{
    std::lock_guard<std::mutex> lock(g_mail_mutex);
    std::string out = "{\"state\":";
    json_escape(out, g_page_state);
    out += ",\"message\":";
    json_escape(out, g_page_message);
    out += "}";
    return out;
}

void bad_token(httplib::Response &res)
{
    res.status = 404;
    res.set_content("not found", "text/plain; charset=utf-8");
}

std::string page_html(bool secure)
{
    std::string html = kRoomEqPageHtml;
    html = replace_all(html, "__TITLE__", g_cfg.title);
    html = replace_all(html, "__TOKEN__", g_cfg.token);
    html = replace_all(html, "__TOTAL__", std::to_string(g_cfg.total_seconds));
    html = replace_all(html, "__RATE__", std::to_string(g_cfg.sample_rate));
    html = replace_all(html, "__SECURE__", secure ? "true" : "false");
    return html;
}

#if defined(P100_HAVE_OPENSSL)
// A self-signed certificate is enough: the page is a one-shot local tool, and
// browsers only need a secure context (they show a warning the user accepts).
// The SAN carries the LAN IP because Chrome ignores CN and refuses a
// certificate without a matching SAN.
bool ensure_cert(const std::string &ip, std::string &cert_path, std::string &key_path,
                 std::string &error)
{
    const std::string dir = p100_config_path("room_eq");
    p100_ensure_config_dir();
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
    const std::string leaf = "cert-" + std::to_string(std::hash<std::string>{}(ip));
    cert_path = dir + "/" + leaf + ".pem";
    key_path = dir + "/" + leaf + ".key";

    FILE *probe = std::fopen(cert_path.c_str(), "rb");
    if (probe)
    {
        std::fclose(probe);
        FILE *kprobe = std::fopen(key_path.c_str(), "rb");
        if (kprobe)
        {
            std::fclose(kprobe);
            return true;
        }
    }

    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!kctx)
    {
        error = "openssl: keygen context";
        return false;
    }
    EVP_PKEY *pkey = nullptr;
    if (EVP_PKEY_keygen_init(kctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) <= 0 ||
        EVP_PKEY_keygen(kctx, &pkey) <= 0)
    {
        EVP_PKEY_CTX_free(kctx);
        error = "openssl: keygen failed";
        return false;
    }
    EVP_PKEY_CTX_free(kctx);

    X509 *x = X509_new();
    if (!x)
    {
        EVP_PKEY_free(pkey);
        error = "openssl: X509_new failed";
        return false;
    }
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), (long)time(nullptr));
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 60L * 60 * 24 * 3650);
    X509_set_pubkey(x, pkey);

    // Build the name from scratch: X509_get_subject_name() returns a const
    // pointer on newer OpenSSL (MSYS2 ships one), which does not compile here.
    X509_NAME *name = X509_NAME_new();
    if (!name ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   (const unsigned char *)"crossfade.local", -1, -1,
                                   0) != 1)
    {
        if (name)
            X509_NAME_free(name);
        X509_free(x);
        EVP_PKEY_free(pkey);
        error = "openssl: could not set the certificate name";
        return false;
    }
    X509_set_subject_name(x, name);
    X509_set_issuer_name(x, name);
    X509_NAME_free(name);

    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x, x, nullptr, nullptr, 0);
    const std::string san = "DNS:crossfade.local,IP:" + ip;
    X509_EXTENSION *ext =
        X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, san.c_str());
    if (ext)
    {
        X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
    }
    X509_sign(x, pkey, EVP_sha256());

    bool ok = true;
    FILE *cf = std::fopen(cert_path.c_str(), "wb");
    if (!cf || !PEM_write_X509(cf, x))
        ok = false;
    if (cf)
        std::fclose(cf);
    FILE *kf = std::fopen(key_path.c_str(), "wb");
    if (!kf || !PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr))
        ok = false;
    if (kf)
        std::fclose(kf);

    X509_free(x);
    EVP_PKEY_free(pkey);
    if (!ok)
        error = "openssl: could not write the certificate";
    return ok;
}
#endif
} // namespace

bool start(const Config &cfg, std::string &error)
{
    std::lock_guard<std::mutex> lock(g_server_mutex);
    if (g_running)
        return true;
    error.clear();
    g_cfg = cfg;
    if (g_cfg.token.empty())
        g_cfg.token = "e";

    std::unique_ptr<httplib::Server> svr;
    bool secure = false;
#if defined(P100_HAVE_OPENSSL)
    if (cfg.prefer_tls)
    {
        std::string cert, key, cerr;
        if (ensure_cert(cfg.bind_ip.empty() ? "127.0.0.1" : cfg.bind_ip, cert, key, cerr))
        {
            svr = std::unique_ptr<httplib::Server>(new httplib::SSLServer(cert.c_str(), key.c_str()));
            secure = svr != nullptr;
        }
    }
#endif
    if (!svr)
        svr = std::unique_ptr<httplib::Server>(new httplib::Server());

    svr->set_payload_max_length(32 * 1024 * 1024);
    svr->set_read_timeout(30, 0);
    svr->set_write_timeout(30, 0);
    svr->set_keep_alive_max_count(16);

    // Everything the workers need is captured by value: g_cfg must never be
    // read unsynchronised from an HTTP thread.
    const std::string token = g_cfg.token;
    const std::string page = page_html(secure);
    char cfg_json[256];
    std::snprintf(cfg_json, sizeof(cfg_json), "{\"total\":%.3f,\"rate\":%d,\"secure\":%s}",
                  g_cfg.total_seconds, g_cfg.sample_rate, secure ? "true" : "false");
    const std::string config_json = cfg_json;
    const int upload_rate = g_cfg.sample_rate;
    const std::size_t max_samples = (std::size_t)g_cfg.sample_rate * 120;

    svr->Get(R"(/e/([A-Za-z0-9_-]+))", [token, page](const httplib::Request &req,
                                                     httplib::Response &res) {
        if (req.matches.size() < 2 || req.matches[1].str() != token)
        {
            bad_token(res);
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(page, "text/html; charset=utf-8");
    });

    svr->Get(R"(/e/([A-Za-z0-9_-]+)/config)",
             [token, config_json](const httplib::Request &req, httplib::Response &res) {
                 if (req.matches.size() < 2 || req.matches[1].str() != token)
                 {
                     bad_token(res);
                     return;
                 }
                 res.set_content(config_json, "application/json");
             });

    svr->Post(R"(/e/([A-Za-z0-9_-]+)/hello)",
              [token](const httplib::Request &req, httplib::Response &res) {
                  if (req.matches.size() < 2 || req.matches[1].str() != token)
                  {
                      bad_token(res);
                      return;
                  }
                  {
                      std::lock_guard<std::mutex> lock(g_mail_mutex);
                      g_user_agent = req.body.substr(0, 512);
                  }
                  res.set_content("{\"ok\":true}", "application/json");
              });

    svr->Post(R"(/e/([A-Za-z0-9_-]+)/ready)",              [token](const httplib::Request &req, httplib::Response &res) {
                  if (req.matches.size() < 2 || req.matches[1].str() != token)
                  {
                      bad_token(res);
                      return;
                  }
                  note_ready();
                  res.set_content("{\"ok\":true}", "application/json");
              });

    svr->Post(R"(/e/([A-Za-z0-9_-]+)/upload)",
              [token, upload_rate, max_samples](const httplib::Request &req,
                                                httplib::Response &res) {
                  if (req.matches.size() < 2 || req.matches[1].str() != token)
                  {
                      bad_token(res);
                      return;
                  }
                  const std::string &body = req.body;
                  if (body.size() < 4096 || (body.size() & 1))
                  {
                      res.status = 400;
                      res.set_content("bad recording", "text/plain");
                      return;
                  }
                  std::vector<std::int16_t> pcm(std::min(body.size() / 2, max_samples));
                  for (std::size_t i = 0; i < pcm.size(); ++i)
                  {
                      const unsigned char lo = (unsigned char)body[i * 2];
                      const unsigned char hi = (unsigned char)body[i * 2 + 1];
                      pcm[i] = (std::int16_t)((unsigned)lo | ((unsigned)hi << 8));
                  }
                  {
                      std::lock_guard<std::mutex> mail(g_mail_mutex);
                      g_pcm = std::move(pcm);
                      g_pcm_rate = upload_rate;
                      g_page_state = "analyzing";
                      g_page_message = "Analysing the room…";
                  }
                  if (g_wake)
                      g_wake();
                  res.set_content("{\"ok\":true}", "application/json");
              });

    svr->Get(R"(/e/([A-Za-z0-9_-]+)/status)",
             [token](const httplib::Request &req, httplib::Response &res) {
                 if (req.matches.size() < 2 || req.matches[1].str() != token)
                 {
                     bad_token(res);
                     return;
                 }
                 res.set_header("Cache-Control", "no-store");
                 res.set_content(status_json(), "application/json");
             });

    int bound_port = 0;
    const std::string bind_ip = cfg.bind_ip.empty() ? "0.0.0.0" : cfg.bind_ip;
    for (int i = 0; i < 12 && bound_port == 0; ++i)
        if (svr->bind_to_port(bind_ip, cfg.port + i))
            bound_port = cfg.port + i;
    if (bound_port == 0 && bind_ip != "0.0.0.0")
        for (int i = 0; i < 12 && bound_port == 0; ++i)
            if (svr->bind_to_port("0.0.0.0", cfg.port + i))
                bound_port = cfg.port + i;
    if (bound_port == 0)
    {
        error = "Could not open a port for the phone page.";
        return false;
    }

    g_cfg.port = bound_port;
    g_secure = secure;
    g_server = std::move(svr);
    httplib::Server *raw = g_server.get();
    g_listen = std::thread([raw] { raw->listen_after_bind(); });

    const std::string scheme = secure ? "https://" : "http://";
    g_url = scheme + cfg.bind_ip + ":" + std::to_string(bound_port) + "/e/" + g_cfg.token;
    g_running = true;
    return true;
}

void stop()
{
    std::unique_ptr<httplib::Server> old;
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        if (!g_running)
            return;
        g_running = false;
        g_url.clear();
        old = std::move(g_server);
        thread = std::move(g_listen);
    }
    if (old)
        old->stop();
    if (thread.joinable())
        thread.join();
    std::lock_guard<std::mutex> mail(g_mail_mutex);
    g_ready = false;
    g_pcm.clear();
    g_pcm_rate = 0;
    g_user_agent.clear();
}

bool running()
{
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return g_running;
}

bool secure()
{
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return g_secure;
}

int port()
{
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return g_running ? g_cfg.port : 0;
}

std::string url()
{
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return g_url;
}

void set_status(const std::string &state, const std::string &message)
{
    std::lock_guard<std::mutex> lock(g_mail_mutex);
    g_page_state = state;
    g_page_message = message;
}

void note_ready()
{
    {
        std::lock_guard<std::mutex> lock(g_mail_mutex);
        g_ready = true;
    }
    if (g_wake)
        g_wake();
}

void set_wake(void (*fn)())
{
    g_wake = fn;
}

std::string phone_user_agent()
{
    std::lock_guard<std::mutex> lock(g_mail_mutex);
    return g_user_agent;
}

bool consume_ready()
{
    std::lock_guard<std::mutex> lock(g_mail_mutex);
    const bool r = g_ready;
    g_ready = false;
    return r;
}

bool take_upload(std::vector<std::int16_t> &pcm, int &rate)
{
    std::lock_guard<std::mutex> lock(g_mail_mutex);
    if (g_pcm.empty())
        return false;
    pcm = std::move(g_pcm);
    g_pcm.clear();
    rate = g_pcm_rate;
    g_pcm_rate = 0;
    return true;
}
} // namespace eqserver
