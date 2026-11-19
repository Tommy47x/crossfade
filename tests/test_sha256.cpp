#include "sha256.h"
#include "test_util.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using testutil::check;

int main()
{
    check(sha256::hex_digest("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 of the empty string");
    check(sha256::hex_digest("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 of abc");
    check(sha256::hex_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256 of the NIST 56-byte vector");

    std::string million_a(1000000, 'a');
    check(sha256::hex_digest(million_a) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "SHA-256 of one million a characters");

    const std::filesystem::path root = testutil::make_config_dir("crossfade-sha256-test");
    const std::filesystem::path file = root / "abc.bin";
    testutil::write_file(file, "abc");
    std::string digest;
    check(sha256::file_hex_digest(file.string(), digest),
          "file digest reads a regular file");
    check(digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "file digest matches the byte digest");
    check(!sha256::file_hex_digest((root / "missing").string(), digest),
          "file digest fails cleanly for a missing file");

    if (testutil::failures == 0)
        std::printf("sha256: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
