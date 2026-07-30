# Multiplayer branch — handoff

Orientation document for anyone (human or agent) picking up this work.
Written at feature-completion, 2026-07-30.

## What this is

The `multiplayer` branch of github.com/RikPi/OpenLoco (fork of
OpenLoco/OpenLoco) turns the game's dormant alpha netcode into a complete,
tested multiplayer implementation — the feature upstream has wanted since
issue #95 (2018). Roughly 25 commits, all built and verified **without any
copyrighted Locomotion assets**, using a stub `g1.DAT`, the free
OpenGraphics objects, and generated fixtures.

## What works (all verified by automated tests)

- **Deterministic lockstep** with per-tick PRNG desync detection, forensic
  S5 dumps, and automatic snapshot resync. Company AI runs identically on
  every peer (upstream gated it to the host, which desynced by design).
- **Portable wire protocol** (version 9): game commands serialized
  field-by-field from typed Args structs (79/85 commands; 6 vanilla stubs
  remain on a raw fallback), version negotiation with readable rejections.
- **Session model**: joining players get their own company (deterministic
  replicated creation), share the host's (co-op) or spectate — host picks
  via `--join_policy`. The server enforces company ownership on every
  received command; clients cannot act as companies they don't own.
- **Resilience**: dropped clients auto-reconnect with a session token and
  reclaim their exact company (seat reservation); if the *host* dies, the
  survivors elect a successor deterministically, it promotes itself to a
  live server in-process, and everyone else rejoins it — the session
  survives host death (window: name-based reclaim, an accepted v1 trust
  tradeoff for friends/LAN play).
- **Discovery**: LAN broadcast discovery plus an internet master server —
  a tiny stdlib-only Go daemon (`tools/master-server/`) speaking the
  game's own UDP protocol, with TTL registry and an HTTP `/servers` JSON
  endpoint. In-game Server Browser merges LAN + master results.
- **UX**: chat (scrollable, real player names), player roster with
  disconnect states, join-by-address with `host:port`/`[ipv6]:port`,
  sane save/load/quit in sessions (local save; host-driven mid-session
  load with client resync; graceful shutdown notifications).

## How to verify everything (10 minutes)

```powershell
# Build (VS2022; cmake path in KNOWLEDGEBASE.md § Build & test)
cmake --preset windows && cmake --build --preset windows-release
# Unit tests (152)
cd build\windows; ctest -C Release
# The smoke matrix — each mode is a self-verifying multi-process drill:
scripts\run_sync_smoke_test.ps1 -TestRename      # client command round-trip
scripts\run_sync_smoke_test.ps1 -TestReconnect   # real outage + seat reclaim
scripts\run_sync_smoke_test.ps1 -TestMigration   # host crash + election (3 procs)
scripts\run_sync_smoke_test.ps1 -TestMaster      # full master-server chain
# also: -TestHostLoad, -TestShutdown, -TestDiscovery, -JoinPolicy coop|spectator
```

One-time machine setup lives in KNOWLEDGEBASE.md § Headless (stub install
dir, `allow_multiple_instances`, competitor fixture generator). CI
(`.github/workflows/multiplayer-sync.yml`) runs build + three smoke modes
on every push to `multiplayer`.

## Where things live

| File | Purpose |
|---|---|
| `HANDOFF.md` | This file — start here |
| `KNOWLEDGEBASE.md` | Operational knowledge: build/test commands, every subsystem's implementation notes, hard-won traps |
| `docs/multiplayer.md` | Architecture and design rationale (invariants, session model, reconnect/migration/master designs) |
| `TASKS.md` | Full task history with per-item verification evidence; open non-development items at the top |
| `src/OpenLoco/src/Network/` | The netcode (facade, client, server, discovery, connection) |
| `src/OpenLoco/src/GameCommands/CommandSerialization.*` | The portable wire codec |
| `tools/master-server/` | The Go master service + deployment files (README: systemd, Docker, Fly.io) |
| `scripts/run_sync_smoke_test.ps1` | The smoke harness (all modes documented in its header) |
| `scripts/gen_competitor_object.py` | Generates the asset-free competitor fixture objects |

## The three rules that keep this codebase correct

1. **Never mutate `GameState` outside the deterministic tick/command
   path.** Machine-local state goes in statics; synced mutations go
   through game commands. (KNOWLEDGEBASE § Determinism traps has the full
   list of subtleties — query/apply double execution, `playerCompanies`
   per-machine bytes, asymmetric Args conversions.)
2. **Bump `kNetworkVersion` on any wire change** (Network.h documents the
   version history v2→v9). The server rejects mismatches readably.
3. **Session bookkeeping (roster, tokens, discovery, migration plans) never
   touches `GameState`** — presentation and connectivity data only.

## What's left (decisions, not development)

1. **Deploy the master server** — `tools/master-server/README.md` has
   systemd/Docker/Fly.io instructions; then set `network.masterServer`
   (or `--master_server`) on hosts and browsers.
2. **Real-asset play test** — the owner doesn't yet own Locomotion (~€6
   GOG/Steam, frequent sales). Point `loco_install_path` at a real
   install and the whole stack gets its first windowed, rendered session.
3. **Upstream contribution** — deliberately paused at the owner's request.
   Ready-made small PRs when wanted: the divergence fixes,
   `nullTerminatedView`, the g1 null-guards, FileStream/Title fixes. The
   networking work itself would suit a draft PR referencing issue #95.

Known v1 limitations (documented, deliberate): IPv4-only discovery/
migration plans, no NAT traversal (internet hosts must forward their
port), name-based migration reclaim window, in-memory master registry.
