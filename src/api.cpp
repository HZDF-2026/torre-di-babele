// api.cpp — see api.h. Routes (all JSON):
//   GET  /v1/status
//   GET  /v1/rooms                        POST /v1/rooms {"name"}
//   GET  /v1/rooms/{r}/messages?since&limit&type&agent
//   GET  /v1/rooms/{r}/wait?since&timeout_ms      (long-poll, blocks)
//   POST /v1/rooms/{r}/say                {"agent","type","content","ref"?}
//   POST /v1/rooms/{r}/claim              {"agent","scope":[..],"ttl_s"?}
//   POST /v1/rooms/{r}/release           {"agent","claim_id"?,"scope"?}
//   GET  /v1/rooms/{r}/claims
//   GET  /v1/rooms/{r}/board              GET/PUT /v1/rooms/{r}/board/{key}
//   GET  /v1/rooms/{r}/tasks              POST /v1/rooms/{r}/tasks {"title",...}
//   POST /v1/rooms/{r}/tasks/{id}/claim|submit|verify
//   GET  /v1/rooms/{r}/society
//   POST /v1/rooms/{r}/goal                GET /v1/rooms/{r}/goal
//   POST /v1/rooms/{r}/goal/achieve|verify|abandon
//   GET  /v1/rooms/{r}/gen                 POST /v1/rooms/{r}/gen/advance
//   GET  /v1/rooms/{r}/roles               POST /v1/rooms/{r}/roles {"role","agent"}
//   GET  /v1/rooms/{r}/verify
//   GET  /v1/search?q&room&limit
//   GET  /                                (web UI shell, no auth)
#include "api.h"

#include <stdexcept>

#include "jsjson.h"
#include "util.h"
#include "webui.h"

namespace gr {

namespace {

HttpResponse json(int status, const Json& body) {
    HttpResponse r;
    r.status = status;
    r.body = body.dump();
    return r;
}

HttpResponse html(int status, const std::string& body) {
    HttpResponse r;
    r.status = status;
    r.body = body;
    r.contentType = "text/html; charset=utf-8";
    return r;
}

HttpResponse err(int status, const std::string& msg) {
    Json j = Json::object();
    j.set("error", Json::string(msg));
    return json(status, j);
}

bool parseBody(const HttpRequest& req, Json& out) {
    if (req.body.empty()) return false;
    return Json::parse(req.body, out) && out.isObj();
}

std::string strField(const Json& obj, const char* key, const std::string& def = "") {
    const Json* v = obj.get(key);
    return (v && v->isStr()) ? v->str : def;
}

long long numField(const Json& obj, const char* key, long long def) {
    const Json* v = obj.get(key);
    return (v && v->isNum()) ? static_cast<long long>(v->num) : def;
}

Json msgJson(const Message& m) {
    Json j = Json::object();
    j.set("id", Json::number(static_cast<double>(m.id)));
    j.set("ts", Json::number(static_cast<double>(m.ts)));
    j.set("agent", Json::string(m.agent));
    j.set("type", Json::string(m.type));
    j.set("content", Json::string(m.content));
    j.set("ref", m.ref >= 0 ? Json::number(static_cast<double>(m.ref)) : Json::null());
    j.set("prev", Json::string(m.prev));
    j.set("hash", Json::string(m.hash));
    return j;
}

Json claimJson(const Claim& c) {
    Json j = Json::object();
    j.set("id", Json::number(static_cast<double>(c.id)));
    j.set("ts", Json::number(static_cast<double>(c.ts)));
    j.set("expiresTs", Json::number(static_cast<double>(c.expiresTs)));
    j.set("agent", Json::string(c.agent));
    Json sc = Json::array();
    for (const std::string& s : c.scope) sc.push(Json::string(s));
    j.set("scope", std::move(sc));
    return j;
}

Json boardJson(const BoardEntry& e) {
    Json j = Json::object();
    j.set("key", Json::string(e.key));
    j.set("value", Json::string(e.value));
    j.set("agent", Json::string(e.agent));
    j.set("ts", Json::number(static_cast<double>(e.ts)));
    return j;
}

Json taskJson(const Task& t) {
    Json j = Json::object();
    j.set("id", Json::number(static_cast<double>(t.id)));
    j.set("title", Json::string(t.title));
    j.set("detail", Json::string(t.detail));
    j.set("status", Json::string(t.status));
    j.set("creator", Json::string(t.creator));
    j.set("assignee", Json::string(t.assignee));
    j.set("evidence", Json::string(t.evidence));
    j.set("verifier", Json::string(t.verifier));
    j.set("createdTs", Json::number(static_cast<double>(t.createdTs)));
    j.set("updatedTs", Json::number(static_cast<double>(t.updatedTs)));
    j.set("gen", Json::number(static_cast<double>(t.gen)));
    return j;
}

Json goalJson(const Goal& g) {
    Json j = Json::object();
    j.set("exists", Json::boolean(g.exists));
    j.set("text", Json::string(g.text));
    j.set("criteria", Json::string(g.criteria));
    j.set("status", Json::string(g.status));
    j.set("proposer", Json::string(g.proposer));
    j.set("achiever", Json::string(g.achiever));
    j.set("evidence", Json::string(g.evidence));
    j.set("verifier", Json::string(g.verifier));
    j.set("createdTs", Json::number(static_cast<double>(g.createdTs)));
    j.set("closedTs", Json::number(static_cast<double>(g.closedTs)));
    return j;
}

Json genJson(const Generation& g) {
    Json j = Json::object();
    j.set("n", Json::number(static_cast<double>(g.n)));
    j.set("status", Json::string(g.status));
    j.set("bornTs", Json::number(static_cast<double>(g.bornTs)));
    j.set("retiredTs", Json::number(static_cast<double>(g.retiredTs)));
    j.set("note", Json::string(g.note));
    return j;
}

Json roleJson(const RoleEntry& r) {
    Json j = Json::object();
    j.set("role", Json::string(r.role));
    j.set("agent", Json::string(r.agent));
    j.set("ts", Json::number(static_cast<double>(r.ts)));
    return j;
}

int activeGen(const Society& s) {
    if (s.gens.empty() || s.gens.back().status != "active") return 0;
    return s.gens.back().n;
}

// Splits "/v1/rooms/{room}/rest..." into room + rest (rest has no leading '/').
bool splitRoomPath(const std::string& path, std::string& room, std::string& rest) {
    const std::string prefix = "/v1/rooms/";
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0)
        return false;
    std::string tail = path.substr(prefix.size());
    size_t slash = tail.find('/');
    if (slash == std::string::npos) {
        room = tail;
        rest = "";
    } else {
        room = tail.substr(0, slash);
        rest = tail.substr(slash + 1);
    }
    return !room.empty();
}

}  // namespace

HttpHandler makeApiRouter(RoomStore& store, const std::string& token) {
    return [&store, token](const HttpRequest& req) -> HttpResponse {
        const std::string& p = req.path;

        // The web UI shell needs no auth: it carries no data, and it is how a
        // human enters the token in the first place.
        if ((p == "/" || p == "/index.html") && req.method == "GET")
            return html(200, kWebUiHtml);

        if (!token.empty()) {
            auto auth = req.headers.find("authorization");
            if (auth == req.headers.end() || auth->second != "Bearer " + token)
                return err(401, "unauthorized — pass Authorization: Bearer <token>");
        }

        if (p == "/v1/status" && req.method == "GET") {
            Json j = Json::object();
            j.set("name", Json::string("greenroom"));
            j.set("version", Json::string(VERSION));
            j.set("rooms", Json::number(static_cast<double>(store.rooms().size())));
            return json(200, j);
        }

        if (p == "/v1/rooms") {
            if (req.method == "GET") {
                Json arr = Json::array();
                for (const std::string& r : store.rooms()) arr.push(Json::string(r));
                Json j = Json::object();
                j.set("rooms", std::move(arr));
                return json(200, j);
            }
            if (req.method == "POST") {
                Json body;
                if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                std::string name = strField(body, "name");
                if (!validRoomName(name)) return err(400, "invalid room name");
                if (!store.createRoom(name)) return err(409, "room exists");
                Json j = Json::object();
                j.set("created", Json::string(name));
                return json(200, j);
            }
            return err(405, "method not allowed");
        }

        std::string room, rest;
        if (splitRoomPath(p, room, rest)) {
            if (!store.roomExists(room)) return err(404, "unknown room: " + room);

            if (rest == "messages" && req.method == "GET") {
                long long since = 0;
                int limit = 0;
                std::string tf, af;
                auto it = req.query.find("since");
                if (it != req.query.end()) {
                    try {
                        since = std::stoll(it->second);
                    } catch (...) {
                        since = 0;
                    }
                }
                it = req.query.find("limit");
                if (it != req.query.end()) {
                    try {
                        limit = std::stoi(it->second);
                    } catch (...) {
                        limit = 0;
                    }
                }
                it = req.query.find("type");
                if (it != req.query.end()) tf = it->second;
                it = req.query.find("agent");
                if (it != req.query.end()) af = it->second;
                Json arr = Json::array();
                for (const Message& m : store.messages(room, since, limit, tf, af))
                    arr.push(msgJson(m));
                Json j = Json::object();
                j.set("room", Json::string(room));
                j.set("messages", std::move(arr));
                return json(200, j);
            }

            if (rest == "wait" && req.method == "GET") {
                long long since = 0, timeoutMs = 30000;
                auto it = req.query.find("since");
                if (it != req.query.end()) {
                    try {
                        since = std::stoll(it->second);
                    } catch (...) {
                        since = 0;
                    }
                }
                it = req.query.find("timeout_ms");
                if (it != req.query.end()) {
                    try {
                        timeoutMs = std::stoll(it->second);
                    } catch (...) {
                        timeoutMs = 30000;
                    }
                }
                Json arr = Json::array();
                for (const Message& m : store.waitMessages(room, since, timeoutMs))
                    arr.push(msgJson(m));
                Json j = Json::object();
                j.set("room", Json::string(room));
                j.set("messages", std::move(arr));
                return json(200, j);
            }

            if (rest == "say" && req.method == "POST") {
                Json body;
                if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                std::string agent = strField(body, "agent");
                std::string type = strField(body, "type");
                std::string content = strField(body, "content");
                long long ref = numField(body, "ref", -1);
                try {
                    Message m = store.say(room, agent, type, content, ref);
                    return json(200, msgJson(m));
                } catch (const std::exception& e) {
                    return err(400, e.what());
                }
            }

            if (rest == "claim" && req.method == "POST") {
                Json body;
                if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                std::string agent = strField(body, "agent");
                long long ttl = numField(body, "ttl_s", 0);
                std::vector<std::string> scope;
                const Json* sc = body.get("scope");
                if (sc && sc->isArr()) {
                    for (const Json& s : sc->arr)
                        if (s.isStr()) scope.push_back(s.str);
                }
                try {
                    ClaimOutcome out = store.claim(room, agent, scope, ttl);
                    if (out.ok) {
                        Json j = Json::object();
                        j.set("ok", Json::boolean(true));
                        j.set("note", Json::string(out.note));
                        j.set("claim", claimJson(out.claim));
                        return json(200, j);
                    }
                    Json j = Json::object();
                    j.set("ok", Json::boolean(false));
                    j.set("error", Json::string("scope held by another agent"));
                    Json arr = Json::array();
                    for (const Claim& c : out.conflicts) arr.push(claimJson(c));
                    j.set("conflicts", std::move(arr));
                    return json(409, j);
                } catch (const std::exception& e) {
                    return err(400, e.what());
                }
            }

            if (rest == "release" && req.method == "POST") {
                Json body;
                if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                std::string agent = strField(body, "agent");
                long long claimId = numField(body, "claim_id", 0);
                std::string scope = strField(body, "scope");
                int n = store.release(room, agent, claimId, scope);
                Json j = Json::object();
                j.set("released", Json::number(static_cast<double>(n)));
                return json(200, j);
            }

            if (rest == "claims" && req.method == "GET") {
                Json arr = Json::array();
                for (const Claim& c : store.claims(room)) arr.push(claimJson(c));
                Json j = Json::object();
                j.set("room", Json::string(room));
                j.set("claims", std::move(arr));
                return json(200, j);
            }

            if (rest == "verify" && req.method == "GET") {
                std::string why;
                bool ok = store.verify(room, why);
                Json j = Json::object();
                j.set("ok", Json::boolean(ok));
                if (!ok) j.set("error", Json::string(why));
                return json(200, j);
            }

            if (rest == "board") {
                if (req.method == "GET") {
                    Json arr = Json::array();
                    for (const BoardEntry& e : store.boardAll(room)) arr.push(boardJson(e));
                    Json j = Json::object();
                    j.set("room", Json::string(room));
                    j.set("entries", std::move(arr));
                    return json(200, j);
                }
                return err(405, "method not allowed");
            }

            if (rest.compare(0, 6, "board/") == 0) {
                std::string key = rest.substr(6);
                if (!validBoardKey(key)) return err(400, "invalid board key");
                if (req.method == "GET") {
                    BoardEntry e = store.boardGet(room, key);
                    if (!e.exists) return err(404, "no such board key: " + key);
                    return json(200, boardJson(e));
                }
                if (req.method == "PUT") {
                    Json body;
                    if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                    std::string value = strField(body, "value");
                    std::string agent = strField(body, "agent");
                    if (!store.boardSet(room, key, value, agent))
                        return err(400, "board set failed");
                    BoardEntry e = store.boardGet(room, key);
                    return json(200, boardJson(e));
                }
                return err(405, "method not allowed");
            }

            if (rest == "tasks") {
                if (req.method == "GET") {
                    Json arr = Json::array();
                    for (const Task& t : store.tasks(room)) arr.push(taskJson(t));
                    Json j = Json::object();
                    j.set("room", Json::string(room));
                    j.set("tasks", std::move(arr));
                    return json(200, j);
                }
                if (req.method == "POST") {
                    Json body;
                    if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                    try {
                        Task t = store.taskCreate(room, strField(body, "title"),
                                                   strField(body, "detail"),
                                                   strField(body, "agent", "anon"));
                        return json(200, taskJson(t));
                    } catch (const std::exception& e) {
                        return err(400, e.what());
                    }
                }
                return err(405, "method not allowed");
            }

            if (rest.compare(0, 6, "tasks/") == 0) {
                std::string tail = rest.substr(6);
                size_t slash = tail.find('/');
                if (slash == std::string::npos) return err(404, "unknown endpoint: " + p);
                long long id = 0;
                try {
                    id = std::stoll(tail.substr(0, slash));
                } catch (...) {
                    return err(400, "bad task id");
                }
                std::string action = tail.substr(slash + 1);
                Json body;
                if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                try {
                    if (action == "claim" && req.method == "POST")
                        return json(200, taskJson(store.taskClaim(room, id, strField(body, "agent"))));
                    if (action == "submit" && req.method == "POST")
                        return json(200, taskJson(store.taskSubmit(room, id, strField(body, "agent"),
                                                                   strField(body, "evidence"))));
                    if (action == "verify" && req.method == "POST") {
                        const Json* a = body.get("accept");
                        bool accept = !(a && a->isBool()) || a->b;
                        return json(200, taskJson(store.taskVerify(room, id, strField(body, "agent"),
                                                                   accept)));
                    }
                } catch (const std::exception& e) {
                    return err(400, e.what());
                }
                return err(404, "unknown endpoint: " + p);
            }

            // ---- society layer ---------------------------------------------
            if (rest == "society" && req.method == "GET") {
                Society s = store.society(room);
                Json j = Json::object();
                j.set("room", Json::string(room));
                j.set("goal", goalJson(s.goal));
                j.set("generation", Json::number(static_cast<double>(activeGen(s))));
                Json gens = Json::array();
                for (const Generation& g : s.gens) gens.push(genJson(g));
                j.set("generations", std::move(gens));
                Json roles = Json::array();
                for (const RoleEntry& r : s.roles) roles.push(roleJson(r));
                j.set("roles", std::move(roles));
                return json(200, j);
            }

            if (rest == "goal") {
                if (req.method == "GET") {
                    Json j = Json::object();
                    j.set("room", Json::string(room));
                    j.set("goal", goalJson(store.society(room).goal));
                    return json(200, j);
                }
                if (req.method == "POST") {
                    Json body;
                    if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                    try {
                        store.goalSet(room, strField(body, "text"), strField(body, "criteria"),
                                     strField(body, "agent", "anon"));
                        Json j = Json::object();
                        j.set("room", Json::string(room));
                        j.set("goal", goalJson(store.society(room).goal));
                        return json(200, j);
                    } catch (const std::exception& e) {
                        std::string m = e.what();
                        return err(m.find("already active") != std::string::npos ? 409 : 400, m);
                    }
                }
                return err(405, "method not allowed");
            }

            if (rest == "goal/achieve" && req.method == "POST") {
                Json body;
                if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                try {
                    store.goalAchieve(room, strField(body, "agent"),
                                      strField(body, "evidence"));
                } catch (const std::exception& e) {
                    return err(400, e.what());
                }
                Json j = Json::object();
                j.set("room", Json::string(room));
                j.set("goal", goalJson(store.society(room).goal));
                return json(200, j);
            }

            if (rest == "goal/verify" && req.method == "POST") {
                Json body;
                if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                try {
                    const Json* a = body.get("accept");
                    bool accept = !(a && a->isBool()) || a->b;
                    store.goalVerify(room, strField(body, "agent"), accept);
                } catch (const std::exception& e) {
                    return err(400, e.what());
                }
                Json j = Json::object();
                j.set("room", Json::string(room));
                j.set("goal", goalJson(store.society(room).goal));
                return json(200, j);
            }

            if (rest == "goal/abandon" && req.method == "POST") {
                Json body;
                if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                try {
                    store.goalAbandon(room, strField(body, "agent"),
                                      strField(body, "reason"));
                } catch (const std::exception& e) {
                    return err(400, e.what());
                }
                Json j = Json::object();
                j.set("room", Json::string(room));
                j.set("goal", goalJson(store.society(room).goal));
                return json(200, j);
            }

            if (rest == "gen") {
                if (req.method != "GET") return err(405, "method not allowed");
                Society s = store.society(room);
                Json j = Json::object();
                j.set("room", Json::string(room));
                j.set("generation", Json::number(static_cast<double>(activeGen(s))));
                Json gens = Json::array();
                for (const Generation& g : s.gens) gens.push(genJson(g));
                j.set("generations", std::move(gens));
                return json(200, j);
            }

            if (rest == "gen/advance" && req.method == "POST") {
                Json body;
                if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                try {
                    store.genAdvance(room, strField(body, "agent"), strField(body, "note"));
                } catch (const std::exception& e) {
                    return err(400, e.what());
                }
                Society s = store.society(room);
                Json j = Json::object();
                j.set("room", Json::string(room));
                j.set("generation", Json::number(static_cast<double>(activeGen(s))));
                Json gens = Json::array();
                for (const Generation& g : s.gens) gens.push(genJson(g));
                j.set("generations", std::move(gens));
                return json(200, j);
            }

            if (rest == "roles") {
                if (req.method == "GET") {
                    Json arr = Json::array();
                    for (const RoleEntry& r : store.society(room).roles)
                        arr.push(roleJson(r));
                    Json j = Json::object();
                    j.set("room", Json::string(room));
                    j.set("roles", std::move(arr));
                    return json(200, j);
                }
                if (req.method == "POST") {
                    Json body;
                    if (!parseBody(req, body)) return err(400, "body must be a JSON object");
                    try {
                        store.roleTake(room, strField(body, "agent", "anon"),
                                       strField(body, "role"));
                    } catch (const std::exception& e) {
                        return err(400, e.what());
                    }
                    Json arr = Json::array();
                    for (const RoleEntry& r : store.society(room).roles)
                        arr.push(roleJson(r));
                    Json j = Json::object();
                    j.set("room", Json::string(room));
                    j.set("roles", std::move(arr));
                    return json(200, j);
                }
                return err(405, "method not allowed");
            }

            return err(404, "unknown endpoint: " + p);
        }

        if (p == "/v1/search" && req.method == "GET") {
            std::string q, roomF;
            int limit = 50;
            auto it = req.query.find("q");
            if (it != req.query.end()) q = it->second;
            it = req.query.find("room");
            if (it != req.query.end()) roomF = it->second;
            it = req.query.find("limit");
            if (it != req.query.end()) {
                try {
                    limit = std::stoi(it->second);
                } catch (...) {
                    limit = 50;
                }
            }
            if (q.empty()) return err(400, "missing q");
            Json arr = Json::array();
            for (const SearchHit& h : store.search(q, roomF, limit)) {
                Json j = msgJson(h.msg);
                j.set("room", Json::string(h.room));
                arr.push(std::move(j));
            }
            Json j = Json::object();
            j.set("query", Json::string(q));
            j.set("hits", std::move(arr));
            return json(200, j);
        }

        return err(404, "unknown endpoint: " + p);
    };
}

}  // namespace gr
