// cli.cpp — see cli.h. Sub-commands proxy to serve over HTTP; `serve` is local.
#include "cli.h"

#include <iostream>
#include <stdexcept>
#include <thread>

#include "api.h"
#include "http.h"
#include "jsjson.h"
#include "mcp.h"
#include "store.h"
#include "util.h"

namespace gr {

namespace {

void usage() {
    std::cerr << "greenroom " << VERSION << " — shared chat room for coding sub-agents\n"
              << "\n"
              << "usage:\n"
              << "  greenroom serve  [--port 7788] [--data DIR] [--bind 127.0.0.1]\n"
              << "  greenroom status\n"
              << "  greenroom rooms\n"
              << "  greenroom create ROOM\n"
              << "  greenroom say    ROOM TYPE CONTENT [--agent A] [--ref N]\n"
              << "  greenroom listen ROOM [--since N] [--limit M] [--follow] [--agent A]\n"
              << "  greenroom claim  ROOM SCOPE... [--ttl 600] [--agent A]\n"
              << "  greenroom release ROOM (--id N | --scope S) [--agent A]\n"
              << "  greenroom claims ROOM\n"
              << "  greenroom board  get ROOM KEY\n"
              << "  greenroom board  set ROOM KEY VALUE [--agent A]\n"
              << "  greenroom verify ROOM\n"
              << "  greenroom mcp\n"
              << "\n"
              << "types: say plan fact ask answer done\n"
              << "env:   GREENROOM_URL (default http://127.0.0.1:7788)\n"
              << "       GREENROOM_AGENT (default --agent, else 'anon')\n"
              << "see PROTOCOL.md for the full protocol.\n";
}

struct Target {
    std::string host = "127.0.0.1";
    int port = 7788;
};

Target parseTarget() {
    Target t;
    std::string url = envOr("GREENROOM_URL", "http://127.0.0.1:7788");
    const std::string prefix = "http://";
    std::string rest = url.compare(0, prefix.size(), prefix) == 0 ? url.substr(prefix.size()) : url;
    size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        t.host = rest.substr(0, colon);
        try {
            t.port = std::stoi(rest.substr(colon + 1));
        } catch (...) {
            t.port = 7788;
        }
    }
    return t;
}

std::string urlEnc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

int fail(const ClientResult& r) {
    std::cerr << "error: " << (r.status == 0 ? r.err : r.body) << "\n";
    return 1;
}

// positionals vs --flags; flags with a value consume the next arg.
struct Parsed {
    std::vector<std::string> pos;
    std::vector<std::pair<std::string, std::string>> flags;
};

bool parseArgs(const std::vector<std::string>& in, Parsed& out,
               const std::vector<std::string>& valueFlags) {
    for (size_t i = 0; i < in.size(); i++) {
        const std::string& a = in[i];
        if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
            std::string key = a.substr(2);
            bool needsValue = false;
            for (const std::string& vf : valueFlags)
                if (vf == key) needsValue = true;
            if (needsValue) {
                if (i + 1 >= in.size()) {
                    std::cerr << "error: --" << key << " needs a value\n";
                    return false;
                }
                out.flags.emplace_back(key, in[++i]);
            } else {
                out.flags.emplace_back(key, "");
            }
        } else {
            out.pos.push_back(a);
        }
    }
    return true;
}

std::string flag(const Parsed& p, const std::string& key, const std::string& def = "") {
    for (const auto& kv : p.flags)
        if (kv.first == key) return kv.second;
    return def;
}

long long flagNum(const Parsed& p, const std::string& key, long long def) {
    std::string v = flag(p, key);
    if (v.empty()) return def;
    try {
        return std::stoll(v);
    } catch (...) {
        return def;
    }
}

bool hasFlag(const Parsed& p, const std::string& key) {
    for (const auto& kv : p.flags)
        if (kv.first == key) return true;
    return false;
}

std::string defaultAgent(const Parsed& p) {
    std::string a = flag(p, "agent");
    if (!a.empty()) return a;
    return envOr("GREENROOM_AGENT", "anon");
}

std::string hhmmss(long long tsMs) {
    long long secs = tsMs / 1000;
    int h = static_cast<int>((secs / 3600) % 24);
    int m = static_cast<int>((secs / 60) % 60);
    int s = static_cast<int>(secs % 60);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", h, m, s);
    return buf;
}

void printMsg(const Json& m) {
    long long id = m.get("id") && m.get("id")->isNum() ? static_cast<long long>(m.get("id")->num) : 0;
    long long ts = m.get("ts") && m.get("ts")->isNum() ? static_cast<long long>(m.get("ts")->num) : 0;
    std::string agent = m.get("agent") ? m.get("agent")->str : "?";
    std::string type = m.get("type") ? m.get("type")->str : "?";
    std::string content = m.get("content") ? m.get("content")->str : "";
    std::string ref;
    if (m.get("ref") && m.get("ref")->isNum())
        ref = " ->#" + std::to_string(static_cast<long long>(m.get("ref")->num));
    std::cout << "#" << id << "  " << hhmmss(ts) << "  " << agent << "  " << type << ref << "  "
              << content << "\n";
}

ClientResult httpGet(const Target& t, const std::string& target) {
    return httpClient(t.host, t.port, "GET", target, "");
}

ClientResult httpPost(const Target& t, const std::string& target, const std::string& body) {
    return httpClient(t.host, t.port, "POST", target, body);
}

ClientResult httpPut(const Target& t, const std::string& target, const std::string& body) {
    return httpClient(t.host, t.port, "PUT", target, body);
}

int cmdServe(const Parsed& p) {
    int port = static_cast<int>(flagNum(p, "port", 7788));
    std::string data = flag(p, "data", "greenroom-data");
    std::string bind = flag(p, "bind", "127.0.0.1");
    RoomStore store(data);
    std::string err;
    HttpServer srv(bind, port, makeApiRouter(store));
    std::cerr << "greenroom " << VERSION << " serving on http://" << bind << ":" << port
              << "  (data: " << data << ")\n";
    if (!srv.run(err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    return 0;
}

int cmdListen(const Parsed& p) {
    if (p.pos.empty()) {
        std::cerr << "error: listen needs ROOM\n";
        return 1;
    }
    Target t = parseTarget();
    std::string room = p.pos[0];
    long long since = flagNum(p, "since", 0);
    long long limit = flagNum(p, "limit", 0);
    std::string agentF = flag(p, "agent");
    bool follow = hasFlag(p, "follow");

    for (;;) {
        std::string target = "/v1/rooms/" + urlEnc(room) + "/messages?since=" +
                              std::to_string(since);
        if (limit > 0) target += "&limit=" + std::to_string(limit);
        if (!agentF.empty()) target += "&agent=" + urlEnc(agentF);
        ClientResult r = httpGet(t, target);
        if (!r.ok) return fail(r);
        Json body;
        if (!Json::parse(r.body, body) || !body.isObj() || !body.get("messages")) {
            std::cerr << "error: bad response\n";
            return 1;
        }
        for (const Json& m : body.get("messages")->arr) {
            printMsg(m);
            long long id = m.get("id") && m.get("id")->isNum()
                               ? static_cast<long long>(m.get("id")->num) : since;
            if (id > since) since = id;
        }
        if (!follow) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

}  // namespace

int runCli(const std::vector<std::string>& args) {
    if (args.empty()) {
        usage();
        return 1;
    }
    const std::string& cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    // Local commands (no server needed).
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        usage();
        return 0;
    }
    if (cmd == "mcp") return runMcp();
    {
        Parsed p;
        if (!parseArgs(rest, p, {"port", "data", "bind", "agent", "ref", "since", "limit",
                                 "ttl", "id", "scope"}))
            return 1;
        if (cmd == "serve") return cmdServe(p);

        // Remote commands.
        Target t = parseTarget();
        if (cmd == "status") {
            ClientResult r = httpGet(t, "/v1/status");
            if (!r.ok) return fail(r);
            std::cout << r.body << "\n";
            return 0;
        }
        if (cmd == "rooms") {
            ClientResult r = httpGet(t, "/v1/rooms");
            if (!r.ok) return fail(r);
            Json body;
            if (Json::parse(r.body, body) && body.get("rooms") && body.get("rooms")->isArr()) {
                for (const Json& room : body.get("rooms")->arr) std::cout << room.str << "\n";
            }
            return 0;
        }
        if (cmd == "create") {
            if (p.pos.empty()) {
                std::cerr << "error: create needs ROOM\n";
                return 1;
            }
            Json body = Json::object();
            body.set("name", Json::string(p.pos[0]));
            ClientResult r = httpPost(t, "/v1/rooms", body.dump());
            if (!r.ok) return fail(r);
            std::cout << "created " << p.pos[0] << "\n";
            return 0;
        }
        if (cmd == "say") {
            if (p.pos.size() < 3) {
                std::cerr << "error: say needs ROOM TYPE CONTENT\n";
                return 1;
            }
            Json body = Json::object();
            body.set("agent", Json::string(defaultAgent(p)));
            body.set("type", Json::string(p.pos[1]));
            body.set("content", Json::string(p.pos[2]));
            long long ref = flagNum(p, "ref", -1);
            if (ref >= 0) body.set("ref", Json::number(static_cast<double>(ref)));
            ClientResult r = httpPost(t, "/v1/rooms/" + urlEnc(p.pos[0]) + "/say", body.dump());
            if (!r.ok) return fail(r);
            Json m;
            if (Json::parse(r.body, m)) printMsg(m);
            return 0;
        }
        if (cmd == "listen") return cmdListen(p);
        if (cmd == "claim") {
            if (p.pos.size() < 2) {
                std::cerr << "error: claim needs ROOM SCOPE...\n";
                return 1;
            }
            Json body = Json::object();
            body.set("agent", Json::string(defaultAgent(p)));
            Json sc = Json::array();
            for (size_t i = 1; i < p.pos.size(); i++) sc.push(Json::string(p.pos[i]));
            body.set("scope", std::move(sc));
            long long ttl = flagNum(p, "ttl", 0);
            if (ttl > 0) body.set("ttl_s", Json::number(static_cast<double>(ttl)));
            ClientResult r = httpPost(t, "/v1/rooms/" + urlEnc(p.pos[0]) + "/claim", body.dump());
            if (r.status == 409) {
                std::cerr << "CONFLICT — scope held by another agent:\n" << r.body << "\n";
                return 2;  // distinct exit code: agents can branch on it
            }
            if (!r.ok) return fail(r);
            Json c;
            if (Json::parse(r.body, c) && c.get("claim")) {
                const Json& cl = *c.get("claim");
                long long id = cl.get("id") && cl.get("id")->isNum()
                                   ? static_cast<long long>(cl.get("id")->num) : 0;
                std::string note = "granted";
                if (const Json* n = c.get("note"); n && n->isStr()) note = n->str;
                std::cout << "claim #" << id << " " << note << "\n";
            }
            return 0;
        }
        if (cmd == "release") {
            if (p.pos.empty()) {
                std::cerr << "error: release needs ROOM (--id N | --scope S)\n";
                return 1;
            }
            Json body = Json::object();
            body.set("agent", Json::string(defaultAgent(p)));
            long long id = flagNum(p, "id", 0);
            std::string scope = flag(p, "scope");
            if (id > 0) body.set("claim_id", Json::number(static_cast<double>(id)));
            if (!scope.empty()) body.set("scope", Json::string(scope));
            ClientResult r =
                httpPost(t, "/v1/rooms/" + urlEnc(p.pos[0]) + "/release", body.dump());
            if (!r.ok) return fail(r);
            std::cout << r.body << "\n";
            return 0;
        }
        if (cmd == "claims") {
            if (p.pos.empty()) {
                std::cerr << "error: claims needs ROOM\n";
                return 1;
            }
            ClientResult r = httpGet(t, "/v1/rooms/" + urlEnc(p.pos[0]) + "/claims");
            if (!r.ok) return fail(r);
            std::cout << r.body << "\n";
            return 0;
        }
        if (cmd == "board") {
            if (p.pos.empty() || (p.pos[0] != "get" && p.pos[0] != "set")) {
                std::cerr << "error: board get ROOM KEY | board set ROOM KEY VALUE\n";
                return 1;
            }
            if (p.pos[0] == "get") {
                if (p.pos.size() < 3) {
                    std::cerr << "error: board get needs ROOM KEY\n";
                    return 1;
                }
                ClientResult r = httpGet(
                    t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/board/" + urlEnc(p.pos[2]));
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            if (p.pos.size() < 4) {
                std::cerr << "error: board set needs ROOM KEY VALUE\n";
                return 1;
            }
            Json body = Json::object();
            body.set("agent", Json::string(defaultAgent(p)));
            body.set("value", Json::string(p.pos[3]));
            ClientResult r = httpPut(
                t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/board/" + urlEnc(p.pos[2]), body.dump());
            if (!r.ok) return fail(r);
            std::cout << "set " << p.pos[2] << "\n";
            return 0;
        }
        if (cmd == "verify") {
            if (p.pos.empty()) {
                std::cerr << "error: verify needs ROOM\n";
                return 1;
            }
            ClientResult r = httpGet(t, "/v1/rooms/" + urlEnc(p.pos[0]) + "/verify");
            if (!r.ok) return fail(r);
            std::cout << r.body << "\n";
            Json body;
            return (Json::parse(r.body, body) && body.get("ok") && body.get("ok")->isBool() &&
                    body.get("ok")->b)
                       ? 0
                       : 1;
        }
    }

    usage();
    return 1;
}

}  // namespace gr
