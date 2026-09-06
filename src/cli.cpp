// cli.cpp — see cli.h. Sub-commands proxy to serve over HTTP; `serve` is local.
#include "cli.h"

#include <iostream>
#include <map>
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
              << "  greenroom serve  [--port 7788] [--data DIR] [--bind 127.0.0.1] [--token S]\n"
              << "  greenroom status\n"
              << "  greenroom agent  register NAME --key KEY\n"
              << "  greenroom agent  list\n"
              << "  greenroom rooms\n"
              << "  greenroom create ROOM [--chamber]      (chamber needs --agent + --key)\n"
              << "  greenroom member list ROOM\n"
              << "  greenroom member add ROOM AGENT        (caller: --agent + --key, member only)\n"
              << "  greenroom say    ROOM TYPE CONTENT [--agent A] [--ref N]\n"
              << "  greenroom listen ROOM [--since N] [--limit M] [--follow] [--agent A]\n"
              << "  greenroom wait   ROOM [--since N] [--timeout-ms 30000]\n"
              << "  greenroom search QUERY [--room R] [--limit N]\n"
              << "  greenroom claim  ROOM SCOPE... [--ttl 600] [--agent A]\n"
              << "  greenroom release ROOM (--id N | --scope S) [--agent A]\n"
              << "  greenroom claims ROOM\n"
              << "  greenroom board  get ROOM KEY\n"
              << "  greenroom board  set ROOM KEY VALUE [--agent A]\n"
              << "  greenroom task add ROOM TITLE... [--detail D] [--agent A] [--human]\n"
              << "  greenroom task list ROOM\n"
              << "  greenroom task claim ROOM ID [--agent A]\n"
              << "  greenroom task submit ROOM ID EVIDENCE... [--agent A]\n"
              << "  greenroom task verify ROOM ID [--agent A] [--reject]\n"
              << "  greenroom post   define ROOM NAME [--verify-task] [--verify-goal] [--model M]\n"
              << "  greenroom post   list ROOM\n"
              << "  greenroom population ROOM\n"
              << "  greenroom report  ROOM [--mode hzdf|company|feudal]\n"
              << "  greenroom goal  set ROOM TEXT... [--criteria C] [--oracle URL] [--agent A]\n"
              << "  greenroom goal  show ROOM\n"
              << "  greenroom goal  achieve ROOM EVIDENCE... [--agent A]\n"
              << "  greenroom goal  verify ROOM [--agent A] [--reject]\n"
              << "  greenroom goal  abandon ROOM REASON... [--agent A]\n"
              << "  greenroom gen   ROOM\n"
              << "  greenroom gen   advance ROOM NOTE... [--agent A]\n"
              << "  greenroom gen   chronicle ROOM TEXT... [--agent A]\n"
              << "  greenroom role  take ROOM ROLE [--agent A]\n"
              << "  greenroom role  list ROOM\n"
              << "  greenroom verify ROOM\n"
              << "  greenroom mcp\n"
              << "\n"
              << "types: say plan fact ask answer done task goal gen role\n"
              << "posts: commander recorder executor reviewer tester + custom (post define)\n"
              << "env:   GREENROOM_URL (default http://127.0.0.1:7788)\n"
              << "       GREENROOM_TOKEN (Bearer token when serve runs with --token)\n"
              << "       GREENROOM_AGENT (default --agent, else 'anon')\n"
              << "       GREENROOM_KEY (identity key; pairs with --agent for chambers)\n"
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

// Identity headers for this invocation (set once in runCli from --key /
// GREENROOM_KEY + the agent name). Sent on every request; the server only
// honors them for chambers and agent registration.
std::map<std::string, std::string> gIdentity;

ClientResult httpGet(const Target& t, const std::string& target) {
    return httpClient(t.host, t.port, "GET", target, "", envOr("GREENROOM_TOKEN", ""), 0,
                      gIdentity);
}

ClientResult httpPost(const Target& t, const std::string& target, const std::string& body) {
    return httpClient(t.host, t.port, "POST", target, body, envOr("GREENROOM_TOKEN", ""), 0,
                      gIdentity);
}

ClientResult httpPut(const Target& t, const std::string& target, const std::string& body) {
    return httpClient(t.host, t.port, "PUT", target, body, envOr("GREENROOM_TOKEN", ""), 0,
                      gIdentity);
}

int cmdServe(const Parsed& p) {
    int port = static_cast<int>(flagNum(p, "port", 7788));
    std::string data = flag(p, "data", defaultDataDir());
    std::string bind = flag(p, "bind", "127.0.0.1");
    std::string token = flag(p, "token", envOr("GREENROOM_TOKEN", ""));
    if ((bind != "127.0.0.1" && bind != "localhost" && bind != "::1") && token.empty()) {
        std::cerr << "warning: binding " << bind << " WITHOUT a token — anyone on the network "
                  << "can read/write every room. Pass --token <secret>.\n";
    }
    RoomStore store(data);
    std::string err;
    HttpServer srv(bind, port, makeApiRouter(store, token));
    std::cerr << "greenroom " << VERSION << " serving on http://" << bind << ":" << port
              << "  (data: " << data << ")"
              << (token.empty() ? "" : "  (auth: token required)") << "\n";
    std::cerr << "web UI: http://" << bind << ":" << port << "/\n";
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
        // Long-poll: block server-side until new messages, then loop.
        ClientResult w = httpGet(t, "/v1/rooms/" + urlEnc(room) +
                                       "/wait?since=" + std::to_string(since) +
                                       "&timeout_ms=25000");
        if (!w.ok) return fail(w);
    }
}

int cmdWait(const Parsed& p) {
    if (p.pos.empty()) {
        std::cerr << "error: wait needs ROOM\n";
        return 1;
    }
    Target t = parseTarget();
    std::string target = "/v1/rooms/" + urlEnc(p.pos[0]) +
                         "/wait?since=" + std::to_string(flagNum(p, "since", 0)) +
                         "&timeout_ms=" + std::to_string(flagNum(p, "timeout-ms", 30000));
    ClientResult r = httpGet(t, target);
    if (!r.ok) return fail(r);
    Json body;
    if (!Json::parse(r.body, body) || !body.isObj() || !body.get("messages")) {
        std::cerr << "error: bad response\n";
        return 1;
    }
    for (const Json& m : body.get("messages")->arr) printMsg(m);
    return 0;
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
        if (!parseArgs(rest, p, {"port", "data", "bind", "token", "agent", "ref", "since",
                                 "limit", "ttl", "id", "scope", "timeout-ms", "detail", "room",
                                 "criteria", "oracle", "key", "model", "mode"}))
            return 1;
        if (cmd == "serve") return cmdServe(p);

        // Identity headers for this invocation: --key (or GREENROOM_KEY)
        // paired with the agent name. The server verifies both against the
        // registry; chambers refuse requests without them.
        {
            std::string key = flag(p, "key");
            if (key.empty()) key = envOr("GREENROOM_KEY", "");
            if (!key.empty()) {
                gIdentity["X-GR-Agent"] = defaultAgent(p);
                gIdentity["X-GR-Key"] = key;
            }
        }

        // Remote commands.
        Target t = parseTarget();
        if (cmd == "status") {
            ClientResult r = httpGet(t, "/v1/status");
            if (!r.ok) return fail(r);
            std::cout << r.body << "\n";
            return 0;
        }
        if (cmd == "agent") {
            if (p.pos.empty()) {
                std::cerr << "error: agent register|list ...\n";
                return 1;
            }
            std::string action = p.pos[0];
            if (action == "register") {
                if (p.pos.size() < 2) {
                    std::cerr << "error: agent register needs NAME --key KEY\n";
                    return 1;
                }
                std::string key = flag(p, "key");
                if (key.empty()) key = envOr("GREENROOM_KEY", "");
                if (key.empty()) {
                    std::cerr << "error: agent register needs --key KEY (8..128 chars)\n";
                    return 1;
                }
                Json body = Json::object();
                body.set("name", Json::string(p.pos[1]));
                body.set("key", Json::string(key));
                ClientResult r = httpPost(t, "/v1/agents", body.dump());
                if (!r.ok) return fail(r);
                std::cout << "registered " << p.pos[1]
                          << " (key hashed with SHA-256, never stored — keep it safe)\n";
                return 0;
            }
            if (action == "list") {
                ClientResult r = httpGet(t, "/v1/agents");
                if (!r.ok) return fail(r);
                Json body;
                if (Json::parse(r.body, body) && body.get("agents") &&
                    body.get("agents")->isArr()) {
                    for (const Json& a : body.get("agents")->arr) std::cout << a.str << "\n";
                }
                return 0;
            }
            std::cerr << "error: agent register|list ...\n";
            return 1;
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
            if (hasFlag(p, "chamber")) {
                body.set("chamber", Json::boolean(true));
                if (gIdentity.empty()) {
                    std::cerr << "error: --chamber needs a registered identity "
                                 "(--agent NAME --key KEY)\n";
                    return 1;
                }
            }
            ClientResult r = httpPost(t, "/v1/rooms", body.dump());
            if (!r.ok) return fail(r);
            std::cout << "created " << p.pos[0]
                      << (hasFlag(p, "chamber") ? " (chamber)" : "") << "\n";
            return 0;
        }
        if (cmd == "member") {
            if (p.pos.size() < 2) {
                std::cerr << "error: member add ROOM AGENT | member list ROOM\n";
                return 1;
            }
            std::string action = p.pos[0];
            if (action == "list") {
                ClientResult r = httpGet(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/members");
                if (!r.ok) return fail(r);
                Json body;
                if (Json::parse(r.body, body) && body.get("members") &&
                    body.get("members")->isArr()) {
                    for (const Json& a : body.get("members")->arr) std::cout << a.str << "\n";
                }
                return 0;
            }
            if (action == "add") {
                if (p.pos.size() < 3) {
                    std::cerr << "error: member add needs ROOM AGENT\n";
                    return 1;
                }
                if (gIdentity.empty()) {
                    std::cerr << "error: member add needs a registered caller "
                                 "(--agent NAME --key KEY)\n";
                    return 1;
                }
                Json body = Json::object();
                body.set("agent", Json::string(p.pos[2]));
                ClientResult r = httpPost(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/members",
                                          body.dump());
                if (!r.ok) return fail(r);
                std::cout << "added " << p.pos[2] << " to " << p.pos[1] << "\n";
                return 0;
            }
            std::cerr << "error: member add|list ...\n";
            return 1;
        }
        if (cmd == "post") {
            if (p.pos.size() < 2) {
                std::cerr << "error: post define ROOM NAME [--verify-task] [--verify-goal] "
                             "[--model M] | post list ROOM\n";
                return 1;
            }
            std::string action = p.pos[0];
            if (action == "list") {
                ClientResult r = httpGet(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/posts");
                if (!r.ok) return fail(r);
                Json body;
                if (Json::parse(r.body, body) && body.get("posts") &&
                    body.get("posts")->isArr()) {
                    for (const Json& pj : body.get("posts")->arr) {
                        std::string name = pj.get("name") ? pj.get("name")->str : "?";
                        bool vt = pj.get("canVerifyTask") && pj.get("canVerifyTask")->isBool() &&
                                  pj.get("canVerifyTask")->b;
                        bool vg = pj.get("canVerifyGoal") && pj.get("canVerifyGoal")->isBool() &&
                                  pj.get("canVerifyGoal")->b;
                        std::string model = pj.get("model") ? pj.get("model")->str : "";
                        bool preset = pj.get("preset") && pj.get("preset")->isBool() &&
                                      pj.get("preset")->b;
                        std::cout << name << (preset ? " (preset)" : "") << "  verify-task: "
                                  << (vt ? "yes" : "no") << "  verify-goal: "
                                  << (vg ? "yes" : "no")
                                  << (model.empty() ? "" : "  model: " + model) << "\n";
                    }
                }
                return 0;
            }
            if (action == "define") {
                if (p.pos.size() < 3) {
                    std::cerr << "error: post define needs ROOM NAME\n";
                    return 1;
                }
                Json body = Json::object();
                body.set("name", Json::string(p.pos[2]));
                body.set("agent", Json::string(defaultAgent(p)));
                if (hasFlag(p, "verify-task")) body.set("canVerifyTask", Json::boolean(true));
                if (hasFlag(p, "verify-goal")) body.set("canVerifyGoal", Json::boolean(true));
                std::string model = flag(p, "model");
                if (!model.empty()) body.set("model", Json::string(model));
                ClientResult r = httpPost(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/posts",
                                          body.dump());
                if (!r.ok) return fail(r);
                std::cout << "defined post " << p.pos[2] << " in " << p.pos[1] << "\n";
                return 0;
            }
            std::cerr << "error: post define|list ...\n";
            return 1;
        }
        if (cmd == "population") {
            if (p.pos.empty()) {
                std::cerr << "error: population needs ROOM\n";
                return 1;
            }
            ClientResult r = httpGet(t, "/v1/rooms/" + urlEnc(p.pos[0]) + "/population");
            if (!r.ok) return fail(r);
            Json body;
            if (Json::parse(r.body, body) && body.get("activeAgents") &&
                body.get("activeAgents")->isArr()) {
                std::cout << "active (" << body.get("activeAgents")->arr.size() << "):";
                for (const Json& a : body.get("activeAgents")->arr)
                    std::cout << " " << a.str;
                std::cout << "\n(parent loop: keep 5-10 active, replenish below 3)\n";
            }
            return 0;
        }
        if (cmd == "report") {
            if (p.pos.empty()) {
                std::cerr << "error: report needs ROOM [--mode hzdf|company|feudal]\n";
                return 1;
            }
            std::string mode = flag(p, "mode");
            if (mode.empty()) mode = "hzdf";
            ClientResult r = httpGet(t, "/v1/rooms/" + urlEnc(p.pos[0]) +
                                          "/report?mode=" + urlEnc(mode));
            if (!r.ok) return fail(r);
            Json body;
            if (Json::parse(r.body, body) && body.get("report"))
                std::cout << body.get("report")->str << "\n";
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
        if (cmd == "wait") return cmdWait(p);
        if (cmd == "search") {
            if (p.pos.empty()) {
                std::cerr << "error: search needs QUERY\n";
                return 1;
            }
            std::string target = "/v1/search?q=" + urlEnc(p.pos[0]);
            if (const std::string& rf = flag(p, "room"); !rf.empty())
                target += "&room=" + urlEnc(rf);
            long long lim = flagNum(p, "limit", 0);
            if (lim > 0) target += "&limit=" + std::to_string(lim);
            ClientResult r = httpGet(t, target);
            if (!r.ok) return fail(r);
            Json body;
            if (Json::parse(r.body, body) && body.get("hits") && body.get("hits")->isArr()) {
                for (const Json& m : body.get("hits")->arr) {
                    std::string room = m.get("room") ? m.get("room")->str : "?";
                    long long id = m.get("id") && m.get("id")->isNum()
                                       ? static_cast<long long>(m.get("id")->num) : 0;
                    std::string agent = m.get("agent") ? m.get("agent")->str : "?";
                    std::string content = m.get("content") ? m.get("content")->str : "";
                    std::cout << room << "  #" << id << "  " << agent << "  " << content << "\n";
                }
            }
            return 0;
        }
        if (cmd == "task") {
            if (p.pos.size() < 2) {
                std::cerr << "error: task add|list|claim|submit|verify ...\n";
                return 1;
            }
            std::string action = p.pos[0];
            if (action == "add") {
                if (p.pos.size() < 3) {
                    std::cerr << "error: task add needs ROOM TITLE...\n";
                    return 1;
                }
                Json body = Json::object();
                std::string title;
                for (size_t i = 2; i < p.pos.size(); i++)
                    title += (title.empty() ? "" : " ") + p.pos[i];
                body.set("title", Json::string(title));
                body.set("detail", Json::string(flag(p, "detail")));
                body.set("agent", Json::string(defaultAgent(p)));
                if (hasFlag(p, "human")) body.set("human", Json::boolean(true));
                ClientResult r = httpPost(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/tasks",
                                          body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            if (action == "list") {
                ClientResult r = httpGet(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/tasks");
                if (!r.ok) return fail(r);
                Json body;
                if (Json::parse(r.body, body) && body.get("tasks") &&
                    body.get("tasks")->isArr()) {
                    for (const Json& tk : body.get("tasks")->arr) {
                        long long id = tk.get("id") && tk.get("id")->isNum()
                                           ? static_cast<long long>(tk.get("id")->num) : 0;
                        std::string st = tk.get("status") ? tk.get("status")->str : "?";
                        std::string ti = tk.get("title") ? tk.get("title")->str : "";
                        std::string asg = tk.get("assignee") ? tk.get("assignee")->str : "";
                        bool human = tk.get("human") && tk.get("human")->isBool() &&
                                     tk.get("human")->b;
                        std::cout << "#" << id << "  [" << st << "]  " << ti
                                  << (human ? "  [human sign-off]" : "")
                                  << (asg.empty() ? "" : "  @" + asg) << "\n";
                    }
                }
                return 0;
            }
            // claim/submit/verify all need ROOM ID.
            if (p.pos.size() < 3) {
                std::cerr << "error: task " << action << " needs ROOM ID\n";
                return 1;
            }
            long long id = 0;
            try {
                id = std::stoll(p.pos[2]);
            } catch (...) {
                std::cerr << "error: bad task id\n";
                return 1;
            }
            std::string base = "/v1/rooms/" + urlEnc(p.pos[1]) + "/tasks/" + std::to_string(id);
            if (action == "claim") {
                Json body = Json::object();
                body.set("agent", Json::string(defaultAgent(p)));
                ClientResult r = httpPost(t, base + "/claim", body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            if (action == "submit") {
                if (p.pos.size() < 4) {
                    std::cerr << "error: task submit needs ROOM ID EVIDENCE...\n";
                    return 1;
                }
                std::string evidence;
                for (size_t i = 3; i < p.pos.size(); i++)
                    evidence += (evidence.empty() ? "" : " ") + p.pos[i];
                Json body = Json::object();
                body.set("agent", Json::string(defaultAgent(p)));
                body.set("evidence", Json::string(evidence));
                ClientResult r = httpPost(t, base + "/submit", body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            if (action == "verify") {
                Json body = Json::object();
                body.set("agent", Json::string(defaultAgent(p)));
                body.set("accept", Json::boolean(!hasFlag(p, "reject")));
                ClientResult r = httpPost(t, base + "/verify", body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            std::cerr << "error: unknown task action: " << action << "\n";
            return 1;
        }
        if (cmd == "goal") {
            if (p.pos.size() < 2) {
                std::cerr << "error: goal set|show|achieve|verify|abandon ...\n";
                return 1;
            }
            std::string action = p.pos[0];
            auto joinRest = [&](size_t from) {
                std::string out;
                for (size_t i = from; i < p.pos.size(); i++)
                    out += (out.empty() ? "" : " ") + p.pos[i];
                return out;
            };
            if (action == "set") {
                if (p.pos.size() < 3) {
                    std::cerr << "error: goal set needs ROOM TEXT...\n";
                    return 1;
                }
                Json body = Json::object();
                body.set("text", Json::string(joinRest(2)));
                body.set("criteria", Json::string(flag(p, "criteria")));
                body.set("oracle", Json::string(flag(p, "oracle")));
                body.set("agent", Json::string(defaultAgent(p)));
                ClientResult r =
                    httpPost(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/goal", body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            if (action == "show") {
                ClientResult r = httpGet(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/goal");
                if (!r.ok) return fail(r);
                Json body;
                if (!Json::parse(r.body, body) || !body.get("goal")) {
                    std::cerr << "error: bad response\n";
                    return 1;
                }
                const Json& g = *body.get("goal");
                if (g.get("exists") && g.get("exists")->isBool() && !g.get("exists")->b) {
                    std::cout << "no god goal declared — the room is not a society\n";
                    return 0;
                }
                std::cout << "goal:    " << (g.get("text") ? g.get("text")->str : "?") << "\n"
                          << "status:  " << (g.get("status") ? g.get("status")->str : "?") << "\n"
                          << "criteria:" << (g.get("criteria") ? g.get("criteria")->str : "") << "\n"
                          << "declared by: "
                          << (g.get("proposer") ? g.get("proposer")->str : "?") << "\n";
                if (g.get("oracle") && !g.get("oracle")->str.empty())
                    std::cout << "oracle:  " << g.get("oracle")->str << "\n";
                if (g.get("evidence") && !g.get("evidence")->str.empty())
                    std::cout << "evidence:" << g.get("evidence")->str << "\n";
                if (g.get("verifier") && !g.get("verifier")->str.empty())
                    std::cout << "verified by: " << g.get("verifier")->str << "\n";
                return 0;
            }
            if (action == "achieve") {
                if (p.pos.size() < 3) {
                    std::cerr << "error: goal achieve needs ROOM EVIDENCE...\n";
                    return 1;
                }
                Json body = Json::object();
                body.set("agent", Json::string(defaultAgent(p)));
                body.set("evidence", Json::string(joinRest(2)));
                ClientResult r =
                    httpPost(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/goal/achieve", body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            if (action == "verify") {
                Json body = Json::object();
                body.set("agent", Json::string(defaultAgent(p)));
                body.set("accept", Json::boolean(!hasFlag(p, "reject")));
                ClientResult r =
                    httpPost(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/goal/verify", body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            if (action == "abandon") {
                Json body = Json::object();
                body.set("agent", Json::string(defaultAgent(p)));
                body.set("reason", Json::string(joinRest(2)));
                ClientResult r =
                    httpPost(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/goal/abandon", body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            std::cerr << "error: unknown goal action: " << action << "\n";
            return 1;
        }
        if (cmd == "gen") {
            if (p.pos.empty()) {
                std::cerr << "error: gen needs ROOM (or: gen advance ROOM NOTE...)\n";
                return 1;
            }
            if (p.pos[0] == "advance") {
                if (p.pos.size() < 3) {
                    std::cerr << "error: gen advance needs ROOM NOTE...\n";
                    return 1;
                }
                std::string note;
                for (size_t i = 2; i < p.pos.size(); i++)
                    note += (note.empty() ? "" : " ") + p.pos[i];
                Json body = Json::object();
                body.set("agent", Json::string(defaultAgent(p)));
                body.set("note", Json::string(note));
                ClientResult r =
                    httpPost(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/gen/advance", body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            if (p.pos[0] == "chronicle") {
                if (p.pos.size() < 3) {
                    std::cerr << "error: gen chronicle needs ROOM TEXT...\n";
                    return 1;
                }
                std::string text;
                for (size_t i = 2; i < p.pos.size(); i++)
                    text += (text.empty() ? "" : " ") + p.pos[i];
                Json body = Json::object();
                body.set("agent", Json::string(defaultAgent(p)));
                body.set("chronicle", Json::string(text));
                ClientResult r =
                    httpPost(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/gen/chronicle", body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            ClientResult r = httpGet(t, "/v1/rooms/" + urlEnc(p.pos[0]) + "/gen");
            if (!r.ok) return fail(r);
            Json body;
            if (!Json::parse(r.body, body) || !body.get("generations")) {
                std::cerr << "error: bad response\n";
                return 1;
            }
            long long curGen = body.get("generation") && body.get("generation")->isNum()
                                   ? static_cast<long long>(body.get("generation")->num) : 0;
            if (curGen == 0) std::cout << "no active generation (no god goal declared)\n";
            else std::cout << "active generation: " << curGen << "\n";
            for (const Json& g : body.get("generations")->arr) {
                long long n = g.get("n") && g.get("n")->isNum()
                                  ? static_cast<long long>(g.get("n")->num) : 0;
                std::string st = g.get("status") ? g.get("status")->str : "?";
                std::string note = g.get("note") ? g.get("note")->str : "";
                std::string chronicle = g.get("chronicle") ? g.get("chronicle")->str : "";
                std::string chronicler = g.get("chronicler") ? g.get("chronicler")->str : "";
                std::cout << "gen " << n << "  [" << st << "]"
                          << (note.empty() ? "" : "  " + note) << "\n";
                if (!chronicle.empty()) {
                    std::string prev =
                        chronicle.size() > 200 ? chronicle.substr(0, 200) + "…" : chronicle;
                    std::cout << "      chronicle (by " << (chronicler.empty() ? "?" : chronicler)
                              << "): " << prev << "\n";
                }
            }
            return 0;
        }
        if (cmd == "role") {
            if (p.pos.size() < 2) {
                std::cerr << "error: role take ROOM ROLE | role list ROOM\n";
                return 1;
            }
            if (p.pos[0] == "take") {
                if (p.pos.size() < 3) {
                    std::cerr << "error: role take needs ROOM ROLE\n";
                    return 1;
                }
                Json body = Json::object();
                body.set("role", Json::string(p.pos[2]));
                body.set("agent", Json::string(defaultAgent(p)));
                ClientResult r =
                    httpPost(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/roles", body.dump());
                if (!r.ok) return fail(r);
                std::cout << r.body << "\n";
                return 0;
            }
            if (p.pos[0] == "list") {
                ClientResult r = httpGet(t, "/v1/rooms/" + urlEnc(p.pos[1]) + "/roles");
                if (!r.ok) return fail(r);
                Json body;
                if (!Json::parse(r.body, body) || !body.get("roles")) {
                    std::cerr << "error: bad response\n";
                    return 1;
                }
                for (const Json& e : body.get("roles")->arr)
                    std::cout << (e.get("role") ? e.get("role")->str : "?") << "  "
                              << (e.get("agent") ? e.get("agent")->str : "?") << "\n";
                return 0;
            }
            std::cerr << "error: unknown role action: " << p.pos[0] << "\n";
            return 1;
        }
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
