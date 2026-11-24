#include "update_apply.h"
#include "sha256.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef P100_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace update_apply
{
namespace
{

constexpr const char *kReleasesBase = "https://github.com/XBoom31/crossfade/releases";
constexpr const char *kLatestDownload = "https://github.com/XBoom31/crossfade/releases/latest/download/";

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

#ifndef P100_WINDOWS

std::string exe_path()
{
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return {};
    buf[n] = '\0';
    return std::string(buf);
}

bool dir_writable(const std::string &dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return false;
    return access(dir.c_str(), W_OK) == 0;
}

bool run_wait(const std::vector<std::string> &args)
{
    const pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0)
    {
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool run_capture(const std::vector<std::string> &args, std::string &out)
{
    out.clear();
    int fds[2];
    if (pipe(fds) != 0)
        return false;
    const pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0)
    {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        out.append(buf, (size_t)n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool run_with_timeout(const std::vector<std::string> &args, int seconds,
                      const std::string &log_path)
{
    const pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0)
    {
        const int fd = open(log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd >= 0)
        {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    const time_t deadline = std::time(nullptr) + seconds;
    for (;;)
    {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (r < 0)
            return false;
        if (std::time(nullptr) >= deadline)
        {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            return false;
        }
        usleep(100 * 1000);
    }
}

bool copy_path(const std::string &src, const std::string &dst, std::string &err)
{
    std::error_code ec;
    const fs::path s(src);
    const fs::path d(dst);
    if (fs::is_directory(s, ec))
    {
        fs::copy(s, d, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
    }
    else
    {
        fs::copy_file(s, d, fs::copy_options::overwrite_existing, ec);
    }
    if (ec)
    {
        err = "Could not copy " + src + ": " + ec.message();
        return false;
    }
    return true;
}

bool backup_path(const std::string &path, const std::string &backup_root,
                 const std::string &rel, std::string &err)
{
    std::error_code ec;
    if (!fs::exists(path, ec))
        return true;
    const fs::path backup = fs::path(backup_root) / rel;
    fs::create_directories(backup.parent_path(), ec);
    if (ec)
    {
        err = "Could not create the update backup directory: " + ec.message();
        return false;
    }
    if (fs::exists(backup, ec))
        fs::remove_all(backup, ec);
    return copy_path(path, backup.string(), err);
}

void restore_path(const std::string &backup_root, const std::string &rel,
                  const std::string &target)
{
    std::error_code ec;
    const fs::path backup = fs::path(backup_root) / rel;
    if (!fs::exists(backup, ec))
        return;
    fs::remove_all(target, ec);
    std::string copy_err;
    copy_path(backup.string(), target, copy_err);
}

// Replace a file through a same-directory temp so the visible path is never
// half-written. 0755 for executables, 0644 otherwise.
bool replace_file(const std::string &src, const std::string &dst,
                  const std::string &backup_root, const std::string &rel,
                  bool executable, std::string &err)
{
    if (!backup_path(dst, backup_root, rel, err))
        return false;
    const std::string tmp = dst + ".update-tmp";
    std::error_code ec;
    fs::create_directories(fs::path(dst).parent_path(), ec);
    if (ec)
    {
        err = "Could not create the install directory: " + ec.message();
        return false;
    }
    fs::remove(tmp, ec);
    if (!copy_path(src, tmp, err))
        return false;
    fs::permissions(tmp,
                    executable ? (fs::perms::owner_exec | fs::perms::group_exec |
                                  fs::perms::others_exec | fs::perms::owner_read |
                                  fs::perms::owner_write | fs::perms::group_read |
                                  fs::perms::others_read)
                               : (fs::perms::owner_read | fs::perms::owner_write |
                                  fs::perms::group_read | fs::perms::others_read),
                    fs::perm_options::replace, ec);
    fs::rename(tmp, dst, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        err = "Could not replace " + dst + ": " + ec.message();
        return false;
    }
    return true;
}

// Same idea for directories: build the new tree next to the destination,
// then swap it in. Avoids merging stale files from an older release.
bool replace_dir(const std::string &src, const std::string &dst,
                 const std::string &backup_root, const std::string &rel,
                 std::string &err)
{
    if (!backup_path(dst, backup_root, rel, err))
        return false;
    const std::string tmp = dst + ".update-tmp";
    const std::string old = dst + ".update-old";
    std::error_code ec;
    fs::create_directories(fs::path(dst).parent_path(), ec);
    if (ec)
    {
        err = "Could not create the install directory: " + ec.message();
        return false;
    }
    fs::remove_all(tmp, ec);
    fs::remove_all(old, ec);
    fs::create_directories(tmp, ec);
    if (!copy_path(src, tmp, err))
    {
        fs::remove_all(tmp, ec);
        return false;
    }
    if (fs::exists(dst, ec))
        fs::rename(dst, old, ec);
    if (!ec)
        fs::rename(tmp, dst, ec);
    if (ec)
    {
        std::error_code restore_ec;
        if (fs::exists(old, restore_ec))
            fs::rename(old, dst, restore_ec);
        fs::remove_all(tmp, restore_ec);
        err = "Could not replace " + dst + ": " + ec.message();
        return false;
    }
    fs::remove_all(old, ec);
    return true;
}

void rewrite_desktop_exec(const std::string &path, const std::string &exec_path)
{
    std::ifstream in(path);
    if (!in)
        return;
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    std::string text = ss.str();
    const std::string exec_line = "Exec=" + exec_path;
    size_t pos = text.find("\nExec=");
    if (pos == std::string::npos)
    {
        if (text.rfind("Exec=", 0) == 0)
            pos = 0;
        else
            return;
    }
    const bool at_start = pos == 0;
    const size_t start = at_start ? 0 : pos + 1;
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
        end = text.size();
    text.replace(start, end - start, exec_line);
    std::ofstream out(path, std::ios::trunc);
    if (out)
        out << text;
}

bool valid_archive_entry(const std::string &name, const std::string &top,
                         std::string &err)
{
    if (name.empty())
        return true;
    if (name[0] == '/')
    {
        err = "The update archive contains an absolute path.";
        return false;
    }
    std::string normalized = name;
    while (!normalized.empty() && normalized.back() == '/')
        normalized.pop_back();
    if (normalized.empty())
        return true;
    if (normalized != top && normalized.rfind(top + "/", 0) != 0)
    {
        err = "The update archive has an unexpected top-level directory.";
        return false;
    }
    size_t start = 0;
    while (start <= normalized.size())
    {
        const size_t slash = normalized.find('/', start);
        const std::string part = normalized.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..")
        {
            err = "The update archive contains a parent-directory path.";
            return false;
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

#endif // !P100_WINDOWS

} // namespace

Target detect_install()
{
    Target t;
#ifdef P100_WINDOWS
    t.kind = Kind::WindowsInstaller;
    t.writable = true;
    return t;
#else
    t.exe = exe_path();
    if (t.exe.empty())
        return t;

    // The release workflow publishes x86_64 Linux assets only. Refuse to
    // download a binary built for a different architecture.
#if !defined(__x86_64__) && !defined(_M_X64)
    t.kind = Kind::None;
    t.root = fs::path(t.exe).parent_path().string();
    return t;
#else
    const char *appimage = std::getenv("APPIMAGE");
    if (appimage && *appimage)
    {
        std::error_code ec;
        const fs::path resolved = fs::weakly_canonical(fs::path(appimage), ec);
        t.kind = Kind::AppImage;
        t.root = (ec ? fs::path(appimage) : resolved).string();
        t.writable = fs::is_regular_file(t.root, ec) && dir_writable(fs::path(t.root).parent_path().string());
        return t;
    }

    const fs::path exe(t.exe);
    const fs::path dir = exe.parent_path();
    std::error_code ec;
    if (fs::exists(dir / "p100-backend.py", ec) || fs::exists(dir / "assets-fonts", ec))
    {
        t.kind = Kind::FlatDir;
        t.root = dir.string();
        t.writable = dir_writable(t.root);
        return t;
    }

    if (exe.filename() == "crossfade")
    {
        const fs::path prefix = dir.parent_path();
        if (fs::exists(prefix / "share" / "crossfade" / "p100-backend.py", ec))
        {
            // Never rewrite a distro-owned tree, even when the app happens to
            // run as root. Package managers must stay in charge of /usr.
            if (prefix == "/usr" || prefix == "/usr/local")
            {
                t.kind = Kind::None;
                t.root = prefix.string();
                return t;
            }
            t.kind = Kind::Prefix;
            t.root = prefix.string();
            t.writable = dir_writable((prefix / "bin").string()) &&
                         dir_writable((prefix / "share" / "crossfade").string());
            return t;
        }
    }

    t.kind = Kind::None;
    t.root = dir.string();
    t.writable = false;
    return t;
#endif // x86_64
#endif
}

bool self_update_supported(const Target &t)
{
    if (t.kind == Kind::WindowsInstaller)
        return true;
    return t.writable && (t.kind == Kind::AppImage || t.kind == Kind::FlatDir ||
                          t.kind == Kind::Prefix);
}

std::string asset_name(Kind kind, const std::string &version)
{
    switch (kind)
    {
    case Kind::AppImage:
        return "crossfade-" + version + "-x86_64.AppImage";
    case Kind::FlatDir:
    case Kind::Prefix:
        return "crossfade-" + version + "-linux-x64.tar.gz";
    default:
        return {};
    }
}

std::string release_page_url() { return std::string(kReleasesBase) + "/latest"; }

std::string asset_url(const std::string &asset) { return std::string(kLatestDownload) + asset; }

bool download(const std::string &url, const std::string &dest, std::string &err)
{
#ifdef P100_WINDOWS
    (void)url;
    (void)dest;
    err = "Not supported on Windows.";
    return false;
#else
    const std::string part = dest + ".part";
    std::error_code ec;
    fs::remove(part, ec);
    if (!run_wait({"curl", "-fsSL", "--connect-timeout", "8", "--max-time", "600",
                   "-o", part, url}))
    {
        fs::remove(part, ec);
        err = "Could not download " + url + ".";
        return false;
    }
    fs::rename(part, dest, ec);
    if (ec)
    {
        fs::remove(part, ec);
        err = "Could not save the downloaded update: " + ec.message();
        return false;
    }
    return true;
#endif
}

bool verify(const std::string &file, const std::string &sums_path,
            const std::string &asset, std::string &err)
{
#ifdef P100_WINDOWS
    (void)file;
    (void)sums_path;
    (void)asset;
    err = "Not supported on Windows.";
    return false;
#else
    std::ifstream in(sums_path);
    if (!in)
    {
        err = "Could not read the release checksums.";
        return false;
    }
    std::string expected;
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream iss(line);
        std::string hash, name;
        if (!(iss >> hash >> name))
            continue;
        if (!name.empty() && name[0] == '*')
            name.erase(0, 1);
        if (name == asset)
        {
            expected = lower(hash);
            break;
        }
    }
    if (expected.size() != 64)
    {
        err = "The release checksums do not list this download.";
        return false;
    }
    std::string actual;
    if (!sha256::file_hex_digest(file, actual))
    {
        err = "Could not read the downloaded update.";
        return false;
    }
    if (actual != expected)
    {
        err = "The downloaded update failed its SHA-256 check.";
        return false;
    }
    return true;
#endif
}

bool extract(const std::string &archive, const std::string &dest_dir,
             const std::string &version, std::string &err)
{
#ifdef P100_WINDOWS
    (void)archive;
    (void)dest_dir;
    (void)version;
    err = "Not supported on Windows.";
    return false;
#else
    const std::string top = "crossfade-" + version + "-linux-x64";
    std::string listing;
    if (!run_capture({"tar", "-tzf", archive}, listing))
    {
        err = "The downloaded archive could not be read.";
        return false;
    }
    std::istringstream lines(listing);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!valid_archive_entry(line, top, err))
            return false;
    }

    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (ec)
    {
        err = "Could not create the update staging directory: " + ec.message();
        return false;
    }
    if (!run_wait({"tar", "-xzf", archive, "-C", dest_dir}))
    {
        err = "The downloaded archive could not be extracted.";
        return false;
    }
    const fs::path root = fs::path(dest_dir) / top;
    if (!fs::is_regular_file(root / "crossfade", ec) ||
        !fs::is_regular_file(root / "p100-backend.py", ec))
    {
        err = "The downloaded archive does not look like a crossfade release.";
        return false;
    }
    return true;
#endif
}

bool apply(const Target &target, const std::string &stage_dir,
           const std::string &version, const std::string &asset,
           std::string &err)
{
    err.clear();
    if (!self_update_supported(target))
    {
        err = "This copy of crossfade cannot update itself.";
        return false;
    }

#ifdef P100_WINDOWS
    (void)stage_dir;
    (void)version;
    (void)asset;
    err = "Windows updates use the one-click installer.";
    return false;
#else
    const std::string backup_root = stage_dir + "/backup";
    std::error_code ec;
    fs::create_directories(backup_root, ec);

    struct Step
    {
        std::string src;
        std::string dst;
        std::string rel;
        bool dir;
        bool exec;
    };
    std::vector<Step> steps;

    if (target.kind == Kind::AppImage)
    {
        const std::string src = stage_dir + "/" + asset;
        if (!fs::is_regular_file(src, ec))
        {
            err = "The downloaded AppImage is missing.";
            return false;
        }
        steps.push_back({src, target.root, "appimage", false, true});
    }
    else
    {
        const std::string root = stage_dir + "/extracted/crossfade-" + version + "-linux-x64";
        if (!fs::is_directory(root, ec))
        {
            err = "The extracted release is missing.";
            return false;
        }
        auto add_file = [&](const std::string &rel, const std::string &dst, bool exec) {
            steps.push_back({root + "/" + rel, dst, rel, false, exec});
        };
        auto add_dir = [&](const std::string &rel, const std::string &dst) {
            steps.push_back({root + "/" + rel, dst, rel, true, false});
        };

        if (target.kind == Kind::FlatDir)
        {
            add_file("crossfade", target.root + "/crossfade", true);
            add_file("crossfade-login", target.root + "/crossfade-login", true);
            add_file("p100-backend.py", target.root + "/p100-backend.py", false);
            add_file("p100-backend-oauth.py", target.root + "/p100-backend-oauth.py", false);
            add_file("requirements.txt", target.root + "/requirements.txt", false);
            add_file("crossfade.desktop", target.root + "/crossfade.desktop", false);
            add_file("README.md", target.root + "/README.md", false);
            add_file("LICENSE", target.root + "/LICENSE", false);
            add_dir("py/cf_backend", target.root + "/py/cf_backend");
            add_dir("scripts", target.root + "/scripts");
            add_dir("assets-fonts", target.root + "/assets/fonts");
        }
        else // Prefix
        {
            const std::string share = target.root + "/share/crossfade";
            add_file("crossfade", target.root + "/bin/crossfade", true);
            add_file("crossfade-login", target.root + "/bin/crossfade-login", true);
            add_file("p100-backend.py", share + "/p100-backend.py", false);
            add_file("p100-backend-oauth.py", share + "/p100-backend-oauth.py", false);
            add_file("requirements.txt", share + "/requirements.txt", false);
            add_dir("py/cf_backend", share + "/py/cf_backend");
            add_file("scripts/crossfade-login-helper.py",
                     share + "/scripts/crossfade-login-helper.py", false);
            add_file("scripts/bootstrap-venv.sh", share + "/scripts/bootstrap-venv.sh", true);
            add_file("scripts/fetch-js-runtime.sh",
                     share + "/scripts/fetch-js-runtime.sh", true);
            add_dir("assets-fonts", share + "/assets/fonts");
            add_file("crossfade.desktop",
                     target.root + "/share/applications/crossfade.desktop", false);
        }
    }

    std::vector<size_t> done;
    auto rollback = [&]() {
        for (auto it = done.rbegin(); it != done.rend(); ++it)
            restore_path(backup_root, steps[*it].rel, steps[*it].dst);
    };

    for (size_t i = 0; i < steps.size(); ++i)
    {
        const Step &s = steps[i];
        std::error_code exists_ec;
        if (!fs::exists(s.src, exists_ec))
            continue; // older/newer releases may not carry an optional file

        const bool ok = s.dir
                            ? replace_dir(s.src, s.dst, backup_root, s.rel, err)
                            : replace_file(s.src, s.dst, backup_root, s.rel, s.exec, err);
        if (!ok)
        {
            rollback();
            return false;
        }
        done.push_back(i);
    }

    if (target.kind == Kind::Prefix)
    {
        const std::string share = target.root + "/share/crossfade";
        const std::string venv_python = share + "/venv/bin/python";
        std::error_code venv_ec;
        if (fs::is_regular_file(venv_python, venv_ec))
            run_with_timeout({share + "/scripts/bootstrap-venv.sh", share + "/venv",
                              share + "/requirements.txt"},
                             180, stage_dir + "/venv.log");
        // Existing installs predate the JavaScript runtime; provision it now so
        // an in-app update does not leave playback on "Requested format is not
        // available". Best-effort: the app still starts if this fails.
        {
            const std::string fetch = share + "/scripts/fetch-js-runtime.sh";
            std::error_code fetch_ec;
            if (fs::is_regular_file(fetch, fetch_ec))
                run_with_timeout({fetch, share + "/runtime"}, 300,
                                 stage_dir + "/runtime.log");
        }
        rewrite_desktop_exec(target.root + "/share/applications/crossfade.desktop",
                             target.root + "/bin/crossfade");
    }
    else if (target.kind == Kind::FlatDir)
    {
        // Portable layout: the runtime lives beside the executable, which is
        // also this update's root.
        const std::string fetch = target.root + "/scripts/fetch-js-runtime.sh";
        std::error_code fetch_ec;
        if (fs::is_regular_file(fetch, fetch_ec))
            run_with_timeout({fetch, target.root + "/runtime"}, 300,
                             stage_dir + "/runtime.log");
        rewrite_desktop_exec(target.root + "/crossfade.desktop",
                             target.root + "/crossfade");
    }
    return true;
#endif
}

void remove_tree(const std::string &path)
{
    std::error_code ec;
    fs::remove_all(path, ec);
}

std::string read_text_file(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
        return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_text_file(const std::string &path, const std::string &text)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;
    out << text;
    return out.good();
}

long long now_epoch()
{
    return (long long)std::time(nullptr);
}

} // namespace update_apply
