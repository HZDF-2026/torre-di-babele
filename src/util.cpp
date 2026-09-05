// util.cpp — see util.h.
#include "util.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace gr {

const char* const VERSION = "0.3.0";

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

void writeBytes(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot open for write: " + path);
    if (!data.empty() && std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        throw std::runtime_error("short write: " + path);
    }
    std::fclose(f);
}

void appendBytes(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) throw std::runtime_error("cannot open for append: " + path);
    if (!data.empty() && std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        throw std::runtime_error("short append: " + path);
    }
    std::fclose(f);
}

std::string readBytes(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open for read: " + path);
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool makeDirs(const std::string& path) {
    if (path.empty()) return false;
    if (fileExists(path)) {
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR)) return true;
        return false;
    }
    std::string cur;
    size_t i = 0;
    if (path[0] == '/') cur = "/";
#ifdef _WIN32
    if (path.size() >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
        cur = path.substr(0, 3);
        i = 3;
    }
#endif
    std::vector<std::string> parts;
    std::string seg;
    for (; i < path.size(); i++) {
        if (path[i] == '/' || path[i] == '\\') {
            if (!seg.empty()) parts.push_back(seg);
            seg.clear();
        } else {
            seg += path[i];
        }
    }
    if (!seg.empty()) parts.push_back(seg);
    for (const std::string& p : parts) {
        if (cur.empty()) cur = p;
        else if (cur.back() == '/' || (cur.size() == 3 && cur[1] == ':')) cur += p;
        else cur += "/" + p;
#ifdef _WIN32
        if (_mkdir(cur.c_str()) != 0) {
#else
        if (::mkdir(cur.c_str(), 0755) != 0) {
#endif
            struct stat st;
            if (::stat(cur.c_str(), &st) != 0 || !(st.st_mode & S_IFDIR)) return false;
        }
    }
    return true;
}

std::string pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

static bool allChars(const std::string& s, bool allowSlash) {
    if (s.empty()) return false;
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (allowSlash && c == '/') ok = true;
        if (!ok) return false;
    }
    return true;
}

bool validRoomName(const std::string& name) {
    return name.size() >= 1 && name.size() <= 64 && allChars(name, false);
}

bool validBoardKey(const std::string& key) {
    return key.size() >= 1 && key.size() <= 128 && allChars(key, true);
}

std::string selfExePath() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return n > 0 ? std::string(buf, n) : std::string("greenroom");
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        return std::string(buf);
    }
    return std::string("greenroom");
#endif
}

bool spawnDetached(const std::vector<std::string>& argv, std::string& errOut) {
    if (argv.empty()) {
        errOut = "empty argv";
        return false;
    }
#ifdef _WIN32
    std::string cmd;
    for (size_t i = 0; i < argv.size(); i++) {
        if (i) cmd += " ";
        cmd += "\"" + argv[i] + "\"";
    }
    STARTUPINFOA si;
    std::memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                        nullptr, &si, &pi)) {
        errOut = "CreateProcess failed";
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    pid_t pid = ::fork();
    if (pid < 0) {
        errOut = "fork failed";
        return false;
    }
    if (pid == 0) {
        ::setsid();
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
        }
        std::vector<char*> cargv;
        for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execv(cargv[0], cargv.data());
        _exit(127);
    }
    int st = 0;
    ::waitpid(pid, &st, 0);  // reap the intermediate child immediately
    return true;
#endif
}

std::string defaultDataDir() {
    std::string d = envOr("GREENROOM_DATA", "");
    if (!d.empty()) return d;
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home) return "greenroom-data";
    return std::string(home) + "/.greenroom";
}

}  // namespace gr
