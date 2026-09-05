// store.h — the room store: rooms, hash-chained messages, claim leases,
// and the blackboard. One instance, one mutex, plain-file persistence.
#ifndef GR_STORE_H
#define GR_STORE_H

#include <condition_variable>
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
    std::string type;      // say|plan|fact|ask|answer|claim|release|veto|done|task|goal|gen|role
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

// Evidence-gated task (PROTOCOL.md §Tasks). Transitions:
//   create → open → claim → claimed → submit → submitted →
//   verify(accept) → done | verify(reject) → open (reopened)
struct Task {
    long long id = 0;         // per-room, 1-based
    std::string title;
    std::string detail;
    std::string status;       // open|claimed|submitted|done
    std::string creator;
    std::string assignee;     // when claimed/submitted
    std::string evidence;     // when submitted
    std::string verifier;     // when done
    long long createdTs = 0;
    long long updatedTs = 0;
    int gen = 0;              // generation this task belongs to (0 = no society)
    bool human = false;       // awaits sovereign sign-off: only agent "human" may verify
};

// The society layer (PROTOCOL.md §Society). A room becomes a society when a
// God Goal is declared. The society exists only to fulfill that goal; while
// the goal is open, every drained generation is retired and the next one is
// born automatically. Roles gate who may verify.
struct Goal {
    bool exists = false;
    std::string text;         // the God Goal itself
    std::string criteria;     // what "achieved" means
    std::string status;       // open|proposed|achieved|abandoned
    std::string proposer;     // who declared it
    std::string achiever;     // who claimed achievement (when proposed)
    std::string evidence;     // the claimed achievement evidence
    std::string verifier;     // who verified the achievement
    std::string oracle;       // URL of the achievement oracle; the predicate lives there, credentials never do
    std::string oracleRead;   // raw oracle reading captured when achievement was verified
    long long oracleReadTs = 0;
    long long createdTs = 0;
    long long closedTs = 0;
};

struct Generation {
    int n = 0;                // 1-based
    std::string status;       // active|retired
    long long bornTs = 0;
    long long retiredTs = 0;
    std::string note;         // why it retired
    std::string chronicle;    // recorder's distilled closing record (fixed budget, max 4096)
    std::string chronicler;   // who wrote it
    long long chronicleTs = 0;
};

struct RoleEntry {
    std::string role;         // commander|recorder|executor|reviewer|tester
    std::string agent;
    long long ts = 0;
};

struct Society {
    Goal goal;
    std::vector<Generation> gens;
    std::vector<RoleEntry> roles;
};

// One reading of a goal oracle: the raw HTTP response plus interpretation.
// Fetched by the API layer (no store lock held) and handed to goalVerify.
struct OracleReading {
    bool fetched = false;     // transport succeeded and response was JSON with boolean "satisfied"
    bool satisfied = false;   // the oracle's verdict
    std::string raw;          // raw response body (capped)
    std::string err;          // transport/parse error text when !fetched
};

// Fetches and interprets an oracle URL ("http://host:port/path?query" → GET →
// {"satisfied": bool}). 5s timeout. Pure helper: touches no store state.
OracleReading readOracle(const std::string& url);

// One search hit: the message plus the room it lives in.
struct SearchHit {
    std::string room;
    Message msg;
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

    // Long-poll: blocks up to timeoutMs until a message with id > since exists.
    // Cap 60 000 ms. Returns every message after since (may be empty on timeout).
    std::vector<Message> waitMessages(const std::string& room, long long since,
                                      long long timeoutMs);

    // Case-insensitive ASCII substring search over content/agent/type. Empty
    // roomFilter = all rooms. Returns newest-first, capped at limit.
    std::vector<SearchHit> search(const std::string& query,
                                  const std::string& roomFilter, int limit);

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

    // Tasks. All throw std::runtime_error on unknown room / bad state.
    // Every transition records a "task"-type message into the room stream.
    Task taskCreate(const std::string& room, const std::string& title,
                    const std::string& detail, const std::string& creator,
                    bool human = false);
    std::vector<Task> tasks(const std::string& room);
    Task taskClaim(const std::string& room, long long id, const std::string& agent);
    Task taskSubmit(const std::string& room, long long id, const std::string& agent,
                    const std::string& evidence);
    // accept=false reopens the task (status back to open, assignee cleared).
    // Verifier must differ from the assignee — that is the evidence gate.
    // human=true tasks (sovereign sign-off) may only be verified by agent
    // "human"; on ordinary tasks "human" bypasses the role gate.
    Task taskVerify(const std::string& room, long long id, const std::string& agent,
                    bool accept);

    // Recompute the hash chain; on failure errOut says where it broke.
    bool verify(const std::string& room, std::string& errOut);

    // Society layer (PROTOCOL.md §Society). All throw std::runtime_error on
    // unknown room / bad state; every transition records goal/gen/role
    // messages into the room stream.
    Society society(const std::string& room);
    // Declares the God Goal — births generation 1 with a genesis task.
    // Throws when a goal is already active (open or proposed). oracle: URL
    // of an achievement oracle (GET it → {"satisfied":bool}); the goal
    // cannot be verified achieved unless the oracle says satisfied.
    void goalSet(const std::string& room, const std::string& text,
                 const std::string& criteria, const std::string& oracle,
                 const std::string& agent);
    // Claims the goal is achieved; needs evidence; sets status "proposed".
    void goalAchieve(const std::string& room, const std::string& agent,
                     const std::string& evidence);
    // Verifies a proposed achievement. Verifier must differ from the achiever
    // and (while roles are registered) hold the reviewer or tester role —
    // agent "human" is the sovereign and always passes that gate. With an
    // oracle declared, accept=true additionally requires a satisfied oracle
    // reading (fetched by the API layer, passed in here).
    // accept=true closes the society; reject reopens the goal (and may birth
    // the next generation if the board drained meanwhile).
    void goalVerify(const std::string& room, const std::string& agent, bool accept,
                    const OracleReading* oracle = nullptr);
    // Gives up on the goal; closes the society as abandoned.
    void goalAbandon(const std::string& room, const std::string& agent,
                     const std::string& reason);
    // Force-retires the active generation and births the next one (for stuck
    // generations whose tasks will never finish).
    void genAdvance(const std::string& room, const std::string& agent,
                    const std::string& note);
    // The recorder's closing duty: distills the active generation into a
    // fixed-budget chronicle (max 4096 chars, last write wins while active,
    // frozen at retirement). The next generation's genesis task starts from
    // chronicles instead of the full history.
    void genChronicle(const std::string& room, const std::string& agent,
                      const std::string& text);
    // Registers an agent under one of the five roles.
    void roleTake(const std::string& room, const std::string& agent,
                  const std::string& role);

    const std::string& dataDir() const { return dataDir_; }

private:
    struct RoomData {
        std::vector<Message> msgs;
        std::vector<Claim> claims;
        long long nextClaimId = 1;
        std::vector<BoardEntry> board;
        std::vector<Task> tasks;
        long long nextTaskId = 1;
        Society soc;
    };

    RoomData& load(const std::string& room);   // caller holds mutex
    Message sayLocked(const std::string& room, const std::string& agent,
                      const std::string& type, const std::string& content,
                      long long ref);          // caller holds mutex
    std::string roomDir(const std::string& room) const;
    void persistClaims(const std::string& room, RoomData& rd);
    void persistBoard(const std::string& room, RoomData& rd);
    void persistTasks(const std::string& room, RoomData& rd);
    void persistSociety(const std::string& room, RoomData& rd);
    void sweepExpired(const std::string& room, RoomData& rd);  // caller holds mutex
    Task* findTask(RoomData& rd, long long id);                // caller holds mutex
    Task createTaskLocked(const std::string& room, RoomData& rd, const std::string& title,
                          const std::string& detail, const std::string& creator, int gen,
                          bool human = false);
    int currentGenLocked(const RoomData& rd) const;
    bool hasVerifyRoleLocked(const RoomData& rd, const std::string& agent) const;
    void retireGenLocked(const std::string& room, RoomData& rd, const std::string& note);
    void birthGenLocked(const std::string& room, RoomData& rd);
    void checkGenDrainLocked(const std::string& room, RoomData& rd);

    std::string dataDir_;
    std::mutex mu_;
    std::condition_variable cv_;  // notified on every new message
    std::map<std::string, RoomData> cache_;
};

// Canonical message hash, PROTOCOL.md §"Message envelope".
std::string messageHash(const std::string& prev, const std::string& room,
                        const Message& m);

}  // namespace gr

#endif  // GR_STORE_H
