#pragma once

#include <string>

// Linux self-update primitives: figure out how this copy was installed,
// download the matching release asset, verify it, and replace the app files
// in place. Windows keeps using the one-click installer in updater.cpp.
namespace update_apply
{

enum class Kind
{
    None,            // read-only/package-managed or a dev build
    WindowsInstaller,
    AppImage,        // one file, replace it
    FlatDir,         // extracted release tarball run in place
    Prefix           // cmake/install.sh prefix (~/.local or any --prefix)
};

struct Target
{
    Kind kind = Kind::None;
    std::string exe;  // resolved running executable
    std::string root; // AppImage file, flat directory, or install prefix
    bool writable = false;
};

// Inspect the running process to decide whether this install can replace
// itself. Package-managed (/usr-owned) installs report Kind::None.
Target detect_install();

bool self_update_supported(const Target &t);

std::string asset_name(Kind kind, const std::string &version);
std::string release_page_url();
std::string asset_url(const std::string &asset);

// Stage operations. All return false and fill `err` on failure.
bool download(const std::string &url, const std::string &dest, std::string &err);
bool verify(const std::string &file, const std::string &sums_path,
            const std::string &asset, std::string &err);
bool extract(const std::string &archive, const std::string &dest_dir,
             const std::string &version, std::string &err);

// Apply an already-downloaded/verified stage. For tarballs `stage_dir` holds
// `extracted/crossfade-<version>-linux-x64/`; for AppImages it holds the
// image itself. Runs after the app has released its files (startup or
// shutdown), so it may rename over the running executable.
bool apply(const Target &target, const std::string &stage_dir,
           const std::string &version, const std::string &asset,
           std::string &err);

void remove_tree(const std::string &path);
std::string read_text_file(const std::string &path);
bool write_text_file(const std::string &path, const std::string &text);
long long now_epoch();

} // namespace update_apply
