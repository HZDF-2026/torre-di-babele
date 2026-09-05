// store.h — the room store: rooms, hash-chained messages, claim leases,
// and the blackboard. One instance, one mutex, plain-file persistence.
#ifndef GR_STORE_H
#define GR_STORE_H

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace gr {

struct Message {
    long long id = 0;      // per-room, 1-based
    long long ts = 0;      // unix ms
    std::string agent;
    std::string type;      // say|plan|fact|ask|answer|claim|release|veto|done
    std::string content;
    long long ref = -1;    // referenced message id, -1 = none
    std::string prev;      // hash of previous message, "genesis" for id 1
    std::string hash;
};

struct Claim {
    long long id = 0;          // per-room, 1-based
    long long ts = 0;
    long long expiresTs = 0;   // ts + ttl_s*1000; 0 = no expiry
    std::string agent;
    std::vector<std::string> scope;
    bool active = true;
};

struct BoardEntry {
    std::string key;
    std::string value;
    std::string agent;
    long long ts = 0;
    bool exists = false;
};

struct ClaimOutcome {
    bool ok = false;
    Claim claim;                     // valid when ok
    std::vector<Claim> conflicts;    // valid when !ok
    std::string note;                // human-readable summary for messages
};

class RoomStore {
public:
    explicit RoomStore(std::string dataDir);

    // All methods are thread-safe (one global mutex).

    std::vector<std::string> rooms();          // sorted
    bool createRoom(const std::string& name);   // false if exists/invalid
    bool roomExists(const std::string& name);

    // throws std::runtime_error on unknown room / bad type
    Message say(const std::string& room, const std::string& agent,
                const std::string& type, const std::string& content,
                long long ref);
    std::vector<Message> messages(const std::string& room, long long since,
                                  int limit, const std::string& typeFilter,
                                  const std::string& agentFilter);

    // Claim semantics per PROTOCOL.md: conflict = scope intersects an ACTIVE
    // claim held by a DIFFERENT agent. Same-agent overlap renews the TTL.
    // Records claim/veto/release messages into the stream.
    ClaimOutcome claim(const std::string& room, const std::string& agent,
                       const std::vector<std::string>& scope, long long ttlSec);
    // Release by claim id (any agent may release its own; "server" releases
    // anything) or by scope prefix. Returns number released.
    int release(const std::string& room, const std::string& agent,
                long long claimId, const std::string& scope);
    std::vector<Claim> claims(const std::string& room);  // active only, TTL swept

    BoardEntry boardGet(const std::string& room, const std::string& key);
    bool boardSet(const std::string& room, const std::string& key,
                  const std::string& value, const std::string& agent);
    std::vector<BoardEntry> boardAll(const std::string& room);

    // Recompute the hash chain; on failure errOut says where it broke.
    bool verify(const std::string& room, std::string& errOut);

    const std::string& dataDir() const { return dataDir_; }

private:
    struct RoomData {
        std::vector<Message> msgs;
        std::vector<Claim> claims;
        long long nextClaimId = 1;
        std::vector<BoardEntry> board;
    };

    RoomData& load(const std::string& room);   // caller holds mutex
    Message sayLocked(const std::string& room, const std::string& agent,
                      const std::string& type, const std::string& content,
                      long long ref);          // caller holds mutex
    std::string roomDir(const std::string& room) const;
    void persistClaims(const std::string& room, RoomData& rd);
    void persistBoard(const std::string& room, RoomData& rd);
    void sweepExpired(const std::string& room, RoomData& rd);  // caller holds mutex

    std::string dataDir_;
    std::mutex mu_;
    std::map<std::string, RoomData> cache_;
};

// Canonical message hash, PROTOCOL.md §"Message envelope".
std::string messageHash(const std::string& prev, const std::string& room,
                        const Message& m);

}  // namespace gr

#endif  // GR_STORE_H
