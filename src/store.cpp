// store.cpp — see store.h.
#include "store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>

#include "jsjson.h"
#include "sha256.h"
#include "util.h"

#include "http.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace gr {

namespace {

Message msgFromJson(const Json& j) {
    Message m;
    m.id = j.get("id") && j.get("id")->isNum() ? static_cast<long long>(j.get("id")->num) : 0;
    m.ts = j.get("ts") && j.get("ts")->isNum() ? static_cast<long long>(j.get("ts")->num) : 0;
    if (j.get("agent")) m.agent = j.get("agent")->str;
    if (j.get("type")) m.type = j.get("type")->str;
    if (j.get("content")) m.content = j.get("content")->str;
    m.ref = j.get("ref") && j.get("ref")->isNum() ? static_cast<long long>(j.get("ref")->num) : -1;
    if (j.get("prev")) m.prev = j.get("prev")->str;
    if (j.get("hash")) m.hash = j.get("hash")->str;
    return m;
}

Json msgToJson(const Message& m) {
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

Claim claimFromJson(const Json& j) {
    Claim c;
    c.id = j.get("id") && j.get("id")->isNum() ? static_cast<long long>(j.get("id")->num) : 0;
    c.ts = j.get("ts") && j.get("ts")->isNum() ? static_cast<long long>(j.get("ts")->num) : 0;
    c.expiresTs = j.get("expiresTs") && j.get("expiresTs")->isNum()
                      ? static_cast<long long>(j.get("expiresTs")->num) : 0;
    if (j.get("agent")) c.agent = j.get("agent")->str;
    if (j.get("scope") && j.get("scope")->isArr()) {
        for (const Json& s : j.get("scope")->arr) c.scope.push_back(s.str);
    }
    c.active = !(j.get("active") && j.get("active")->isBool()) || j.get("active")->b;
    return c;
}

Json claimToJson(const Claim& c) {
    Json j = Json::object();
    j.set("id", Json::number(static_cast<double>(c.id)));
    j.set("ts", Json::number(static_cast<double>(c.ts)));
    j.set("expiresTs", Json::number(static_cast<double>(c.expiresTs)));
    j.set("agent", Json::string(c.agent));
    Json sc = Json::array();
    for (const std::string& s : c.scope) sc.push(Json::string(s));
    j.set("scope", std::move(sc));
    j.set("active", Json::boolean(c.active));
    return j;
}

BoardEntry boardFromJson(const std::string& key, const Json& j) {
    BoardEntry e;
    e.key = key;
    if (j.get("value")) e.value = j.get("value")->str;
    if (j.get("agent")) e.agent = j.get("agent")->str;
    e.ts = j.get("ts") && j.get("ts")->isNum() ? static_cast<long long>(j.get("ts")->num) : 0;
    e.exists = true;
    return e;
}

bool scopeIntersects(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (const std::string& x : a)
        for (const std::string& y : b)
            if (x == y) return true;
    return false;
}

Task taskFromJson(const Json& j) {
    Task t;
    t.id = j.get("id") && j.get("id")->isNum() ? static_cast<long long>(j.get("id")->num) : 0;
    if (j.get("title")) t.title = j.get("title")->str;
    if (j.get("detail")) t.detail = j.get("detail")->str;
    if (j.get("status")) t.status = j.get("status")->str;
    if (j.get("creator")) t.creator = j.get("creator")->str;
    if (j.get("assignee")) t.assignee = j.get("assignee")->str;
    if (j.get("evidence")) t.evidence = j.get("evidence")->str;
    if (j.get("verifier")) t.verifier = j.get("verifier")->str;
    t.createdTs = j.get("createdTs") && j.get("createdTs")->isNum()
                      ? static_cast<long long>(j.get("createdTs")->num) : 0;
    t.updatedTs = j.get("updatedTs") && j.get("updatedTs")->isNum()
                      ? static_cast<long long>(j.get("updatedTs")->num) : 0;
    t.gen = j.get("gen") && j.get("gen")->isNum() ? static_cast<int>(j.get("gen")->num) : 0;
    t.human = j.get("human") && j.get("human")->isBool() && j.get("human")->b;
    return t;
}

Json taskToJson(const Task& t) {
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
    j.set("human", Json::boolean(t.human));
    return j;
}

std::string jsonStr(const Json& j, const char* key) {
    const Json* v = j.get(key);
    return (v && v->isStr()) ? v->str : "";
}

long long jsonNum(const Json& j, const char* key, long long def = 0) {
    const Json* v = j.get(key);
    return (v && v->isNum()) ? static_cast<long long>(v->num) : def;
}

Society societyFromJson(const Json& j) {
    Society s;
    if (const Json* g = j.get("goal"); g && g->isObj()) {
        s.goal.exists = true;
        s.goal.text = jsonStr(*g, "text");
        s.goal.criteria = jsonStr(*g, "criteria");
        s.goal.status = jsonStr(*g, "status");
        s.goal.proposer = jsonStr(*g, "proposer");
        s.goal.achiever = jsonStr(*g, "achiever");
        s.goal.evidence = jsonStr(*g, "evidence");
        s.goal.verifier = jsonStr(*g, "verifier");
        s.goal.oracle = jsonStr(*g, "oracle");
        s.goal.oracleRead = jsonStr(*g, "oracleRead");
        s.goal.oracleReadTs = jsonNum(*g, "oracleReadTs");
        s.goal.createdTs = jsonNum(*g, "createdTs");
        s.goal.closedTs = jsonNum(*g, "closedTs");
    }
    if (const Json* a = j.get("generations"); a && a->isArr()) {
        for (const Json& gj : a->arr) {
            Generation g;
            g.n = static_cast<int>(jsonNum(gj, "n"));
            g.status = jsonStr(gj, "status");
            g.bornTs = jsonNum(gj, "bornTs");
            g.retiredTs = jsonNum(gj, "retiredTs");
            g.note = jsonStr(gj, "note");
            g.chronicle = jsonStr(gj, "chronicle");
            g.chronicler = jsonStr(gj, "chronicler");
            g.chronicleTs = jsonNum(gj, "chronicleTs");
            s.gens.push_back(g);
        }
    }
    if (const Json* a = j.get("roles"); a && a->isArr()) {
        for (const Json& rj : a->arr) {
            RoleEntry r;
            r.role = jsonStr(rj, "role");
            r.agent = jsonStr(rj, "agent");
            r.ts = jsonNum(rj, "ts");
            s.roles.push_back(r);
        }
    }
    if (const Json* a = j.get("posts"); a && a->isArr()) {
        for (const Json& pj : a->arr) {
            PostDef p;
            p.name = jsonStr(pj, "name");
            if (const Json* b = pj.get("canVerifyTask"); b && b->isBool())
                p.canVerifyTask = b->b;
            if (const Json* b = pj.get("canVerifyGoal"); b && b->isBool())
                p.canVerifyGoal = b->b;
            p.model = jsonStr(pj, "model");
            p.createdBy = jsonStr(pj, "createdBy");
            p.createdTs = jsonNum(pj, "createdTs");
            s.posts.push_back(p);
        }
    }
    return s;
}

Json goalToJson(const Goal& g) {
    Json j = Json::object();
    j.set("exists", Json::boolean(g.exists));
    j.set("text", Json::string(g.text));
    j.set("criteria", Json::string(g.criteria));
    j.set("status", Json::string(g.status));
    j.set("proposer", Json::string(g.proposer));
    j.set("achiever", Json::string(g.achiever));
    j.set("evidence", Json::string(g.evidence));
    j.set("verifier", Json::string(g.verifier));
    j.set("oracle", Json::string(g.oracle));
    j.set("oracleRead", Json::string(g.oracleRead));
    j.set("oracleReadTs", Json::number(static_cast<double>(g.oracleReadTs)));
    j.set("createdTs", Json::number(static_cast<double>(g.createdTs)));
    j.set("closedTs", Json::number(static_cast<double>(g.closedTs)));
    return j;
}

Json societyToJson(const Society& s) {
    Json obj = Json::object();
    if (s.goal.exists) obj.set("goal", goalToJson(s.goal));
    Json gens = Json::array();
    for (const Generation& g : s.gens) {
        Json gj = Json::object();
        gj.set("n", Json::number(static_cast<double>(g.n)));
        gj.set("status", Json::string(g.status));
        gj.set("bornTs", Json::number(static_cast<double>(g.bornTs)));
        gj.set("retiredTs", Json::number(static_cast<double>(g.retiredTs)));
        gj.set("note", Json::string(g.note));
        gj.set("chronicle", Json::string(g.chronicle));
        gj.set("chronicler", Json::string(g.chronicler));
        gj.set("chronicleTs", Json::number(static_cast<double>(g.chronicleTs)));
        gens.push(std::move(gj));
    }
    obj.set("generations", std::move(gens));
    Json roles = Json::array();
    for (const RoleEntry& r : s.roles) {
        Json rj = Json::object();
        rj.set("role", Json::string(r.role));
        rj.set("agent", Json::string(r.agent));
        rj.set("ts", Json::number(static_cast<double>(r.ts)));
        roles.push(std::move(rj));
    }
    obj.set("roles", std::move(roles));
    Json posts = Json::array();
    for (const PostDef& p : s.posts) {
        Json pj = Json::object();
        pj.set("name", Json::string(p.name));
        pj.set("canVerifyTask", Json::boolean(p.canVerifyTask));
        pj.set("canVerifyGoal", Json::boolean(p.canVerifyGoal));
        pj.set("model", Json::string(p.model));
        pj.set("createdBy", Json::string(p.createdBy));
        pj.set("createdTs", Json::number(static_cast<double>(p.createdTs)));
        posts.push(std::move(pj));
    }
    obj.set("posts", std::move(posts));
    return obj;
}

// The five preset posts. reviewer/tester carry both verify bits — the classic
// division-of-labor gate. Custom posts (defined per room) extend this table.
struct PostBits {
    const char* name;
    bool vt;
    bool vg;
};
const PostBits kPresets[] = {
    {"commander", false, false}, {"recorder", false, false},
    {"executor", false, false},   {"reviewer", true, true},
    {"tester", true, true},
};

bool isPresetPost(const std::string& name) {
    for (const PostBits& p : kPresets)
        if (name == p.name) return true;
    return false;
}

bool validPostName(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '-' && c != '_' && c != '.')
            return false;
    return true;
}

RoomMeta roomMetaFromJson(const Json& j) {
    RoomMeta m;
    if (const Json* b = j.get("chamber"); b && b->isBool()) m.chamber = b->b;
    m.creator = jsonStr(j, "creator");
    if (const Json* a = j.get("members"); a && a->isArr()) {
        for (const Json& mj : a->arr)
            if (mj.isStr()) m.members.push_back(mj.str);
    }
    m.createdTs = jsonNum(j, "createdTs");
    return m;
}

Json roomMetaToJson(const RoomMeta& m) {
    Json j = Json::object();
    j.set("chamber", Json::boolean(m.chamber));
    j.set("creator", Json::string(m.creator));
    Json arr = Json::array();
    for (const std::string& s : m.members) arr.push(Json::string(s));
    j.set("members", std::move(arr));
    j.set("createdTs", Json::number(static_cast<double>(m.createdTs)));
    return j;
}

std::string asciiLower(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string joinScope(const std::vector<std::string>& scope) {
    std::string out;
    for (size_t i = 0; i < scope.size(); i++) {
        if (i) out += ", ";
        out += scope[i];
    }
    return out;
}

bool isAsciiAlnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Cheap credential heuristics. Goals are public, hash-chained forever and
// quoted into every generation's genesis briefing, so obvious credentials
// are refused at the door.
bool looksLikeCredential(const std::string& s) {
    std::string low = asciiLower(s);
    static const char* pairs[] = {"password:", "password=", "passwd=", "pwd=",
                                  "token=", "secret=", "apikey=", "api_key=",
                                  nullptr};
    for (int i = 0; pairs[i]; i++)
        if (low.find(pairs[i]) != std::string::npos) return true;
    // "密码"/"口令" followed by an ASCII alnum (possibly after :：= or spaces)
    static const char* cn[] = {"密码", "口令", nullptr};
    for (int i = 0; cn[i]; i++) {
        size_t pos = s.find(cn[i]);
        while (pos != std::string::npos) {
            size_t j = pos + std::strlen(cn[i]);
            while (j < s.size() && (s[j] == ' ' || s[j] == ':' || s[j] == '=' ||
                                    static_cast<unsigned char>(s[j]) == 0xA3 /*：*/))
                j++;
            if (j < s.size() && isAsciiAlnum(s[j])) return true;
            pos = s.find(cn[i], pos + 1);
        }
    }
    // Bank-card-shaped run of 16–19 consecutive digits.
    int run = 0;
    for (char c : s) {
        run = (c >= '0' && c <= '9') ? run + 1 : 0;
        if (run >= 16 && run <= 19) return true;
        if (run > 19) return false;
    }
    return false;
}

}  // namespace

OracleReading readOracle(const std::string& url) {
    OracleReading out;
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0) {
        out.err = "oracle must be an http:// URL";
        return out;
    }
    std::string rest = url.substr(prefix.size());
    if (rest.size() > 512) {
        out.err = "oracle URL too long (max 512)";
        return out;
    }
    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string target = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (hostport.empty()) {
        out.err = "oracle URL has no host";
        return out;
    }
    std::string host = hostport;
    int port = 80;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        try {
            port = std::stoi(hostport.substr(colon + 1));
        } catch (...) {
            out.err = "oracle URL has a bad port";
            return out;
        }
    }
    if (host.empty() || port <= 0 || port > 65535) {
        out.err = "oracle URL has a bad host/port";
        return out;
    }
    ClientResult r = httpClient(host, port, "GET", target, "", "", 5000);
    if (!r.ok) {
        out.err = "oracle fetch failed: " + (r.err.empty() ? r.body : r.err);
        return out;
    }
    out.raw = r.body.size() > 2048 ? r.body.substr(0, 2048) : r.body;
    Json j;
    if (!Json::parse(r.body, j) || !j.isObj()) {
        out.err = "oracle response is not a JSON object";
        return out;
    }
    const Json* sat = j.get("satisfied");
    if (!sat || !sat->isBool()) {
        out.err = "oracle response lacks a boolean 'satisfied' field";
        return out;
    }
    out.fetched = true;
    out.satisfied = sat->b;
    return out;
}

std::string messageHash(const std::string& prev, const std::string& room,
                        const Message& m) {
    std::string ref = m.ref >= 0 ? std::to_string(m.ref) : "-";
    std::string buf = prev + "\n" + room + "\n" + std::to_string(m.id) + "\n" +
                      std::to_string(m.ts) + "\n" + m.agent + "\n" + m.type + "\n" +
                      m.content + "\n" + ref + "\n";
    return sha256Hex(buf);
}

RoomStore::RoomStore(std::string dataDir) : dataDir_(std::move(dataDir)) {
    makeDirs(pathJoin(dataDir_, "rooms"));
}

std::string RoomStore::roomDir(const std::string& room) const {
    return pathJoin(pathJoin(dataDir_, "rooms"), room);
}

RoomStore::RoomData& RoomStore::load(const std::string& room) {
    if (!roomExists(room)) throw std::runtime_error("unknown room: " + room);
    // Caller holds mutex; rooms stay cached for the process lifetime.
    auto it = cache_.find(room);
    if (it != cache_.end()) return it->second;

    RoomData rd;
    std::string mf = pathJoin(roomDir(room), "messages.jsonl");
    if (fileExists(mf)) {
        std::string raw = readBytes(mf);
        for (const std::string& line : split(raw, '\n')) {
            std::string t = trim(line);
            if (t.empty()) continue;
            Json j;
            if (!Json::parse(t, j)) throw std::runtime_error("corrupt message line in " + room);
            Message m = msgFromJson(j);
            if (m.id >= 1) rd.msgs.push_back(std::move(m));
        }
    }
    std::string cf = pathJoin(roomDir(room), "claims.json");
    if (fileExists(cf)) {
        Json j;
        if (Json::parse(readBytes(cf), j) && j.isArr()) {
            for (const Json& c : j.arr) rd.claims.push_back(claimFromJson(c));
        }
    }
    std::string bf = pathJoin(roomDir(room), "board.json");
    if (fileExists(bf)) {
        Json j;
        if (Json::parse(readBytes(bf), j) && j.isObj()) {
            for (const auto& kv : j.obj) {
                if (kv.second.isObj()) rd.board.push_back(boardFromJson(kv.first, kv.second));
            }
        }
    }
    std::string tf = pathJoin(roomDir(room), "tasks.json");
    if (fileExists(tf)) {
        Json j;
        if (Json::parse(readBytes(tf), j) && j.isArr()) {
            for (const Json& t : j.arr) rd.tasks.push_back(taskFromJson(t));
        }
    }
    std::string sf = pathJoin(roomDir(room), "society.json");
    if (fileExists(sf)) {
        Json j;
        if (Json::parse(readBytes(sf), j) && j.isObj()) rd.soc = societyFromJson(j);
    }
    std::string rf = pathJoin(roomDir(room), "room.json");
    if (fileExists(rf)) {
        Json j;
        if (Json::parse(readBytes(rf), j) && j.isObj()) rd.meta = roomMetaFromJson(j);
    }
    long long maxClaimId = 0;
    for (const Claim& c : rd.claims) maxClaimId = std::max(maxClaimId, c.id);
    rd.nextClaimId = maxClaimId + 1;
    long long maxTaskId = 0;
    for (const Task& t : rd.tasks) maxTaskId = std::max(maxTaskId, t.id);
    rd.nextTaskId = maxTaskId + 1;
    return cache_.emplace(room, std::move(rd)).first->second;
}

bool RoomStore::roomExists(const std::string& room) {
    if (!validRoomName(room)) return false;
    struct stat st;
    return ::stat(roomDir(room).c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

bool RoomStore::createRoom(const std::string& room, bool chamber,
                           const std::string& creator) {
    // Validate before any side effect: a throw must not leave a half-built
    // room directory behind.
    if (chamber && creator.empty())
        throw std::runtime_error("a chamber needs a registered creator — send identity "
                                 "headers (X-GR-Agent/X-GR-Key)");
    std::lock_guard<std::mutex> lock(mu_);
    if (!validRoomName(room) || roomExists(room)) return false;
    if (!makeDirs(roomDir(room))) return false;
    load(room);  // initializes empty cache entry
    RoomData& rd = load(room);
    rd.meta.chamber = chamber;
    rd.meta.creator = creator;
    rd.meta.createdTs = nowMs();
    if (chamber) rd.meta.members.push_back(creator);  // creator is the first member
    persistMeta(room, rd);
    sayLocked(room, "server", "say",
              chamber ? "room created (chamber — members only, actions are identity-bound)"
                      : "room created",
              -1);
    return true;
}

RoomMeta RoomStore::roomMeta(const std::string& room) {
    std::lock_guard<std::mutex> lock(mu_);
    return load(room).meta;
}

void RoomStore::persistMeta(const std::string& room, RoomData& rd) {
    writeBytes(pathJoin(roomDir(room), "room.json"), roomMetaToJson(rd.meta).dump() + "\n");
}

void RoomStore::memberAdd(const std::string& room, const std::string& caller,
                          const std::string& agent) {
    std::lock_guard<std::mutex> lock(mu_);
    if (caller.empty() || caller.size() > 64) throw std::runtime_error("bad caller name");
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    loadAgents();
    bool known = false;
    for (const AgentEntry& a : agents_)
        if (a.name == agent) known = true;
    if (!known)
        throw std::runtime_error("unknown agent: " + agent +
                                 " — members must be registered agents");
    RoomData& rd = load(room);
    if (!rd.meta.chamber)
        throw std::runtime_error("not a chamber room — membership only guards chambers");
    bool callerIsMember = false;
    for (const std::string& m : rd.meta.members)
        if (m == caller) callerIsMember = true;
    if (!callerIsMember)
        throw std::runtime_error("only chamber members may add members");
    for (const std::string& m : rd.meta.members)
        if (m == agent) return;  // idempotent
    rd.meta.members.push_back(agent);
    std::sort(rd.meta.members.begin(), rd.meta.members.end());
    persistMeta(room, rd);
    sayLocked(room, "server", "role", caller + " added " + agent + " to the chamber", -1);
}

bool RoomStore::canView(const std::string& room, const std::string& viewer) {
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    if (!rd.meta.chamber) return true;
    for (const std::string& m : rd.meta.members)
        if (m == viewer) return true;
    return false;
}

std::vector<std::string> RoomStore::visibleRooms(const std::string& viewer) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    for (const std::string& room : rooms()) {
        RoomData& rd = load(room);
        if (!rd.meta.chamber) {
            out.push_back(room);
            continue;
        }
        for (const std::string& m : rd.meta.members)
            if (m == viewer) {
                out.push_back(room);
                break;
            }
    }
    return out;
}

// ---- agent identity registry ----------------------------------------------

void RoomStore::loadAgents() {
    if (agentsLoaded_) return;
    agentsLoaded_ = true;
    std::string f = pathJoin(dataDir_, "agents.json");
    if (!fileExists(f)) return;
    Json j;
    if (!Json::parse(readBytes(f), j) || !j.isArr()) return;
    for (const Json& aj : j.arr) {
        AgentEntry a;
        a.name = jsonStr(aj, "name");
        a.keyHash = jsonStr(aj, "keyHash");
        a.createdTs = jsonNum(aj, "createdTs");
        if (!a.name.empty() && !a.keyHash.empty()) agents_.push_back(a);
    }
}

void RoomStore::persistAgents() {
    Json arr = Json::array();
    for (const AgentEntry& a : agents_) {
        Json j = Json::object();
        j.set("name", Json::string(a.name));
        j.set("keyHash", Json::string(a.keyHash));
        j.set("createdTs", Json::number(static_cast<double>(a.createdTs)));
        arr.push(std::move(j));
    }
    writeBytes(pathJoin(dataDir_, "agents.json"), arr.dump() + "\n");
}

void RoomStore::agentRegister(const std::string& name, const std::string& key) {
    if (name.empty() || name.size() > 64) throw std::runtime_error("bad agent name");
    if (key.size() < 8 || key.size() > 128)
        throw std::runtime_error("bad key (8..128 chars) — keys are hashed with SHA-256, "
                                 "never stored");
    std::lock_guard<std::mutex> lock(mu_);
    loadAgents();
    for (const AgentEntry& a : agents_)
        if (a.name == name)
            throw std::runtime_error("agent name already registered: " + name);
    AgentEntry a;
    a.name = name;
    a.keyHash = sha256Hex(key);
    a.createdTs = nowMs();
    agents_.push_back(a);
    std::sort(agents_.begin(), agents_.end(),
              [](const AgentEntry& x, const AgentEntry& y) { return x.name < y.name; });
    persistAgents();
}

std::vector<std::string> RoomStore::agentList() {
    std::lock_guard<std::mutex> lock(mu_);
    loadAgents();
    std::vector<std::string> out;
    for (const AgentEntry& a : agents_) out.push_back(a.name);
    return out;
}

bool RoomStore::agentCheck(const std::string& name, const std::string& key) {
    if (name.empty() || key.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    loadAgents();
    for (const AgentEntry& a : agents_)
        if (a.name == name) return a.keyHash == sha256Hex(key);
    return false;
}

// ---- population -------------------------------------------------------------

Population RoomStore::population(const std::string& room) {
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    sweepExpired(room, rd);
    long long cutoff = nowMs() - 1800000;
    std::set<std::string> act;
    for (const Claim& c : rd.claims)
        if (c.active && c.agent != "server") act.insert(c.agent);
    for (const Message& m : rd.msgs)
        if (m.ts >= cutoff && m.agent != "server") act.insert(m.agent);
    for (const Task& t : rd.tasks)
        if ((t.status == "claimed" || t.status == "submitted") && !t.assignee.empty())
            act.insert(t.assignee);
    Population p;
    p.active.assign(act.begin(), act.end());
    return p;
}

// ---- reports -----------------------------------------------------------------

namespace {

// Plain-data snapshot the report renderers work on — no RoomData dependency,
// built once under the store lock, then rendered lock-free.
struct ReportSnap {
    std::string room;
    bool chamber = false;
    bool hasGoal = false;
    std::string goalText, goalStatus, goalCriteria;
    int genCount = 0;
    int activeGen = 0;
    struct Phase {
        int n = 0;
        bool active = false;
        std::string chronicle;
        int done = 0;
        int total = 0;
    };
    std::vector<Phase> phases;
    int tDone = 0, tInFlight = 0, tOpen = 0, tHuman = 0;
    std::vector<std::string> openTasks, doneTasks, inFlightTasks, humanTasks;
    std::map<std::string, int> byAgentDone, byAgentInFlight;
    std::map<std::string, std::string> agentPost;
    int facts = 0;
    std::vector<std::string> recentFacts;      // newest-first, max 3
    std::vector<std::string> openAsks;         // "id|agent|content"
    int answeredAsks = 0;
    std::vector<std::string> activeAgents;     // sorted
    int activeClaims = 0;
};

// Cap by code points, never cutting a UTF-8 sequence mid-way.
std::string utf8Truncate(const std::string& s, size_t maxCp) {
    size_t cp = 0, i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (cp >= maxCp) break;
        ++cp;
        i = (i + len > s.size()) ? s.size() : i + len;
    }
    return s.substr(0, i);
}

std::string firstLine(const std::string& text, size_t maxCp) {
    for (const std::string& line : split(text, '\n')) {
        std::string t = trim(line);
        if (!t.empty()) return utf8Truncate(t, maxCp);
    }
    return "";
}

// HZDF-2026 distillation shape: phase history (one line per generation),
// laws (inside the latest chronicle, verbatim), open questions.
std::string renderHzdf(const ReportSnap& s) {
    std::ostringstream o;
    o << "== HZDF-2026 report · room " << s.room
      << (s.chamber ? " (chamber)" : "") << " ==\n";
    if (s.hasGoal) {
        o << "goal [" << s.goalStatus << "]: " << utf8Truncate(s.goalText, 160) << "\n"
          << "criteria: " << (s.goalCriteria.empty() ? "-" : utf8Truncate(s.goalCriteria, 160))
          << "\n";
    } else {
        o << "no goal — a flat collaboration room\n";
    }
    o << "generations: " << s.genCount
      << (s.activeGen > 0 ? " (G" + std::to_string(s.activeGen) + " active)" : "")
      << " · tasks: " << s.tDone << " done / " << s.tInFlight << " in-flight / "
      << s.tOpen << " open · facts: " << s.facts
      << " · unanswered asks: " << s.openAsks.size()
      << " · active agents: " << s.activeAgents.size() << "\n";

    o << "== phase history ==\n";
    if (s.phases.empty()) o << "(no generations yet)\n";
    for (const ReportSnap::Phase& p : s.phases) {
        o << "G" << p.n << (p.active ? " active  " : " retired  ") << "tasks "
          << p.done << "/" << p.total << "  "
          << (p.chronicle.empty() ? "(no chronicle)"
                                  : firstLine(p.chronicle, 100))
          << "\n";
    }

    o << "== latest chronicle (verbatim — laws inside) ==\n";
    const std::string* latest = nullptr;
    for (const ReportSnap::Phase& p : s.phases)
        if (!p.chronicle.empty()) latest = &p.chronicle;
    if (latest) o << *latest << "\n";
    else o << "(none yet — the recorder distills before generation turnover)\n";

    o << "== open questions ==\n";
    int lines = 0;
    if (s.hasGoal && (s.goalStatus == "open" || s.goalStatus == "proposed")) {
        o << "- the goal remains " << s.goalStatus << "\n";
        ++lines;
    }
    for (const std::string& t : s.openTasks) {
        if (lines >= 10) { o << "… and " << (s.openTasks.size() + s.openAsks.size() - lines)
                             << " more\n"; break; }
        o << "- open task " << t << "\n";
        ++lines;
    }
    for (const std::string& a : s.openAsks) {
        if (lines >= 10) break;
        o << "- " << a << "\n";
        ++lines;
    }
    if (lines == 0) o << "(none)\n";
    return o.str();
}

// Corporate briefing: TL;DR, KPIs, per-post rollup, risks, next steps.
std::string renderCompany(const ReportSnap& s) {
    std::ostringstream o;
    o << "== company briefing · " << s.room << (s.chamber ? " (chamber)" : "")
      << " ==\n";
    o << "TL;DR: ";
    if (s.hasGoal)
        o << "goal [" << s.goalStatus << "] — " << utf8Truncate(s.goalText, 120)
          << ". ";
    else
        o << "no goal — flat collaboration. ";
    o << s.genCount << " generation(s) in; board " << s.tDone << " done / "
      << s.tInFlight << " in-flight / " << s.tOpen << " open.\n";
    o << "KPIs: facts " << s.facts << " · asks " << s.answeredAsks
      << " answered / " << s.openAsks.size() << " open · active agents "
      << s.activeAgents.size() << " · active claims " << s.activeClaims;
    if (s.tHuman > 0) o << " · human sign-off pending " << s.tHuman;
    o << "\n";

    o << "by post:\n";
    std::set<std::string> staff(s.activeAgents.begin(), s.activeAgents.end());
    for (const auto& kv : s.byAgentDone) staff.insert(kv.first);
    for (const auto& kv : s.byAgentInFlight) staff.insert(kv.first);
    if (staff.empty()) {
        o << "  (nobody on the board yet)\n";
    } else {
        for (const std::string& a : staff) {
            std::string post = "(no post)";
            auto it = s.agentPost.find(a);
            if (it != s.agentPost.end()) post = it->second;
            int done = 0, inf = 0;
            if (s.byAgentDone.count(a)) done = s.byAgentDone.at(a);
            if (s.byAgentInFlight.count(a)) inf = s.byAgentInFlight.at(a);
            o << "  " << a << " — " << post << ": " << done << " done, " << inf
              << " in-flight\n";
        }
    }

    o << "risks & blockers:\n";
    int rl = 0;
    for (const std::string& t : s.humanTasks) { o << "  - human sign-off pending: " << t << "\n"; ++rl; }
    for (const std::string& a : s.openAsks) {
        if (rl >= 10) break;
        o << "  - " << a << "\n";
        ++rl;
    }
    if (rl == 0) o << "  (none)\n";

    o << "next steps:\n";
    if (s.openTasks.empty()) {
        o << "  (board is clear"
          << (s.hasGoal && s.goalStatus == "open"
                  ? " — propose achievement or retire the generation"
                  : "")
          << ")\n";
    } else {
        int nl = 0;
        for (const std::string& t : s.openTasks) {
            if (nl >= 10) { o << "  … and " << (s.openTasks.size() - nl) << " more\n"; break; }
            o << "  - " << t << "\n";
            ++nl;
        }
    }
    return o.str();
}

// Court memorial: one memorial covering 国祚/朝代/军情/贡赋/民生/请旨/臣工.
std::string renderFeudal(const ReportSnap& s) {
    std::ostringstream o;
    o << "奏为恭报 " << s.room << (s.chamber ? "（密室）" : "")
      << " 一域军政民情折";
    if (s.activeGen > 0) o << "（第" << s.activeGen << "朝）";
    o << "\n";

    o << "一、国祚。";
    if (!s.hasGoal) {
        o << "未立国祚，散装共事之域。\n";
    } else {
        std::string st = s.goalStatus == "open"        ? "犹悬"
                         : s.goalStatus == "proposed"  ? "已奏请验功，待核"
                         : s.goalStatus == "achieved"  ? "大功告成"
                                                      : "业已罢黜";
        o << "国是「" << utf8Truncate(s.goalText, 80) << "」今" << st << "。\n";
    }

    o << "一、朝代。历" << s.genCount << "朝";
    if (s.activeGen > 0) o << "，今第" << s.activeGen << "朝当值";
    o << "；臣工" << s.activeAgents.size() << "员在值。\n";

    int total = s.tDone + s.tInFlight + s.tOpen;
    o << "一、军情（任务战况）。计" << total << "件：已克" << s.tDone
      << "，交战" << s.tInFlight << "，未动" << s.tOpen << "。\n";
    if (total == 0) {
        o << "  （无战事）\n";
    } else {
        int ml = 0;
        for (const std::string& t : s.inFlightTasks) {
            if (ml >= 10) { o << "  ……余" << (total - ml) << "件从略。\n"; break; }
            o << "  " << t << "\n";
            ++ml;
        }
        for (const std::string& t : s.openTasks) {
            if (ml >= 10) { o << "  ……余" << (total - ml) << "件从略。\n"; break; }
            o << "  " << t << "\n";
            ++ml;
        }
    }

    o << "一、贡赋（验讫之功）。\n";
    if (s.doneTasks.empty()) {
        o << "  （暂无贡赋）\n";
    } else {
        for (size_t i = 0; i < s.doneTasks.size() && i < 10; ++i)
            o << "  " << s.doneTasks[i] << "\n";
        if (s.doneTasks.size() > 10)
            o << "  ……余" << (s.doneTasks.size() - 10) << "件从略。\n";
    }

    o << "一、民生（事实计" << s.facts << "条，近3条）。\n";
    if (s.recentFacts.empty()) {
        o << "  （民生无录）\n";
    } else {
        for (const std::string& f : s.recentFacts) o << "  " << f << "\n";
    }

    o << "一、请旨（待圣裁）。\n";
    int pl = 0;
    for (const std::string& a : s.openAsks) { o << "  " << a << "\n"; ++pl; }
    for (const std::string& t : s.humanTasks) { o << "  " << t << " — 须 human 圣裁\n"; ++pl; }
    if (pl == 0) o << "  （无事请旨）\n";

    o << "一、臣工在值。";
    if (s.activeAgents.empty()) {
        o << "（空朝，无人当值）";
    } else {
        for (size_t i = 0; i < s.activeAgents.size(); ++i) {
            if (i) o << "、";
            o << s.activeAgents[i];
        }
    }
    o << "\n如蒙圣鉴，谨此奏闻。\n";
    return o.str();
}

}  // namespace

std::string RoomStore::genReport(const std::string& room, const std::string& mode) {
    if (mode != "hzdf" && mode != "company" && mode != "feudal")
        throw std::runtime_error("unknown report mode: " + mode + " (hzdf|company|feudal)");
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);  // throws on unknown room
    sweepExpired(room, rd);

    ReportSnap s;
    s.room = room;
    s.chamber = rd.meta.chamber;
    s.hasGoal = rd.soc.goal.exists;
    s.goalText = rd.soc.goal.text;
    s.goalStatus = rd.soc.goal.status;
    s.goalCriteria = rd.soc.goal.criteria;
    s.genCount = (int)rd.soc.gens.size();
    s.activeGen = currentGenLocked(rd);

    for (const Generation& g : rd.soc.gens) {
        ReportSnap::Phase p;
        p.n = g.n;
        p.active = (g.n == s.activeGen);
        p.chronicle = g.chronicle;
        for (const Task& t : rd.tasks)
            if (t.gen == g.n) {
                ++p.total;
                if (t.status == "done") ++p.done;
            }
        s.phases.push_back(p);
    }

    for (const Task& t : rd.tasks) {
        if (t.status == "done") {
            ++s.tDone;
            s.doneTasks.push_back("#" + std::to_string(t.id) + " " + t.title + " — " +
                                  t.assignee + "贡，" + t.verifier + "验讫");
            if (!t.assignee.empty()) ++s.byAgentDone[t.assignee];
        } else if (t.status == "claimed" || t.status == "submitted") {
            ++s.tInFlight;
            s.inFlightTasks.push_back("#" + std::to_string(t.id) + "【交战·" +
                                      t.assignee + "领兵】" + t.title);
            if (!t.assignee.empty()) ++s.byAgentInFlight[t.assignee];
        } else {
            ++s.tOpen;
            s.openTasks.push_back("#" + std::to_string(t.id) + " " + t.title);
        }
        if (t.human && t.status != "done") {
            ++s.tHuman;
            s.humanTasks.push_back("#" + std::to_string(t.id) + " " + t.title);
        }
    }

    for (const RoleEntry& r : rd.soc.roles) s.agentPost[r.agent] = r.role;

    std::set<long long> answered;
    for (const Message& m : rd.msgs) {
        if (m.type == "fact") {
            ++s.facts;
            if (s.recentFacts.size() < 3)
                s.recentFacts.push_back(m.agent + "：" + utf8Truncate(m.content, 80));
        } else if (m.type == "answer" && m.ref > 0) {
            answered.insert(m.ref);
        }
    }
    // newest-first recentFacts (messages arrive oldest-first)
    std::reverse(s.recentFacts.begin(), s.recentFacts.end());
    for (const Message& m : rd.msgs) {
        if (m.type != "ask" || answered.count(m.id)) continue;
        s.openAsks.push_back("ask #" + std::to_string(m.id) + " (" + m.agent +
                             "): " + utf8Truncate(m.content, 80));
    }
    // newest asks first (they are the pressing ones)
    std::reverse(s.openAsks.begin(), s.openAsks.end());

    long long cutoff = nowMs() - 1800000;
    std::set<std::string> act;
    for (const Claim& c : rd.claims)
        if (c.active && c.agent != "server") { act.insert(c.agent); ++s.activeClaims; }
    for (const Message& m : rd.msgs)
        if (m.ts >= cutoff && m.agent != "server") act.insert(m.agent);
    for (const Task& t : rd.tasks)
        if ((t.status == "claimed" || t.status == "submitted") && !t.assignee.empty())
            act.insert(t.assignee);
    s.activeAgents.assign(act.begin(), act.end());
    s.answeredAsks = (int)answered.size();

    if (mode == "hzdf") return renderHzdf(s);
    if (mode == "company") return renderCompany(s);
    return renderFeudal(s);
}


Message RoomStore::say(const std::string& room, const std::string& agent,
                       const std::string& type, const std::string& content,
                       long long ref) {
    std::lock_guard<std::mutex> lock(mu_);
    return sayLocked(room, agent, type, content, ref);
}

Message RoomStore::sayLocked(const std::string& room, const std::string& agent,
                             const std::string& type, const std::string& content,
                             long long ref) {
    static const char* types[] = {"say", "plan", "fact", "ask", "answer",
                                  "claim", "release", "veto", "done", "task",
                                  "goal", "gen", "role", nullptr};
    bool okType = false;
    for (int i = 0; types[i]; i++)
        if (type == types[i]) okType = true;
    if (!okType) throw std::runtime_error("bad message type: " + type);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    if (content.empty()) throw std::runtime_error("empty content");
    if (content.size() > 16384) throw std::runtime_error("content too long (max 16384)");

    RoomData& rd = load(room);
    Message m;
    m.id = rd.msgs.empty() ? 1 : rd.msgs.back().id + 1;
    m.ts = nowMs();
    m.agent = agent;
    m.type = type;
    m.content = content;
    m.ref = ref;
    m.prev = rd.msgs.empty() ? std::string("genesis") : rd.msgs.back().hash;
    m.hash = messageHash(m.prev, room, m);
    rd.msgs.push_back(m);
    appendBytes(pathJoin(roomDir(room), "messages.jsonl"), msgToJson(m).dump() + "\n");
    cv_.notify_all();
    return m;
}

std::vector<Message> RoomStore::messages(const std::string& room, long long since,
                                         int limit, const std::string& typeFilter,
                                         const std::string& agentFilter) {
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    std::vector<Message> out;
    for (const Message& m : rd.msgs) {
        if (m.id <= since) continue;
        if (!typeFilter.empty() && m.type != typeFilter) continue;
        if (!agentFilter.empty() && m.agent != agentFilter) continue;
        out.push_back(m);
    }
    if (limit > 0 && static_cast<int>(out.size()) > limit)
        out.assign(out.end() - limit, out.end());
    return out;
}

std::vector<Message> RoomStore::waitMessages(const std::string& room, long long since,
                                             long long timeoutMs) {
    if (timeoutMs < 0) timeoutMs = 0;
    if (timeoutMs > 60000) timeoutMs = 60000;
    std::unique_lock<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&rd, since] {
        return !rd.msgs.empty() && rd.msgs.back().id > since;
    });
    std::vector<Message> out;
    for (const Message& m : rd.msgs)
        if (m.id > since) out.push_back(m);
    return out;
}

std::vector<SearchHit> RoomStore::search(const std::string& query,
                                         const std::string& roomFilter, int limit,
                                         const std::string& viewer) {
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    std::string needle = asciiLower(query);
    if (needle.empty()) return {};
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SearchHit> out;
    for (const std::string& room : rooms()) {
        if (!roomFilter.empty() && room != roomFilter) continue;
        RoomData& rd = load(room);
        // Chambers are invisible to non-members even when named explicitly.
        if (rd.meta.chamber) {
            bool member = false;
            for (const std::string& m : rd.meta.members)
                if (m == viewer) member = true;
            if (!member) continue;
        }
        for (auto it = rd.msgs.rbegin(); it != rd.msgs.rend(); ++it) {
            std::string hay = asciiLower(it->content + "\n" + it->agent + "\n" + it->type);
            if (hay.find(needle) == std::string::npos) continue;
            SearchHit h;
            h.room = room;
            h.msg = *it;
            out.push_back(h);
            if (static_cast<int>(out.size()) >= limit) return out;
        }
    }
    return out;
}

Task* RoomStore::findTask(RoomData& rd, long long id) {
    for (Task& t : rd.tasks)
        if (t.id == id) return &t;
    return nullptr;
}

Task RoomStore::createTaskLocked(const std::string& room, RoomData& rd, const std::string& title,
                                 const std::string& detail, const std::string& creator, int gen,
                                 bool human) {
    Task t;
    t.id = rd.nextTaskId++;
    t.title = title;
    t.detail = detail;
    t.status = "open";
    t.creator = creator;
    t.gen = gen;
    t.human = human;
    t.createdTs = nowMs();
    t.updatedTs = t.createdTs;
    rd.tasks.push_back(t);
    persistTasks(room, rd);
    sayLocked(room, "server", "task",
              "task #" + std::to_string(t.id) + " \"" + title + "\" created by " + creator +
                  (human ? " — awaits human sign-off" : ""),
              -1);
    return t;
}

Task RoomStore::taskCreate(const std::string& room, const std::string& title,
                           const std::string& detail, const std::string& creator,
                           bool human) {
    std::lock_guard<std::mutex> lock(mu_);
    if (title.empty() || title.size() > 512) throw std::runtime_error("bad task title");
    if (detail.size() > 4096) throw std::runtime_error("detail too long (max 4096)");
    if (creator.empty() || creator.size() > 64) throw std::runtime_error("bad agent name");
    RoomData& rd = load(room);
    return createTaskLocked(room, rd, title, detail, creator, currentGenLocked(rd), human);
}

std::vector<Task> RoomStore::tasks(const std::string& room) {
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    return rd.tasks;
}

Task RoomStore::taskClaim(const std::string& room, long long id, const std::string& agent) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    RoomData& rd = load(room);
    Task* t = findTask(rd, id);
    if (!t) throw std::runtime_error("no such task: #" + std::to_string(id));
    if (t->status != "open")
        throw std::runtime_error("task #" + std::to_string(id) + " is not open (status: " +
                                 t->status + ")");
    t->status = "claimed";
    t->assignee = agent;
    t->updatedTs = nowMs();
    persistTasks(room, rd);
    sayLocked(room, "server", "task",
              "task #" + std::to_string(id) + " \"" + t->title + "\" claimed by " + agent, -1);
    return *t;
}

Task RoomStore::taskSubmit(const std::string& room, long long id, const std::string& agent,
                           const std::string& evidence) {
    std::lock_guard<std::mutex> lock(mu_);
    if (evidence.empty()) throw std::runtime_error("evidence required to submit");
    if (evidence.size() > 4096) throw std::runtime_error("evidence too long (max 4096)");
    RoomData& rd = load(room);
    Task* t = findTask(rd, id);
    if (!t) throw std::runtime_error("no such task: #" + std::to_string(id));
    if (t->status != "claimed")
        throw std::runtime_error("task #" + std::to_string(id) + " is not claimed (status: " +
                                 t->status + ")");
    if (t->assignee != agent)
        throw std::runtime_error("task #" + std::to_string(id) + " is assigned to " + t->assignee);
    t->status = "submitted";
    t->evidence = evidence;
    t->updatedTs = nowMs();
    persistTasks(room, rd);
    std::string ev = evidence.size() > 200 ? evidence.substr(0, 200) + "…" : evidence;
    sayLocked(room, "server", "task",
              "task #" + std::to_string(id) + " submitted by " + agent + ": " + ev, -1);
    return *t;
}

Task RoomStore::taskVerify(const std::string& room, long long id, const std::string& agent,
                           bool accept) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    RoomData& rd = load(room);
    Task* t = findTask(rd, id);
    if (!t) throw std::runtime_error("no such task: #" + std::to_string(id));
    if (t->status != "submitted")
        throw std::runtime_error("task #" + std::to_string(id) + " is not submitted (status: " +
                                 t->status + ")");
    // The evidence gate: the verifier must be a different agent than the submitter.
    if (agent == t->assignee)
        throw std::runtime_error("task #" + std::to_string(id) +
                                  " cannot be verified by its own assignee — evidence gate");
    if (t->human) {
        // Sovereign sign-off: a human-gate task may only be verified by the
        // human it awaits — no agent role substitutes for the sovereign.
        if (agent != "human")
            throw std::runtime_error("task #" + std::to_string(id) +
                                      " awaits human sign-off — only the agent 'human' may verify "
                                      "it");
    } else if (!rd.soc.roles.empty() && agent != "human" &&
               !canVerifyTaskLocked(rd, agent)) {
        // The division-of-labor gate: once roles are registered, only posts
        // carrying the verify-task bit may verify (sovereign 'human' excepted).
        throw std::runtime_error("task #" + std::to_string(id) +
                                  " verification requires a post with the verify-task bit "
                                  "while roles are registered");
    }
    t->verifier = agent;
    t->updatedTs = nowMs();
    if (accept) {
        t->status = "done";
        persistTasks(room, rd);
        sayLocked(room, "server", "task",
                  "task #" + std::to_string(id) + " \"" + t->title +
                      "\" verified DONE by " + agent,
                  -1);
        Task out = *t;  // copy before the drain check may grow the vector
        // A drained generation with the goal still open births the next one.
        checkGenDrainLocked(room, rd);
        return out;
    }
    t->status = "open";
    t->assignee.clear();
    t->evidence.clear();
    persistTasks(room, rd);
    sayLocked(room, "server", "task",
              "task #" + std::to_string(id) + " \"" + t->title + "\" rejected by " + agent +
                  " — reopened",
              -1);
    return *t;
}

void RoomStore::persistTasks(const std::string& room, RoomData& rd) {
    Json arr = Json::array();
    for (const Task& t : rd.tasks) arr.push(taskToJson(t));
    writeBytes(pathJoin(roomDir(room), "tasks.json"), arr.dump() + "\n");
}

void RoomStore::sweepExpired(const std::string& room, RoomData& rd) {
    long long now = nowMs();
    bool changed = false;
    for (Claim& c : rd.claims) {
        if (c.active && c.expiresTs > 0 && c.expiresTs <= now) {
            c.active = false;
            changed = true;
            Message m = sayLocked(room, "server", "release",
                                  "claim #" + std::to_string(c.id) + " by " + c.agent +
                                      " expired [" + joinScope(c.scope) + "]", -1);
        }
    }
    if (changed) persistClaims(room, rd);
}

ClaimOutcome RoomStore::claim(const std::string& room, const std::string& agent,
                              const std::vector<std::string>& scope, long long ttlSec) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    if (scope.empty()) throw std::runtime_error("claim needs at least one scope entry");
    for (const std::string& s : scope) {
        if (s.empty() || s.size() > 256) throw std::runtime_error("bad scope entry");
    }
    if (ttlSec < 0 || ttlSec > 86400) throw std::runtime_error("ttl_s out of range (0..86400)");
    if (ttlSec == 0) ttlSec = 600;

    RoomData& rd = load(room);
    sweepExpired(room, rd);

    // Renew same-agent overlapping claims; conflict with others.
    ClaimOutcome out;
    for (Claim& c : rd.claims) {
        if (!c.active || c.agent != agent) continue;
        if (scopeIntersects(c.scope, scope)) {
            c.expiresTs = nowMs() + ttlSec * 1000;
            c.ts = nowMs();
            persistClaims(room, rd);
            out.ok = true;
            out.claim = c;
            out.note = "renewed";
            sayLocked(room, "server", "claim",
                      agent + " renewed claim #" + std::to_string(c.id) + " [" +
                          joinScope(scope) + "] ttl=" + std::to_string(ttlSec) + "s", -1);
            return out;
        }
    }
    for (const Claim& c : rd.claims) {
        if (c.active && c.agent != agent && scopeIntersects(c.scope, scope)) {
            out.conflicts.push_back(c);
        }
    }
    if (!out.conflicts.empty()) {
        out.ok = false;
        std::string who;
        for (const Claim& c : out.conflicts) who += (who.empty() ? "" : "; ") + c.agent;
        sayLocked(room, "server", "veto",
                  "claim by " + agent + " [" + joinScope(scope) + "] vetoed — held by " + who, -1);
        return out;
    }

    Claim c;
    c.id = rd.nextClaimId++;
    c.ts = nowMs();
    c.expiresTs = nowMs() + ttlSec * 1000;
    c.agent = agent;
    c.scope = scope;
    c.active = true;
    rd.claims.push_back(c);
    persistClaims(room, rd);
    out.ok = true;
    out.claim = c;
    out.note = "granted";
    sayLocked(room, "server", "claim",
              agent + " claimed [" + joinScope(scope) + "] ttl=" + std::to_string(ttlSec) + "s", -1);
    return out;
}

int RoomStore::release(const std::string& room, const std::string& agent, long long claimId,
                       const std::string& scope) {
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    sweepExpired(room, rd);
    int released = 0;
    for (Claim& c : rd.claims) {
        if (!c.active) continue;
        if (claimId > 0) {
            if (c.id != claimId) continue;
        } else if (!scope.empty()) {
            if (std::find(c.scope.begin(), c.scope.end(), scope) == c.scope.end()) continue;
        } else {
            continue;
        }
        if (agent != c.agent && agent != "server") continue;
        c.active = false;
        released++;
        sayLocked(room, "server", "release",
                  agent + " released claim #" + std::to_string(c.id) + " [" +
                      joinScope(c.scope) + "]", -1);
    }
    if (released > 0) persistClaims(room, rd);
    return released;
}

std::vector<Claim> RoomStore::claims(const std::string& room) {
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    sweepExpired(room, rd);
    std::vector<Claim> out;
    for (const Claim& c : rd.claims)
        if (c.active) out.push_back(c);
    return out;
}

BoardEntry RoomStore::boardGet(const std::string& room, const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    for (const BoardEntry& e : rd.board)
        if (e.key == key) return e;
    return BoardEntry{};
}

bool RoomStore::boardSet(const std::string& room, const std::string& key,
                          const std::string& value, const std::string& agent) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!validBoardKey(key)) return false;
    if (value.size() > 65536) return false;
    RoomData& rd = load(room);
    for (BoardEntry& e : rd.board) {
        if (e.key == key) {
            e.value = value;
            e.agent = agent;
            e.ts = nowMs();
            persistBoard(room, rd);
            return true;
        }
    }
    BoardEntry e;
    e.key = key;
    e.value = value;
    e.agent = agent;
    e.ts = nowMs();
    e.exists = true;
    rd.board.push_back(e);
    persistBoard(room, rd);
    return true;
}

std::vector<BoardEntry> RoomStore::boardAll(const std::string& room) {
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    return rd.board;
}

void RoomStore::persistClaims(const std::string& room, RoomData& rd) {
    Json arr = Json::array();
    for (const Claim& c : rd.claims) arr.push(claimToJson(c));
    writeBytes(pathJoin(roomDir(room), "claims.json"), arr.dump() + "\n");
}

void RoomStore::persistBoard(const std::string& room, RoomData& rd) {
    Json obj = Json::object();
    for (const BoardEntry& e : rd.board) {
        Json v = Json::object();
        v.set("value", Json::string(e.value));
        v.set("agent", Json::string(e.agent));
        v.set("ts", Json::number(static_cast<double>(e.ts)));
        obj.set(e.key, std::move(v));
    }
    writeBytes(pathJoin(roomDir(room), "board.json"), obj.dump() + "\n");
}

bool RoomStore::verify(const std::string& room, std::string& errOut) {
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    std::string prev = "genesis";
    for (const Message& m : rd.msgs) {
        if (m.prev != prev) {
            errOut = "chain break at id " + std::to_string(m.id) + ": prev mismatch";
            return false;
        }
        if (messageHash(m.prev, room, m) != m.hash) {
            errOut = "chain break at id " + std::to_string(m.id) + ": hash mismatch";
            return false;
        }
        prev = m.hash;
    }
    return true;
}

// ---- society layer ---------------------------------------------------------

Society RoomStore::society(const std::string& room) {
    std::lock_guard<std::mutex> lock(mu_);
    RoomData& rd = load(room);
    return rd.soc;
}

int RoomStore::currentGenLocked(const RoomData& rd) const {
    if (rd.soc.gens.empty() || rd.soc.gens.back().status != "active") return 0;
    return rd.soc.gens.back().n;
}

// The effective post table: the five presets plus the room's custom posts.
std::vector<PostDef> RoomStore::effectivePostsLocked(const RoomData& rd) {
    std::vector<PostDef> out;
    for (const PostBits& p : kPresets) {
        PostDef d;
        d.name = p.name;
        d.canVerifyTask = p.vt;
        d.canVerifyGoal = p.vg;
        d.preset = true;
        out.push_back(d);
    }
    for (const PostDef& c : rd.soc.posts) out.push_back(c);
    return out;
}

bool RoomStore::canVerifyTaskLocked(const RoomData& rd, const std::string& agent) const {
    for (const PostDef& p : effectivePostsLocked(rd)) {
        if (!p.canVerifyTask) continue;
        for (const RoleEntry& r : rd.soc.roles)
            if (r.agent == agent && r.role == p.name) return true;
    }
    return false;
}

bool RoomStore::canVerifyGoalLocked(const RoomData& rd, const std::string& agent) const {
    for (const PostDef& p : effectivePostsLocked(rd)) {
        if (!p.canVerifyGoal) continue;
        for (const RoleEntry& r : rd.soc.roles)
            if (r.agent == agent && r.role == p.name) return true;
    }
    return false;
}

void RoomStore::persistSociety(const std::string& room, RoomData& rd) {
    writeBytes(pathJoin(roomDir(room), "society.json"), societyToJson(rd.soc).dump() + "\n");
}

void RoomStore::retireGenLocked(const std::string& room, RoomData& rd, const std::string& note) {
    if (rd.soc.gens.empty() || rd.soc.gens.back().status != "active") return;
    Generation& g = rd.soc.gens.back();
    g.status = "retired";
    g.retiredTs = nowMs();
    g.note = note;
    persistSociety(room, rd);
    sayLocked(room, "server", "gen",
              "generation " + std::to_string(g.n) + " retired — " + note, -1);
}

void RoomStore::birthGenLocked(const std::string& room, RoomData& rd) {
    int n = rd.soc.gens.empty() ? 1 : rd.soc.gens.back().n + 1;
    Generation g;
    g.n = n;
    g.status = "active";
    g.bornTs = nowMs();
    rd.soc.gens.push_back(g);
    persistSociety(room, rd);
    // The genesis task: the new generation's ritual entry point. It keeps
    // every generation's board non-empty, so birth can never loop. From gen 2
    // on, with chronicles available, the entry point is the distilled record
    // — not a re-read of the whole history (context is the scarce resource).
    bool anyChronicle = false;
    for (const Generation& p : rd.soc.gens)
        if (!p.chronicle.empty()) anyChronicle = true;
    // The generation that just retired (gen n-1) carries the freshest
    // distillation; its absence means recent work is only in raw history.
    bool prevHasChronicle = n > 1 && !rd.soc.gens[n - 2].chronicle.empty();
    std::string detail;
    if (n > 1 && prevHasChronicle) {
        detail = "The god goal is still open. Start from the chronicles of past "
                 "generations (`greenroom gen <room>`) — the distilled record of what "
                 "was tried, what worked and what remains; do NOT re-read the full "
                 "history (use `greenroom search` for cold storage). Assess the gap, "
                 "then create and distribute this generation's tasks.";
    } else if (n > 1 && anyChronicle) {
        detail = "The god goal is still open. The previous generation left no "
                 "chronicle — read the chronicles of older generations (`greenroom "
                 "gen <room>`) and the recent history since they were written "
                 "(listen --since 0), assess the gap, then create and distribute "
                 "this generation's tasks.";
    } else if (n > 1) {
        detail = "The god goal is still open. Past generations left no chronicle — "
                 "read the full room history (listen --since 0), assess the gap, "
                 "then create and distribute this generation's tasks.";
    } else {
        detail = "The god goal is still open. Read the full room history "
                 "(listen --since 0), assess the gap, then create and "
                 "distribute this generation's tasks.";
    }
    createTaskLocked(room, rd,
                     "Generation " + std::to_string(n) + ": assess and plan",
                     detail, "server", n, false);
    sayLocked(room, "server", "gen",
              "generation " + std::to_string(n) + " born — the god goal is still open", -1);
}

void RoomStore::checkGenDrainLocked(const std::string& room, RoomData& rd) {
    if (!rd.soc.goal.exists || rd.soc.goal.status != "open") return;
    if (rd.soc.gens.empty() || rd.soc.gens.back().status != "active") return;
    int cur = rd.soc.gens.back().n;
    for (const Task& t : rd.tasks)
        if (t.gen == cur && t.status != "done") return;  // work remains
    const Generation& g = rd.soc.gens.back();
    retireGenLocked(room, rd,
                    g.chronicle.empty() ? "task board drained (no chronicle submitted)"
                                       : "task board drained");
    birthGenLocked(room, rd);
}

void RoomStore::goalSet(const std::string& room, const std::string& text,
                        const std::string& criteria, const std::string& oracle,
                        const std::string& agent) {
    std::lock_guard<std::mutex> lock(mu_);
    if (text.empty() || text.size() > 2048) throw std::runtime_error("bad goal text (1..2048)");
    if (criteria.size() > 2048) throw std::runtime_error("criteria too long (max 2048)");
    if (oracle.size() > 512) throw std::runtime_error("oracle URL too long (max 512)");
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    // Goals are public and hash-chained forever — refuse obvious credentials
    // and oracle URLs that carry them in the query string.
    if (looksLikeCredential(text + "\n" + criteria + "\n" + oracle))
        throw std::runtime_error("goal looks like it contains credentials — goals are public "
                                 "and hash-chained forever; keep credentials inside the oracle "
                                 "service");
    if (!oracle.empty() && oracle.compare(0, 7, "http://") != 0)
        throw std::runtime_error("oracle must be an http:// URL");
    RoomData& rd = load(room);
    if (rd.soc.goal.exists &&
        (rd.soc.goal.status == "open" || rd.soc.goal.status == "proposed"))
        throw std::runtime_error("a god goal is already active — achieve, verify or abandon it "
                                 "first");
    rd.soc.goal = Goal{};
    rd.soc.goal.exists = true;
    rd.soc.goal.text = text;
    rd.soc.goal.criteria = criteria;
    rd.soc.goal.oracle = oracle;
    rd.soc.goal.status = "open";
    rd.soc.goal.proposer = agent;
    rd.soc.goal.createdTs = nowMs();
    // Generations are NOT reset here: numbering stays monotonic over the
    // room's whole lifetime, so tasks left over from an old society (gen n)
    // can never be conflated with a new society's generations (gen > n).
    persistSociety(room, rd);
    sayLocked(room, "server", "goal",
              "god goal declared by " + agent + ": \"" + text + "\"" +
                  (criteria.empty() ? "" : " (criteria: " + criteria + ")") +
                  (oracle.empty() ? "" : " (oracle: " + oracle + ")") +
                  " — society born",
              -1);
    birthGenLocked(room, rd);
}

void RoomStore::goalAchieve(const std::string& room, const std::string& agent,
                            const std::string& evidence) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    if (evidence.empty()) throw std::runtime_error("evidence required to claim achievement");
    if (evidence.size() > 4096) throw std::runtime_error("evidence too long (max 4096)");
    RoomData& rd = load(room);
    if (!rd.soc.goal.exists) throw std::runtime_error("no god goal declared");
    if (rd.soc.goal.status != "open")
        throw std::runtime_error("goal is not open (status: " + rd.soc.goal.status + ")");
    rd.soc.goal.status = "proposed";
    rd.soc.goal.achiever = agent;
    rd.soc.goal.evidence = evidence;
    persistSociety(room, rd);
    std::string ev = evidence.size() > 200 ? evidence.substr(0, 200) + "…" : evidence;
    sayLocked(room, "server", "goal",
              agent + " proposed god-goal achievement: " + ev, -1);
}

void RoomStore::goalVerify(const std::string& room, const std::string& agent, bool accept,
                           const OracleReading* oracle) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    RoomData& rd = load(room);
    if (!rd.soc.goal.exists) throw std::runtime_error("no god goal declared");
    if (rd.soc.goal.status != "proposed")
        throw std::runtime_error("goal is not proposed (status: " + rd.soc.goal.status + ")");
    if (agent == rd.soc.goal.achiever)
        throw std::runtime_error("the god goal cannot be verified by its own achiever — "
                                 "evidence gate");
    // The sovereign 'human' passes the division-of-labor gate unconditionally.
    if (!rd.soc.roles.empty() && agent != "human" && !canVerifyGoalLocked(rd, agent))
        throw std::runtime_error("god-goal verification requires a post with the verify-goal "
                                 "bit while roles are registered");
    Goal& g = rd.soc.goal;
    if (accept) {
        // The oracle gate: with an oracle declared, the society cannot close
        // unless the external predicate says satisfied. The verifier is a
        // trigger, not a judge.
        if (!g.oracle.empty()) {
            if (!oracle || !oracle->fetched)
                throw std::runtime_error(
                    "oracle unreadable: " +
                    (oracle && !oracle->err.empty() ? oracle->err : "no reading supplied") +
                    " — the god goal cannot be closed without the oracle");
            if (!oracle->satisfied)
                throw std::runtime_error("oracle says NOT satisfied — the god goal cannot be "
                                         "closed (reading: " +
                                         oracle->raw + ")");
            g.oracleRead = oracle->raw;
            g.oracleReadTs = nowMs();
        }
        g.status = "achieved";
        g.verifier = agent;
        g.closedTs = nowMs();
        persistSociety(room, rd);
        retireGenLocked(room, rd, "god goal achieved");
        sayLocked(room, "server", "goal",
                  "god goal ACHIEVED — verified by " + agent + ", society closed after " +
                      std::to_string(rd.soc.gens.size()) + " generation(s)",
                  -1);
    } else {
        g.status = "open";
        g.achiever.clear();
        g.evidence.clear();
        persistSociety(room, rd);
        sayLocked(room, "server", "goal",
                  "achievement claim rejected by " + agent + " — goal reopened", -1);
        // The board may have drained while the claim was pending.
        checkGenDrainLocked(room, rd);
    }
}

void RoomStore::goalAbandon(const std::string& room, const std::string& agent,
                            const std::string& reason) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    if (reason.size() > 2048) throw std::runtime_error("reason too long (max 2048)");
    RoomData& rd = load(room);
    if (!rd.soc.goal.exists) throw std::runtime_error("no god goal declared");
    if (rd.soc.goal.status == "achieved" || rd.soc.goal.status == "abandoned")
        throw std::runtime_error("society is already closed (status: " + rd.soc.goal.status +
                                 ")");
    rd.soc.goal.status = "abandoned";
    rd.soc.goal.closedTs = nowMs();
    persistSociety(room, rd);
    retireGenLocked(room, rd, "god goal abandoned");
    sayLocked(room, "server", "goal",
              "god goal abandoned by " + agent +
                  (reason.empty() ? "" : ": " + reason) + " — society closed",
              -1);
}

void RoomStore::genAdvance(const std::string& room, const std::string& agent,
                           const std::string& note) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    if (note.size() > 2048) throw std::runtime_error("note too long (max 2048)");
    RoomData& rd = load(room);
    if (!rd.soc.goal.exists) throw std::runtime_error("no god goal declared");
    if (rd.soc.goal.status == "achieved" || rd.soc.goal.status == "abandoned")
        throw std::runtime_error("society is closed (status: " + rd.soc.goal.status + ")");
    if (rd.soc.goal.status == "proposed")
        throw std::runtime_error("achievement proposal pending verification");
    if (rd.soc.gens.empty() || rd.soc.gens.back().status != "active")
        throw std::runtime_error("no active generation");
    retireGenLocked(room, rd, "forced by " + agent + (note.empty() ? "" : ": " + note));
    birthGenLocked(room, rd);
}

void RoomStore::genChronicle(const std::string& room, const std::string& agent,
                              const std::string& text) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    if (text.empty() || text.size() > 4096)
        throw std::runtime_error("bad chronicle text (1..4096) — the fixed budget is the "
                                 "distillation discipline");
    RoomData& rd = load(room);
    if (rd.soc.gens.empty() || rd.soc.gens.back().status != "active")
        throw std::runtime_error("no active generation to chronicle");
    Generation& g = rd.soc.gens.back();
    g.chronicle = text;
    g.chronicler = agent;
    g.chronicleTs = nowMs();
    persistSociety(room, rd);
    std::string ev = text.size() > 120 ? text.substr(0, 120) + "…" : text;
    sayLocked(room, "server", "gen",
              "generation " + std::to_string(g.n) + " chronicle updated by " + agent + ": " + ev,
              -1);
}

void RoomStore::roleTake(const std::string& room, const std::string& agent,
                         const std::string& role) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    RoomData& rd = load(room);
    if (!isPresetPost(role)) {
        bool known = false;
        for (const PostDef& p : rd.soc.posts)
            if (p.name == role) known = true;
        if (!known)
            throw std::runtime_error("unknown post: " + role +
                                     " — define it first (society post-define)");
    }
    for (const RoleEntry& r : rd.soc.roles)
        if (r.role == role && r.agent == agent) return;  // idempotent
    RoleEntry r;
    r.role = role;
    r.agent = agent;
    r.ts = nowMs();
    rd.soc.roles.push_back(r);
    persistSociety(room, rd);
    sayLocked(room, "server", "role", agent + " took post " + role, -1);
}

void RoomStore::postDefine(const std::string& room, const std::string& name,
                            bool canVerifyTask, bool canVerifyGoal,
                            const std::string& model, const std::string& agent) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    if (model.size() > 128) throw std::runtime_error("model too long (max 128)");
    if (!validPostName(name))
        throw std::runtime_error("bad post name (1..32 chars of [A-Za-z0-9-_.])");
    if (isPresetPost(name))
        throw std::runtime_error("post name reserved by a preset: " + name);
    RoomData& rd = load(room);
    for (const PostDef& p : rd.soc.posts)
        if (p.name == name)
            throw std::runtime_error("post already defined: " + name);
    PostDef p;
    p.name = name;
    p.canVerifyTask = canVerifyTask;
    p.canVerifyGoal = canVerifyGoal;
    p.model = model;
    p.createdBy = agent;
    p.createdTs = nowMs();
    p.preset = false;
    rd.soc.posts.push_back(p);
    persistSociety(room, rd);
    sayLocked(room, "server", "role",
              agent + " defined post " + name +
                  " (verify-task: " + (canVerifyTask ? "yes" : "no") +
                  ", verify-goal: " + (canVerifyGoal ? "yes" : "no") +
                  (model.empty() ? "" : ", model: " + model) + ")",
              -1);
}

std::vector<PostDef> RoomStore::posts(const std::string& room) {
    std::lock_guard<std::mutex> lock(mu_);
    return effectivePostsLocked(load(room));
}

// -----------------------------------------------------------------------------

std::vector<std::string> RoomStore::rooms() {
    std::vector<std::string> out;
    std::string base = pathJoin(dataDir_, "rooms");
#ifdef _WIN32
    std::string pat = base + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) out.push_back(name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(base.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string full = pathJoin(base, name);
        struct stat st;
        if (::stat(full.c_str(), &st) == 0 && (st.st_mode & S_IFDIR)) out.push_back(name);
    }
    closedir(d);
#endif
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace gr
