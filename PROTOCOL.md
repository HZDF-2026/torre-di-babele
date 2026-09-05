# greenroom protocol v1

*The room behind the stage. Every actor meets there before the curtain.*

greenroom is a local chat room for the sub-agents of TRAE Code / TRAE Work.
The problem it solves: sub-agents spawn, work, and die in isolation. One
agent's findings are re-discovered (re-read, re-derived, re-paid) by the next.
Two agents edit the same file and collide. greenroom gives them one shared
place to coordinate — a blackboard, not a message bus.

One binary, three faces:

```
greenroom serve   the room server: localhost HTTP, append-only rooms,
                  claim leases, a shared blackboard, a SHA-256 hash chain
greenroom <cmd>   the CLI: what sub-agents speak (say / listen / claim / board)
greenroom mcp     an MCP stdio server: what the parent agent speaks
```

Everything goes through `serve`. CLI and MCP are HTTP clients of it, so there
is exactly one source of truth per room.

## Concepts

| concept | meaning |
|---|---|
| **room** | one task, one shared context. Name: `[a-z0-9-]`, e.g. `refactor-auth`. Rooms are directories under the data dir; they persist across server restarts. |
| **agent** | a participant's declared name, e.g. `explore-1`, `impl-2`, `lead`. Free text; your name is your reputation. |
| **message** | an append-only line in the room's stream, hash-chained to the previous one. |
| **claim** | a lease: "I own `src/a.cpp` for the next 10 minutes." Conflicts are rejected with 409 and broadcast as `veto`. |
| **board** | a room-level key/value blackboard: decisions, progress, the map of who-touched-what. |

## Message types

| type | who sends | semantics |
|---|---|---|
| `say` | any agent | free-form coordination chatter |
| `plan` | any agent | announce intent before acting ("I will rename the retry helper across 6 files") |
| `fact` | any agent | publish a reusable finding, with provenance in the text: `net/retry.go:45 — retries live here, 3 attempts, backoff 200ms` |
| `ask` | any agent | a question to the room |
| `answer` | any agent | reply to an `ask`; must set `ref` to the ask's id |
| `claim` | server | recorded when a claim succeeds (scope + ttl in content) |
| `release` | server | recorded when a claim is released or expires |
| `veto` | server | recorded when a claim was rejected for conflict — names both agents and the overlapping scope |
| `done` | any agent | task finished or agent leaving; summarize what holds now |

Server-sent messages have `agent: "server"`. They are part of the stream so
that a late-joining agent reads the full coordination history with one
`listen --since 0`.

## Message envelope

```json
{"id":7,"ts":1693900012233,"agent":"explore-1","type":"fact",
 "content":"net/retry.go:45 — retries live here","ref":null,
 "prev":"<sha256 of message 6>","hash":"<sha256 of this message>"}
```

- `id` — per-room, monotonically increasing, starts at 1.
- `ts` — unix epoch milliseconds.
- `ref` — id of the referenced message, or null.
- Hash chain: `hash = sha256(prev + "\n" + room + "\n" + id + "\n" + ts + "\n" + agent + "\n" + type + "\n" + content + "\n" + ref-or-"-" + "\n")`. The first message of a room has `prev: "genesis"`. `greenroom verify ROOM` recomputes the chain; tampering with any line breaks the chain at that point.

## Claims (the collision guard)

- `claim ROOM scope... [--ttl 600]` — scope entries are file paths or task labels. Success records a `claim` message.
- A new claim **conflicts** when any scope entry intersects an active claim **held by a different agent**. The server answers HTTP 409, records a `veto` message naming both agents, and takes no lease.
- Same agent re-claiming overlapping scope is a renewal (extends the TTL), not a conflict.
- TTL defaults to 600 s. Expired claims are dropped lazily (on read or on new claims) and recorded as `release` with reason `expired`.
- `release ROOM --id N` or `release ROOM --scope S` gives the lease back early.

Rules of engagement for agents:

1. **Claim before you touch.** If you lost the 409, wait, or coordinate via `say`/`ask`, or work elsewhere.
2. **Facts are money.** Every file you had to open is a fact another agent should never have to open again. Publish with `path:line — finding`.
3. **Read before you write code**: `listen --since 0` first; the board may already hold the answer.
4. **Close the loop**: `done` when you leave, `release` what you hold.

## Blackboard keys (convention)

Free-form, but these prefixes are established:

| prefix | example | use |
|---|---|---|
| `decision/*` | `decision/db-choice` | an agreed choice, with rationale |
| `progress/*` | `progress/impl-2` | what an agent is doing, updated in place |
| `map/*` | `map/net-retry` | "topic → where it lives" — findings index |
| `answer/*` | `answer/auth-ttl` | canonical answer to a question |

## HTTP API (v1)

All JSON. `serve` binds 127.0.0.1 by default.

```
GET  /v1/status                                 server info
GET  /v1/rooms                                  room list
POST /v1/rooms                                  {"name": "..."} create (409 if exists)

GET  /v1/rooms/{room}/messages?since=N&limit=M&type=T&agent=A
POST /v1/rooms/{room}/say                      {"agent","type","content","ref"?}
POST /v1/rooms/{room}/claim                    {"agent","scope":[..],"ttl_s"?}
POST /v1/rooms/{room}/release                   {"agent","claim_id"?,"scope"?}
GET  /v1/rooms/{room}/claims                   active claims (lazy TTL sweep)
GET  /v1/rooms/{room}/board                    all entries
GET  /v1/rooms/{room}/board/{key}
PUT  /v1/rooms/{room}/board/{key}               {"agent","value"}
GET  /v1/rooms/{room}/verify                   chain check
```

- Unknown room → 404. Malformed body → 400. Claim conflict → 409 with the conflicting claims in the body.
- `listen` maps to `GET messages`; `--follow` is client-side polling (`since` = last seen id, 1 s interval).
- `limit` keeps the NEWEST M messages (chat semantics: the tail), not the oldest.

## CLI

```
greenroom serve  [--port 7788] [--data DIR] [--bind 127.0.0.1]
greenroom status
greenroom rooms
greenroom create ROOM
greenroom say    ROOM TYPE CONTENT [--agent A] [--ref N]
greenroom listen ROOM [--since N] [--limit M] [--follow] [--agent A]
greenroom claim  ROOM SCOPE... [--ttl 600] [--agent A]
greenroom release ROOM (--id N | --scope S) [--agent A]
greenroom claims ROOM
greenroom board  get ROOM KEY
greenroom board  set ROOM KEY VALUE
greenroom verify ROOM
greenroom mcp
```

- `--agent` defaults to `$GREENROOM_AGENT`, else `anon`.
- Server address: `$GREENROOM_URL`, else `http://127.0.0.1:7788`.
- `listen` prints one line per message: `#id ts agent type [->ref] content`, newest last. `--agent` filters.

## MCP tools (stdio, for the parent agent)

`greenroom mcp` speaks JSON-RPC 2.0 on stdio (newline-delimited) and proxies to
`serve`. Tools: `greenroom_protocol`, `greenroom_status`, `greenroom_rooms`,
`greenroom_create_room`, `greenroom_say`, `greenroom_listen`,
`greenroom_claim`, `greenroom_release`, `greenroom_claims`,
`greenroom_board_get`, `greenroom_board_set`, `greenroom_verify`.

`greenroom_protocol` returns the sub-agent briefing below — call it, and paste
the text into every sub-agent prompt you spawn.

## Sub-agent briefing (the spawn template)

> You are agent **{name}** working in greenroom room **{room}** — a shared
> workspace for parallel agents. Protocol, in order:
>
> 1. **Sync first**: run `greenroom listen {room} --since 0`. Everything other
>    agents found is already there — do not re-read files they reported as
>    facts; trust and build on them.
> 2. **Claim before touching**: `greenroom claim {room} <path-or-task> --agent {name}`
>    for every file or task you will modify. HTTP 409 means someone owns it:
>    do not fight, pick other work or ask in the room.
> 3. **Publish as you go**: every non-trivial finding becomes
>    `greenroom say {room} fact "<path>:<line> — <finding>" --agent {name}`.
>    Facts are the room's currency; yours save the next agent a read.
> 4. **Ask, don't stall**: `greenroom say {room} ask "<question>" --agent {name}`;
>    answer others with `answer` and `--ref <id>`.
> 5. **Leave clean**: `greenroom release {room} --scope <s> --agent {name}`,
>    then `greenroom say {room} done "<one-line summary>" --agent {name}`.
>
> Claims expire after their TTL — if your work takes longer, re-claim. The room
> is hash-chained and audited; say what you did, do what you said.

## Storage layout

```
<datadir>/rooms/<room>/messages.jsonl   one message per line, append-only
<datadir>/rooms/<room>/claims.json      active leases (rewritten on change)
<datadir>/rooms/<room>/board.json       blackboard KV (rewritten on change)
```

Plain files on purpose: human-readable, git-friendly, easy to archive with a
session. `messages.jsonl` never shrinks; `claims.json` / `board.json` are
current-state snapshots.

## Portability

C++17, standard library only, no third-party deps. One thread per connection;
one mutex around the store — sub-agent traffic is tiny, simplicity wins.

- Windows 11 x64: `build.ps1` (MinGW-w64, links `ws2_32`)
- Linux ARM64: `make` (g++ ≥ 9) — build on the target, no cross toolchain needed

## Design rationale

- **HTTP, not files, not pipes**: sub-agents have a shell; `curl` works on both
  platforms — but a dedicated CLI beats curl on Windows (PowerShell's `curl` is
  an alias for `Invoke-WebRequest` with quoting traps). One binary serves both.
- **CLI for sub-agents, MCP for the parent**: sub-agents are sandboxed turn-based
  workers; the parent agent is the only long-lived orchestrator that can hold an
  MCP connection.
- **Append-only + hash chain**: coordination history is evidence. When two
  agents disagree about "who said what", the chain settles it — same lesson as
  the poc-evidence project.
- **Claims with TTL**: liveness over correctness — a crashed agent's lease must
  heal itself, or the room deadlocks.
- **Blackboard, not just chat**: facts scroll away in a stream; the board holds
  current truth (`decision/*`, `map/*`) with stable keys.
