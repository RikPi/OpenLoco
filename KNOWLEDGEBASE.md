# Multiplayer branch — knowledge base

Hard-won facts about the codebase and environment. Companion to `TASKS.md`
(status) and `docs/multiplayer.md` (design).

## Build & test (this machine)

- CMake: `"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`
- Configure: `cmake --preset windows` (VS2022, `x64-windows-static` vcpkg).
  Build: `cmake --build --preset windows-release` (add `--target App` or
  `OpenLocoTests` for speed). Tests: `ctest -C Release` in `build\windows`
  — currently 144/144.
- Never run two MSBuild invocations on the tree concurrently.
- Game data is exe-relative (`data/objects` = OpenGraphics, fetched by
  CMake); run from `build\windows\Release`.

## Headless / asset-free operation

- Vanilla assets are NOT required for automated testing:
  - Stub install dir `build\fake-locomotion\Data\g1.DAT` (8 zero bytes)
    passes `validateLocoInstallPath` and parses in `loadG1` (fixed-size
    `_g1Elements` stays zeroed; empty-element guards are in
    `loadDefaultPalette` and `PaletteMap::getForColour`).
  - `%APPDATA%\OpenLoco\openloco.yml` needs `allow_multiple_instances: true`
    (global named mutex otherwise blocks host+client on one machine).
  - `gensave <out.sv5> [--seed N]` generates a byte-deterministic fixture
    (same seed ⇒ identical file). Current fixture has 0 towns/industries
    because OpenGraphics lacks those object types; competitors are now
    covered by a hand-crafted fixture object (see § Competitor object
    fixture) placed in `data/objects/Competitor/`.
  - `--headless` skips window/cursor/audio and runs the plain update loop;
    `host`/`join` work under it.
- Proven smoke test: `gensave` → start host (`--headless host fixture.sv5`)
  → start client (`--headless join 127.0.0.1`) → wait → check
  `%APPDATA%\OpenLoco\logs\openloco_*.log`. Success = exactly one
  `Accepted new client`, client `Scene transition: boot -> gameplay`, zero
  `[ERR]`/desync lines. Scripted with assertions + exit code:
  `scripts\run_sync_smoke_test.ps1 [-JoinPolicy own|coop|spectator]
  [-RunSeconds N] [-Seed N]`. With the competitor fixture installed (see below),
  also expect host `Assigned company N to client '...'` and client
  `Assigned company N` — the join-time `createPlayerCompany` success path.
- stdout is fully buffered and LOST when the process is killed — use the
  file logs (they flush per line), or stderr.
- `simulate <save> N [-o out]` + `compare` = single-process determinism
  harness (upstream CI uses this with private assets).
- git-bash (this environment's Bash tool) strips unquoted backslashes from
  arguments (`C:\foo\bar` → `C:foobar`), silently breaking
  `--locomotion_path`/similar Windows-path CLI args — use forward slashes
  (`C:/foo/bar`) or quote the backslashed form.

## Competitor object fixture

- OpenGraphics' release zip (fetched by `FetchContent_Declare(openloco_objects
  ...)` in `CMakeLists.txt`) ships a Competitor/ folder with zero files —
  verified by inspecting the fetched source at
  `build\windows\_deps\openloco_objects-src\Competitor` (empty) — so there is
  no upstream object to copy or rebuild; it had to be hand-crafted.
- `scripts/gen_competitor_object.py` writes a minimal valid Competitor `.DAT`
  to `data\objects\Competitor\FIXTCOMP.DAT` (regenerate with
  `python scripts/gen_competitor_object.py [out_path]`; default path assumes
  the standard `build\windows\Release` layout). No CMake/build wiring — it's
  a standalone generator, run once, output committed like any other data
  file would be (not committed by this change per task constraints).
- On-disk format (reverse-engineered and checksum-verified byte-for-byte
  against a real shipped object, `OG_COAL.dat`, before trusting it):
  `ObjectHeader` (0x10 bytes: `flags` u32 [byte 0 = type|sourceGame<<6],
  `name[8]`, `checksum` u32) + one `SawyerStreamWriter` chunk (`encoding` u8,
  `length` u32, payload) holding the `CompetitorObject` struct (0x38 bytes,
  `CompetitorObject.h`) + two string-table entries (`ObjectStringTable.cpp`:
  repeated `[lang u8][cstring\0]`, terminated by `0xFF`) + an image table
  (`ObjectImageTable.cpp`: `G1Header{numEntries,totalSize}` + elements +
  pixel data). `checksum` = rotl-based `computeObjectChecksum`
  (`ObjectManager.cpp`) over (header flags byte 0, header name, decoded
  chunk payload) — no full-file trailing checksum is used for `.DAT` objects
  (that's a separate S5-save-only mechanism).
- Minimality found by reading `CompetitorObject::load`/`::validate`: only
  `emotions` bit 0 needs to be set (validate requirement); `intelligence`/
  `aggressiveness`/`competitiveness` must be in [1,9]; `firstName`/
  `lastName`/`images[9]` in the file are irrelevant (unconditionally
  overwritten by `load()` from the string/image tables). An **empty** image
  table (`G1Header{numEntries=0, totalSize=0}`) parses successfully and is
  never dereferenced unless the object is actually drawn, which never
  happens headless (`Ui::render`/`Gfx::renderAndUpdate` null-check `_window`
  first) — so no real sprite data was needed, contrary to the initial
  worry. `availableNamePrefixes`/`availablePlayStyles` are unused on the
  join path (`createCompany(..., isPlayer=true)` skips that branch) but were
  given bit 0 anyway as a defensive guard against an empty-vector index if
  the same object is ever picked for an AI company.
- No loader code changes were needed — the existing null-guards and
  zero-entry-table handling were already sufficient.
- The object index (`%APPDATA%\OpenLoco\plugin.dat`) auto-rebuilds when
  `ObjectFolderState` (file count/total size/date hash) changes, so dropping
  a new `.DAT` into `data\objects\Competitor\` is picked up on next launch
  without manually deleting the cache (deleting it is harmless and was done
  once here for a clean-slate check).

## Lockstep architecture facts

- One tick = `GameScene::tick()`: gate on `Network::shouldProcessTick`,
  increment `scenarioTicks`, `Network::processGameCommands(tick)`, then
  subsystem updates in fixed order; `Network::onTickProcessed` records the
  client's per-tick PRNG for desync verification against server pings.
- Commands: `doCommand` → (networked, top-level, non-ghost, NOT in-tick)
  queue to server; server assigns monotonic `index` + execution `tick`,
  executes locally in `runGameCommands`, broadcasts (failed commands too —
  index continuity; they fail identically everywhere; error UI only on the
  issuing machine).
- `GameCommands::isInTickExecution()` — set around the deterministic tick
  section; commands issued there (AI, replicated replay) apply inline on
  every peer instead of being re-queued. Required because AI runs on all
  peers (host-only AI desyncs: `aiThink` mutates state directly).
- Wire format: `GameCommandPacket` carries field-serialized Args
  (`GameCommands::encode/decodeCommandArgs`, little-endian, codec table in
  `CommandSerialization.cpp`; raw-registers fallback for 3 complex + 8 stub
  commands). `Network::toWirePacket`/`fromWirePacket` convert to/from the
  in-memory `QueuedGameCommand`. Bump `kNetworkVersion` on any wire change
  (server rejects mismatched clients with a readable message).
- Desync handling: mismatch → both sides dump S5 to `save/desync/`
  (`desync_<role>_tick<T>_at<C>.sv5`, diff offline with `compare`), client
  freezes (`resyncing` status) and re-requests the snapshot; stale queued
  commands are pruned by index after `processFullState`.

## Determinism traps (learned the hard way)

- Never mutate `GameState` from UI/interface-tick code — it desyncs peers.
  Machine-local state goes in file-local statics (precedent:
  `ownerStatusThrottle` in CompanyManager.cpp). Synced mutations go through
  game commands (precedent: `switchCompany` cheat wraps
  `setControllingId`).
- `GameState.playerCompanies[2]` (S5-synced) is an ordered pair: the SET is
  identical across peers, slot 0 = "my company" (vanilla convention,
  swapped per machine by `Title::loadTitle`; `CompanyManager::reset` has
  the network branch). The two bytes legitimately differ between peers —
  byte-level comparisons must treat them as per-machine
  (`GameSaveCompare` logs rather than fails them).
- `isPlayerCompany` = membership in that 2-slot array; it gates AI-think.
  With >2 humans this must move to a deterministic human-company set
  (session model Phase B).
- `Args(registers)` constructors and `operator registers()` are not always
  inverses (rename commands: ctor stores the 12-byte chunk at buffer
  start; operator reads from the chunk offset) — generic codec round-trips
  can corrupt; renames use a dedicated chunk codec.
- `registers` sub-fields: writing e.g. `regs.cx` leaves the upper half of
  `ecx` at the 0xCCCCCCCC default — consistent with how vanilla call sites
  behave, but don't compare full registers blobs for equality.
- Sign extension: packing two int16 coords into one int32
  (`(b << 16) | a`) corrupts `b` when `a < 0` — mask with `& 0xFFFF`
  (fixed in Raise/LowerLand).

## Codebase facts

- Company creation: `CompanyManager::createPlayerCompany()` (player, sets
  playerCompanies), `createAiCompany()`/`produceCompanies()` (AI, every
  192 ticks, runs on all peers), both over the internal
  `createCompany(competitorId, isPlayer)` allocator
  (CompanyManager.cpp:~546). Company slot free ⇔ `name == StringIds::empty`.
  `kMaxCompanies = 15`, `CompanyId::neutral = 15`, `null = 255`. No
  player-vs-AI flag on Company — extrinsic via playerCompanies.
- `createPlayerCompany` reads machine-local Config (preferred names) — NOT
  network-deterministic as-is; a replicated variant must carry or
  deterministically derive its inputs. Names can be applied afterwards via
  the existing rename commands.
- `_updatingCompanyId` (GameCommands.cpp file-static) is process-local and
  recomputed per dispatch — distinct from `playerCompanies[0]`.
- Vanilla two-player leftovers: `MultiPlayer.h` flag bag, unreachable
  `S5::LoadFlags::twoPlayer` path, hidden TitleMenu toggle, hard-coded
  "company 0 = host, company 1 = client" convention. Command ids 67/70/72
  are still unimplemented multiplayer stubs; 69 is now `createPlayerCompany`
  (session model Phase B). `Ui.cpp`'s vanilla `do_69()` call site (under
  `MultiPlayer::flags::flag_2`) is dead in practice on this branch but was
  updated to reference the renamed enum value so it still compiles.
- Session model Phase B (`createPlayerCompany`, game command id 69): a game
  command's implementation function is called *twice* per apply (see
  `loc_4313C6` in `GameCommands.cpp`) — once with `Flags::apply` stripped
  (cost estimation) and once with it set (real effect). Anything that
  consumes the synced PRNG (company allocation, competitor selection) must
  only run on the `flags & Flags::apply` branch, exactly like
  `removeCompanyHeadquarters`/`updateOwnerStatus`, or it desyncs by
  advancing the RNG twice on the machine that pays for both passes.
  `CompanyManager::createJoiningPlayerCompany()` reuses `selectNewCompetitor()`
  (the AI-creation path) rather than `createPlayerCompany()`'s
  Config-preferred-face logic, and deliberately does not touch
  `playerCompanies[]` — that pair stays per-machine, set later by the
  client itself via `setControllingId` on receipt of `CompanyAssignmentPacket`.
- Human-company set: `CompanyManager` keeps a file-static `uint16_t`
  bitmask (`markCompanyAsHuman`/`isHumanCompany`/`clearHumanCompanies`/
  get·set `HumanCompanyMask`) mirroring which companies are human-controlled
  beyond the vanilla two-slot `playerCompanies[]` pair. It is NOT part of
  `GameState` — it is a machine-local mirror kept identical across peers
  purely because it is only ever mutated (a) inside the replicated
  `createPlayerCompany` command (same execution on every peer) or (b) from
  the snapshot's `ExtraState::humanCompanyMask` for late joiners, plus one
  direct call in `Network::openServer()` to mark the host. `isPlayerCompany`
  now ORs membership in `playerCompanies[]` with
  `SceneManager::isNetworked() && isHumanCompany(id)`, so single-player/AI
  gating is untouched and only networked games consult the extra set.
- Server-side join tagging: `NetworkServer`'s internal command queue element
  is `ServerQueuedGameCommand { QueuedGameCommand cmd; client_id_t requestedBy; }`
  (`requestedBy == 0` means "not join-tagged"; client ids start at 1) — this
  tag never reaches the wire (`QueuedGameCommand`/`GameCommandPacket` are
  unchanged). `onReceiveStateRequestPacket` queues a `createPlayerCompany`
  command tagged with the joining client's id right after sending the last
  state chunk; `runGameCommands()` reads `LegacyReturnState::lastCreatedCompanyId`
  after `doCommandForReal` to resolve the assignment and send
  `CompanyAssignmentPacket` (new, targeted, not broadcast).
- Headless fixture caveat, resolved for the competitor case: OpenGraphics
  ships zero competitor objects, so `createPlayerCompany` used to always
  fail (`selectNewCompetitor()` returning `kNullObjectId`), leaving a
  joining client a spectator by design. A hand-crafted fixture object now
  fixes this (see § Competitor object fixture) — joins are assigned a real
  company. Towns/industries are still absent (separate object types, not
  addressed by this fixture), so AI-active sync still can't be exercised.
- A company permanently consumes its competitor object
  (`selectNewCompetitor` skips in-use competitors) — fixtures need one
  competitor object per company that will ever exist; the generator emits 8
  (`FIXTCP00..07`). Delete `%APPDATA%\OpenLoco\plugin.dat` (the object
  index cache) after changing the object set or the new objects are not
  seen.
- gensave creates a player company when a competitor object is available
  (host then owns a company: coop policy + client command tests work);
  logs and continues without one.
- Join policy (Phase C): host-side `--join_policy <own|coop|spectator>`
  (CommandLine `JoinPolicy`, applied in
  `NetworkServer::onReceiveStateRequestPacket`). `coop` reuses the host's
  company via the assignment packet — no game command involved, since no
  state changes; `spectator` (and failed company creation) sends an
  explicit null `CompanyAssignmentPacket` so the client knows its role.
- Chat is packet-level (`sendChatMessage` relay), never a game command;
  UI in `Ui/Windows/Chat.cpp` (WindowType::chat = 38).
- Windows are declared via facades in `Ui/WindowManager.h`, registered in
  `WindowType.h`, sources listed explicitly in `src/OpenLoco/CMakeLists.txt`.
- `Ui::render`/`Gfx::renderAndUpdate` already null-check `_window` — that,
  not sprite hardening, is why headless works.
- Upstream CI's determinism test uses two private repos (LocomotionAssets,
  TestData) — unavailable to forks; our gensave path replaces them.

## Session / environment

- Branch `multiplayer`; `origin` = upstream OpenLoco/OpenLoco (SSH). User
  has no fork remote yet — do not push until one is added.
- User does not own Locomotion (not free; ~€6 GOG/Steam, frequent sales).
- Commit style: imperative subject, body explains why, trailer
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
