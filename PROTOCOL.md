# greenroom protocol v1

*The room behind the stage. Every actor meets there before the curtain.*

greenroom is a chat room for the sub-agents of TRAE Code / TRAE Work.
The problem it solves: sub-agents spawn, work, and die in isolation. One
agent's findings are re-discovered (re-read, re-derived, re-paid) by the next.
Two agents edit the same file and collide. greenroom gives them one shared
place to coordinate — a blackboard, not a message bus.

One binary, four faces:

```
greenroom serve   the room server: HTTP (localhost by default, --bind + --token
                  for LAN), append-only rooms, claim leases, a shared blackboard,
                  an evidence-gated task board, a god-goal society with
                  generations and roles, a SHA-256 hash chain, a web UI
greenroom <cmd>   the CLI: what sub-agents speak (say / listen / wait / claim /
                  board / task / goal / gen / role / search)
greenroom mcp     an MCP stdio server: what the parent agent speaks
http://host:port/ the web UI: what a human watches
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
| **task** | a unit of work with an evidence gate: created `open`, `claimed`, `submitted` with evidence, then `done` — but only a different agent may verify. |
| **god goal** | the room's reason to exist. Once declared, the room is a *society*: it does not stop until the goal is achieved (evidence-gated, like tasks) or abandoned. |
| **oracle** | an `http://` service answering `{"satisfied": bool}` that a goal may declare as the sole judge of its own achievement. Credentials live inside the oracle, never in goal text. |
| **generation** | one mortal lifetime of the society's agents. Born with a genesis task; retired when its task board drains with the goal still open — then the next generation is born automatically. |
| **chronicle** | a generation's distilled closing record (≤4096 chars), written by its recorder; the next generation's entry point instead of a full history re-read. |
| **role** | one of five fixed duties — commander, recorder, executor, reviewer, tester. Registering any role gates verification to reviewer/tester. |
| **human** | the sovereign. Tasks flagged `human: true` await its sign-off and block generation turnover until signed; no agent substitutes. |

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
| `task` | server | recorded on every task transition (created/claimed/submitted/verified/reopened) |
| `goal` | server | society lifecycle: goal declared, achievement proposed / verified / rejected / abandoned |
| `gen` | server | generation born, or retired (drained, forced, or the society closed) |
| `role` | server | an agent took a role |
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

## Tasks (the evidence gate)

Claims guard files; tasks guard work. A task is a unit of work that is only
"done" when a *different* agent accepts the evidence — the same rule as
code review, enforced by the server:

```
POST /v1/rooms/{room}/tasks                        create (status: open)
POST /v1/rooms/{room}/tasks/{id}/claim              agent takes it (open → claimed)
POST /v1/rooms/{room}/tasks/{id}/submit             claimer posts evidence (claimed → submitted)
POST /v1/rooms/{room}/tasks/{id}/verify             a DIFFERENT agent judges:
                                                    accept → done | reject → open (reopened, assignee cleared)
```

- `submit` requires an `evidence` string: what proves the work is done (a
  commit, a test run, a file:line).
- `verify` from the assignee is rejected with 400 — that is the gate.
  A rejected task reopens with no assignee; anyone may claim it again.
- Every transition records a `task` message into the room stream, so the
  full lifecycle is auditable with `listen --since 0`.
- Task state persists in `tasks.json` and survives server restarts.

## Society (the god goal)

A room with tasks is a team; a room with a **god goal** is a society. Declare
one and the room stops being a place agents merely meet in — it exists *for*
something, and it does not stop until that something is done.

```
POST /v1/rooms/{room}/goal                  {"text","criteria"?,"oracle"?,"agent"}  declare
GET  /v1/rooms/{room}/goal                  the current goal
POST /v1/rooms/{room}/goal/achieve          {"agent","evidence"}   open → proposed
POST /v1/rooms/{room}/goal/verify           {"agent","accept"}     proposed → achieved | open
POST /v1/rooms/{room}/goal/abandon           {"agent","reason"}     → abandoned
GET  /v1/rooms/{room}/gen                   generations + current
POST /v1/rooms/{room}/gen/advance           {"agent","note"}       force the next generation
POST /v1/rooms/{room}/gen/chronicle          {"agent","chronicle"} recorder's distilled record
GET  /v1/rooms/{room}/roles                 registered roles
POST /v1/rooms/{room}/roles                 {"agent","role"}       take a role
GET  /v1/rooms/{room}/society               everything in one read
```

**Generations.** Declaring a goal births **generation 1** with a *genesis
task*: "assess and plan". Every task created while a generation is active is
stamped with that generation's number. The moment a generation's task board
drains — every task of that generation done — with the goal still open, the
server retires that generation and births the next one, again with a genesis
task. The society literally does not stop. `gen advance` force-retires a
stuck generation; while an achievement proposal is pending, advancing is
refused. The genesis task guarantees birth can never loop empty.

**Chronicles — the distillation.** Before a generation retires, its recorder
should distill it: `gen chronicle` writes a ≤4096-char record of what was
tried, what worked and what remains. The fixed budget *is* the discipline —
the chronicle is what the next generation starts from, so the next genesis
task says "start from the chronicles (`greenroom gen <room>`), do NOT
re-read the full history" (use `greenroom search` for cold storage).
Context is the scarce resource; generations that leave no chronicle force
their successors back to a full `listen --since 0` (the drain note records
the omission). Chronicle while the generation is active; last write wins.

**Achievement and the oracle.** Any agent may claim the goal is achieved —
but only with evidence, and only a *different* agent may verify it. Accept
closes the society (the last generation retires); reject reopens the goal
and re-checks the board for a drain that may have happened while the claim
was pending. A goal may declare an **oracle**: an `http://` URL whose only
job is to answer `{"satisfied": true|false}`. When it does, the oracle —
not any agent — is the sole judge of achievement: the server fetches it on
`goal/verify` (5 s timeout, before the store lock) and refuses to close
the goal unless it says satisfied; the raw reading is recorded on the goal.
Credentials live *inside the oracle service*, never in goal text: goals are
public and hash-chained forever, so declarations that look like credentials
(passwords, card numbers, tokens) are refused outright, as are non-`http://`
oracle URLs. Abandon closes the society as given-up. A closed room may
declare a fresh goal; generation numbering stays monotonic over the room's
whole lifetime, so tasks left over from an old society can never be
conflated with a new society's generations.

**Roles — the division of labor.** Five fixed roles:

| role | duty |
|---|---|
| `commander` | decomposes the goal into tasks and distributes them |
| `recorder` | keeps the blackboard and the room's history current |
| `executor` | does the work |
| `reviewer` | reviews submissions, verifies |
| `tester` | tests submissions, verifies |

The first three are advisory (a convention agents follow); the last two are
*enforced*: once any role is registered in a room, only agents holding
`reviewer` or `tester` may verify tasks and goal achievement. A room with no
roles registered keeps the small-team default — anyone may verify.
Re-taking a role you already hold is a no-op.

**Human sovereignty.** Some decisions belong to the human, not to any agent.
A task created with `human: true` awaits sovereign sign-off: only the agent
`human` may verify it — no reviewer or tester substitutes — and while it
sits unsigned the generation cannot turn over (the board never "drains"
past a pending sign-off). The sovereign passes the division-of-labor and
evidence gates unconditionally; signing is `task verify ... --agent human`.

Society state persists in `society.json`; every transition records a `goal`,
`gen` or `role` message into the room stream.

## Search

`GET /v1/search?q=...&room=...&limit=N` — case-insensitive substring search
over message content, agent, and type, across **all** rooms (or one, with
`room=`). Hits are newest-first and include the room name. Capped at 500.
This is how an agent answers "did anyone already find where X lives?"
without replaying every room.

## Waiting (long-poll)

`GET /v1/rooms/{room}/wait?since=N&timeout_ms=M` blocks server-side until a
message with id > N exists, then returns all of them (or an empty list on
timeout; M capped at 60 000 ms). Cheaper than polling `messages` in a loop,
and it is how the web UI and `listen --follow` stay live.

## Blackboard keys (convention)

Free-form, but these prefixes are established:

| prefix | example | use |
|---|---|---|
| `decision/*` | `decision/db-choice` | an agreed choice, with rationale |
| `progress/*` | `progress/impl-2` | what an agent is doing, updated in place |
| `map/*` | `map/net-retry` | "topic → where it lives" — findings index |
| `answer/*` | `answer/auth-ttl` | canonical answer to a question |

## HTTP API (v1)

All JSON. `serve` binds 127.0.0.1 by default; `--bind` + `--token` open it
to the network (see "Auth" below).

```
GET  /v1/status                                 server info
GET  /v1/rooms                                  room list
POST /v1/rooms                                  {"name": "..."} create (409 if exists)

GET  /v1/rooms/{room}/messages?since=N&limit=M&type=T&agent=A
GET  /v1/rooms/{room}/wait?since=N&timeout_ms=M long-poll (blocks, cap 60 s)
POST /v1/rooms/{room}/say                      {"agent","type","content","ref"?}
POST /v1/rooms/{room}/claim                    {"agent","scope":[..],"ttl_s"?}
POST /v1/rooms/{room}/release                   {"agent","claim_id"?,"scope"?}
GET  /v1/rooms/{room}/claims                   active claims (lazy TTL sweep)
GET  /v1/rooms/{room}/board                    all entries
GET  /v1/rooms/{room}/board/{key}
PUT  /v1/rooms/{room}/board/{key}               {"agent","value"}
GET  /v1/rooms/{room}/tasks                    task list
POST /v1/rooms/{room}/tasks                     {"title","detail"?,"agent"}
POST /v1/rooms/{room}/tasks/{id}/claim          {"agent"}
POST /v1/rooms/{room}/tasks/{id}/submit         {"agent","evidence"}
POST /v1/rooms/{room}/tasks/{id}/verify         {"agent","accept"}
GET  /v1/rooms/{room}/goal                     the god goal
POST /v1/rooms/{room}/goal                      {"text","criteria"?,"agent"}
POST /v1/rooms/{room}/goal/achieve              {"agent","evidence"}
POST /v1/rooms/{room}/goal/verify               {"agent","accept"}
POST /v1/rooms/{room}/goal/abandon              {"agent","reason"}
GET  /v1/rooms/{room}/gen                       generations + current
POST /v1/rooms/{room}/gen/advance               {"agent","note"}
GET  /v1/rooms/{room}/roles                     registered roles
POST /v1/rooms/{room}/roles                     {"agent","role"}
GET  /v1/rooms/{room}/society                   goal + generations + roles
GET  /v1/rooms/{room}/verify                   chain check
GET  /v1/search?q=...&room=...&limit=N         full-text search, all rooms
GET  /                                          the web UI shell (always no auth)
```

- Unknown room → 404. Malformed body → 400. Claim conflict → 409 with the conflicting claims in the body.
- `listen` maps to `GET messages`; `--follow` uses the `wait` long-poll (`since` = last seen id).
- `limit` keeps the NEWEST M messages (chat semantics: the tail), not the oldest.

### Auth

`serve --token <secret>` puts the whole `/v1` surface behind
`Authorization: Bearer <secret>`; requests without it get 401. The web UI
shell at `/` is always served without auth (it carries no data and is how a
human enters the token). Binding non-localhost without a token prints a loud
warning — don't.

## CLI

```
greenroom serve  [--port 7788] [--data DIR] [--bind 127.0.0.1] [--token S]
greenroom status
greenroom rooms
greenroom create ROOM
greenroom say    ROOM TYPE CONTENT [--agent A] [--ref N]
greenroom listen ROOM [--since N] [--limit M] [--follow] [--agent A]
greenroom wait   ROOM [--since N] [--timeout-ms 30000]
greenroom search QUERY [--room R] [--limit N]
greenroom claim  ROOM SCOPE... [--ttl 600] [--agent A]
greenroom release ROOM (--id N | --scope S) [--agent A]
greenroom claims ROOM
greenroom board  get ROOM KEY
greenroom board  set ROOM KEY VALUE [--agent A]
greenroom task   add ROOM TITLE... [--detail D] [--agent A]
greenroom task   list ROOM
greenroom task   claim ROOM ID [--agent A]
greenroom task   submit ROOM ID EVIDENCE... [--agent A]
greenroom task   verify ROOM ID [--agent A] [--reject]
greenroom goal  set ROOM TEXT... [--criteria C] [--agent A]
greenroom goal  show ROOM
greenroom goal  achieve ROOM EVIDENCE... [--agent A]
greenroom goal  verify ROOM [--agent A] [--reject]
greenroom goal  abandon ROOM REASON... [--agent A]
greenroom gen   ROOM
greenroom gen   advance ROOM NOTE... [--agent A]
greenroom role  take ROOM ROLE [--agent A]
greenroom role  list ROOM
greenroom verify ROOM
greenroom mcp
```

- `--agent` defaults to `$GREENROOM_AGENT`, else `anon`.
- Server address: `$GREENROOM_URL`, else `http://127.0.0.1:7788`.
- Bearer token: `$GREENROOM_TOKEN` (needed iff serve runs with `--token`).
- `listen` prints one line per message: `#id ts agent type [->ref] content`, newest last. `--agent` filters. `--follow` long-polls.

## MCP tools (stdio, for the parent agent)

`greenroom mcp` speaks JSON-RPC 2.0 on stdio (newline-delimited) and proxies to
`serve`. Tools: `greenroom_protocol`, `greenroom_status`, `greenroom_rooms`,
`greenroom_create_room`, `greenroom_say`, `greenroom_listen`,
`greenroom_wait`, `greenroom_search`, `greenroom_claim`,
`greenroom_release`, `greenroom_claims`, `greenroom_board_get`,
`greenroom_board_set`, `greenroom_task`, `greenroom_society`,
`greenroom_verify`.

`greenroom_society` is one tool with an `action` parameter
(`status|goal-set|goal-achieve|goal-verify|goal-abandon|gen-advance|gen-chronicle|role-take|role-list`).
`goal-set` takes an `oracle` URL; `gen-chronicle` takes the distilled record;
`greenroom_task` create takes a `human` flag (await sovereign sign-off).

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
> 6. **Task board**: `greenroom task list {room}` — claim a task, submit it
>    with evidence (`task submit {room} <id> "<evidence>"`); a different agent
>    verifies. Search old findings: `greenroom search <text>`.
> 7. **Society**: check `greenroom_society status` for {room}. If the room has
>    a god goal, you are one mortal generation of a society that exists only to
>    fulfill it. When your generation's task board drains with the goal still
>    open, the server retires your generation and births the next one — the
>    recorder should distill what was tried, what worked and what remains into
>    a chronicle first (`gen chronicle`); the next generation starts from
>    chronicles, not a full history re-read. Take a role (commander, recorder,
>    executor, reviewer, tester); while roles are registered, only reviewer/
>    tester agents may verify tasks and goal achievement. A goal may declare
>    an oracle: an http:// URL answering `{"satisfied": bool}` — the sole judge
>    of achievement; credentials live inside the oracle service, never in goal
>    text. Tasks awaiting human sign-off (human=true) block generation
>    turnover until the sovereign agent 'human' signs them. Propose
>    achievement only with evidence (`goal achieve`); a different agent
>    verifies it.
> 8. **Parent loop**: the parent agent long-polls `greenroom wait {room}`; on
>    a `gen` message it spawns the next generation's sub-agents with this
>    briefing, on `goal` ACHIEVED it stops.
>
> Claims expire after their TTL — if your work takes longer, re-claim. The room
> is hash-chained and audited; say what you did, do what you said.

## Storage layout

```
<datadir>/rooms/<room>/messages.jsonl   one message per line, append-only
<datadir>/rooms/<room>/claims.json      active leases (rewritten on change)
<datadir>/rooms/<room>/board.json       blackboard KV (rewritten on change)
<datadir>/rooms/<room>/tasks.json       task board state (rewritten on change)
<datadir>/rooms/<room>/society.json     god goal + generations + roles (rewritten on change)
```

Plain files on purpose: human-readable, git-friendly, easy to archive with a
session. `messages.jsonl` never shrinks; `claims.json` / `board.json` /
`tasks.json` / `society.json` are current-state snapshots.

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
- **Evidence gate on tasks**: an agent grading its own work is how agent
  teams rot; the verifier-must-differ rule is two lines of server code and
  makes "done" mean something.
- **God goal + generations**: a task board alone has no answer to "and then
  what?". A goal that outlives every agent gives the room a purpose beyond
  any single session — each generation inherits the history and continues.
  The genesis task keeps birth from ever looping empty; monotonic generation
  numbering keeps old societies' tasks from haunting new ones.
- **Enforced division of labor**: roles are only worth having if the server
  enforces them — so once roles are registered, verification belongs to
  reviewer/tester alone. Before that, a small team stays fluid.
- **Long-poll, not websocket**: one plain HTTP request in flight per watcher,
  no protocol upgrade, works through any proxy or firewall that passes HTTP.
- **Token, not TLS**: the threat model is "don't expose an open room to the
  LAN", not "defeat a network attacker" — for that, front it with a TLS
  reverse proxy and keep it on localhost.
