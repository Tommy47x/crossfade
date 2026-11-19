#pragma once

#include <cstddef>
#include <string>

// Small in-tree SHA-256 so update downloads can be verified without
// shelling out to sha256sum/shasum (which the AppImage cannot count on).
namespace sha256
{

std::string hex_digest(const void *data, size_t len);
std::string hex_digest(const std::string &s);

// Streams the file from disk. Returns false when it cannot be read.
bool file_hex_digest(const std::string &path, std::string &out);

} // namespace sha256
