// store.cpp — see store.h.
#include "store.h"

#include <algorithm>
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
    long long maxClaimId = 0;
    for (const Claim& c : rd.claims) maxClaimId = std::max(maxClaimId, c.id);
    rd.nextClaimId = maxClaimId + 1;
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
                                  "claim", "release", "veto", "done", nullptr};
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
