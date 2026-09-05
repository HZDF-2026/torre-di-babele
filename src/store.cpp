// store.cpp — see store.h.
#include "store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>

#include "jsjson.h"
#include "sha256.h"
#include "util.h"

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
    return obj;
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

}  // namespace

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

bool RoomStore::createRoom(const std::string& room) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!validRoomName(room) || roomExists(room)) return false;
    if (!makeDirs(roomDir(room))) return false;
    load(room);  // initializes empty cache entry
    sayLocked(room, "server", "say", "room created", -1);
    return true;
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
                                         const std::string& roomFilter, int limit) {
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    std::string needle = asciiLower(query);
    if (needle.empty()) return {};
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SearchHit> out;
    for (const std::string& room : rooms()) {
        if (!roomFilter.empty() && room != roomFilter) continue;
        RoomData& rd = load(room);
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
                                 const std::string& detail, const std::string& creator, int gen) {
    Task t;
    t.id = rd.nextTaskId++;
    t.title = title;
    t.detail = detail;
    t.status = "open";
    t.creator = creator;
    t.gen = gen;
    t.createdTs = nowMs();
    t.updatedTs = t.createdTs;
    rd.tasks.push_back(t);
    persistTasks(room, rd);
    sayLocked(room, "server", "task",
              "task #" + std::to_string(t.id) + " \"" + title + "\" created by " + creator,
              -1);
    return t;
}

Task RoomStore::taskCreate(const std::string& room, const std::string& title,
                           const std::string& detail, const std::string& creator) {
    std::lock_guard<std::mutex> lock(mu_);
    if (title.empty() || title.size() > 512) throw std::runtime_error("bad task title");
    if (detail.size() > 4096) throw std::runtime_error("detail too long (max 4096)");
    if (creator.empty() || creator.size() > 64) throw std::runtime_error("bad agent name");
    RoomData& rd = load(room);
    return createTaskLocked(room, rd, title, detail, creator, currentGenLocked(rd));
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
    // The division-of-labor gate: once roles are registered, only the
    // reviewer and tester roles may verify.
    if (!rd.soc.roles.empty() && !hasVerifyRoleLocked(rd, agent))
        throw std::runtime_error("task #" + std::to_string(id) +
                                  " verification requires the reviewer or tester role while "
                                  "roles are registered");
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

bool RoomStore::hasVerifyRoleLocked(const RoomData& rd, const std::string& agent) const {
    for (const RoleEntry& r : rd.soc.roles)
        if (r.agent == agent && (r.role == "reviewer" || r.role == "tester")) return true;
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
    // every generation's board non-empty, so birth can never loop.
    createTaskLocked(room, rd,
                     "Generation " + std::to_string(n) + ": assess and plan",
                     "The god goal is still open. Read the full room history "
                     "(listen --since 0), assess the gap, then create and "
                     "distribute this generation's tasks.",
                     "server", n);
    sayLocked(room, "server", "gen",
              "generation " + std::to_string(n) + " born — the god goal is still open", -1);
}

void RoomStore::checkGenDrainLocked(const std::string& room, RoomData& rd) {
    if (!rd.soc.goal.exists || rd.soc.goal.status != "open") return;
    if (rd.soc.gens.empty() || rd.soc.gens.back().status != "active") return;
    int cur = rd.soc.gens.back().n;
    for (const Task& t : rd.tasks)
        if (t.gen == cur && t.status != "done") return;  // work remains
    retireGenLocked(room, rd, "task board drained");
    birthGenLocked(room, rd);
}

void RoomStore::goalSet(const std::string& room, const std::string& text,
                        const std::string& criteria, const std::string& agent) {
    std::lock_guard<std::mutex> lock(mu_);
    if (text.empty() || text.size() > 2048) throw std::runtime_error("bad goal text (1..2048)");
    if (criteria.size() > 2048) throw std::runtime_error("criteria too long (max 2048)");
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    RoomData& rd = load(room);
    if (rd.soc.goal.exists &&
        (rd.soc.goal.status == "open" || rd.soc.goal.status == "proposed"))
        throw std::runtime_error("a god goal is already active — achieve, verify or abandon it "
                                 "first");
    rd.soc.goal = Goal{};
    rd.soc.goal.exists = true;
    rd.soc.goal.text = text;
    rd.soc.goal.criteria = criteria;
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

void RoomStore::goalVerify(const std::string& room, const std::string& agent, bool accept) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    RoomData& rd = load(room);
    if (!rd.soc.goal.exists) throw std::runtime_error("no god goal declared");
    if (rd.soc.goal.status != "proposed")
        throw std::runtime_error("goal is not proposed (status: " + rd.soc.goal.status + ")");
    if (agent == rd.soc.goal.achiever)
        throw std::runtime_error("the god goal cannot be verified by its own achiever — "
                                 "evidence gate");
    if (!rd.soc.roles.empty() && !hasVerifyRoleLocked(rd, agent))
        throw std::runtime_error("god-goal verification requires the reviewer or tester role "
                                 "while roles are registered");
    Goal& g = rd.soc.goal;
    if (accept) {
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

void RoomStore::roleTake(const std::string& room, const std::string& agent,
                         const std::string& role) {
    static const char* roles[] = {"commander", "recorder", "executor", "reviewer", "tester",
                                  nullptr};
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty() || agent.size() > 64) throw std::runtime_error("bad agent name");
    bool ok = false;
    for (int i = 0; roles[i]; i++)
        if (role == roles[i]) ok = true;
    if (!ok)
        throw std::runtime_error("unknown role: " + role +
                                 " (commander|recorder|executor|reviewer|tester)");
    RoomData& rd = load(room);
    for (const RoleEntry& r : rd.soc.roles)
        if (r.role == role && r.agent == agent) return;  // idempotent
    RoleEntry r;
    r.role = role;
    r.agent = agent;
    r.ts = nowMs();
    rd.soc.roles.push_back(r);
    persistSociety(room, rd);
    sayLocked(room, "server", "role", agent + " took role " + role, -1);
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
