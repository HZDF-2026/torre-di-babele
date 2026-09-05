// test_unit.cpp — store-level unit tests (no sockets). Run from repo root:
//   make test
// Uses a temp data dir; each suite gets a fresh RoomStore.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "jsjson.h"
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
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
