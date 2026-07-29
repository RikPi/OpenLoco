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
session.

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

### Master server (phase 2 — constraints)

Out of scope for this pass, but the LAN design above should not preclude it:
a future internet-wide server list needs only a tiny, stateless HTTP
announce/list service (a single small binary, or something free-tier-hosting
friendly like a personal VPS or Cloudflare-Workers-class platform — no
database or persistent session state required). Servers would announce
themselves to it periodically with a TTL (so a crashed/killed server simply
expires off the list rather than needing explicit deregistration); the
master service's URL would be an ordinary config value; and the in-game
browser would merge its results with LAN discovery into one list. None of
this is implemented here.

## Test strategy

- `CommandSerializationTests` cover the wire codecs.
- Headless fixture generation (procedural map via `MapGenerator`, exported
  with `S5::exportGameStateToFile`) plus stub-`g1.DAT` init enables an
  automated host/join lockstep test with zero vanilla assets: run a host and
  a client (`SDL_VIDEODRIVER=dummy`), play scripted commands, assert the
  desync detector stays silent, then byte-compare exported states.
- The existing `simulate`/`compare` CLI remains the single-process
  determinism harness.
