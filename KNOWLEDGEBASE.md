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
    (same seed ⇒ identical file). Current fixture has 0 towns/industries/
    competitors because OpenGraphics lacks those object types.
  - `--headless` skips window/cursor/audio and runs the plain update loop;
    `host`/`join` work under it.
- Proven smoke test: `gensave` → start host (`--headless host fixture.sv5`)
  → start client (`--headless join 127.0.0.1`) → wait → check
  `%APPDATA%\OpenLoco\logs\openloco_*.log`. Success = exactly one
  `Accepted new client`, client `Scene transition: boot -> gameplay`, zero
  `[ERR]`/desync lines.
- stdout is fully buffered and LOST when the process is killed — use the
  file logs (they flush per line), or stderr.
- `simulate <save> N [-o out]` + `compare` = single-process determinism
  harness (upstream CI uses this with private assets).

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
  "company 0 = host, company 1 = client" convention. Command ids
  67/69/70/72 are unimplemented multiplayer stubs (69 planned for
  `createPlayerCompany`).
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
