// api.cpp — see api.h. Routes (all JSON):
//   GET  /v1/status
//   GET  /v1/rooms                        POST /v1/rooms {"name"}
//   GET  /v1/rooms/{r}/messages?since&limit&type&agent
//   POST /v1/rooms/{r}/say                {"agent","type","content","ref"?}
//   POST /v1/rooms/{r}/claim              {"agent","scope":[..],"ttl_s"?}
//   POST /v1/rooms/{r}/release           {"agent","claim_id"?,"scope"?}
//   GET  /v1/rooms/{r}/claims
//   GET  /v1/rooms/{r}/board              GET/PUT /v1/rooms/{r}/board/{key}
//   GET  /v1/rooms/{r}/verify
#include "api.h"

#include <stdexcept>

#include "jsjson.h"
#include "util.h"

namespace gr {

namespace {

HttpResponse json(int status, const Json& body) {
    HttpResponse r;
    r.status = status;
    r.body = body.dump();
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

HttpHandler makeApiRouter(RoomStore& store) {
    return [&store](const HttpRequest& req) -> HttpResponse {
        const std::string& p = req.path;

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

            return err(404, "unknown endpoint: " + p);
        }

        return err(404, "unknown endpoint: " + p);
    };
}

}  // namespace gr
