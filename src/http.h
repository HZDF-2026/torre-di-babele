// http.h — minimal HTTP/1.1 server + client over blocking sockets.
// Win32 (WinSock2) and POSIX. Thread-per-connection; Connection: close.
#ifndef GR_HTTP_H
#define GR_HTTP_H

#include <functional>
#include <map>
#include <string>

namespace gr {

struct HttpRequest {
    std::string method;
    std::string path;                        // decoded, no query
    std::string rawPath;                      // as received, no query
    std::map<std::string, std::string> query; // decoded
    std::map<std::string, std::string> headers; // lowercased keys, trimmed values
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string body;                         // JSON or HTML text
    std::string contentType = "application/json; charset=utf-8";
};

// Handler returns the response; exceptions inside are caught by the server
// and become 500. Unknown-path handling is the router's job (return 404).
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    HttpServer(const std::string& bindAddr, int port, HttpHandler handler);
    // Blocks forever; returns false when listen/bind fails (errOut filled).
    bool run(std::string& errOut);

private:
    std::string bind_;
    int port_;
    HttpHandler handler_;
};

struct ClientResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string err;   // transport-level error text when !ok
};

// One-shot request against http://host:port + rawTarget (path?query).
// bearer: when non-empty, sent as "Authorization: Bearer <bearer>".
ClientResult httpClient(const std::string& host, int port, const std::string& method,
                        const std::string& rawTarget, const std::string& body,
                        const std::string& bearer = "");

// One-shot socket layer init (Win: WSAStartup). Safe to call repeatedly.
bool netInit(std::string& errOut);

}  // namespace gr

#endif  // GR_HTTP_H
