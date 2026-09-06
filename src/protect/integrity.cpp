// integrity.cpp — read own image, hash it. Windows: GetModuleFileNameA;
// POSIX: /proc/self/exe. Reading a file while it is mapped is fine on both.
#include "protect/integrity.h"

#include <cstdio>
#include <string>
#include <vector>

#include "sha256.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace gr {

std::string selfSha256() {
    char path[4096];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(nullptr, path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) return "";
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return "";
    path[n] = '\0';
#endif
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return "";
    std::string bytes;
    char buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, got);
    std::fclose(f);
    return sha256Hex(bytes);
}

}  // namespace gr
