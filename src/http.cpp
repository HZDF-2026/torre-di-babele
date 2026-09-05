// http.cpp — see http.h.
#include "http.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "util.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Sock = SOCKET;
static const Sock kBadSock = INVALID_SOCKET;
static void closeSock(Sock s) { closesocket(s); }
using SockLen = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Sock = int;
static const Sock kBadSock = -1;
static void closeSock(Sock s) { ::close(s); }
using SockLen = socklen_t;
#endif

namespace gr {

namespace {

bool sendAll(Sock s, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        int n = ::send(s, data + off, static_cast<int>(len - off), 0);
#else
        ssize_t n = ::send(s, data + off, len - off, 0);
#endif
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// Reads until the connection closes or n bytes arrive; returns bytes read.
bool recvAll(Sock s, std::string& out) {
    char buf[8192];
    for (;;) {
#ifdef _WIN32
        int n = ::recv(s, buf, sizeof buf, 0);
#else
        ssize_t n = ::recv(s, buf, sizeof buf, 0);
#endif
        if (n < 0) return false;
        if (n == 0) return true;
        out.append(buf, static_cast<size_t>(n));
    }
}

// Reads exactly len bytes.
bool recvExact(Sock s, std::string& out, size_t len) {
    out.reserve(out.size() + len);
    char buf[8192];
    while (out.size() < len) {
        size_t want = std::min(len - out.size(), sizeof buf);
#ifdef _WIN32
        int n = ::recv(s, buf, static_cast<int>(want), 0);
#else
        ssize_t n = ::recv(s, buf, want, 0);
#endif
        if (n <= 0) return false;
        out.append(buf, static_cast<size_t>(n));
    }
    return true;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
            out += static_cast<char>(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

const char* statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        default: return "Status";
    }
}

}  // namespace

bool netInit(std::string& errOut) {
#ifdef _WIN32
    static bool done = false;
    static bool ok = false;
    if (done) return ok;
    WSADATA wsa;
    int r = WSAStartup(MAKEWORD(2, 2), &wsa);
    done = true;
    ok = (r == 0);
    if (r != 0) errOut = "WSAStartup failed";
    return ok;
#else
    (void)errOut;
    return true;
#endif
}

HttpServer::HttpServer(const std::string& bindAddr, int port, HttpHandler handler)
    : bind_(bindAddr), port_(port), handler_(std::move(handler)) {}

bool HttpServer::run(std::string& errOut) {
    if (!netInit(errOut)) return false;
    Sock ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls == kBadSock) {
        errOut = "socket() failed";
        return false;
    }
    int on = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port_));
    addr.sin_addr.s_addr = inet_addr(bind_.c_str());
    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        errOut = "bind failed on " + bind_ + ":" + std::to_string(port_);
        closeSock(ls);
        return false;
    }
    if (::listen(ls, 16) != 0) {
        errOut = "listen failed";
        closeSock(ls);
        return false;
    }

    for (;;) {
        sockaddr_in peer;
        SockLen peerLen = sizeof peer;
        Sock cs = ::accept(ls, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (cs == kBadSock) continue;
        std::thread([this, cs] {
            // Read request head until CRLFCRLF.
            std::string head;
            char buf[4096];
            size_t headEnd = std::string::npos;
            while (headEnd == std::string::npos) {
#ifdef _WIN32
                int n = ::recv(cs, buf, sizeof buf, 0);
#else
                ssize_t n = ::recv(cs, buf, sizeof buf, 0);
#endif
                if (n <= 0) {
                    closeSock(cs);
                    return;
                }
                head.append(buf, static_cast<size_t>(n));
                if (head.size() > 1 << 20) {  // 1 MiB head cap
                    closeSock(cs);
                    return;
                }
                headEnd = head.find("\r\n\r\n");
            }

            // Parse request line + headers.
            HttpRequest req;
            {
                std::string headPart = head.substr(0, headEnd);
                std::vector<std::string> lines = split(headPart, '\n');
                if (lines.empty()) {
                    closeSock(cs);
                    return;
                }
                std::string first = trim(lines[0]);
                size_t sp1 = first.find(' ');
                size_t sp2 = first.rfind(' ');
                if (sp1 == std::string::npos || sp2 == sp1) {
                    closeSock(cs);
                    return;
                }
                req.method = first.substr(0, sp1);
                req.rawPath = first.substr(sp1 + 1, sp2 - sp1 - 1);
                size_t q = req.rawPath.find('?');
                std::string pathOnly =
                    (q == std::string::npos) ? req.rawPath : req.rawPath.substr(0, q);
                req.path = urlDecode(pathOnly);
                if (q != std::string::npos) {
                    std::string qs = req.rawPath.substr(q + 1);
                    for (const std::string& kv : split(qs, '&')) {
                        if (kv.empty()) continue;
                        size_t eq = kv.find('=');
                        if (eq == std::string::npos) {
                            req.query[urlDecode(kv)] = "";
                        } else {
                            req.query[urlDecode(kv.substr(0, eq))] = urlDecode(kv.substr(eq + 1));
                        }
                    }
                }
                // Header lines: "Key: value" — key lowercased for lookup.
                for (size_t i = 1; i < lines.size(); i++) {
                    const std::string& ln = lines[i];
                    size_t colon = ln.find(':');
                    if (colon == std::string::npos) continue;
                    std::string key = ln.substr(0, colon);
                    for (char& c : key) c = static_cast<char>(std::tolower((unsigned char)c));
                    req.headers[key] = trim(ln.substr(colon + 1));
                }
            }

            // Body by Content-Length.
            {
                std::string headLower;
                for (char c : head.substr(0, headEnd)) headLower += (char)std::tolower((unsigned char)c);
                size_t p = headLower.find("content-length:");
                if (p != std::string::npos) {
                    size_t vs = head.find(':', p) + 1;
                    size_t ve = head.find("\r\n", vs);
                    std::string v = trim(head.substr(vs, ve == std::string::npos ? std::string::npos : ve - vs));
                    long long len = 0;
                    try {
                        len = std::stoll(v);
                    } catch (...) {
                        len = 0;
                    }
                    if (len < 0 || len > (8 << 20)) {  // 8 MiB body cap
                        closeSock(cs);
                        return;
                    }
                    std::string body = head.substr(headEnd + 4);
                    if (body.size() < static_cast<size_t>(len) &&
                        !recvExact(cs, body, static_cast<size_t>(len))) {
                        closeSock(cs);
                        return;
                    }
                    req.body = body.substr(0, static_cast<size_t>(len));
                }
            }

            // Dispatch.
            HttpResponse res;
            try {
                res = handler_(req);
            } catch (const std::exception& e) {
                res.status = 500;
                res.body = "{\"error\":\"" + std::string(e.what()) + "\"}";
            } catch (...) {
                res.status = 500;
                res.body = "{\"error\":\"unknown error\"}";
            }
            std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + statusText(res.status) +
                              "\r\nContent-Type: " + res.contentType +
                              "\r\nContent-Length: " + std::to_string(res.body.size()) +
                              "\r\nConnection: close\r\n\r\n" + res.body;
            sendAll(cs, out.data(), out.size());
            closeSock(cs);
        }).detach();
    }
}

ClientResult httpClient(const std::string& host, int port, const std::string& method,
                        const std::string& rawTarget, const std::string& body,
                        const std::string& bearer) {
    ClientResult r;
    std::string err;
    if (!netInit(err)) {
        r.err = err;
        return r;
    }
    Sock s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSock) {
        r.err = "socket() failed";
        return r;
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        r.err = "connect failed — is `greenroom serve` running? (see GREENROOM_URL)";
        closeSock(s);
        return r;
    }
    std::string req = method + " " + rawTarget + " HTTP/1.1\r\nHost: " + host +
                      "\r\nContent-Type: application/json\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\n";
    if (!bearer.empty()) req += "Authorization: Bearer " + bearer + "\r\n";
    req += "\r\n" + body;
    if (!sendAll(s, req.data(), req.size())) {
        r.err = "send failed";
        closeSock(s);
        return r;
    }
    std::string raw;
    if (!recvAll(s, raw)) {
        r.err = "recv failed";
        closeSock(s);
        return r;
    }
    closeSock(s);

    size_t headEnd = raw.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        r.err = "malformed response";
        return r;
    }
    std::string head = raw.substr(0, headEnd);
    r.body = raw.substr(headEnd + 4);
    size_t sp = head.find(' ');
    if (sp != std::string::npos) {
        try {
            r.status = std::stoi(head.substr(sp + 1, 3));
            r.ok = (r.status >= 200 && r.status < 300);
        } catch (...) {
            r.err = "bad status line";
            return r;
        }
    }
    // Defense against chunked transfer: none — server always sends length.
    return r;
}

}  // namespace gr
