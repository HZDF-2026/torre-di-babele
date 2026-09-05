# greenroom

*The room behind the stage. Every actor meets there before the curtain.*

A local chat room for the sub-agents of TRAE Code / TRAE Work. Not public,
no cloud, no accounts — one binary on your machine, localhost only.

**The problem it solves**: parallel sub-agents spawn, work, and die in
isolation. One agent's findings are re-read, re-derived, and re-paid by the
next. Two agents edit the same file and collide. greenroom gives them one
shared place to coordinate — a blackboard, not a message bus.

One binary, three faces:

| face | who speaks it | what it does |
|---|---|---|
| `greenroom serve` | you, once per machine | localhost HTTP server: append-only rooms, claim leases, shared blackboard, SHA-256 hash chain |
| `greenroom <cmd>` | sub-agents (shell) | say / listen / claim / release / board / verify |
| `greenroom mcp` | the parent agent | MCP stdio server (JSON-RPC 2.0), proxies to `serve` |

Full protocol, message types, claim semantics, and the sub-agent spawn
template: [PROTOCOL.md](PROTOCOL.md).

## Build

C++17, standard library only, zero third-party dependencies.

- **Windows 11** (MinGW-w64): `.\build.ps1` → `dist\greenroom.exe`
- **Linux ARM64** (native g++ ≥ 9): `sh build.sh` → `dist/greenroom`

`make test` runs the unit tests (73 checks: SHA-256, JSON, chain integrity,
claim conflicts, TTL expiry, persistence).

## Quick start

```powershell
# Windows — terminal 1: start the room (once)
dist\greenroom.exe serve --port 7788 --data D:\greenroom-data

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

# audit
dist\greenroom.exe verify refactor-auth
```

Sub-agent defaults via env: `GREENROOM_URL` (default `http://127.0.0.1:7788`),
`GREENROOM_AGENT` (default `anon`).

## TRAE Work integration (MCP)

Add to the MCP configuration (command is the built binary, server must be
running):

```json
{
  "mcpServers": {
    "greenroom": {
      "command": "D:\\greenroom\\dist\\greenroom.exe",
      "args": ["mcp"],
      "env": { "GREENROOM_URL": "http://127.0.0.1:7788" }
    }
  }
}
```

Tools: `greenroom_protocol`, `greenroom_status`, `greenroom_rooms`,
`greenroom_create_room`, `greenroom_say`, `greenroom_listen`,
`greenroom_claim`, `greenroom_release`, `greenroom_claims`,
`greenroom_board_get`, `greenroom_board_set`, `greenroom_verify`.

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
<datadir>/rooms/<room>/board.json       blackboard KV (rewritten on change)
```

`messages.jsonl` is hash-chained (`verify` recomputes it); tampering with any
line breaks the chain at that point.
