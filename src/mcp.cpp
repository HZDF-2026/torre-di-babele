// mcp.cpp — see mcp.h. Newline-delimited JSON-RPC 2.0 on stdio.
//
// Tool set (proxied to serve via HTTP):
//   greenroom_protocol                          the sub-agent briefing text
//   greenroom_status / rooms / create_room
//   greenroom_say / listen
//   greenroom_claim / release / claims
//   greenroom_board_get / board_set
//   greenroom_verify
#include "mcp.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

#include "http.h"
#include "jsjson.h"
#include "util.h"

namespace gr {

namespace {

const char* kProtocolBriefing =
    "You are agent {name} working in greenroom room {room} — a shared workspace "
    "for parallel agents. Protocol, in order:\n"
    "1. Sync first: run `greenroom listen {room} --since 0`. Everything other "
    "agents found is already there — do not re-read files they reported as facts; "
    "trust and build on them.\n"
    "2. Claim before touching: `greenroom claim {room} <path-or-task> --agent {name}` "
    "for every file or task you will modify. HTTP 409 means someone owns it: do not "
    "fight, pick other work or ask in the room.\n"
    "3. Publish as you go: every non-trivial finding becomes "
    "`greenroom say {room} fact \"<path>:<line> — <finding>\" --agent {name}`. "
    "Facts are the room's currency; yours save the next agent a read.\n"
    "4. Ask, don't stall: `greenroom say {room} ask \"<question>\" --agent {name}`; "
    "answer others with `answer` and `--ref <id>`.\n"
    "5. Leave clean: `greenroom release {room} --scope <s> --agent {name}`, then "
    "`greenroom say {room} done \"<one-line summary>\" --agent {name}`.\n"
    "6. Task board: `greenroom task list {room}` — claim a task, submit it with "
    "evidence (`task submit {room} <id> \"<evidence>\"`); a different agent "
    "verifies. Search old findings: `greenroom search <text>`.\n"
    "7. Society: check `greenroom_society status` for {room}. If the room has a "
    "god goal, you are one mortal generation of a society that exists only to "
    "fulfill it. When your generation's task board drains with the goal still "
    "open, the server retires your generation and births the next one — the "
    "recorder should distill what was tried, what worked and what remains into "
    "a chronicle first (`gen chronicle`); the next generation starts from "
    "chronicles, not a full history re-read. Take a role (commander, recorder, "
    "executor, reviewer, tester); while roles are registered, only reviewer/"
    "tester agents may verify tasks and goal achievement. A goal may declare "
    "an oracle: an http:// URL answering {\"satisfied\": bool} — the sole judge "
    "of achievement; credentials live inside the oracle service, never in goal "
    "text. Tasks awaiting human sign-off (human=true) block generation "
    "turnover until the sovereign agent 'human' signs them. Propose "
    "achievement only with evidence (`goal achieve`); a different agent "
    "verifies it.\n"
    "8. Parent loop: the parent agent long-polls `greenroom wait {room}`; on a "
    "`gen` message it spawns the next generation's sub-agents with this "
    "briefing, on `goal` ACHIEVED it stops.\n"
    "Claims expire after their TTL — if your work takes longer, re-claim. The room "
    "is hash-chained and audited; say what you did, do what you said.";

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

Json rpcResult(long long id, Json result) {
    Json j = Json::object();
    j.set("jsonrpc", Json::string("2.0"));
    j.set("id", Json::number(static_cast<double>(id)));
    j.set("result", std::move(result));
    return j;
}

Json rpcError(long long id, int code, const std::string& msg) {
    Json j = Json::object();
    j.set("jsonrpc", Json::string("2.0"));
    j.set("id", Json::number(static_cast<double>(id)));
    Json e = Json::object();
    e.set("code", Json::number(static_cast<double>(code)));
    e.set("message", Json::string(msg));
    j.set("error", std::move(e));
    return j;
}

Json textContent(const std::string& text) {
    Json c = Json::object();
    c.set("type", Json::string("text"));
    c.set("text", Json::string(text));
    return c;
}

Json toolOk(const std::string& text) {
    Json r = Json::object();
    Json arr = Json::array();
    arr.push(textContent(text));
    r.set("content", std::move(arr));
    return r;
}

Json toolErr(const std::string& text) {
    Json r = Json::object();
    Json arr = Json::array();
    arr.push(textContent(text));
    r.set("content", std::move(arr));
    r.set("isError", Json::boolean(true));
    return r;
}

// ---- helpers to build tool schemas ------------------------------------

Json propStr(const std::string& desc) {
    Json j = Json::object();
    j.set("type", Json::string("string"));
    j.set("description", Json::string(desc));
    return j;
}

Json propNum(const std::string& desc) {
    Json j = Json::object();
    j.set("type", Json::string("number"));
    j.set("description", Json::string(desc));
    return j;
}

Json propStrArr(const std::string& desc) {
    Json j = Json::object();
    j.set("type", Json::string("array"));
    j.set("description", Json::string(desc));
    Json items = Json::object();
    items.set("type", Json::string("string"));
    j.set("items", std::move(items));
    return j;
}

Json toolDef(const std::string& name, const std::string& desc, Json props,
             const std::vector<std::string>& req) {
    Json t = Json::object();
    t.set("name", Json::string(name));
    t.set("description", Json::string(desc));
    Json sch = Json::object();
    sch.set("type", Json::string("object"));
    sch.set("properties", std::move(props));
    Json r = Json::array();
    for (const std::string& k : req) r.push(Json::string(k));
    sch.set("required", std::move(r));
    t.set("inputSchema", std::move(sch));
    return t;
}

Json toolsList() {
    Json tools = Json::array();
    {
        Json p = Json::object();
        tools.push(toolDef("greenroom_protocol",
                           "Returns the greenroom sub-agent briefing. Call it and paste the "
                           "text into every sub-agent prompt you spawn; replace {name} and "
                           "{room}.",
                           p, {}));
    }
    {
        Json p = Json::object();
        tools.push(toolDef("greenroom_status", "Server status: version, room count.", p, {}));
    }
    {
        Json p = Json::object();
        tools.push(toolDef("greenroom_rooms", "List all rooms.", p, {}));
    }
    {
        Json p = Json::object();
        p.set("room", propStr("Room name, [a-zA-Z0-9._-]"));
        tools.push(toolDef("greenroom_create_room", "Create a room for one task.", p, {"room"}));
    }
    {
        Json p = Json::object();
        p.set("room", propStr("Room name"));
        p.set("agent", propStr("Sending agent name"));
        p.set("type", propStr("say|plan|fact|ask|answer|done"));
        p.set("content", propStr("Message text"));
        p.set("ref", propNum("Referenced message id (answers), optional"));
        tools.push(toolDef("greenroom_say", "Post a message to a room.", p,
                           {"room", "agent", "type", "content"}));
    }
    {
        Json p = Json::object();
        p.set("room", propStr("Room name"));
        p.set("since", propNum("Only messages with id > since; 0 = all"));
        p.set("limit", propNum("Max messages to return"));
        tools.push(toolDef("greenroom_listen", "Read messages from a room.", p, {"room"}));
    }
    {
        Json p = Json::object();
        p.set("room", propStr("Room name"));
        p.set("since", propNum("Wait for messages with id > since; 0 = any message"));
        p.set("timeout_ms", propNum("Max wait, default 30000; server caps at 60000"));
        tools.push(toolDef("greenroom_wait",
                           "Long-poll: block until a new message arrives, then return it. "
                           "Cheaper than repeated listen calls.",
                           p, {"room"}));
    }
    {
        Json p = Json::object();
        p.set("query", propStr("Case-insensitive substring to find in content/agent/type"));
        p.set("room", propStr("Restrict to one room, optional"));
        p.set("limit", propNum("Max hits, default 50"));
        tools.push(toolDef("greenroom_search", "Search messages across all rooms.", p,
                           {"query"}));
    }
    {
        Json p = Json::object();
        p.set("room", propStr("Room name"));
        p.set("agent", propStr("Claiming agent name"));
        p.set("scope", propStrArr("Files or task labels to own"));
        p.set("ttl_s", propNum("Lease seconds, default 600"));
        tools.push(toolDef("greenroom_claim",
                           "Claim ownership of files/tasks. 409 = someone else owns it.",
                           p, {"room", "agent", "scope"}));
    }
    {
        Json p = Json::object();
        p.set("room", propStr("Room name"));
        p.set("agent", propStr("Releasing agent name"));
        p.set("claim_id", propNum("Claim id, optional"));
        p.set("scope", propStr("Scope entry, optional"));
        tools.push(toolDef("greenroom_release", "Release a claim.", p, {"room", "agent"}));
    }
    {
        Json p = Json::object();
        p.set("room", propStr("Room name"));
        tools.push(toolDef("greenroom_claims", "List active claims in a room.", p, {"room"}));
    }
    {
        Json p = Json::object();
        p.set("room", propStr("Room name"));
        p.set("key", propStr("Board key, e.g. decision/db-choice"));
        tools.push(toolDef("greenroom_board_get", "Read one blackboard entry.", p,
                           {"room", "key"}));
    }
    {
        Json p = Json::object();
        p.set("room", propStr("Room name"));
        p.set("key", propStr("Board key"));
        p.set("value", propStr("Value text (free-form, may be JSON)"));
        p.set("agent", propStr("Writing agent name"));
        tools.push(toolDef("greenroom_board_set", "Write one blackboard entry.", p,
                           {"room", "key", "value", "agent"}));
    }
    {
        Json p = Json::object();
        p.set("room", propStr("Room name"));
        tools.push(toolDef("greenroom_verify", "Verify the room's SHA-256 message chain.", p,
                           {"room"}));
    }
    {
        Json p2 = Json::object();
        p2.set("action", propStr("create|list|claim|submit|verify"));
        p2.set("room", propStr("Room name"));
        p2.set("id", propNum("Task id (claim/submit/verify)"));
        p2.set("title", propStr("Task title (create)"));
        p2.set("detail", propStr("Task detail (create, optional)"));
        p2.set("evidence", propStr("What proves it is done (submit)"));
        Json acc = Json::object();
        acc.set("type", Json::string("boolean"));
        acc.set("description", Json::string("verify: true=accept, false=reject+reopen"));
        p2.set("accept", std::move(acc));
        Json hum = Json::object();
        hum.set("type", Json::string("boolean"));
        hum.set("description",
                Json::string("create: mark as awaiting human sign-off — only agent 'human' "
                             "may verify it and generation turnover blocks until signed"));
        p2.set("human", std::move(hum));
        p2.set("agent", propStr("Acting agent name"));
        tools.push(toolDef("greenroom_task",
                           "Evidence-gated task board: create, claim, submit (with "
                           "evidence), verify (must be a different agent than the "
                           "submitter), list.",
                           p2, {"action", "room"}));
    }
    {
        Json p3 = Json::object();
        p3.set("action", propStr("status|goal-set|goal-achieve|goal-verify|goal-abandon|"
                                 "gen-advance|gen-chronicle|role-take|role-list"));
        p3.set("room", propStr("Room name"));
        p3.set("agent", propStr("Acting agent name"));
        p3.set("text", propStr("goal-set: the god goal text"));
        p3.set("criteria", propStr("goal-set: what 'achieved' means (optional)"));
        p3.set("oracle", propStr("goal-set: http:// URL of the achievement oracle — it must "
                                 "answer {\"satisfied\": bool}; credentials stay inside the "
                                 "oracle service, never in goal text (optional)"));
        p3.set("evidence", propStr("goal-achieve: what proves the goal is achieved"));
        p3.set("reason", propStr("goal-abandon: why"));
        p3.set("note", propStr("gen-advance: why this generation is being retired"));
        p3.set("chronicle", propStr("gen-chronicle: the recorder's distilled closing record "
                                     "for the active generation, max 4096 chars"));
        p3.set("role", propStr("role-take: commander|recorder|executor|reviewer|tester"));
        Json acc = Json::object();
        acc.set("type", Json::string("boolean"));
        acc.set("description", Json::string("goal-verify: true=accept, false=reject"));
        p3.set("accept", std::move(acc));
        tools.push(toolDef("greenroom_society",
                           "The society layer: a god goal the room exists to fulfill, "
                           "generations of agents (a new one is born automatically "
                           "whenever a generation drains with the goal still open), and "
                           "the five roles. While roles are registered only "
                           "reviewer/tester may verify. With an oracle declared, goal "
                           "verification requires it to answer satisfied. The recorder "
                           "closes each generation with a fixed-budget chronicle the "
                           "next generation starts from.",
                           p3, {"action", "room"}));
    }
    Json out = Json::object();
    out.set("tools", std::move(tools));
    return out;
}

// ---- HTTP proxy helpers ------------------------------------------------

std::string get(const Target& t, const std::string& target) {
    ClientResult r =
        httpClient(t.host, t.port, "GET", target, "", envOr("GREENROOM_TOKEN", ""));
    if (!r.ok && r.status == 0) return "{\"error\":\"transport: " + r.err + "\"}";
    return r.body;
}

std::string post(const Target& t, const std::string& target, const Json& body) {
    ClientResult r =
        httpClient(t.host, t.port, "POST", target, body.dump(), envOr("GREENROOM_TOKEN", ""));
    if (!r.ok && r.status == 0) return "{\"error\":\"transport: " + r.err + "\"}";
    return r.body;
}

std::string put(const Target& t, const std::string& target, const Json& body) {
    ClientResult r =
        httpClient(t.host, t.port, "PUT", target, body.dump(), envOr("GREENROOM_TOKEN", ""));
    if (!r.ok && r.status == 0) return "{\"error\":\"transport: " + r.err + "\"}";
    return r.body;
}

std::string argStr(const Json& args, const char* key, const std::string& def = "") {
    const Json* v = args.get(key);
    return (v && v->isStr()) ? v->str : def;
}

long long argNum(const Json& args, const char* key, long long def) {
    const Json* v = args.get(key);
    return (v && v->isNum()) ? static_cast<long long>(v->num) : def;
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

std::string callTool(const std::string& name, const Json& args) {
    Target t = parseTarget();

    if (name == "greenroom_protocol") return kProtocolBriefing;
    if (name == "greenroom_status") return get(t, "/v1/status");
    if (name == "greenroom_rooms") return get(t, "/v1/rooms");

    if (name == "greenroom_create_room") {
        Json body = Json::object();
        body.set("name", Json::string(argStr(args, "room")));
        return post(t, "/v1/rooms", body);
    }
    if (name == "greenroom_search") {
        std::string q = argStr(args, "query");
        if (q.empty()) return "{\"error\":\"query required\"}";
        std::string target = "/v1/search?q=" + urlEnc(q);
        std::string rf = argStr(args, "room");
        if (!rf.empty()) target += "&room=" + urlEnc(rf);
        long long lim = argNum(args, "limit", 0);
        if (lim > 0) target += "&limit=" + std::to_string(lim);
        return get(t, target);
    }
    std::string room = argStr(args, "room");
    if (room.empty()) return "{\"error\":\"room required\"}";
    std::string base = "/v1/rooms/" + urlEnc(room);

    if (name == "greenroom_say") {
        Json body = Json::object();
        body.set("agent", Json::string(argStr(args, "agent")));
        body.set("type", Json::string(argStr(args, "type", "say")));
        body.set("content", Json::string(argStr(args, "content")));
        long long ref = argNum(args, "ref", -1);
        if (ref >= 0) body.set("ref", Json::number(static_cast<double>(ref)));
        return post(t, base + "/say", body);
    }
    if (name == "greenroom_listen") {
        std::string q = "?since=" + std::to_string(argNum(args, "since", 0));
        if (argNum(args, "limit", 0) > 0)
            q += "&limit=" + std::to_string(argNum(args, "limit", 0));
        return get(t, base + "/messages" + q);
    }
    if (name == "greenroom_wait") {
        std::string q = "?since=" + std::to_string(argNum(args, "since", 0));
        long long to = argNum(args, "timeout_ms", 0);
        if (to > 0) q += "&timeout_ms=" + std::to_string(to);
        return get(t, base + "/wait" + q);
    }
    if (name == "greenroom_task") {
        std::string action = argStr(args, "action");
        std::string agent = argStr(args, "agent");
        long long id = argNum(args, "id", 0);
        if (action == "list") return get(t, base + "/tasks");
        if (action == "create") {
            Json body = Json::object();
            body.set("title", Json::string(argStr(args, "title")));
            body.set("detail", Json::string(argStr(args, "detail")));
            body.set("agent", Json::string(agent.empty() ? "anon" : agent));
            if (const Json* h = args.get("human"); h && h->isBool() && h->b)
                body.set("human", Json::boolean(true));
            return post(t, base + "/tasks", body);
        }
        if (id <= 0) return "{\"error\":\"id required for claim/submit/verify\"}";
        std::string sub = base + "/tasks/" + std::to_string(id);
        if (action == "claim") {
            Json body = Json::object();
            body.set("agent", Json::string(agent));
            return post(t, sub + "/claim", body);
        }
        if (action == "submit") {
            Json body = Json::object();
            body.set("agent", Json::string(agent));
            body.set("evidence", Json::string(argStr(args, "evidence")));
            return post(t, sub + "/submit", body);
        }
        if (action == "verify") {
            Json body = Json::object();
            body.set("agent", Json::string(agent));
            bool accept = true;
            if (const Json* a = args.get("accept"); a && a->isBool()) accept = a->b;
            body.set("accept", Json::boolean(accept));
            return post(t, sub + "/verify", body);
        }
        return "{\"error\":\"unknown action: " + action + "\"}";
    }
    if (name == "greenroom_society") {
        std::string action = argStr(args, "action");
        std::string agent = argStr(args, "agent");
        if (action == "status") return get(t, base + "/society");
        if (action == "goal-set") {
            Json body = Json::object();
            body.set("text", Json::string(argStr(args, "text")));
            body.set("criteria", Json::string(argStr(args, "criteria")));
            body.set("oracle", Json::string(argStr(args, "oracle")));
            body.set("agent", Json::string(agent.empty() ? "anon" : agent));
            return post(t, base + "/goal", body);
        }
        if (action == "goal-achieve") {
            Json body = Json::object();
            body.set("agent", Json::string(agent));
            body.set("evidence", Json::string(argStr(args, "evidence")));
            return post(t, base + "/goal/achieve", body);
        }
        if (action == "goal-verify") {
            Json body = Json::object();
            body.set("agent", Json::string(agent));
            bool accept = true;
            if (const Json* a = args.get("accept"); a && a->isBool()) accept = a->b;
            body.set("accept", Json::boolean(accept));
            return post(t, base + "/goal/verify", body);
        }
        if (action == "goal-abandon") {
            Json body = Json::object();
            body.set("agent", Json::string(agent));
            body.set("reason", Json::string(argStr(args, "reason")));
            return post(t, base + "/goal/abandon", body);
        }
        if (action == "gen-advance") {
            Json body = Json::object();
            body.set("agent", Json::string(agent));
            body.set("note", Json::string(argStr(args, "note")));
            return post(t, base + "/gen/advance", body);
        }
        if (action == "gen-chronicle") {
            Json body = Json::object();
            body.set("agent", Json::string(agent.empty() ? "anon" : agent));
            body.set("chronicle", Json::string(argStr(args, "chronicle")));
            return post(t, base + "/gen/chronicle", body);
        }
        if (action == "role-take") {
            Json body = Json::object();
            body.set("role", Json::string(argStr(args, "role")));
            body.set("agent", Json::string(agent.empty() ? "anon" : agent));
            return post(t, base + "/roles", body);
        }
        if (action == "role-list") return get(t, base + "/roles");
        return "{\"error\":\"unknown action: " + action + "\"}";
    }
    if (name == "greenroom_claim") {
        Json body = Json::object();
        body.set("agent", Json::string(argStr(args, "agent")));
        Json sc = Json::array();
        if (const Json* a = args.get("scope");
            a && a->isArr()) {
            for (const Json& s : a->arr)
                if (s.isStr()) sc.push(Json::string(s.str));
        }
        body.set("scope", std::move(sc));
        long long ttl = argNum(args, "ttl_s", 0);
        if (ttl > 0) body.set("ttl_s", Json::number(static_cast<double>(ttl)));
        return post(t, base + "/claim", body);
    }
    if (name == "greenroom_release") {
        Json body = Json::object();
        body.set("agent", Json::string(argStr(args, "agent")));
        long long cid = argNum(args, "claim_id", 0);
        std::string scope = argStr(args, "scope");
        if (cid > 0) body.set("claim_id", Json::number(static_cast<double>(cid)));
        if (!scope.empty()) body.set("scope", Json::string(scope));
        return post(t, base + "/release", body);
    }
    if (name == "greenroom_claims") return get(t, base + "/claims");
    if (name == "greenroom_board_get")
        return get(t, base + "/board/" + urlEnc(argStr(args, "key")));
    if (name == "greenroom_board_set") {
        Json body = Json::object();
        body.set("agent", Json::string(argStr(args, "agent")));
        body.set("value", Json::string(argStr(args, "value")));
        return put(t, base + "/board/" + urlEnc(argStr(args, "key")), body);
    }
    if (name == "greenroom_verify") return get(t, base + "/verify");
    return "{\"error\":\"unknown tool: " + name + "\"}";
}

// If serve is not up (e.g. after a machine reboot), start it detached so the
// MCP tools work without manual setup. Remote targets are never started.
void ensureServeRunning() {
    Target t = parseTarget();
    std::string token = envOr("GREENROOM_TOKEN", "");
    ClientResult r = httpClient(t.host, t.port, "GET", "/v1/status", "", token);
    if (r.status != 0) return;  // any HTTP answer (even 401) means it is up
    if (t.host != "127.0.0.1" && t.host != "localhost") return;
    std::vector<std::string> argv{selfExePath(), "serve", "--port", std::to_string(t.port),
                                  "--data", defaultDataDir()};
    if (!token.empty()) {
        argv.push_back("--token");
        argv.push_back(token);
    }
    std::string err;
    if (!spawnDetached(argv, err)) return;
    for (int i = 0; i < 50; i++) {  // up to 5 s
        ClientResult probe = httpClient(t.host, t.port, "GET", "/v1/status", "", token);
        if (probe.status != 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace

int runMcp() {
    ensureServeRunning();
    std::string line;
    while (std::getline(std::cin, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;
        Json req;
        if (!Json::parse(t, req) || !req.isObj()) continue;
        const Json* idJ = req.get("id");
        long long id = (idJ && idJ->isNum()) ? static_cast<long long>(idJ->num) : -1;
        const Json* methodJ = req.get("method");
        std::string method = (methodJ && methodJ->isStr()) ? methodJ->str : "";

        if (method.rfind("notifications/", 0) == 0) continue;  // no response
        if (method == "initialize") {
            Json r = Json::object();
            r.set("protocolVersion", Json::string("2024-11-05"));
            Json caps = Json::object();
            Json toolsCap = Json::object();
            caps.set("tools", std::move(toolsCap));
            r.set("capabilities", std::move(caps));
            Json info = Json::object();
            info.set("name", Json::string("greenroom"));
            info.set("version", Json::string(VERSION));
            r.set("serverInfo", std::move(info));
            std::cout << rpcResult(id, std::move(r)).dump() << "\n" << std::flush;
            continue;
        }
        if (method == "tools/list") {
            std::cout << rpcResult(id, toolsList()).dump() << "\n" << std::flush;
            continue;
        }
        if (method == "tools/call") {
            const Json* params = req.get("params");
            std::string name;
            Json args = Json::object();
            if (params && params->isObj()) {
                const Json* n = params->get("name");
                if (n && n->isStr()) name = n->str;
                const Json* a = params->get("arguments");
                if (a && a->isObj()) args = *a;
            }
            std::string out = callTool(name, args);
            // Surface HTTP-level errors as isError results, not RPC errors.
            Json body;
            bool isErr = false;
            if (Json::parse(out, body) && body.isObj() && body.get("error")) {
                isErr = true;
            }
            std::cout << rpcResult(id, isErr ? toolErr(out) : toolOk(out)).dump() << "\n"
                      << std::flush;
            continue;
        }
        if (method == "ping") {
            Json r = Json::object();
            std::cout << rpcResult(id, std::move(r)).dump() << "\n" << std::flush;
            continue;
        }
        if (id >= 0)
            std::cout << rpcError(id, -32601, "method not found: " + method).dump() << "\n"
                      << std::flush;
    }
    return 0;
}

}  // namespace gr
