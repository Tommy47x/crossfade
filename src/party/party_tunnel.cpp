#include "party_tunnel.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef P100_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace party_tunnel
{
namespace
{

using Clock = std::chrono::steady_clock;

constexpr int kStartTimeoutSeconds = 30;

struct Impl
{
    std::mutex mu;
    Status status;
    std::string log_tail;
    std::atomic<bool> stopping{false};
#ifndef P100_WINDOWS
    pid_t pid = -1;
    int read_fd = -1;
#else
    HANDLE process = nullptr;
    HANDLE read_pipe = nullptr;
    HANDLE job = nullptr; // kills cloudflared if crossfade crashes
#endif
    std::thread reader;
};

Impl g_impl;

std::string strip_ansi(const std::string &line)
{
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i)
    {
        if ((unsigned char)line[i] == 0x1b)
        {
            // CSI: ESC [ ... final byte in @..~
            if (i + 1 < line.size() && line[i + 1] == '[')
            {
                i += 2;
                while (i < line.size())
                {
                    const unsigned char c = (unsigned char)line[i];
                    if (c >= 0x40 && c <= 0x7e)
                        break;
                    ++i;
                }
                continue;
            }
            if (i + 1 < line.size())
                ++i;
            continue;
        }
        out += line[i];
    }
    return out;
}

bool is_host_char(unsigned char c)
{
    return std::isalnum(c) != 0 || c == '-' || c == '.';
}

void remember_line(const std::string &line)
{
    if (line.empty())
        return;
    g_impl.log_tail += line;
    g_impl.log_tail += '\n';
    constexpr size_t kKeep = 3000;
    if (g_impl.log_tail.size() > kKeep)
        g_impl.log_tail.erase(0, g_impl.log_tail.size() - kKeep);
}

std::string last_log_line_locked()
{
    size_t end = g_impl.log_tail.size();
    while (end > 0 && (g_impl.log_tail[end - 1] == '\n' || g_impl.log_tail[end - 1] == '\r'))
        --end;
    size_t start = g_impl.log_tail.rfind('\n', end == 0 ? 0 : end - 1);
    start = start == std::string::npos ? 0 : start + 1;
    std::string line = g_impl.log_tail.substr(start, end - start);
    if (line.size() > 220)
        line = line.substr(line.size() - 220);
    return line;
}

void handle_log_line(const std::string &raw)
{
    const std::string line = strip_ansi(raw);
    std::lock_guard<std::mutex> lock(g_impl.mu);
    remember_line(line);
    if (g_impl.status.state != State::Starting)
        return;
    const std::string url = parse_url(line);
    if (url.empty())
        return;
    g_impl.status.state = State::Live;
    g_impl.status.url = url;
    g_impl.status.host = url.substr(std::strlen("https://"));
    g_impl.status.error.clear();
}

void terminate_child()
{
#ifndef P100_WINDOWS
    pid_t pid = -1;
    {
        std::lock_guard<std::mutex> lock(g_impl.mu);
        pid = g_impl.pid;
    }
    if (pid > 0)
    {
        // The child calls setsid(), so this also takes out any helper it
        // spawned. SIGKILL is fine: the tunnel holds no state worth saving.
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
#else
    HANDLE process = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_impl.mu);
        process = g_impl.process;
    }
    if (process)
        TerminateProcess(process, 0);
#endif
}

void finish_reader(bool user_stopped)
{
    std::lock_guard<std::mutex> lock(g_impl.mu);
    if (user_stopped)
    {
        g_impl.status = Status{};
        g_impl.log_tail.clear();
        return;
    }
    if (g_impl.status.state == State::Live)
    {
        g_impl.status.state = State::Error;
        g_impl.status.url.clear();
        g_impl.status.host.clear();
        g_impl.status.error = "The public link closed unexpectedly.";
    }
    else if (g_impl.status.state == State::Starting)
    {
        g_impl.status.state = State::Error;
        const std::string tail = last_log_line_locked();
        g_impl.status.error = tail.empty()
                                  ? "cloudflared exited before reporting a public link."
                                  : "cloudflared exited before reporting a public link (" + tail + ")";
    }
}

#ifndef P100_WINDOWS
void reader_main()
{
    const Clock::time_point deadline =
        Clock::now() + std::chrono::seconds(kStartTimeoutSeconds);
    std::string pending;
    char buf[1024];

    while (true)
    {
        if (g_impl.stopping.load(std::memory_order_relaxed))
            break;

        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(g_impl.mu);
            fd = g_impl.read_fd;
        }
        if (fd < 0)
            break;

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int rc = poll(&pfd, 1, 500);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (rc == 0)
        {
            bool timed_out = false;
            {
                std::lock_guard<std::mutex> lock(g_impl.mu);
                timed_out = g_impl.status.state == State::Starting && Clock::now() >= deadline;
                if (timed_out)
                {
                    g_impl.status.state = State::Error;
                    g_impl.status.error =
                        "cloudflared did not report a public link in time. Check your "
                        "internet connection.";
                }
            }
            if (timed_out)
            {
                terminate_child();
                break;
            }
            continue;
        }

        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        pending.append(buf, (size_t)n);
        size_t nl = std::string::npos;
        while ((nl = pending.find('\n')) != std::string::npos)
        {
            handle_log_line(pending.substr(0, nl));
            pending.erase(0, nl + 1);
        }
        // A pathological line without newlines must not grow forever.
        if (pending.size() > 8192)
            pending.clear();
    }
    if (!pending.empty())
        handle_log_line(pending);

    pid_t pid = -1;
    {
        std::lock_guard<std::mutex> lock(g_impl.mu);
        pid = g_impl.pid;
    }
    if (pid > 0)
    {
        int ignored = 0;
        while (waitpid(pid, &ignored, 0) < 0 && errno == EINTR)
        {
        }
    }

    const bool user_stopped = g_impl.stopping.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_impl.mu);
        g_impl.pid = -1;
        if (g_impl.read_fd >= 0)
        {
            close(g_impl.read_fd);
            g_impl.read_fd = -1;
        }
    }
    finish_reader(user_stopped);
}
#else
void reader_main()
{
    const Clock::time_point deadline =
        Clock::now() + std::chrono::seconds(kStartTimeoutSeconds);
    std::string pending;

    while (!g_impl.stopping.load(std::memory_order_relaxed))
    {
        HANDLE pipe = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_impl.mu);
            pipe = g_impl.read_pipe;
        }
        if (!pipe)
            break;

        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            break;
        if (available == 0)
        {
            bool timed_out = false;
            {
                std::lock_guard<std::mutex> lock(g_impl.mu);
                timed_out = g_impl.status.state == State::Starting && Clock::now() >= deadline;
                if (timed_out)
                {
                    g_impl.status.state = State::Error;
                    g_impl.status.error =
                        "cloudflared did not report a public link in time. Check your "
                        "internet connection.";
                }
            }
            if (timed_out)
            {
                terminate_child();
                break;
            }
            Sleep(200);
            continue;
        }

        char buf[1024];
        const DWORD want = std::min<DWORD>(available, sizeof(buf) - 1);
        DWORD got = 0;
        if (!ReadFile(pipe, buf, want, &got, nullptr) || got == 0)
            break;
        pending.append(buf, (size_t)got);
        size_t nl = std::string::npos;
        while ((nl = pending.find('\n')) != std::string::npos)
        {
            handle_log_line(pending.substr(0, nl));
            pending.erase(0, nl + 1);
        }
        if (pending.size() > 8192)
            pending.clear();
    }
    if (!pending.empty())
        handle_log_line(pending);

    HANDLE process = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_impl.mu);
        process = g_impl.process;
    }
    if (process)
        WaitForSingleObject(process, 5000);

    const bool user_stopped = g_impl.stopping.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_impl.mu);
        if (g_impl.read_pipe)
        {
            CloseHandle(g_impl.read_pipe);
            g_impl.read_pipe = nullptr;
        }
        if (g_impl.process)
        {
            CloseHandle(g_impl.process);
            g_impl.process = nullptr;
        }
        if (g_impl.job)
        {
            CloseHandle(g_impl.job);
            g_impl.job = nullptr;
        }
    }
    finish_reader(user_stopped);
}
#endif

bool file_runnable(const std::string &path)
{
    if (path.empty())
        return false;
#ifdef P100_WINDOWS
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           access(path.c_str(), X_OK) == 0;
#endif
}

std::string find_in_path(const char *name)
{
#ifdef P100_WINDOWS
    char buf[MAX_PATH * 4] = {};
    const DWORD n = SearchPathA(nullptr, name, ".exe", (DWORD)sizeof(buf), buf, nullptr);
    if (n > 0 && n < sizeof(buf))
        return std::string(buf, n);
    return "";
#else
    const char *env = std::getenv("PATH");
    if (!env || !*env)
        return "";
    const std::string path = env;
    size_t start = 0;
    while (start <= path.size())
    {
        const size_t end = path.find(':', start);
        const std::string dir =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty())
        {
            const std::string candidate = dir + "/" + name;
            if (file_runnable(candidate))
                return candidate;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return "";
#endif
}

std::string find_cloudflared(const std::string &override_path)
{
    if (file_runnable(override_path))
        return override_path;

    if (const char *env = std::getenv("P100_CLOUDFLARED"); env && *env)
        if (file_runnable(env))
            return env;

    if (const std::string found = find_in_path("cloudflared"); !found.empty())
        return found;

#ifdef P100_WINDOWS
    std::vector<std::string> candidates;
    if (const char *local = std::getenv("LOCALAPPDATA"); local && *local)
    {
        candidates.push_back(std::string(local) + "\\cloudflared\\cloudflared.exe");
        candidates.push_back(std::string(local) +
                            "\\Microsoft\\WinGet\\Links\\cloudflared.exe");
    }
    if (const char *programs = std::getenv("ProgramFiles"); programs && *programs)
        candidates.push_back(std::string(programs) + "\\cloudflared\\cloudflared.exe");
    for (const std::string &candidate : candidates)
        if (file_runnable(candidate))
            return candidate;
#else
    std::vector<std::string> candidates;
    if (const char *home = std::getenv("HOME"); home && *home)
        candidates.push_back(std::string(home) + "/.local/bin/cloudflared");
    candidates.push_back("/usr/local/bin/cloudflared");
    candidates.push_back("/usr/bin/cloudflared");
    candidates.push_back("/opt/homebrew/bin/cloudflared");
    candidates.push_back("/snap/bin/cloudflared");
    for (const std::string &candidate : candidates)
        if (file_runnable(candidate))
            return candidate;
#endif
    return "";
}

} // namespace

std::string parse_url(const std::string &line)
{
    const std::string clean = strip_ansi(line);
    const std::string scheme = "https://";
    size_t pos = clean.find(scheme);
    while (pos != std::string::npos)
    {
        const size_t start = pos + scheme.size();
        size_t end = start;
        while (end < clean.size() && is_host_char((unsigned char)clean[end]))
            ++end;
        std::string host = clean.substr(start, end - start);
        while (!host.empty() && host.back() == '.')
            host.pop_back();
        const std::string suffix = ".trycloudflare.com";
        if (host.size() > suffix.size() &&
            host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            for (char &c : host)
                c = (char)std::tolower((unsigned char)c);
            return scheme + host;
        }
        pos = clean.find(scheme, pos + scheme.size());
    }
    return "";
}

bool available(std::string &out_path, const std::string &override_path)
{
    // The Settings page and the Party hero ask every frame; a PATH scan per
    // frame is wasteful, so keep the answer for a couple of seconds. A newly
    // installed binary still shows up promptly.
    static std::mutex cache_mu;
    static Clock::time_point cache_at{};
    static std::string cache_override;
    static std::string cache_path;
    static bool cache_ok = false;
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        if (cache_at != Clock::time_point{} && cache_override == override_path &&
            Clock::now() - cache_at < std::chrono::seconds(2))
        {
            out_path = cache_path;
            return cache_ok;
        }
    }

    std::string path = find_cloudflared(override_path);
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        cache_at = Clock::now();
        cache_override = override_path;
        cache_path = path;
        cache_ok = !path.empty();
    }
    out_path = std::move(path);
    return !out_path.empty();
}

bool start(const std::string &origin_ip, int port, const std::string &binary_path,
           std::string &error)
{
    error.clear();
    stop();
    auto fail = [&](const std::string &message) {
        std::lock_guard<std::mutex> lock(g_impl.mu);
        g_impl.status = Status{};
        g_impl.status.state = State::Error;
        g_impl.status.error = message;
        error = message;
        return false;
    };
    if (binary_path.empty() || !file_runnable(binary_path))
        return fail("cloudflared was not found.");
    if (origin_ip.empty() || port <= 0)
        return fail("The party address is not ready.");

    g_impl.stopping.store(false, std::memory_order_relaxed);
    const std::string origin = "http://" + origin_ip + ":" + std::to_string(port);

#ifndef P100_WINDOWS
    int fds[2];
    if (pipe(fds) != 0)
        return fail(std::string("Could not create a pipe: ") + std::strerror(errno));
    // A GUI launcher may have closed stdin/stdout/stderr, in which case pipe()
    // hands out fds 0..2 and the child's dup2/close dance would close the
    // wrong ends. Move them above the standard descriptors first.
    for (int &fd : fds)
    {
        if (fd >= 0 && fd < 3)
        {
            const int moved = fcntl(fd, F_DUPFD, 3);
            if (moved >= 0)
            {
                close(fd);
                fd = moved;
            }
        }
    }
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 4096)
        max_fd = 4096;

    const pid_t pid = fork();
    if (pid == 0)
    {
        // Child: minimal, async-signal-safe work between fork and exec.
        setsid();
#ifdef __linux__
        // Never leave a public tunnel alive if crossfade crashes.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() == 1)
            _exit(1);
#endif
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        // Do not leak the app's sockets/pipes into the long-lived tunnel.
        for (int fd = 3; fd < (int)max_fd; ++fd)
            close(fd);
        execl(binary_path.c_str(), binary_path.c_str(), "tunnel", "--url", origin.c_str(),
              "--no-autoupdate", (char *)nullptr);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0)
    {
        close(fds[0]);
        return fail(std::string("Could not start cloudflared: ") + std::strerror(errno));
    }
    {
        std::lock_guard<std::mutex> lock(g_impl.mu);
        g_impl.pid = pid;
        g_impl.read_fd = fds[0];
        g_impl.status = Status{};
        g_impl.status.state = State::Starting;
        g_impl.log_tail.clear();
    }
#else
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        return fail("Could not create the cloudflared pipe.");
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;

    // A kill-on-close job object is the Windows equivalent of the POSIX
    // parent-death signal: a crash must not leave a public tunnel running.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                     sizeof(info)))
        {
            CloseHandle(job);
            job = nullptr;
        }
    }

    std::string cmd = "\"" + binary_path + "\" tunnel --url " + origin + " --no-autoupdate";
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP |
                                       CREATE_SUSPENDED,
                                   nullptr, nullptr, &si, &pi);
    CloseHandle(write_pipe);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (!ok)
    {
        if (job)
            CloseHandle(job);
        CloseHandle(read_pipe);
        return fail("Could not start cloudflared (error " +
                    std::to_string((unsigned long)GetLastError()) + ").");
    }
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lock(g_impl.mu);
        g_impl.process = pi.hProcess;
        g_impl.read_pipe = read_pipe;
        g_impl.job = job;
        g_impl.status = Status{};
        g_impl.status.state = State::Starting;
        g_impl.log_tail.clear();
    }
#endif

    if (g_impl.reader.joinable())
        g_impl.reader.join();
    g_impl.reader = std::thread(reader_main);
    return true;
}

void stop()
{
    g_impl.stopping.store(true, std::memory_order_relaxed);
    terminate_child();
    if (g_impl.reader.joinable())
        g_impl.reader.join();
    {
        std::lock_guard<std::mutex> lock(g_impl.mu);
        g_impl.status = Status{};
        g_impl.log_tail.clear();
#ifndef P100_WINDOWS
        g_impl.pid = -1;
        g_impl.read_fd = -1;
#else
        if (g_impl.job)
        {
            CloseHandle(g_impl.job);
            g_impl.job = nullptr;
        }
        g_impl.process = nullptr;
        g_impl.read_pipe = nullptr;
#endif
    }
}

Status status()
{
    std::lock_guard<std::mutex> lock(g_impl.mu);
    return g_impl.status;
}

} // namespace party_tunnel
