# Multiplayer architecture (working document)

Status of the `multiplayer` branch and the design being built on it. The goal
sequence: (1) provable lockstep sync between instances, (2) a real player
session model (N players, co-op company sharing, spectators), (3) lobby and
discovery.

## What exists

OpenLoco ships an alpha lockstep client/server implementation
(`src/OpenLoco/src/Network/`): reliable-UDP transport, server-ordered game
command replication (`GameCommandPacket` with server-assigned `index` and
execution `tick`), client tick gating (`shouldProcessTick`), join by chunked
S5 snapshot transfer, packet-level chat, `host`/`join` CLI verbs.

The simulation is a fixed-timestep deterministic lockstep: identical
`GameState` (including the embedded PRNG) + identical ordered command stream
=> identical simulation on every peer. All player actions funnel through
`GameCommands::doCommand`.

## Added on this branch

- **Wire format v2** (`GameCommands/CommandSerialization.{h,cpp}`): commands
  are serialized field-by-field (little-endian, layout-independent) from
  their typed Args structs instead of shipping the raw x86-style `registers`
  blob. 74/85 commands are typed (6 renames via a shared chunk codec); the
  rest (3 complex-type, 8 vanilla stubs) use an explicit raw fallback. The
  server validates the client network version on connect.
- **Desync detection**: clients record the PRNG state after every simulated
  tick and verify it against the server state advertised in pings. On
  mismatch: both sides export S5 dumps to `save/desync/` (for offline
  `compare`), the client freezes its simulation and auto-resyncs via a fresh
  snapshot, pruning stale queued commands.
- **Divergence fixes**: `updateOwnerStatus` throttle moved out of synced
  state; the TEMP hack that dropped command 73 removed; failed commands are
  documented as intentionally rebroadcast (index continuity; deterministic
  failure on every peer; error UI only on the issuing machine).
- **Chat UI** (`Ui/Windows/Chat.cpp`), packet-level by design — chat does not
  mutate game state, so it stays out of the lockstep stream.
- **Headless operation without vanilla assets**: init tolerates a stub
  `g1.DAT` (empty-element null guards in `Gfx::loadDefaultPalette` and
  `PaletteMap::getForColour`), enabling automated sync tests using only
  OpenGraphics objects.

## Known determinism gaps (must fix for stable sync)

1. **AI is host-only** — `CompanyManager::tick()` gates AI thinking and
   AI-company creation on `!isNetworked() || isNetworkHost()`. AI mutates
   company/game state directly (manager-internal, not via replicated
   commands), so any AI activity desyncs clients. Fix direction: remove the
   gate so every peer runs the (deterministic) AI identically, and make
   `doCommand` bypass network queueing when invoked from within tick
   execution (replicated command replay and subsystem updates) so AI-issued
   commands apply inline everywhere instead of being re-queued by each peer.
   An "in tick execution" flag around `Network::processGameCommands` and the
   subsystem tick covers this.
2. **`playerCompanies` byte divergence** — `GameState.playerCompanies[2]` is
   an ordered pair whose *set* is identical on all peers but whose slot 0
   means "the local player's company" (vanilla convention; `Title::loadTitle`
   swaps it for clients). The two bytes therefore legitimately differ between
   peers. Any byte-level state comparison must treat these two bytes as
   per-machine (GameSaveCompare already logs rather than fails them).

## Session model (design)

Roles: **company owner** (own company, competitive), **company member**
(shares a company, co-op), **spectator** (no company). Default assignment:
competitive-first — a joining player gets a new company if a slot is free,
else spectator. Identity is session-scoped (`client_id_t` + name from the
connect packet).

Invariants derived from the code:

- The set of companies and which companies are human-controlled is synced
  state and must change only deterministically. Company creation for a
  joining player must be a game command (precedent: the `switchCompany`
  cheat wraps `setControllingId` in `GameCommand::cheat` precisely so it
  replicates). `CompanyManager::createPlayerCompany()` is the allocator to
  wrap.
- "Which company is mine" is per-machine. Keep the vanilla convention
  (`playerCompanies[0]` = mine) so ~90 call sites and the paint code keep
  working; the two-byte divergence is accepted and excluded from comparison.
- With more than two humans, membership checks (`isPlayerCompany`) must
  consult a deterministic human-company set maintained by join/leave game
  commands (mirrored in the snapshot's `ExtraState` for late joiners), not
  the fixed two-slot array.
- The server must override the company on every received command with its
  own client→company mapping and drop commands from spectators. Clients
  cannot act as companies they don't own.

Join flow: connect → version check → snapshot (existing) → server issues
`createPlayerCompany` command (or co-op/spectator assignment) → assignment
packet tells the client its company id → client sets local controlling id.

## Reconnect (design — next up)

Goal: a client that loses its connection can rejoin the same session and
reclaim its company, instead of being treated as a brand-new player.

- **Session token.** On first accept, the server generates a random token
  per client and sends it alongside the company assignment. The client
  keeps it in memory for the lifetime of the process.
- **Reclaim on connect.** `ConnectPacket` grows a token field (zeroed =
  fresh join; requires a network version bump). If the token matches a
  reserved seat, the server re-attaches the client to its previous company
  (no `createPlayerCompany`, no join policy) and the normal snapshot
  transfer + assignment re-send flow follows — the same machinery a desync
  resync already uses.
- **Seat reservation.** On timeout/disconnect the server moves the client
  entry to a reserved list (token, name, company) instead of dropping it.
  V1 keeps seats reserved for the session's lifetime; an expiry policy
  (seat becomes joinable/AI after N minutes) can come later. The roster
  shows reserved seats as "(disconnected)".
- **Client auto-retry.** V1: when an established connection times out, the
  client automatically attempts to rejoin the same endpoint with its token
  a few times (backoff), surfacing progress in the status window, before
  giving up and returning to title. UI-initiated manual reconnect can come
  later.
- **Determinism note.** Reconnection is pure session bookkeeping: no game
  state changes on either path (the company simply keeps existing and,
  while its seat is empty, its commands are absent — identical on every
  peer). Only the human-company mask must NOT be cleared on disconnect,
  which is already the case.

Host migration is out of scope for this design; a dead host still ends the
session. (Superseded — see § Host migration below.)

## Host migration (implemented, network version 9)

Goal: when the host dies or vanishes, the remaining clients continue the
session under a new host instead of falling back to the title screen.

Implemented as designed below; see KNOWLEDGEBASE.md § Host migration for
implementation notes (wire layout, election/promotion code paths, the port
subtlety, the headless test hooks) and TASKS.md's Milestone 4 entry for the
verification record (exact log lines, ctest/smoke results).

What makes this tractable in a lockstep architecture: every client already
holds the complete authoritative game state, the human-company mask, and
the public roster — the only things that die with the host are command
ordering, the seat/token table, and everyone's knowledge of each other's
addresses.

- **Migration plan broadcast.** While healthy, the server periodically
  (~10 s and on roster change) sends every client a `migrationPlan`
  packet: an ordered list of successor candidates — client ids paired
  with their public endpoints *as observed by the server* — ordered by
  client id (deterministic). Clients just store the latest plan.
- **Detection and election.** A client whose established connection times
  out first runs the existing auto-retry (the host may merely have
  blipped). Only after retries are exhausted does migration begin: the
  successor is the first entry of the last received plan that is not the
  dead host. Every client computes this independently and identically.
- **Becoming the host.** If the successor is *me*: open a server on my
  configured port, seed the roster from my latest roster snapshot (every
  entry becomes a reserved seat), keep the human-company mask as-is (no
  game-state change at all), and enter a ~60 s **migration window**.
- **Rejoining.** Everyone else waits a few seconds (successor boot time),
  then connects to the successor's endpoint from the plan. Tokens issued
  by the dead host are meaningless to the successor, so during the
  migration window seats may be reclaimed by (name, company) match
  against the seeded roster — trust-on-first-reconnect, an accepted v1
  weakness for friends/LAN play, documented; after the window, unclaimed
  seats stay reserved exactly like ordinary disconnects. The normal
  snapshot resync then aligns everyone to the successor's tick (a client
  slightly ahead simply resyncs down; commands the dead host never
  broadcast are lost, which is indistinguishable from them never having
  been sent).
- **The old host's company** becomes a reserved seat like any other
  disconnect; a recovered old host rejoins by name match within the
  window or sits reserved after it.
- **Reachability limits (v1).** The successor's endpoint is whatever the
  dead server observed; behind NAT that address is generally unreachable
  from other internet clients. V1 targets LAN and directly-reachable
  successors; NAT traversal is explicitly out of scope. The master
  server keeps working transparently: a migrated host that has a master
  configured simply starts announcing itself.

Wire cost: one new packet kind (`migrationPlan`), a reclaim variant of the
connect flow (name+company based, gated to the migration window), one
version bump.

## LAN server discovery (implemented, network version 7)

Phase 1 of the lobby (see the goal sequence at the top of this document).
Two new connectionless packet kinds (`discoveryRequest`/`discoveryResponse`,
`Packet.h`) bypass `NetworkConnection` sequencing entirely — no acks,
handled directly in `NetworkServer::onReceivePacket` for endpoints that are
not an established client, alongside the existing connect handling. The
server answers ANY `discoveryRequest` regardless of the requester's version
(the request carries none) and self-describes its own version in the
response, so a browser can grey out incompatible servers instead of simply
failing to find them.

Client side (`Network/ServerDiscovery.{h,cpp}`) opens its own UDP socket and
probes roughly once a second to `255.255.255.255:<port>` (LAN broadcast) and
`127.0.0.1:<port>` (loopback, since broadcast usually doesn't reach it) —
both target `kDefaultPort` only, so a server bound to a non-default port is
not discovered this way (it remains reachable via "Join by address").
Responses are deduplicated by endpoint and expire after ~5s without a fresh
reply. Facade: `Network::beginServerDiscovery()` / `endServerDiscovery()` /
`getDiscoveredServers()`.

UI: `Ui/Windows/ServerBrowser.cpp` (`WindowType::serverBrowser`) lists
discovered servers, greying out incompatible versions, and is now what
TitleMenu's multiplayer button opens (instead of the raw address prompt
directly) — the prompt itself lives on in the browser's "Join by address"
button, sharing `Network::parseServerAddress` with the browser's row-click
path. Discovery runs only while the window is open.

### Master server (phase 2 — design)

An internet-wide server list, revised from the earlier "HTTP announce"
sketch: the master speaks the game's own UDP packet framing instead of
HTTP, so the game reuses its tested discovery machinery and gains no HTTP
client dependency. The service (`tools/master-server/`, Go, single static
binary — runs identically on a personal VPS under systemd or on a
UDP-capable free tier such as Fly.io; a container image is provided) is
stateless: an in-memory registry with TTL expiry, no database.

Protocol (game `Packet` framing, little-endian, connectionless):

- `masterAnnounce` (game server → master, every ~30 s and on roster
  change): version, game port, player count/max, join policy, host name.
  The master records the announce's *observed* source address plus the
  advertised game port — i.e. the public endpoint. Entries expire after
  ~90 s without a fresh announce, so crashed servers just fall off.
- `masterQuery` (browser → master): cookie.
- `masterServerList` (master → browser): cookie echo + entries
  (address, port, version, players, policy, name). IPv4 only in v1.

Game integration: `network.masterServer` config value ("host:port", empty
disables); a hosting server announces when set; the in-game browser
queries the master alongside LAN probes and merges both into one list
(entries tagged by source). Known v1 limitation: no NAT traversal — an
internet-reachable host must forward its game port; the master only makes
it *discoverable*, not *reachable*.

Service hardening: per-IP announce cap, bounded registry size, no
amplification (list responses only to queriers that sent a well-formed
request; response size bounded), structured logs, graceful shutdown. A
small HTTP `GET /servers` JSON endpoint on a separate port serves humans
and monitoring, not the game.

## Test strategy

- `CommandSerializationTests` cover the wire codecs.
- Headless fixture generation (procedural map via `MapGenerator`, exported
  with `S5::exportGameStateToFile`) plus stub-`g1.DAT` init enables an
  automated host/join lockstep test with zero vanilla assets: run a host and
  a client (`SDL_VIDEODRIVER=dummy`), play scripted commands, assert the
  desync detector stays silent, then byte-compare exported states.
- The existing `simulate`/`compare` CLI remains the single-process
  determinism harness.
