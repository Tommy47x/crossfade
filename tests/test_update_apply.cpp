#include "update_apply.h"
#include "test_util.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using testutil::check;

namespace
{

void write(const fs::path &path, const std::string &text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string read(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    const fs::path root = testutil::make_config_dir("crossfade-update-apply-test");
    const fs::path prefix = root / "prefix";
    const fs::path stage = root / "stage";
    const std::string version = "9.9.9";
    const std::string asset = "crossfade-" + version + "-linux-x64.tar.gz";
    const std::string top = "crossfade-" + version + "-linux-x64";
    const fs::path payload = stage / "extracted" / top;

    // An existing install.sh-style prefix.
    write(prefix / "bin" / "crossfade", "old-binary");
    write(prefix / "bin" / "crossfade-login", "old-login");
    write(prefix / "share" / "crossfade" / "p100-backend.py", "old-backend");
    write(prefix / "share" / "crossfade" / "venv" / "keep.txt", "venv-data");
    write(prefix / "share" / "crossfade" / "py" / "cf_backend" / "stale.py", "stale");
    write(prefix / "share" / "crossfade" / "assets" / "fonts" / "old.ttf", "old-font");
    write(prefix / "share" / "applications" / "crossfade.desktop",
          "[Desktop Entry]\nName=crossfade\nExec=/old/bin/crossfade\n");

    // A staged release payload with the flat tarball layout.
    write(payload / "crossfade", "new-binary");
    write(payload / "crossfade-login", "new-login");
    write(payload / "p100-backend.py", "new-backend");
    write(payload / "p100-backend-oauth.py", "new-oauth");
    write(payload / "requirements.txt", "ytmusicapi\n");
    write(payload / "py" / "cf_backend" / "__init__.py", "new-python");
    write(payload / "scripts" / "crossfade-login-helper.py", "helper");
    write(payload / "scripts" / "bootstrap-venv.sh", "#!/bin/sh\nexit 0\n");
    write(payload / "assets-fonts" / "Inter-Regular.ttf", "new-font");
    write(payload / "crossfade.desktop",
          "[Desktop Entry]\nName=crossfade\nExec=crossfade %U\n");

    update_apply::Target target;
    target.kind = update_apply::Kind::Prefix;
    target.root = prefix.string();
    target.exe = (prefix / "bin" / "crossfade").string();
    target.writable = true;

    std::string err;
    const bool ok = update_apply::apply(target, stage.string(), version, asset, err);
    check(ok, err.empty() ? "prefix apply succeeds" : err.c_str());
    check(read(prefix / "bin" / "crossfade") == "new-binary", "binary replaced");
    check(read(prefix / "bin" / "crossfade-login") == "new-login", "login helper replaced");
    check(read(prefix / "share" / "crossfade" / "p100-backend.py") == "new-backend",
          "backend replaced");
    check(read(prefix / "share" / "crossfade" / "venv" / "keep.txt") == "venv-data",
          "venv is preserved");
    check(!fs::exists(prefix / "share" / "crossfade" / "py" / "cf_backend" / "stale.py"),
          "stale backend files removed");
    check(fs::exists(prefix / "share" / "crossfade" / "assets" / "fonts" /
                     "Inter-Regular.ttf"),
          "fonts updated");
    check(!fs::exists(prefix / "share" / "crossfade" / "assets" / "fonts" / "old.ttf"),
          "old fonts removed");
    const std::string desktop =
        read(prefix / "share" / "applications" / "crossfade.desktop");
    check(desktop.find("Exec=" + prefix.string() + "/bin/crossfade") != std::string::npos,
          "desktop Exec rewritten to the absolute prefix path");
    check(desktop.find("\nName=crossfade") != std::string::npos,
          "desktop file content preserved");

    // A flat tarball run in place.
    const fs::path flat = root / "flat";
    write(flat / "crossfade", "old-flat");
    write(flat / "p100-backend.py", "old-flat-backend");
    update_apply::Target flat_target;
    flat_target.kind = update_apply::Kind::FlatDir;
    flat_target.root = flat.string();
    flat_target.writable = true;

    const fs::path flat_stage = root / "flat-stage";
    const fs::path flat_payload = flat_stage / "extracted" / top;
    write(flat_payload / "crossfade", "new-flat");
    write(flat_payload / "p100-backend.py", "new-flat-backend");
    write(flat_payload / "assets-fonts" / "Inter-Regular.ttf", "flat-font");
    err.clear();
    check(update_apply::apply(flat_target, flat_stage.string(), version, asset, err),
          "flat apply succeeds");
    check(read(flat / "crossfade") == "new-flat", "flat binary replaced");
    check(read(flat / "p100-backend.py") == "new-flat-backend", "flat backend replaced");
    check(fs::exists(flat / "assets" / "fonts" / "Inter-Regular.ttf"),
          "flat fonts land where p100_asset_dir looks");

    // An AppImage replaces one file.
    const fs::path image = root / "crossfade.AppImage";
    write(image, "old-image");
    const fs::path image_stage = root / "image-stage";
    const std::string image_asset = "crossfade-" + version + "-x86_64.AppImage";
    write(image_stage / image_asset, "new-image");
    update_apply::Target image_target;
    image_target.kind = update_apply::Kind::AppImage;
    image_target.root = image.string();
    image_target.writable = true;
    err.clear();
    check(update_apply::apply(image_target, image_stage.string(), version, image_asset, err),
          "AppImage apply succeeds");
    check(read(image) == "new-image", "AppImage replaced in one file");

    check(update_apply::asset_name(update_apply::Kind::AppImage, "1.2.3") ==
              "crossfade-1.2.3-x86_64.AppImage",
          "AppImage asset name");
    check(update_apply::asset_name(update_apply::Kind::Prefix, "1.2.3") ==
              "crossfade-1.2.3-linux-x64.tar.gz",
          "tarball asset name");
    check(update_apply::asset_name(update_apply::Kind::None, "1.2.3").empty(),
          "unsupported install has no asset");

    testutil::check(!update_apply::self_update_supported(update_apply::Target{}),
                    "an undetected install is not self-updatable");

    if (testutil::failures == 0)
        std::printf("update_apply: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
