// test_unit.cpp — store-level unit tests (no sockets). Run from repo root:
//   make test
// Uses a temp data dir; each suite gets a fresh RoomStore.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "jsjson.h"
#include "protect/auth.h"
#include "protect/integrity.h"
#include "protect/vm.h"
#include "sha256.h"
#include "store.h"
#include "util.h"

static int gFail = 0;
static int gPass = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            gPass++;                                                    \
        } else {                                                        \
            gFail++;                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                       \
    do {                                                                         \
        std::string _a = (a), _b = (b);                                          \
        if (_a == _b) {                                                          \
            gPass++;                                                             \
        } else {                                                                 \
            gFail++;                                                             \
            std::printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__,    \
                        _a.c_str(), _b.c_str());                                  \
        }                                                                        \
    } while (0)

static std::string tmpDir(const char* tag) {
    std::string base = "build/testdata-" + std::string(tag);
    std::error_code ec;
    std::filesystem::remove_all(base, ec);  // fresh state every run
    gr::makeDirs(base);
    return base;
}

static void testSha256() {
    CHECK_EQ_STR(gr::sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ_STR(gr::sha256Hex("abc"),
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ_STR(gr::sha256Hex(std::string(1000000, 'a')),
                 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void testJsonRoundTrip() {
    gr::Json j;
    CHECK(gr::Json::parse("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":\"e\"}}", j));
    CHECK(j.isObj());
    CHECK_EQ_STR(j.get("a")->dump(), "1");
    CHECK_EQ_STR(j.get("b")->dump(), "[true,null,\"x\"]");
    CHECK_EQ_STR(j.dump(), "{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":\"e\"}}");
    gr::Json bad;
    CHECK(!gr::Json::parse("{\"a\":}", bad));
    CHECK(!gr::Json::parse("not json", bad));
}

static void testRoomLifecycle() {
    gr::RoomStore st(tmpDir("lifecycle"));
    CHECK(st.createRoom("task-a"));
    CHECK(!st.createRoom("task-a"));      // duplicate rejected
    CHECK(!st.createRoom("bad name"));   // invalid name rejected
    CHECK(!st.createRoom("../escape"));
    auto rooms = st.rooms();
    CHECK(rooms.size() == 1 && rooms[0] == "task-a");

    auto msgs = st.messages("task-a", 0, 0, "", "");
    CHECK(msgs.size() == 1);              // "room created" genesis message
    CHECK(msgs[0].agent == "server");
    CHECK_EQ_STR(msgs[0].prev, "genesis");

    auto m1 = st.say("task-a", "explore-1", "fact", "found TTL at config.py:42", -1);
    CHECK(m1.id == 2);
    CHECK(m1.prev == msgs[0].hash);
    auto m2 = st.say("task-a", "impl-1", "answer", "use AUTH_TTL env", m1.id);
    CHECK(m2.id == 3);
    CHECK(m2.ref == m1.id);

    // Filters.
    CHECK(st.messages("task-a", 0, 0, "fact", "").size() == 1);
    CHECK(st.messages("task-a", 0, 0, "", "explore-1").size() == 1);
    CHECK(st.messages("task-a", 2, 0, "", "").size() == 1);
    auto tail = st.messages("task-a", 0, 2, "", "");
    CHECK(tail.size() == 2);
    CHECK(tail[0].id == 2 && tail[1].id == 3);  // limit keeps the NEWEST messages

    // Bad type throws.
    bool threw = false;
    try {
        st.say("task-a", "x", "shout", "hi", -1);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // Unknown room throws.
    threw = false;
    try {
        st.say("nope", "x", "say", "hi", -1);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

static void testClaimSemantics() {
    gr::RoomStore st(tmpDir("claims"));
    st.createRoom("t");
    st.say("t", "a1", "say", "hello", -1);

    // Grant.
    auto out = st.claim("t", "a1", {"src/a.cpp", "src/b.cpp"}, 600);
    CHECK(out.ok);
    CHECK(out.claim.id == 1);
    CHECK(st.claims("t").size() == 1);

    // Different agent, same scope -> conflict.
    auto conflict = st.claim("t", "a2", {"src/b.cpp", "src/c.cpp"}, 600);
    CHECK(!conflict.ok);
    CHECK(conflict.conflicts.size() == 1);
    CHECK(conflict.conflicts[0].agent == "a1");

    // Disjoint scope -> fine.
    auto ok2 = st.claim("t", "a2", {"src/d.cpp"}, 600);
    CHECK(ok2.ok);

    // Same agent re-claim overlapping -> renewal, not conflict.
    auto renew = st.claim("t", "a1", {"src/a.cpp"}, 900);
    CHECK(renew.ok);
    CHECK_EQ_STR(renew.note, "renewed");
    CHECK(st.claims("t").size() == 2);  // a1's (renewed) + a2's

    // Release by scope, only own claims.
    CHECK(st.release("t", "a1", 0, "src/a.cpp") == 1);
    CHECK(st.claims("t").size() == 1);
    CHECK(st.release("t", "a1", 0, "src/d.cpp") == 0);  // not his

    // Release by id.
    CHECK(st.release("t", "a2", 2, "") == 1);
    CHECK(st.claims("t").empty());

    // Stream saw claim/veto/release messages.
    auto msgs = st.messages("t", 0, 0, "", "");
    int nClaim = 0, nVeto = 0, nRelease = 0;
    for (const auto& m : msgs) {
        if (m.agent != "server") continue;
        if (m.type == "claim") nClaim++;
        if (m.type == "veto") nVeto++;
        if (m.type == "release") nRelease++;
    }
    CHECK(nClaim == 3);   // grant, a2 grant, renewal
    CHECK(nVeto == 1);     // a2's blocked claim
    CHECK(nRelease == 2);  // a1 release, a2 release
}

static void testClaimTtlExpiry() {
    gr::RoomStore st(tmpDir("ttl"));
    st.createRoom("t");
    st.claim("t", "a1", {"x"}, 1);  // 1 second
    CHECK(st.claims("t").size() == 1);
    // Force expiry without sleeping: claim with ttl 1 then manually wait is
    // slow; instead verify a 0-ttl boundary via the range check (ttl<=0 means
    // default 600). Real expiry is exercised in the integration test.
    auto out = st.claim("t", "a1", {"y"}, 0);
    CHECK(out.ok);
    CHECK(st.claims("t").size() == 2);
}

static void testBoard() {
    gr::RoomStore st(tmpDir("board"));
    st.createRoom("t");
    CHECK(!st.boardGet("t", "decision/x").exists);
    CHECK(st.boardSet("t", "decision/x", "use sqlite", "a1"));
    auto e = st.boardGet("t", "decision/x");
    CHECK(e.exists);
    CHECK_EQ_STR(e.value, "use sqlite");
    CHECK_EQ_STR(e.agent, "a1");
    CHECK(st.boardSet("t", "decision/x", "actually use jsonl", "a2"));
    e = st.boardGet("t", "decision/x");
    CHECK_EQ_STR(e.value, "actually use jsonl");
    CHECK_EQ_STR(e.agent, "a2");
    CHECK(!st.boardSet("t", "bad key!", "v", "a1"));
    CHECK(st.boardAll("t").size() == 1);
}

static void testChainVerify() {
    gr::RoomStore st(tmpDir("chain"));
    st.createRoom("t");
    for (int i = 0; i < 10; i++) st.say("t", "a", "fact", "finding " + std::to_string(i), -1);
    std::string err;
    CHECK(st.verify("t", err));
    CHECK_EQ_STR(err, "");

    // Tamper with the on-disk stream, reload in a fresh store, chain breaks.
    std::string mf = gr::pathJoin(gr::pathJoin(st.dataDir(), "rooms"), "t/messages.jsonl");
    std::string raw = gr::readBytes(mf);
    size_t at = raw.find("finding 3");
    CHECK(at != std::string::npos);
    raw[at + 8] = 'X';  // "finding 3" -> "finding X"
    gr::writeBytes(mf, raw);
    gr::RoomStore st2(st.dataDir());
    CHECK(!st2.verify("t", err));
    CHECK(err.find("hash mismatch") != std::string::npos);
}

static void testReloadPersistence() {
    std::string dir = tmpDir("reload");
    {
        gr::RoomStore st(dir);
        st.createRoom("t");
        st.say("t", "a1", "fact", "persisted fact", -1);
        st.claim("t", "a1", {"s"}, 600);
        st.boardSet("t", "k", "v", "a1");
    }
    gr::RoomStore st(dir);
    auto msgs = st.messages("t", 0, 0, "", "");
    CHECK(msgs.size() == 3);  // room created + fact + claim
    CHECK(st.claims("t").size() == 1);
    CHECK(st.boardGet("t", "k").value == "v");
    // Continue the stream after reload: ids and chain continue.
    auto m = st.say("t", "a2", "say", "after reload", -1);
    CHECK(m.id == 4);
    CHECK(m.prev == msgs.back().hash);
    std::string err;
    CHECK(st.verify("t", err));
}

static void testMessageHashCanonical() {
    gr::Message m;
    m.id = 1;
    m.ts = 1693900000123LL;
    m.agent = "server";
    m.type = "say";
    m.content = "room created";
    m.ref = -1;
    std::string h = gr::messageHash("genesis", "t", m);
    // Deterministic across runs/platforms.
    CHECK_EQ_STR(h, gr::messageHash("genesis", "t", m));
    // Any field change changes the hash.
    m.content = "room created ";
    CHECK(h != gr::messageHash("genesis", "t", m));
}

static void testSocietyLifecycle() {
    gr::RoomStore st(tmpDir("soc"));
    st.createRoom("t");

    // No goal yet: no society.
    gr::Society s = st.society("t");
    CHECK(!s.goal.exists);
    CHECK(s.gens.empty());
    CHECK(s.roles.empty());

    // Declare the god goal: gen 1 is born with a genesis task.
    st.goalSet("t", "ship v1.0", "tag pushed", "", "founder");
    s = st.society("t");
    CHECK(s.goal.exists);
    CHECK_EQ_STR(s.goal.status, "open");
    CHECK_EQ_STR(s.goal.proposer, "founder");
    CHECK(s.gens.size() == 1);
    CHECK_EQ_STR(s.gens[0].status, "active");
    auto ts = st.tasks("t");
    CHECK(ts.size() == 1);  // the genesis task
    CHECK(ts[0].gen == 1);
    CHECK_EQ_STR(ts[0].creator, "server");

    // A second goal cannot be declared while one is active.
    bool threw = false;
    try { st.goalSet("t", "other", "", "", "founder"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // Gen-1 agents finish every task: the board drains with the goal still
    // open → gen 1 retires and gen 2 is born (with its own genesis task).
    st.taskClaim("t", 1, "a1");
    st.taskSubmit("t", 1, "a1", "assessment done");
    st.taskVerify("t", 1, "a2", true);
    s = st.society("t");
    CHECK(s.gens.size() == 2);
    CHECK_EQ_STR(s.gens[0].status, "retired");
    CHECK_EQ_STR(s.gens[0].note, "task board drained (no chronicle submitted)");
    CHECK_EQ_STR(s.gens[1].status, "active");
    ts = st.tasks("t");
    CHECK(ts.size() == 2);
    CHECK(ts[1].gen == 2);

    // Gen-2 does real work and finishes everything (genesis task included) → gen 3.
    gr::Task w = st.taskCreate("t", "work", "do it", "a1");
    CHECK(w.gen == 2);
    st.taskClaim("t", 2, "a1");
    st.taskSubmit("t", 2, "a1", "assessment");
    st.taskClaim("t", w.id, "a1");
    st.taskSubmit("t", w.id, "a1", "plan ready");
    st.taskVerify("t", 2, "a2", true);
    st.taskVerify("t", w.id, "a2", true);
    CHECK(st.society("t").gens.size() == 3);

    // Achievement with evidence → proposed.
    st.goalAchieve("t", "a1", "v1.0 tag pushed, release notes attached");
    s = st.society("t");
    CHECK_EQ_STR(s.goal.status, "proposed");
    CHECK_EQ_STR(s.goal.achiever, "a1");

    // The achiever cannot verify his own claim — evidence gate.
    threw = false;
    try { st.goalVerify("t", "a1", true); } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("evidence gate") != std::string::npos);
    }
    CHECK(threw);

    // Reject: the goal reopens (gen 3's genesis task is still open, no drain).
    st.goalVerify("t", "a2", false);
    s = st.society("t");
    CHECK_EQ_STR(s.goal.status, "open");
    CHECK(s.goal.achiever.empty());
    CHECK(s.gens.size() == 3);

    // Achieve again, accept: society closes, the generation retires.
    st.goalAchieve("t", "a1", "tag v1.0 pushed");
    st.goalVerify("t", "a2", true);
    s = st.society("t");
    CHECK_EQ_STR(s.goal.status, "achieved");
    CHECK_EQ_STR(s.goal.verifier, "a2");
    CHECK(s.goal.closedTs > 0);
    CHECK_EQ_STR(s.gens.back().status, "retired");

    // A closed society can declare a new goal; generation numbering stays
    // monotonic (gen 4), so old-lineage tasks can never block a new gen.
    st.goalSet("t", "ship v2.0", "", "", "founder2");
    s = st.society("t");
    CHECK_EQ_STR(s.goal.status, "open");
    CHECK(s.gens.size() == 4);
    CHECK_EQ_STR(s.gens[3].status, "active");

    // Abandon closes it again; double-close is rejected.
    st.goalAbandon("t", "founder2", "out of scope");
    CHECK_EQ_STR(st.society("t").goal.status, "abandoned");
    threw = false;
    try { st.goalAbandon("t", "founder2", "again"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // The stream recorded the society's messages.
    int nGoal = 0, nGen = 0;
    for (const auto& m : st.messages("t", 0, 0, "", "")) {
        if (m.agent != "server") continue;
        if (m.type == "goal") nGoal++;
        if (m.type == "gen") nGen++;
    }
    CHECK(nGoal >= 7);  // declare, propose, reject, propose, achieve, declare, abandon
    CHECK(nGen >= 8);   // 3 born + 2 drained-retired + closed + born + closed
}

static void testSocietyRoles() {
    gr::RoomStore st(tmpDir("socroles"));
    st.createRoom("t");
    st.goalSet("t", "goal", "", "", "founder");

    // Unknown role rejected; valid roles register; re-take is idempotent.
    bool threw = false;
    try { st.roleTake("t", "a1", "janitor"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    st.roleTake("t", "a1", "executor");
    st.roleTake("t", "a2", "reviewer");
    st.roleTake("t", "a3", "tester");
    st.roleTake("t", "a2", "reviewer");
    auto s = st.society("t");
    CHECK(s.roles.size() == 3);

    // Roles gate task verification: a plain agent may not verify.
    st.taskClaim("t", 1, "a1");
    st.taskSubmit("t", 1, "a1", "done");
    threw = false;
    try { st.taskVerify("t", 1, "a4", true); } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("verify-task bit") != std::string::npos);
    }
    CHECK(threw);
    // The reviewer may.
    st.taskVerify("t", 1, "a2", true);
    CHECK_EQ_STR(st.tasks("t")[0].status, "done");
    CHECK(st.society("t").gens.size() == 2);  // drained → gen 2

    // Roles gate goal verification too — a plain agent and the achiever fail.
    st.goalAchieve("t", "a1", "goal reached");
    threw = false;
    try { st.goalVerify("t", "a4", true); } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("verify-goal bit") != std::string::npos);
    }
    CHECK(threw);
    threw = false;
    try { st.goalVerify("t", "a1", true); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    // The tester verifies; the society closes.
    st.goalVerify("t", "a3", true);
    CHECK_EQ_STR(st.society("t").goal.status, "achieved");
}

static void testSocietyForcedGen() {
    gr::RoomStore st(tmpDir("socgen"));
    st.createRoom("t");
    st.goalSet("t", "goal", "", "", "founder");

    // genAdvance force-retires even with open tasks on the board.
    st.genAdvance("t", "founder", "stuck generation");
    auto s = st.society("t");
    CHECK(s.gens.size() == 2);
    CHECK_EQ_STR(s.gens[0].note, "forced by founder: stuck generation");
    CHECK_EQ_STR(s.gens[1].status, "active");
    CHECK(st.tasks("t").size() == 2);  // both genesis tasks

    // While an achievement proposal is pending, gen advance is refused.
    st.goalAchieve("t", "a1", "done");
    bool threw = false;
    try { st.genAdvance("t", "a1", "nope"); } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("pending") != std::string::npos);
    }
    CHECK(threw);
    // A closed society has no generations to advance.
    st.goalVerify("t", "a2", true);
    threw = false;
    try { st.genAdvance("t", "a1", "nope"); } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("closed") != std::string::npos);
    }
    CHECK(threw);
}

static void testSocietyPersistence() {
    std::string dir = tmpDir("socpersist");
    {
        gr::RoomStore st(dir);
        st.createRoom("t");
        st.goalSet("t", "persist the goal", "criteria here",
                   "http://127.0.0.1:1/oracle", "founder");
        st.roleTake("t", "rev", "reviewer");
        st.genAdvance("t", "founder", "forcing");
        st.genChronicle("t", "rec", "gen 2 distilled");
        st.goalAchieve("t", "a1", "evidence string");
    }
    gr::RoomStore st(dir);
    auto s = st.society("t");
    CHECK(s.goal.exists);
    CHECK_EQ_STR(s.goal.text, "persist the goal");
    CHECK_EQ_STR(s.goal.criteria, "criteria here");
    CHECK_EQ_STR(s.goal.oracle, "http://127.0.0.1:1/oracle");
    CHECK_EQ_STR(s.goal.status, "proposed");
    CHECK_EQ_STR(s.goal.achiever, "a1");
    CHECK_EQ_STR(s.goal.evidence, "evidence string");
    CHECK(s.gens.size() == 2);
    CHECK_EQ_STR(s.gens[1].chronicle, "gen 2 distilled");
    CHECK_EQ_STR(s.gens[1].chronicler, "rec");
    CHECK(s.roles.size() == 1);
    CHECK_EQ_STR(s.roles[0].role, "reviewer");
    CHECK_EQ_STR(s.roles[0].agent, "rev");
    // The pending proposal is still verifiable after reload — but only with a
    // satisfied oracle reading, since the goal declares one.
    gr::OracleReading yes;
    yes.fetched = true;
    yes.satisfied = true;
    yes.raw = "{\"satisfied\":true}";
    bool threw = false;
    try { st.goalVerify("t", "rev", true, nullptr); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    st.goalVerify("t", "rev", true, &yes);
    CHECK_EQ_STR(st.society("t").goal.status, "achieved");
    CHECK_EQ_STR(st.society("t").goal.oracleRead, "{\"satisfied\":true}");
}

static void testOracleGate() {
    gr::RoomStore st(tmpDir("oracle"));
    st.createRoom("t");

    // Credential-shaped goal text is refused at the door: goals are public
    // and hash-chained forever.
    bool threw = false;
    try { st.goalSet("t", "check the account, password=hunter2", "", "", "f"); }
    catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("credentials") != std::string::npos);
    }
    CHECK(threw);
    threw = false;
    try { st.goalSet("t", "balance over threshold", "card 4111111111111111", "", "f"); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
    // The oracle URL itself is scanned too (query-string tokens).
    threw = false;
    try { st.goalSet("t", "balance over threshold", "", "http://x/o?token=abc", "f"); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
    // Non-http oracles are rejected.
    threw = false;
    try { st.goalSet("t", "goal", "", "https://x/oracle", "f"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // A clean goal with an http oracle is accepted and remembered.
    st.goalSet("t", "bank balance over threshold", "",
               "http://127.0.0.1:19999/oracle", "founder");
    auto s = st.society("t");
    CHECK(s.goal.exists);
    CHECK_EQ_STR(s.goal.oracle, "http://127.0.0.1:19999/oracle");

    st.goalAchieve("t", "a1", "statement shows the balance");

    // Accept without any reading is refused — the API layer supplies one.
    threw = false;
    try { st.goalVerify("t", "a2", true, nullptr); } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("oracle unreadable") != std::string::npos);
    }
    CHECK(threw);
    // An unreadable reading (transport failed) is refused the same way.
    gr::OracleReading bad;  // fetched=false
    threw = false;
    try { st.goalVerify("t", "a2", true, &bad); } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("oracle unreadable") != std::string::npos);
    }
    CHECK(threw);
    // A "not satisfied" reading is refused: the verifier is a trigger, not a judge.
    gr::OracleReading no;
    no.fetched = true;
    no.satisfied = false;
    no.raw = "{\"satisfied\":false}";
    threw = false;
    try { st.goalVerify("t", "a2", true, &no); } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("NOT satisfied") != std::string::npos);
    }
    CHECK(threw);
    CHECK_EQ_STR(st.society("t").goal.status, "proposed");

    // Reject (accept=false) needs no oracle at all.
    st.goalVerify("t", "a2", false);
    CHECK_EQ_STR(st.society("t").goal.status, "open");

    // A satisfied reading closes the society and records the reading.
    st.goalAchieve("t", "a1", "statement shows the balance again");
    gr::OracleReading yes;
    yes.fetched = true;
    yes.satisfied = true;
    yes.raw = "{\"satisfied\":true,\"balance\":1500}";
    st.goalVerify("t", "a2", true, &yes);
    s = st.society("t");
    CHECK_EQ_STR(s.goal.status, "achieved");
    CHECK_EQ_STR(s.goal.oracleRead, "{\"satisfied\":true,\"balance\":1500}");
    CHECK(s.goal.oracleReadTs > 0);
}

static void testHumanSovereign() {
    gr::RoomStore st(tmpDir("human"));
    st.createRoom("t");
    st.goalSet("t", "goal", "", "", "founder");

    // A human-gate task is created flagged.
    gr::Task h = st.taskCreate("t", "approve the payout", "sign-off required", "a1", true);
    CHECK(h.human);

    st.taskClaim("t", h.id, "a1");
    st.taskSubmit("t", h.id, "a1", "payout drafted, needs sign-off");

    // Even a registered reviewer cannot sign for the sovereign.
    st.roleTake("t", "rev", "reviewer");
    bool threw = false;
    try { st.taskVerify("t", h.id, "rev", true); } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("human sign-off") != std::string::npos);
    }
    CHECK(threw);

    // The board cannot drain while the human task is unsigned: finish the
    // genesis task and confirm the generation does NOT turn over.
    st.taskClaim("t", 1, "a1");
    st.taskSubmit("t", 1, "a1", "assessment done");
    st.taskVerify("t", 1, "rev", true);
    CHECK(st.society("t").gens.size() == 1);  // still gen 1 — human task blocks

    // The sovereign signs; only NOW the generation turns over.
    st.taskVerify("t", h.id, "human", true);
    CHECK(st.society("t").gens.size() == 2);
    auto ts = st.tasks("t");
    CHECK(ts.size() == 3);  // genesis 1, human task, genesis 2
    CHECK_EQ_STR(ts[1].status, "done");
    CHECK_EQ_STR(ts[1].verifier, "human");

    // The sovereign also passes the division-of-labor gate on ordinary tasks
    // (which are never human-flagged).
    gr::Task o = st.taskCreate("t", "ordinary task", "", "a1");
    CHECK(!o.human);
    st.taskClaim("t", o.id, "a1");
    st.taskSubmit("t", o.id, "a1", "done");
    st.taskVerify("t", o.id, "human", true);
    CHECK_EQ_STR(st.tasks("t")[o.id - 1].status, "done");
}

static void testChronicle() {
    gr::RoomStore st(tmpDir("chronicle"));
    st.createRoom("t");
    st.goalSet("t", "goal", "", "", "founder");

    // Bounds: empty and over-budget texts are refused.
    bool threw = false;
    try { st.genChronicle("t", "rec", ""); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { st.genChronicle("t", "rec", std::string(4097, 'x')); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // The recorder distills gen 1 while it is still active.
    st.genChronicle("t", "recorder-1", "gen 1: tried X, worked; Y remains open.");
    auto s = st.society("t");
    CHECK_EQ_STR(s.gens[0].chronicle, "gen 1: tried X, worked; Y remains open.");
    CHECK_EQ_STR(s.gens[0].chronicler, "recorder-1");
    CHECK(s.gens[0].chronicleTs > 0);

    // Last write wins while the generation is active.
    st.genChronicle("t", "recorder-1", "gen 1 (rev 2): X done; Y still open.");
    CHECK_EQ_STR(st.society("t").gens[0].chronicle, "gen 1 (rev 2): X done; Y still open.");

    // Drain: gen 1 retires with a plain note, and gen 2's genesis task starts
    // from the chronicles instead of a full history re-read.
    st.taskClaim("t", 1, "a1");
    st.taskSubmit("t", 1, "a1", "assessment");
    st.taskVerify("t", 1, "a2", true);
    s = st.society("t");
    CHECK(s.gens.size() == 2);
    CHECK_EQ_STR(s.gens[0].note, "task board drained");
    CHECK_EQ_STR(s.gens[0].chronicle, "gen 1 (rev 2): X done; Y still open.");  // frozen at retirement
    auto ts = st.tasks("t");
    CHECK_EQ_STR(ts[1].title, "Generation 2: assess and plan");
    CHECK(ts[1].detail.find("chronicles") != std::string::npos);
    CHECK(ts[1].detail.find("do NOT re-read") != std::string::npos);

    // A generation that leaves no chronicle gets the fallback briefing.
    st.genAdvance("t", "a1", "force");
    ts = st.tasks("t");
    CHECK(ts.size() == 3);
    CHECK(ts[2].detail.find("no chronicle") != std::string::npos);

    // The chronicle persists across reload.
    std::string dir = tmpDir("chronpersist");
    {
        gr::RoomStore p(dir);
        p.createRoom("t");
        p.goalSet("t", "goal", "", "", "f");
        p.genChronicle("t", "rec", "remembered");
    }
    gr::RoomStore p2(dir);
    CHECK_EQ_STR(p2.society("t").gens[0].chronicle, "remembered");
    CHECK_EQ_STR(p2.society("t").gens[0].chronicler, "rec");
}

static void testAgentRegistry() {
    std::string dir = tmpDir("agents");
    {
        gr::RoomStore st(dir);
        st.agentRegister("alice", "correct horse battery");
        st.agentRegister("bob", "staple pony nine");
        auto names = st.agentList();
        CHECK(names.size() == 2);
        CHECK_EQ_STR(names[0], "alice");
        CHECK_EQ_STR(names[1], "bob");
        CHECK(st.agentCheck("alice", "correct horse battery"));
        CHECK(!st.agentCheck("alice", "wrong key"));
        CHECK(!st.agentCheck("unknown", "correct horse battery"));
        CHECK(!st.agentCheck("alice", ""));  // empty key never matches
        bool threw = false;
        try { st.agentRegister("alice", "another key"); } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try { st.agentRegister("carol", "short"); } catch (const std::exception&) {
            threw = true;  // key under 8 chars refused
        }
        CHECK(threw);
    }
    // The registry persists across reloads (hashes only — never keys).
    gr::RoomStore st2(dir);
    CHECK(st2.agentCheck("bob", "staple pony nine"));
    CHECK(!st2.agentCheck("bob", "nope"));
}

static void testChamber() {
    gr::RoomStore st(tmpDir("chamber"));
    st.agentRegister("founder", "founder-key-123");
    st.agentRegister("member2", "member2-key-123");
    st.agentRegister("outsider", "outsider-key1");
    st.createRoom("secret", true, "founder");

    auto meta = st.roomMeta("secret");
    CHECK(meta.chamber);
    CHECK_EQ_STR(meta.creator, "founder");
    CHECK(meta.members.size() == 1);
    CHECK_EQ_STR(meta.members[0], "founder");

    // Only members see the chamber.
    CHECK(st.canView("secret", "founder"));
    CHECK(!st.canView("secret", "outsider"));
    CHECK(!st.canView("secret", ""));
    auto vis = st.visibleRooms("outsider");
    CHECK(vis.empty());
    vis = st.visibleRooms("founder");
    CHECK(vis.size() == 1 && vis[0] == "secret");

    // Membership: only members may add; targets must be registered.
    st.memberAdd("secret", "founder", "member2");
    CHECK(st.canView("secret", "member2"));
    bool threw = false;
    try { st.memberAdd("secret", "outsider", "founder"); } catch (const std::exception&) {
        threw = true;  // outsider is not a member
    }
    CHECK(threw);
    threw = false;
    try { st.memberAdd("secret", "founder", "ghost"); } catch (const std::exception&) {
        threw = true;  // ghost is not registered
    }
    CHECK(threw);
    st.memberAdd("secret", "founder", "member2");  // idempotent
    CHECK(st.roomMeta("secret").members.size() == 2);

    // A chamber without a creator is refused.
    threw = false;
    try { st.createRoom("bad", true, ""); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(!st.roomExists("bad"));
}

static void testCustomPosts() {
    gr::RoomStore st(tmpDir("posts"));
    st.createRoom("t");
    st.goalSet("t", "goal", "", "", "founder");

    // The five presets are always in effect.
    auto ps = st.posts("t");
    CHECK(ps.size() == 5);

    // A custom post with both verify bits.
    st.postDefine("t", "auditor", true, true, "qwen-max", "founder");
    st.postDefine("t", "scribe", false, false, "", "founder");
    ps = st.posts("t");
    CHECK(ps.size() == 7);
    const gr::PostDef* auditor = nullptr;
    for (const auto& p : ps)
        if (p.name == "auditor") auditor = &p;
    CHECK(auditor != nullptr);
    CHECK(auditor->canVerifyTask && auditor->canVerifyGoal);
    CHECK_EQ_STR(auditor->model, "qwen-max");

    // Reserved / duplicate names are refused.
    bool threw = false;
    try { st.postDefine("t", "reviewer", true, true, "", "x"); } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try { st.postDefine("t", "auditor", false, false, "", "x"); } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try { st.postDefine("t", "bad name!", false, false, "", "x"); } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // An unknown post cannot be taken; the custom one can, and its bits gate.
    threw = false;
    try { st.roleTake("t", "a1", "nope"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    st.roleTake("t", "a1", "executor");
    st.roleTake("t", "a2", "auditor");
    st.roleTake("t", "a3", "scribe");

    st.taskClaim("t", 1, "a1");
    st.taskSubmit("t", 1, "a1", "done");
    threw = false;  // scribe carries no verify-task bit
    try { st.taskVerify("t", 1, "a3", true); } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("verify-task bit") != std::string::npos);
    }
    CHECK(threw);
    st.taskVerify("t", 1, "a2", true);  // auditor may

    st.goalAchieve("t", "a1", "done");
    threw = false;
    try { st.goalVerify("t", "a3", true); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    st.goalVerify("t", "a2", true);
    CHECK_EQ_STR(st.society("t").goal.status, "achieved");
}

static void testPopulation() {
    gr::RoomStore st(tmpDir("population"));
    st.createRoom("t");

    // Fresh room: nobody active.
    auto pop = st.population("t");
    CHECK(pop.active.empty());

    // A say makes the speaker active (30-minute window).
    st.say("t", "a1", "fact", "found something", -1);
    st.say("t", "a2", "fact", "me too", -1);
    pop = st.population("t");
    CHECK(pop.active.size() == 2);

    // Active claims count even for silent agents.
    st.claim("t", "a3", {"src/x.cpp"}, 600);
    pop = st.population("t");
    CHECK(pop.active.size() == 3);

    // In-flight tasks count.
    st.taskCreate("t", "job", "", "server");
    st.taskClaim("t", 1, "a4");
    pop = st.population("t");
    CHECK(pop.active.size() == 4);

    // Done tasks do not.
    st.taskSubmit("t", 1, "a4", "ev");
    st.taskVerify("t", 1, "a5", true);
    pop = st.population("t");
    CHECK(pop.active.size() == 3);

    // "server" is never counted.
    st.say("t", "server", "say", "housekeeping", -1);
    pop = st.population("t");
    CHECK(pop.active.size() == 3);
}

static void testReportModes() {
    gr::RoomStore st(tmpDir("report"));
    st.createRoom("t");
    st.goalSet("t", "ship the parser", "all tests green", "", "founder");
    st.roleTake("t", "rec", "recorder");
    st.roleTake("t", "a3", "reviewer");

    // Board: #1 genesis claimed, #2 done, #3/#4 open, #5 awaiting human.
    st.taskClaim("t", 1, "a1");
    st.taskCreate("t", "fix lexer", "", "founder");
    st.taskCreate("t", "wire oracle", "", "founder");
    st.taskCreate("t", "write docs", "", "founder");
    st.taskCreate("t", "sign checklist", "", "founder", true);
    st.taskClaim("t", 2, "a2");
    st.taskSubmit("t", 2, "a2", "lexer green");
    st.taskVerify("t", 2, "a3", true);

    st.say("t", "a1", "fact", "lexer is recursive-descent", -1);
    st.say("t", "a2", "fact", "oracle answers satisfied", -1);
    st.say("t", "a1", "ask", "which grammar do we target?", -1);
    auto deadlineAsk = st.say("t", "a3", "ask", "deadline?", -1);
    st.say("t", "founder", "answer", "tomorrow", deadlineAsk.id);
    st.genChronicle("t", "rec", "phase 1 | lexer done | verdict: ok | 3 tests");

    auto has = [](const std::string& hay, const std::string& needle) {
        return hay.find(needle) != std::string::npos;
    };

    // Bad mode / unknown room refuse.
    bool threw = false;
    try { st.genReport("t", "nonsense"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { st.genReport("nope", "hzdf"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // hzdf: phase history | laws (chronicle verbatim) | open questions.
    std::string r = st.genReport("t", "hzdf");
    CHECK(has(r, "== HZDF-2026 report · room t =="));
    CHECK(has(r, "goal [open]: ship the parser"));
    CHECK(has(r, "criteria: all tests green"));
    CHECK(has(r, "generations: 1 (G1 active)"));
    CHECK(has(r, "tasks: 1 done / 1 in-flight / 3 open · facts: 2 · unanswered asks: 1"));
    CHECK(has(r, "== phase history =="));
    CHECK(has(r, "G1 active  tasks 1/5"));
    CHECK(has(r, "== latest chronicle (verbatim — laws inside) =="));
    CHECK(has(r, "phase 1 | lexer done | verdict: ok | 3 tests"));
    CHECK(has(r, "== open questions =="));
    CHECK(has(r, "- the goal remains open"));
    CHECK(has(r, "- open task #3 wire oracle"));
    CHECK(has(r, "ask #"));

    // company: TL;DR / KPIs / by post / risks / next steps.
    r = st.genReport("t", "company");
    CHECK(has(r, "== company briefing · t =="));
    CHECK(has(r, "TL;DR: goal [open] — ship the parser."));
    CHECK(has(r, "KPIs: facts 2 · asks 1 answered / 1 open"));
    CHECK(has(r, "by post:"));
    CHECK(has(r, "a2 — (no post): 1 done, 0 in-flight"));
    CHECK(has(r, "a3 — reviewer: 0 done, 0 in-flight"));
    CHECK(has(r, "risks & blockers:"));
    CHECK(has(r, "human sign-off pending: #5 sign checklist"));
    CHECK(has(r, "next steps:"));
    CHECK(has(r, "- #3 wire oracle"));

    // feudal: one memorial, 国祚/朝代/军情/贡赋/民生/请旨/臣工.
    r = st.genReport("t", "feudal");
    CHECK(has(r, "奏为恭报 t 一域军政民情折（第1朝）"));
    CHECK(has(r, "一、国祚。国是「ship the parser」今犹悬。"));
    CHECK(has(r, "历1朝，今第1朝当值"));
    CHECK(has(r, "一、军情（任务战况）。计5件：已克1，交战1，未动3。"));
    CHECK(has(r, "【交战·a1领兵】"));
    CHECK(has(r, "一、贡赋（验讫之功）。"));
    CHECK(has(r, "a2贡，a3验讫"));
    CHECK(has(r, "一、民生（事实计2条，近3条）。"));
    CHECK(has(r, "一、请旨（待圣裁）。"));
    CHECK(has(r, "须 human 圣裁"));
    CHECK(has(r, "如蒙圣鉴，谨此奏闻。"));
}

// ---- protection layer (BVM) ---------------------------------------------------

using gr::BVM_ADDI;
using gr::BVM_ADD;
using gr::BVM_HALT;
using gr::BVM_JMP;
using gr::BVM_LD8;
using gr::BVM_LOADI;
using gr::BVM_SHL;
using gr::BVM_ST8;
using gr::BVM_SYS;
using gr::BVM_SYS_SHA256HEX;

static std::vector<uint8_t> bvmIns(uint8_t op, uint8_t dst, uint8_t s1, uint8_t s2,
                                   uint32_t imm) {
    std::vector<uint8_t> v;
    v.push_back(op);
    v.push_back(dst);
    v.push_back(s1);
    v.push_back(s2);
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(imm >> (8 * i)));
    return v;
}

static std::vector<uint8_t> concat(std::vector<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

static void testBvmArithmetic() {
    // r1 = 21; r2 = 1; r3 = r1 << r2 = 42; exit r3
    std::vector<uint8_t> code = concat({
        bvmIns(BVM_LOADI, 1, 0, 0, 21),
        bvmIns(BVM_LOADI, 2, 0, 0, 1),
        bvmIns(BVM_SHL, 3, 1, 2, 0),
        bvmIns(BVM_HALT, 3, 0, 0, 0),
    });
    std::vector<uint8_t> data(16);
    CHECK(gr::bvmRun(code.data(), code.size(), data.data(), data.size(), 1000) == 42);

    // ST8 then LD8 roundtrip, plus ADDI.
    code = concat({
        bvmIns(BVM_LOADI, 1, 0, 0, 7),
        bvmIns(BVM_LOADI, 4, 0, 0, 4),         // r4 = 4 (address)
        bvmIns(BVM_ST8, 4, 1, 0, 0),           // data[r4] = 7
        bvmIns(BVM_LOADI, 5, 0, 0, 4),
        bvmIns(BVM_LD8, 6, 5, 0, 0),           // r6 = data[4]
        bvmIns(BVM_ADDI, 7, 6, 0, 100),         // r7 = 107
        bvmIns(BVM_HALT, 7, 0, 0, 0),
    });
    CHECK(gr::bvmRun(code.data(), code.size(), data.data(), data.size(), 1000) == 107);

    // Unknown opcode -> fail closed.
    code = concat({bvmIns(0xEE, 0, 0, 0, 0)});
    CHECK(gr::bvmRun(code.data(), code.size(), data.data(), data.size(), 1000) == -1);

    // Infinite loop -> step budget exhausted -> fail closed.
    code = concat({bvmIns(BVM_JMP, 0, 0, 0, 0)});
    CHECK(gr::bvmRun(code.data(), code.size(), data.data(), data.size(), 100) == -1);

    // Out-of-bounds LD8 -> fail closed.
    code = concat({bvmIns(BVM_LOADI, 1, 0, 0, 999),
                   bvmIns(BVM_LD8, 2, 1, 0, 0),
                   bvmIns(BVM_HALT, 2, 0, 0, 0)});
    CHECK(gr::bvmRun(code.data(), code.size(), data.data(), data.size(), 1000) == -1);

    // Truncated code -> fail closed.
    code = concat({bvmIns(BVM_LOADI, 1, 0, 0, 1)});
    code.pop_back();
    CHECK(gr::bvmRun(code.data(), code.size(), data.data(), data.size(), 1000) == -1);
}

static void testBvmSha256Syscall() {
    // SYS_SHA256HEX of data[16..18] ("abc") must land at data[28..91].
    std::vector<uint8_t> code = concat({
        bvmIns(BVM_LOADI, 1, 0, 0, 16),
        bvmIns(BVM_LOADI, 2, 0, 0, 3),
        bvmIns(BVM_SYS, 3, 1, 2, BVM_SYS_SHA256HEX),
        bvmIns(BVM_HALT, 3, 0, 0, 0),
    });
    std::vector<uint8_t> data(128, 0);
    std::memcpy(data.data() + 16, "abc", 3);
    CHECK(gr::bvmRun(code.data(), code.size(), data.data(), data.size(), 1000) == 0);
    CHECK(std::memcmp(data.data() + 28, gr::sha256Hex("abc").data(), 64) == 0);
}

static void testBvmAuth() {
    std::vector<std::pair<std::string, std::string>> reg = {
        {"alice", gr::sha256Hex("correct horse battery")},
        {"bob", gr::sha256Hex("staple pony nine")},
    };
    CHECK(gr::bvmAgentCheck(reg, "alice", "correct horse battery"));
    CHECK(gr::bvmAgentCheck(reg, "bob", "staple pony nine"));
    CHECK(!gr::bvmAgentCheck(reg, "alice", "wrong key"));
    CHECK(!gr::bvmAgentCheck(reg, "unknown", "correct horse battery"));
    CHECK(!gr::bvmAgentCheck(reg, "alice", ""));
    CHECK(!gr::bvmAgentCheck(reg, "", "key"));
    // malformed registry hash -> fail closed
    std::vector<std::pair<std::string, std::string>> bad = {{"alice", "short"}};
    CHECK(!gr::bvmAgentCheck(bad, "alice", "correct horse battery"));
    // empty registry never authenticates
    CHECK(!gr::bvmAgentCheck({}, "alice", "correct horse battery"));
    // long names still match byte-for-byte through the VM
    std::string longName(64, 'x');
    reg.push_back({longName, gr::sha256Hex("long agent key")});
    CHECK(gr::bvmAgentCheck(reg, longName, "long agent key"));
}

static void testSelfHash() {
    std::string h = gr::selfSha256();
    CHECK(h.size() == 64);
    bool hex = h.find_first_not_of("0123456789abcdef") == std::string::npos;
    CHECK(hex);
}

int main() {
    testSha256();
    testJsonRoundTrip();
    testRoomLifecycle();
    testClaimSemantics();
    testClaimTtlExpiry();
    testBoard();
    testChainVerify();
    testReloadPersistence();
    testMessageHashCanonical();
    testSocietyLifecycle();
    testSocietyRoles();
    testSocietyForcedGen();
    testSocietyPersistence();
    testOracleGate();
    testHumanSovereign();
    testChronicle();
    testAgentRegistry();
    testChamber();
    testCustomPosts();
    testPopulation();
    testReportModes();
    testBvmArithmetic();
    testBvmSha256Syscall();
    testBvmAuth();
    testSelfHash();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
