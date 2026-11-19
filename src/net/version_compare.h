#pragma once

#include <string>
#include <vector>

// Release version helpers shared by the updater and its unit tests.
//
// The app's own version comes from the generated version.h
// (CROSSFADE_VERSION). Release tags are "vX.Y.Z" and version.txt contains
// just "X.Y.Z", so both spellings are accepted.
namespace version_compare
{
// Trim whitespace/quotes and a leading "v"/"V" from a release version.
std::string trim(std::string s);

// Split "0.4.2" (or "0.4.2-1", which stops at the dash) into numbers.
// Returns an empty vector when the string does not start with a digit.
std::vector<int> parse(const std::string &v);

// Canonical dotted form of a version ("v0.4.2-1" -> "0.4.2"), or an empty
// string when there is no numeric version. Use this for anything that ends up
// in a URL or a path; release data is untrusted input.
std::string canonical(const std::string &v);

// True when `latest` is strictly newer than `current`. Missing components
// count as zero, so "0.5" > "0.4.9".
bool newer(const std::string &latest, const std::string &current);
} // namespace version_compare
