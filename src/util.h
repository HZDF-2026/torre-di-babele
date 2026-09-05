// util.h — greenroom platform layer: time, file IO, paths, env.
#ifndef GR_UTIL_H
#define GR_UTIL_H

#include <string>
#include <vector>

namespace gr {

extern const char* const VERSION;

// Unix epoch milliseconds.
long long nowMs();

// getenv with fallback.
std::string envOr(const char* name, const std::string& fallback);

// File IO, binary, no translation. read throws when unreadable.
void writeBytes(const std::string& path, const std::string& data);
void appendBytes(const std::string& path, const std::string& data);
std::string readBytes(const std::string& path);
bool fileExists(const std::string& path);

// mkdir -p. Returns true if the dir exists (or was created).
bool makeDirs(const std::string& path);

// Join with '/' (room names and keys are plain [a-z0-9._/-], separators are
// forward slashes on every platform for storage consistency).
std::string pathJoin(const std::string& a, const std::string& b);

// Split s by any char in delim; drops empty trailing fields only when keepEmpty=false.
std::vector<std::string> split(const std::string& s, char delim);

// Trim ASCII whitespace both ends.
std::string trim(const std::string& s);

// True when name is a safe room name: [a-zA-Z0-9._-], 1..64 chars.
bool validRoomName(const std::string& name);

// True when key is a safe board key: [a-zA-Z0-9._/-], 1..128 chars.
bool validBoardKey(const std::string& key);

}  // namespace gr

#endif  // GR_UTIL_H
