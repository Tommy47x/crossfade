#include "version_compare.h"
#include "test_util.h"

#include <cstdio>
using testutil::check;

int main()
{
    using namespace version_compare;

    check(trim("  v0.4.2\n") == "0.4.2", "trim strips spaces and a v prefix");
    check(trim("\"v1.2.3\"") == "1.2.3", "trim strips quotes");
    check(trim("0.4.2-1") == "0.4.2-1", "trim keeps the version suffix");
    check(trim("") == "", "trim of an empty string");

    check(parse("0.4.2") == std::vector<int>({0, 4, 2}), "parse dotted version");
    check(parse("0.4.2-1") == std::vector<int>({0, 4, 2}), "parse stops at a dash");
    check(parse("garbage").empty(), "parse rejects garbage");
    check(parse("").empty(), "parse rejects an empty string");
    check(canonical("v0.4.2-1") == "0.4.2", "canonical drops a suffix");
    check(canonical(" 007.01 ") == "7.1", "canonical normalises components");
    check(canonical("garbage").empty(), "canonical rejects garbage");

    check(newer("0.4.10", "0.4.9"), "numeric components compare as numbers");
    check(newer("0.5", "0.4.9"), "a missing component counts as zero");
    check(newer("1.0.0", "0.99.99"), "major version wins");
    check(!newer("0.4.2", "0.4.2"), "equal versions are not newer");
    check(!newer("0.4", "0.4.0"), "0.4 equals 0.4.0");
    check(!newer("0.4.2", "0.4.3"), "older versions are not newer");
    check(!newer("garbage", "0.4.2"), "malformed latest is never an update");
    check(!newer("0.4.2", "garbage"), "malformed current is never an update");

    // A hostile version.txt must not overflow the component parser.
    const auto huge = parse("999999999999999999999.1");
    check(huge.size() == 2 && huge[0] >= 0, "huge component clamps safely");

    if (testutil::failures == 0)
        std::printf("version_compare: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
