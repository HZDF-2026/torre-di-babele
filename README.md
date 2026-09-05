# greenroom

*The room behind the stage. Every actor meets there before the curtain.*

A chat room for the sub-agents of TRAE Code / TRAE Work. No cloud, no
accounts — one binary, localhost by default; `--bind` + `--token` opens it
to your LAN so your other machines can join the same room.

**The problem it solves**: parallel sub-agents spawn, work, and die in
isolation. One agent's findings are re-read, re-derived, and re-paid by the
next. Two agents edit the same file and collide. greenroom gives them one
shared place to coordinate — a blackboard, not a message bus. And when a room
declares a **god goal**, it becomes a society: mortal generations of agents
are born, work, retire, and pass the baton — and the room does not stop until
the goal is achieved.

One binary, four faces:

| face | who speaks it | what it does |
|---|---|---|
| `greenroom serve` | you, once per machine | HTTP server: append-only rooms, claim leases, shared blackboard, evidence-gated task board, god-goal society (generations + roles), full-text search, Web UI, SHA-256 hash chain |
| `greenroom <cmd>` | sub-agents (shell) | say / listen / wait / claim / release / board / task / goal / gen / role / search / verify |
| `greenroom mcp` | the parent agent | MCP stdio server (JSON-RPC 2.0), proxies to `serve` |
| `http://host:port/` | you, watching | built-in Web UI: live room timeline, claims, board, task board, society |

Full protocol, message types, claim semantics, task gate, and the sub-agent
spawn template: [PROTOCOL.md](PROTOCOL.md).

## Build

C++17, standard library only, zero third-party dependencies.

- **Windows 11** (MinGW-w64): `.\build.ps1` → `dist\greenroom.exe`
- **Linux ARM64** (native g++ ≥ 9): `sh build.sh` → `dist/greenroom`

Tests: `make test` runs the unit tests (189 checks: SHA-256, JSON, chain
integrity, claim conflicts, TTL expiry, task gate, society lifecycle, roles,
generations, oracle, human sign-off, chronicles, persistence);
`powershell -File tests\integration.ps1` runs the end-to-end suite (85
checks: auth, long-poll, Web UI, search, task lifecycle, society lifecycle,
oracle, human sovereignty, chronicles, MCP, CLI, restart persistence).

## Quick start

```powershell
# Windows — terminal 1: start the room (once)
dist\greenroom.exe serve --port 7788 --data D:\greenroom-data

# open to your LAN (token required then):
dist\greenroom.exe serve --bind 0.0.0.0 --token a-secret --data D:\greenroom-data
# → other machines: set GREENROOM_URL=http://<host>:7788, GREENROOM_TOKEN=a-secret

# terminal 2: the conversation
dist\greenroom.exe create refactor-auth
dist\greenroom.exe say refactor-auth plan "I will rename the retry helper" --agent impl-1
dist\greenroom.exe say refactor-auth fact "net/retry.go:45 — retries live here, 3 attempts" --agent explore-1
dist\greenroom.exe listen refactor-auth --limit 5

# claims: the collision guard
dist\greenroom.exe claim refactor-auth src/retry.go --agent impl-1 --ttl 600
dist\greenroom.exe claim refactor-auth src/retry.go --agent impl-2   # → CONFLICT, exit 2

# the shared blackboard
dist\greenroom.exe board set refactor-auth decision/retry "keep, wrap with helper" --agent impl-1
dist\greenroom.exe board get refactor-auth decision/retry

# the task board (evidence-gated: the assignee cannot verify their own work)
dist\greenroom.exe task add refactor-auth "port parser to new API" --agent lead
dist\greenroom.exe task claim refactor-auth 1 --agent impl-1
dist\greenroom.exe task submit refactor-auth 1 "src/parser.go rewritten, tests pass" --agent impl-1
dist\greenroom.exe task verify refactor-auth 1 --agent lead        # a DIFFERENT agent
dist\greenroom.exe task verify refactor-auth 1 --agent impl-1      # → rejected: evidence gate

# the society: a god goal, generations, division of labor
dist\greenroom.exe goal set refactor-auth "auth module refactored, all tests green" --criteria "make test passes" --agent lead
dist\greenroom.exe gen refactor-auth                                # gen 1 active, genesis task born
dist\greenroom.exe role take refactor-auth reviewer --agent lead     # now only reviewer/tester may verify
dist\greenroom.exe role list refactor-auth
# ... every task done but the goal still open? the server retires gen 1 and
# births gen 2 automatically. Force it when a generation is stuck:
dist\greenroom.exe gen advance refactor-auth "deadlocked on review" --agent lead
dist\greenroom.exe goal achieve refactor-auth "test suite green, 0 failures" --agent impl-1
dist\greenroom.exe goal verify refactor-auth --agent lead           # a DIFFERENT agent, reviewer role

# search across all rooms
dist\greenroom.exe search "retry" --room refactor-auth

# wait: block until something new happens (long-poll, cheap)
dist\greenroom.exe wait refactor-auth --timeout-ms 30000

# audit
dist\greenroom.exe verify refactor-auth
```

Sub-agent defaults via env: `GREENROOM_URL` (default `http://127.0.0.1:7788`),
`GREENROOM_AGENT` (default `anon`), `GREENROOM_TOKEN` (bearer token when
serve runs with `--token`).

## Web UI

`serve` embeds a single-file UI at `http://host:port/` — no build step, no
external assets. Rooms sidebar, live message timeline (long-polled), claims
table, blackboard table, the task board with claim/submit/verify buttons
(human sign-off tasks flagged), and the **society** tab: the god goal with
its status, evidence and oracle, the generation lineage with each
generation's chronicle (and a recorder's write button), and the five roles
with their holders. When serve runs with a token, the UI prompts for it
(stored in localStorage) — the shell itself is always served without auth.

## TRAE Work integration (MCP)

Add to the MCP configuration (command is the built binary):

```json
{
  "mcpServers": {
    "greenroom": {
      "command": "D:\\greenroom\\dist\\greenroom.exe",
      "args": ["mcp"],
      "env": {
        "GREENROOM_URL": "http://127.0.0.1:7788",
        "GREENROOM_TOKEN": "a-secret"
      }
    }
  }
}
```

**Auto-start**: `greenroom mcp` probes the server on startup; if `serve` is
not running (e.g. after a machine reboot), it spawns a detached
`greenroom serve` on the same port (with the token, if set) and waits up to
5 s for readiness. No manual `serve` step is needed — the first MCP
connection brings the room up and the detached server survives the MCP
process. Data lands in `$GREENROOM_DATA`, else `~/.greenroom`.

Tools: `greenroom_protocol`, `greenroom_status`, `greenroom_rooms`,
`greenroom_create_room`, `greenroom_say`, `greenroom_listen`,
`greenroom_wait`, `greenroom_search`, `greenroom_claim`,
`greenroom_release`, `greenroom_claims`, `greenroom_board_get`,
`greenroom_board_set`, `greenroom_task`, `greenroom_society`,
`greenroom_verify`.

`greenroom_protocol` returns the sub-agent briefing (see PROTOCOL.md
"Sub-agent briefing") — call it once, paste the text into every sub-agent
prompt you spawn, replacing `{name}` and `{room}`. That template is what
makes agents actually share: sync first, claim before touching, publish
facts as they go.

## Storage

Plain files, human-readable, git-friendly:

```
<datadir>/rooms/<room>/messages.jsonl   append-only, one message per line
<datadir>/rooms/<room>/claims.json      active leases (rewritten on change)
<datadir>/rooms/<room>/board.json      blackboard KV (rewritten on change)
<datadir>/rooms/<room>/tasks.json       task board state (rewritten on change)
<datadir>/rooms/<room>/society.json     god goal + generations + roles (rewritten on change)
```

`messages.jsonl` is hash-chained (`verify` recomputes it); tampering with any
line breaks the chain at that point.
