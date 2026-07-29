# Multiplayer branch — knowledge base

Hard-won facts about the codebase and environment. Companion to `TASKS.md`
(status) and `docs/multiplayer.md` (design).

## Build & test (this machine)

- CMake: `"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`
- Configure: `cmake --preset windows` (VS2022, `x64-windows-static` vcpkg).
  Build: `cmake --build --preset windows-release` (add `--target App` or
  `OpenLocoTests` for speed). Tests: `ctest -C Release` in `build\windows`
  — currently 145/145.
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

## CI

- `.github/workflows/multiplayer-sync.yml`: a new, standalone workflow (does
  NOT modify upstream's `.github/workflows/ci.yml`) that runs the headless
  sync smoke matrix on push to `multiplayer` + `workflow_dispatch`, on this
  fork (github.com/RikPi/OpenLoco).
- One `windows-2022` job, deliberately mirroring ci.yml's Windows job (the
  `windows-2022` matrix entry: `windows` configure preset, `windows-release`
  build preset, `build\windows\Release` output) so the `actions/cache@v5`
  vcpkg binary cache key (`windows-2022-64-vcpkg-${{ env.ImageVersion }}-${{
  hashFiles('vcpkg*.json') }}`) is byte-for-byte the same as ci.yml's — cache
  entries are shared across workflows on the same ref, so this job rides on
  the cache ci.yml already warms on the branch instead of rebuilding vcpkg
  packages from scratch. Only the `App` target is built (not
  `OpenLocoTests`); `vcpkg.json` is a manifest (no per-target feature gating),
  so the same deps install either way — skipping the test target only saves
  its compile time.
- Headless fixtures are (re)created in-job every run, not committed: stub
  `Data\g1.DAT` (8 zero bytes) under a workspace-relative
  `build\fake-locomotion`, `%APPDATA%\OpenLoco\openloco.yml` with
  `allow_multiple_instances: true`, and `python3 scripts/gen_competitor_object.py`
  pointed explicitly at `build\windows\Release\data\objects\Competitor` (the
  script's own default assumes that same layout when run with no args, but
  the job passes it explicitly — see prior note on `$PSScriptRoot`-relative
  defaults not matching CI's working directory in general).
- Smoke matrix: three sequential `scripts\run_sync_smoke_test.ps1`
  invocations (own+`-TestRename`, coop+`-TestRename`, spectator),
  `-RunSeconds 45` each, explicit `-BuildDir`/`-LocomotionPath` (never rely on
  the script's `$PSScriptRoot`-derived defaults from a CI working directory).
  Deliberately NOT wrapped in `if: always()` — the script clears
  `%APPDATA%\OpenLoco\logs` at the start of each run, so if an earlier policy
  fails and a later step still ran, its fresh log-dir wipe would destroy the
  failing run's diagnostic logs before the final upload step ever sees them.
  Default step semantics (stop the job on first failing step) keep the
  failing run's logs intact for `actions/upload-artifact@v4` (`if: failure()`,
  path `%APPDATA%\OpenLoco\logs\*`, `if-no-files-found: warn` since the dir
  may not exist yet if the build itself failed).
- Locally validated (this cannot be done for the Actions run itself, only its
  static ingredients): the YAML parses (`python3 -c "import yaml;
  yaml.safe_load(open(...))"`; pyyaml renders the bare `on:` key as Python
  `True` — a known YAML-1.1 quirk in the *parser*, not a workflow bug, GitHub
  Actions itself reads it as the string key); every referenced path exists
  (`scripts/run_sync_smoke_test.ps1`, `scripts/gen_competitor_object.py`, the
  `windows`/`windows-release` CMake presets); the smoke script's parameter
  names (`-BuildDir`, `-LocomotionPath`, `-JoinPolicy`, `-TestRename`,
  `-RunSeconds`) match what the workflow passes; the stub-`g1.DAT` and
  `openloco.yml`-write PowerShell snippets were run verbatim against a
  scratch temp dir (confirmed: 8 zero bytes, no BOM on the YAML file); `git
  describe`-based versioning (`cmake/OpenLocoVersion.cmake` calls
  `find_package(Git)`) is why `fetch-depth: 0` is kept, matching ci.yml.
- What only a real Actions run can prove: actual vcpkg binary-cache hit rate
  (first run on the branch has to build vcpkg packages for real — same as
  ci.yml's first run ever did); whether UDP loopback (127.0.0.1) triggers any
  interactive Windows Firewall prompt on a hosted `windows-2022` runner — no
  contrary evidence found (Windows Filtering Platform documentation states
  loopback traffic is exempt from WFP filtering, and hosted runners have no
  interactive desktop session to show a GUI prompt anyway, but this project
  has not observed it directly); actual wall-clock time against the 60-minute
  job timeout; whether `python3` (vs `python`) resolves on the actual
  `windows-2022` image as assumed.

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
- `loadSaveQuitGame` (21) is deliberately LOCAL-ONLY when networked (see
  the `isLocalOnly` carve-out in `doCommand`): saving exports the identical
  local state, loading/quitting means leaving the session. Vanilla instead
  replicated the prompt to every peer and resolved Save through a dead
  two-player handshake (MultiPlayer flags 2/3/4 → commands 69/70/72) —
  removed; beware resurrecting any `do_69`-style helper, id 69 is
  createPlayerCompany now.
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

## Client round-trip test hook

- `--test_rename <name>` (`CommandLine.h`/`.cpp`): a hidden test-only CLI
  option, deliberately absent from `printHelp` (a code comment marks it as a
  test hook instead). On a joining client it drives a self-verifying
  round-trip test of a client-issued game command through the whole lockstep
  pipeline (client `doCommand` → network queue → server orders + broadcasts
  → both peers apply at the same tick) — the one thing the existing smoke
  test never exercised (the client only ever *received* commands).
- Lives in `NetworkClient` as a small state machine
  (`_testRenameState`: `none` → `assignedWaitingConnected` → `pendingIssue`
  → `pendingVerify` → `done`), driven from `onUpdate()` — i.e. the
  main-thread loop *outside* `GameScene::tick()`, so `GameCommands::
  isInTickExecution()` is false and the issued command takes the normal
  networked `doCommand` path (queue to server), never a direct `GameState`
  mutation, satisfying the "don't mutate state outside a command" rule.
- **Timing bug found and fixed**: arming the 2s-to-issue countdown directly
  off `receiveCompanyAssignmentPacket` is wrong — that packet routinely
  arrives *before* the state-transfer chunks finish and `_status` flips to
  `NetworkClientStatus::connected` (observed ~7s gap between "Assigned
  company" and "Scene transition: boot -> gameplay" in testing). If the
  rename is issued while not yet `connected`, `Network::isConnected()` is
  false, so `GameCommands::doCommand`'s networked branch
  (`!isGhost && !_inTickExecution && Network::isConnected()`) is skipped and
  it falls through to `doCommandForReal` — a **local, non-networked apply**.
  In practice it also had no visible effect (pre-gameplay state), but the
  real danger is that path bypassing the server entirely, which would prove
  nothing (or desync a real game). Fix: on assignment, only record
  `assignedWaitingConnected`; `updateTestRenameHook()` waits for `_status ==
  connected` before starting the real 2s deadline.
- **Rename chunk order fact** (learned by reading the two existing call
  sites, `Ui/Windows/CompanyWindow.cpp`'s `renameCompany` and
  `CompanyManager.cpp`'s preferred-name setter — both agree): the 36-char
  name buffer is copied whole into `ChangeCompanyNameArgs::buffer`, then
  `doCommand` is called **three times with `bufferIndex` in the order 1, 2,
  0** — not 0, 1, 2. `ChangeCompanyNameArgs::operator registers()` maps
  bufferIndex → buffer offset via `{24, 0, 12}` (index 0 reads the *last*
  12 chars), and the server-side command
  (`GameCommands::changeCompanyName`) maps bufferIndex → reassembly offset
  via `transformTable = {2, 0, 1}`, only committing the rename when
  `bufferIndex == 0` arrives (the third call). Getting the order wrong
  either scrambles the name or (bufferIndex 0 first) commits an
  empty/garbage name before the other two chunks ever arrive. This already
  has a dedicated wire codec (`kRenameChunkCodec` /
  `encodeRenameChunk`/`decodeRenameChunk` in `CommandSerialization.cpp`,
  shared with `vehicleRename`/`changeStationName`/`changeCompanyOwnerName`/
  `renameTown`/`renameIndustry`) precisely because the generic typed codec
  can't round-trip this buffer-offset scheme (see existing note under
  Determinism traps).
- The test hook must call `GameCommands::setUpdatingCompanyId(company)`
  before `doCommand` — `_updatingCompanyId` (not any per-window "current
  company") is what `queueGameCommand` attributes the command to.
- Verification reads the company's *actual* current name back out of
  `GameState` via `StringManager::formatString(buffer, company->name)` (from
  `Localisation/Formatting.h`, not `Localisation/StringManager.h` — the
  latter only has the raw string-table primitives; `formatString` lives in
  `Formatting.h`, same as the rename command's own name-clash check).
- `scripts\run_sync_smoke_test.ps1` gained `-TestRename` (passes
  `--test_rename SyncTest` to the client, asserts `[TEST] rename verified:
  'SyncTest'` in the client log when `-Expect company`). Also fixed the
  script's default `-Expect` derivation: it previously treated every policy
  except `own` as `spectator`, which is wrong for `coop` (shares the host's
  real company via the assignment packet — a `company` outcome, not
  `spectator`); default is now `spectator` only for `-JoinPolicy spectator`.

## Player roster and graceful disconnect (network version 4)

- Wire additions in `Packet.h`: `PacketKind::rosterUpdate` /
  `RosterUpdatePacket` (`RosterEntry{ client_id_t id; CompanyId company;
  uint8_t nameLength; char name[31] }`, `count` prefix, max 32 entries -
  `static_assert(sizeof(RosterUpdatePacket) <= kMaxPacketDataSize)`) and
  `PacketKind::serverClosing` / `ServerClosingPacket` (no payload).
  `RosterUpdatePacket::size()` follows `SendChatMessage`'s pattern exactly
  (`this->entries + count`, not `sizeof(RosterUpdatePacket)`) so only the
  entries actually in use go on the wire, not the full reserved 32-entry
  buffer.
- `Network::PlayerRosterEntry` (`Network.h`) is the presentation-level type
  (`id`, `company`, `std::string name`) UI code and `Network.cpp` work with;
  `Network::toWirePacket`/`fromWirePacket` overloads convert to/from
  `RosterUpdatePacket`. This is intentionally a separate type from the wire
  `RosterEntry` - keeps roster data out of `GameState`/game commands by
  construction (nothing here can accidentally be typed as, or fed to,
  `GameCommands::doCommand`).
- Single facade call for all roster reads: `Network::getPlayerRoster()`.
  On the server it rebuilds from the live `_clients` list plus a synthetic
  host entry every call (`NetworkServer::buildRoster()`, cheap, no caching
  needed); on a client it returns the last `RosterUpdatePacket` received
  (`NetworkClient::getRoster()`). `client_id_t 0` is reserved for the host
  in both the roster and chat (matches the pre-existing convention in
  `NetworkServer::sendChatMessage`, which already used sender id `0` for
  host-originated chat).
- `NetworkServer::broadcastRosterUpdate()` is called from every place the
  roster can change: `createNewClient` (client accepted), coop/spectator
  assignment in `onReceiveStateRequestPacket`, the `createPlayerCompany`
  join-flow resolution in `runGameCommands` (both the success and
  spectator-fallback branches), and `removedTimedOutClients` (only if a
  client was actually removed).
- Display-name rule (`Network::resolveDisplayName`, `Network.h`/`.cpp`):
  strip everything from the first NUL onward, then `Utility::trim` the
  rest; falls back to `"Player #<id>"` if that's empty. This replaced a
  latent bug in `Utility::nullTerminatedView` (`String.hpp`) - it returns
  `std::string_view(src, N)` on **both** branches of its loop (the
  early-return-on-NUL branch was dead code), so a short name in a `char[32]`
  buffer always carried the trailing NUL padding into the resulting
  `std::string`, which is what actually produced the "blank padding" in
  logs (embedded NULs mid-string, not just trailing whitespace). Left
  `nullTerminatedView` itself unchanged (out of scope; used elsewhere) and
  just stopped using it for names. Applied at the one place a raw name
  enters the system (`NetworkServer::createNewClient`, from `ConnectPacket`)
  and for the host's own `Config::get().preferredOwnerName` in
  `buildRoster()` - every other log/roster/chat site downstream already
  gets the resolved name.
- Chat sender names: `Network::receiveChatMessage` now looks the sender's
  `client_id_t` up in `Network::getPlayerRoster()` (falls back to the old
  `"Player #N"` only if not found, e.g. the very first message before any
  `RosterUpdatePacket` has arrived) and passes the resolved name to
  `Ui::Windows::Chat::addMessage`, whose signature changed from
  `(uint32_t clientId, ...)` to `(std::string_view senderName, ...)` -
  name resolution now happens once, at the network layer, not duplicated
  in the window.
- `Ui/Windows/PlayerList.cpp`: read-only window, modeled closely on
  `Chat.cpp` (same facade/registration/CMakeLists pattern). `draw()` calls
  `Network::getPlayerRoster()` directly every frame it's visible (cheap,
  see above) and renders `"name - company N"` / `"name - spectator"` per
  entry - no scrolling/sorting, intentionally minimal. New `WindowType`
  slot: `playerList = 62`, the first value past `debug = 61` (the
  highest-numbered existing entry; slots `5` and `8` are also unused gaps
  earlier in the enum but 62 avoids any doubt about whether a low gap has
  latent vanilla meaning).
- No dedicated UI trigger exists for the player list yet (would need a new
  caption/menu string, i.e. asset changes, to do "properly"). Cheapest
  reachable trigger per the task: `Ui::Windows::PlayerList::open()` is
  called right alongside `Chat::open()` in
  `TimePanel::beginSendChatMessage` - opening chat also opens the roster.
  Slightly redundant UX (every chat-open pops both windows) but needs zero
  new localised strings; `PlayerList`'s own caption uses `StringIds::empty`
  like `NetworkStatus.cpp` does. A dedicated "show players" dropdown entry
  or a button on the Chat window are both better long-term options, noted
  as a follow-up rather than done here.
- `NetworkServer::onClose()` sends `ServerClosingPacket` to all clients via
  `sendPacketToAll` before `NetworkBase::close()` clears `_sockets`.
  `NetworkConnection::sendPacket` writes to the UDP socket synchronously
  (`_socket->sendData(...)`), so this does not depend on the receive
  thread (already joined by the time `onClose()` runs) or on any extra
  flush - the send either succeeds as a normal best-effort UDP write or it
  doesn't, same as every other packet.
- `NetworkClient::receiveServerClosingPacket` logs "Server is shutting
  down", posts the same text to the Chat window (`Ui::Windows::
  Chat::addMessage("Server", ...)` - deliberately not `NetworkStatus`,
  to avoid opening a fresh window with a `this`-capturing close callback
  moments before the `NetworkClient` object is destroyed by
  `Network::tick()`'s post-`update()` `isClosed()` check - see
  `NetworkBase::close()`/`Network::close()`), requests the title scene
  (`SceneManager::requestScene` just sets a deferred flag, safe to call
  from anywhere on the main thread), then calls `close()`. Calling `close()`
  from inside a packet handler is an already-established pattern here -
  `processReceivedPackets()`'s loop explicitly checks `_serverConnection ==
  nullptr` after each handler for exactly this reason (see
  `receiveConnectionResponsePacket`'s rejection path, pre-existing).
- Verified headless (2-player, `own` join policy): client log shows
  `[INF] Roster: 2 players: 'Player #0' company 0, 'Player #1' spectator`
  right after joining - the first broadcast, sent when the client is
  accepted, before its `createPlayerCompany` command has resolved - then
  `[INF] Roster: 2 players: 'Player #0' company 0, 'Player #1' company 1`
  once the assignment lands (second broadcast, from `runGameCommands`),
  immediately followed by `Scene transition: boot -> gameplay`; zero
  `[ERR]`/desync lines. Names show as `Player #0`/`Player #1` (not garbage/blank) because
  the headless fixture's `preferredOwnerName`/connect name are empty and
  the fallback rule is working as designed, not because trimming failed.
- `serverClosing` could not be exercised end-to-end headless at the time
  this was written: `--headless` had no clean-quit trigger (`Network::close()`
  was only ever reached via UI quit flows) - verified by code review plus
  the fact the build/tests/existing smoke test still pass. What *was*
  verified headless is the deliberately-different hard-kill path:
  `Stop-Process` on the host produces no `serverClosing` (expected - the
  process never runs its destructor chain), and the client's pre-existing
  15s connection-timeout path still fires correctly (`Connection with
  server timed out` / `Disconnected from server` in the client log, process
  stays alive afterwards, no crash). **Now exercised**: see §
  Graceful shutdown test hook below.

## Host-driven mid-session load test hook

- `--test_host_load <seconds>` (`CommandLine.h`/`.cpp`): a hidden,
  host-side test-only CLI option (deliberately absent from `printHelp`,
  same pattern as `--test_rename`). Once the server has been up for
  `<seconds>` AND at least one client's join assignment has resolved
  (`Client::assignmentResolved`), it programmatically drives the exact
  sequence `Game::loadGame`'s networked-host branch uses (see Game.cpp) —
  `S5::importSaveToGameState(fs::u8path(getCommandLineOptions().path), ...)`
  (the same fixture the host was started with, via the `host <path>` CLI
  arg), then `SceneManager::requestScene(gameplay)`, then the facade
  `Network::requestAllClientsResync()` — without needing the file-browse
  dialog that path normally opens.
- Lives in `NetworkServer` as `updateTestHostLoadHook()`, driven from
  `onUpdate()` (the main-thread network tick, outside `GameScene::tick()`)
  — mirroring how the client's `--test_rename` hook drives itself from its
  own `onUpdate()`. `_testHostLoadDone` (bool) ensures it fires once.
  `_startTime` (set in `listen()`) gives the uptime clock.
- **Naming trap avoided**: inside a `NetworkServer` member function,
  unqualified `requestAllClientsResync()` resolves to the *member*
  `NetworkServer::requestAllClientsResync()` (ordinary C++ member-lookup
  hides the free function of the same name in the enclosing
  `OpenLoco::Network` namespace, regardless of visibility) — calling that
  member directly would skip the facade's human-company-set reset
  (`CompanyManager::clearHumanCompanies()` + `markCompanyAsHuman(host)` in
  `Network.cpp`). The hook therefore calls the fully-qualified
  `Network::requestAllClientsResync()` (resolves via the file's `using
  namespace OpenLoco;` bringing the nested `Network` namespace into scope
  for qualified lookup), exactly like `Game::loadGame` does.
- Waiting for `assignmentResolved` (not just "a client connected") matters:
  reloading before the join flow has resolved would prove nothing about
  the resync path, and could race the initial `createPlayerCompany`
  command.
- `scripts\run_sync_smoke_test.ps1` gained `-TestHostLoad` (passes
  `--test_host_load 20` to the host; asserts the client log contains `Host
  loaded a new game` followed by a *second* `Assigned company \d+` line —
  the fresh post-reload assignment, found by comparing `Select-String`
  match `LineNumber`s — plus the standard zero-error assertion). Enforces
  `-RunSeconds >= 50` (20s to fire + resync/reassignment time) and
  `-JoinPolicy own`/`coop` (a spectator client never gets an `Assigned
  company` line to check for a second one).
- Verified headless (own policy, 55s run): host logs `[TEST] host reloaded
  save` then `Requested resync from 1 client(s) after state reload`
  (interestingly followed by `Scene transition: gameplay -> gameplay` — the
  host re-enters the same scene id, which is a legitimate no-op transition,
  not a bug); client logs `Host loaded a new game; resyncing`, then a fresh
  `Assigned company 1` (the company id happened to come out the same as
  before the reload — expected, since the reloaded fixture recreates the
  same single competitor slot the host itself doesn't consume); zero
  `[ERR]`/desync lines for the remainder of the run.

## Graceful shutdown test hook

- `--test_shutdown_after <seconds>` (`CommandLine.h`/`.cpp`): a hidden,
  host-side test-only CLI option, same hidden-hook pattern as the two
  above. Once the server has been up for `<seconds>`, it logs `[TEST]
  closing server` and calls `close()`.
- Lives in `NetworkServer` as `updateTestShutdownHook()`, also driven from
  `onUpdate()`. `_testShutdownTriggered` (bool) ensures it fires once.
- **Which `close()`, and why it's safe**: unqualified `close()` inside a
  `NetworkServer` member function resolves to the *inherited*
  `NetworkBase::close()` (member lookup again hides the free
  `Network::close()` facade of the same name) — this is the
  already-established pattern `NetworkClient::receiveServerClosingPacket`
  uses to close itself from inside its own packet handler.
  `NetworkBase::close()` only runs `onClose()` (which sends
  `ServerClosingPacket` to every client and drops the networked scene
  flags) and sets `_isClosed = true`; it does **not** destroy the
  `NetworkServer` object. The owning `unique_ptr` (`Network.cpp`'s static
  `_server`) is only reset by the facade `Network::tick()`, and only
  *after* `update()`/`onUpdate()` returns — so there is no
  self-destruction hazard in calling `close()` from inside `onUpdate()`.
  The host process is never exited: once the networked/networkHost scene
  flags are gone, it simply keeps simulating as single-player, which is
  the intended behaviour (the hook tests the *client's* reaction, not host
  teardown).
- `scripts\run_sync_smoke_test.ps1` gained `-TestShutdown` (passes
  `--test_shutdown_after 25` to the host; asserts the client log contains
  `Server is shutting down` and does *not* contain `timed out`). No
  standard assertion needed adjusting: neither process exits under this
  hook (the host keeps running single-player; the client returns to title
  but its process stays up), so the existing both-alive-at-end check
  already holds. Enforces `-RunSeconds >= 35`.
- Verified headless (own policy, 40s run): host logs `[TEST] closing
  server` then `Server closed`; client logs `Server is shutting down` →
  `Disconnected from server` → `Scene transition: gameplay -> title`; no
  crash, no timeout message, zero `[ERR]`/desync lines.

## Title sequence fixture

- Exercising the graceful-shutdown hook's client-side reaction end-to-end
  for the first time (previously only code-review-verified — see §
  Graceful disconnect) surfaced a genuine headless-fixture gap:
  `Title::loadTitle()` (`Title.cpp`) unconditionally reads
  `Data/title.dat` from the Locomotion install path whenever the title
  scene is entered, and the stub `fake-locomotion` install dir only ever
  shipped `Data\g1.DAT`. Without it, the client crashed outright (an
  uncaught `Exception::RuntimeError` thrown by `FileStream`'s constructor
  when the read-mode `fopen` fails — this happens *before*
  `S5::importSaveToGameState(Stream&, flags)`'s own try/catch even starts,
  since it's thrown one call frame earlier, in the `(const fs::path&,
  flags)` overload). Side note, not fixed (unrelated file, out of scope):
  `FileStream`'s failure message is hard-coded to say "for writing" even
  when the failing open was for reading.
- Fixed test-fixture-only (no production code touched): the smoke script
  now copies its `gensave` fixture to `Data\title.dat` under
  `$LocomotionPath` every run (harmless for runs that never reach title).
  A plain copy isn't enough, though — `importSaveToGameState` additionally
  requires the S5 `Header`'s `isTitleSequence` bit
  (`S5::HeaderFlags::isTitleSequence`, gated behind the
  `DO_TITLE_SEQUENCE_CHECKS` macro, unconditionally `#define`d at the top
  of `S5.cpp`) to be set, and rejects the file ("File was not a title
  sequence") otherwise; no `S5::SaveFlags` option sets that bit on export,
  so a plain fixture always fails it as-is.
- `run_sync_smoke_test.ps1`'s `Set-TitleSequenceFlag` patches that single
  bit into an existing S5 file in place, then recomputes the file
  checksum — mirroring the reverse-engineered byte-patch approach already
  used for the Competitor object fixture (`gen_competitor_object.py`).
  Byte layout, reverse-engineered from `S5.cpp`/`SawyerStream.cpp` and
  checksum-verified against a real `gensave` fixture before trusting it
  (decoding chunk-relative byte 1 yielded exactly `HeaderFlags::
  hasSaveDetails` (8), and bytes 4-7 decoded to the exact `kCurrentVersion`
  constant `0x62262`):
  - The first chunk in any S5 file is always the 32-byte `Header`
    (`S5.h`), written by `exportGameState` as `[encoding u8][length u32
    LE][encoded payload]` with `encoding = SawyerEncoding::rotate` (3, the
    file's very first byte).
  - `SawyerStreamWriter::encodeRotate` applies a per-byte `std::rotl(byte,
    code)` with `code` cycling `1, 3, 5, 7, ...` (`code_i = (1 + 2*i) mod
    8`, restarting at 1 for each chunk); `SawyerStreamReader::decodeRotate`
    inverts it with `std::rotr(byte, code)` using the *same* code
    sequence — easy to misread as symmetric (both named "rotate", the
    reader's own source even calls `std::rotr` too — it's the *writer*
    that uses `rotl`, confirmed by reading `SawyerStreamWriter::
    encodeRotate` specifically, not just grepping "rotate" and finding the
    reader function first, which is the mistake that produced a garbage
    decode of the version field on the first attempt here).
  - `Header::flags` (`HeaderFlags`) is the chunk's byte index 1, so `code
    = 3` there; patch = decode with `rotr(_, 3)`, OR in `0x04`
    (`isTitleSequence`), re-encode with `rotl(_, 3)`.
  - The whole file's trailing 4 bytes are a checksum: a plain additive sum
    of every other byte (`SawyerStreamWriter::write`/`writeChecksum`), not
    a rotate/CRC — trivial to recompute after any in-place byte edit.
  - No SaveFlags option needs to change and no other chunk shifts, since
    the rotate cipher is byte-length-preserving and only one byte's
    plaintext changes.

## Options multiplayer checkbox, config-driven host bind/port, chat polish

- "Enable multiplayer" checkbox: `Ui/Windows/Options.cpp`, Miscellaneous tab
  (`namespace Misc`). A new "Multiplayer" group box was inserted between the
  existing "Vehicle behaviour" and "Save options" groups (window height grew
  266 -> 302; every widget below the insertion point - the save-options
  group box, its two dropdown/stepper rows, and the export-plugin-objects
  checkbox - had to shift down by the same 36px, since widget positions
  here are absolute, not flow-laid-out). Widget-array order must exactly
  match the `enum widx` declaration order (positional `WidgetIndex_t`) -
  the new `groupMultiplayer`/`enableMultiplayer` enum values and their
  `Widgets::GroupBox`/`Widgets::Checkbox` entries were inserted at the same
  relative position in both places. Bound to `Config::get().network.enabled`
  + `Config::write()` in a new `enableMultiplayerMouseUp()`, following the
  exact shape of every other checkbox handler in this tab (e.g.
  `disableTownExpansionMouseUp`). Toggling also calls
  `WindowManager::invalidate(WindowType::titleMenu)` - required because the
  title screen's multiplayer button's `hidden` flag is only recomputed in
  `TitleMenu.cpp`'s own `prepareDraw` (gated on this same
  `config.network.enabled`, see line ~189), which only reruns on that
  window's own update/redraw cycle; an explicit invalidate is the
  established way other cross-window config toggles in this file already
  handle this (c.f. `enableCheatsToolbarButtonMouseUp` invalidating
  `WindowType::topToolbar`).
- Localisation choice for the checkbox: this branch's language files are
  `data/language/*.yml`, keyed by numeric string id under a top-level
  `strings:` map; `StringIds.h` gives each id a C++ name.
  `Localisation::loadLanguageFile()` (`LanguageFiles.cpp`) always loads
  `en-GB.yml` first as the fallback table, then overlays the selected
  language's file on top of it (`swapString` only touches ids present in
  that second file) - so any id missing from a non-English file simply
  falls back to its English text, never a crash or blank. This branch's
  existing chat window (`chat_title`/`chat_send_message`/`chat_instructions`
  = ids 1716-1718) had already established the preferred pattern here:
  reuse a vanilla string id whose *original, already-translated-into-all-
  17-languages* meaning already matches the new use, rather than adding a
  new one. Applying that same search here found id 1466 ("Two Player Game"
  in en-GB; already translated in every shipped language, e.g. nl-NL
  already literally says "Multiplayer") - a vanilla two-player-setup-dialog
  title string that is completely dead code on this branch (grepped: no
  `StringIds::` name existed for it, and no raw `1466` literal appears
  anywhere in `src/`). It was named `StringIds::multiplayer_group_title`
  and reused as the new group box's title, with zero yml changes. No
  similarly-fitting existing id existed for the checkbox's own label
  wording ("Enable multiplayer" as an actual clickable action, not a
  section title), so one new string was appended en-GB-only:
  `StringIds::option_enable_multiplayer` = 2458 (the next free id after the
  existing table's max, 2457). The tooltip reuses
  `StringIds::title_multiplayer_toggle_tooltip` (1567, "Toggle between
  single player and two player mode") unchanged - already the exact tooltip
  on the TitleMenu's own multiplayer toggle button, so the wording already
  fits describing this checkbox too.
- Host bind/port via config: `Config::Network` (`Config.h`) gained
  `std::string bind` and `uint16_t port{ 11754 }`, read/written under
  `network.bind`/`network.port` in `Config.cpp` (same read/write shape as
  the pre-existing `network.enabled`). `Network::openServer()`
  (`Network.cpp`) now resolves bind/port as: CLI `--bind` if non-empty,
  else `Config::get().network.bind`; CLI `--port` if present
  (`std::optional`), else the config port (falling back to
  `Network::kDefaultPort` only in the defensive case config port is 0,
  which config normally never produces given its `11754` default). CLI
  behaviour is byte-for-byte unchanged (headless test hosts and the smoke
  test always pass explicit `--bind`/`--port`-equivalent defaults through
  the CLI path, so neither fallback branch is ever exercised there - this
  is a pure addition for the previously-dead "UI hosts with no CLI args"
  case). No UI was added to edit these two config fields directly (out of
  scope for this pass - a host-setup dialog can read/write them going
  forward); `Config.h`'s own comment block ("int32_t used for all numeric
  config variables for easier yaml-cpp serialisation") was deliberately not
  followed for `port` since the task specified `uint16_t` and yaml-cpp's
  generic stream-based `convert<T>` already handles it fine with no new
  `ConfigConvert.hpp` specialisation needed (verified by a clean build).
- Chat window history/scrolling: `Ui/Windows/Chat.cpp`'s storage cap
  (`kMaxMessages`) raised from 12 to 100; a separate `kVisibleMessages = 12`
  keeps the same number of lines actually drawn (the window has no
  scrollbar/viewport widget - a real `ScrollView` would need that machinery
  added and was judged out of scope for this pass, noted as a follow-up).
  `draw()` was changed from iterating the whole (previously <=12-entry)
  deque to iterating only `_history[_history.size() - kVisibleMessages ..]`
  (clamped to 0 when there are fewer than `kVisibleMessages` total), so the
  window always shows the most recent messages, bottom-anchored in time
  (oldest-of-the-shown-batch at the top, newest at the bottom) - "simplest
  acceptable" per the task, rather than a full scrollback viewer.
- Chat "Players" button: added `Widx::kPlayersBtn`
  (`Widgets::Button`, 8,153 / 70x14) that calls `PlayerList::open()`
  (already declared in the same `OpenLoco::Ui::Windows` block Chat.cpp's
  `WindowManager.h` include brings in - no new include needed), giving the
  roster window a second reachable trigger besides the existing "opens
  alongside Chat" hook in `TimePanel::beginSendChatMessage`. The Send button
  was narrowed from the former full-width 304x14 to 230x14 (position moved
  from x=8 to x=82) to make room on the same row rather than growing the
  window. New string `StringIds::chat_players_button` = 2459 ("Players",
  en-GB.yml only) - same fallback reasoning as `option_enable_multiplayer`
  above; no existing string fit this exact label.
- Verification limitation, honestly noted: all three of the above are
  windowed-UI features (Options checkbox, config-driven host dialog path,
  Chat window rendering/buttons) with no headless test hook - unlike the
  CLI-hook-driven features elsewhere in this file, there was no way to
  exercise the checkbox click, the config-driven bind/port fallback branch,
  or the Chat draw/button-click paths end-to-end under `--headless`
  (which skips window creation entirely). Verification for these three
  items is therefore build-clean + code-review + the standard regression
  battery (full `windows-release` build of `App` and `OpenLocoTests`, full
  `ctest -C Release` 145/145, `scripts\run_sync_smoke_test.ps1 -TestRename`
  own-policy PASS) rather than a targeted runtime check of the new UI
  itself. The `network.enabled`/join-policy runtime behaviour these UI
  pieces read and write was already headless-verified by the pre-existing
  smoke test machinery, and is unchanged by this pass.

## Reconnect implementation notes (network version 6)

- Wire additions (`Packet.h`): `ConnectPacket` gained `uint64_t token{}` (0 =
  fresh join); `CompanyAssignmentPacket` gained `uint64_t token{}` (echoed on
  every send site - coop/spectator/resend-existing in
  `onReceiveStateRequestPacket`, success/spectator-fallback in
  `runGameCommands` - five sites total); `RosterEntry` gained `uint8_t
  reserved{}`. The presentation-level `Network::PlayerRosterEntry` (`Network.h`)
  gained a matching `bool reserved{}`, converted in both directions by
  `toWirePacket`/`fromWirePacket` (roster overloads, `Network.cpp`).
- Session tokens are generated server-side only, in a file-local
  `generateSessionToken()` (`NetworkServer.cpp`) using `std::random_device`
  directly (two 32-bit draws combined into a `uint64_t`, remapping an
  all-zero result to 1 since 0 is the wire's "fresh join" sentinel). This is
  deliberately non-deterministic - the task/design doc are explicit that
  session bookkeeping is not game state and determinism rules don't apply to
  it. Contrast with the synced `GameState.rng` (`Core::Prng`), which must
  never be touched by anything reconnect-related.
- `NetworkServer::Client` gained a `token` field (assigned via
  `generateSessionToken()` for a fresh join, or restored from a reclaimed
  seat). A new `NetworkServer::ReservedSeat` (`NetworkServer.h`) holds
  `{token, id, name, company, assignmentResolved}` and is stored in a new
  `_reservedSeats` vector, kept for the whole session's lifetime (no expiry
  policy - matches the design doc's V1 scope).
- Seat reservation: `NetworkServer::removedTimedOutClients()` now moves a
  timed-out client's fields into a `ReservedSeat` (logging `Reserved seat for
  '<name>' (company N) pending reconnect`) instead of just erasing it. This
  never touches `GameState`, the replicated command stream, or the
  human-company mask - the company simply keeps existing with an empty seat,
  which was already the correct, deterministic behaviour on every peer
  before this change (disconnect never cleared the human-company mask).
- Reclaim on connect: `NetworkServer::createNewClient()` checks a non-zero
  `ConnectPacket::token` against `_reservedSeats` **before** falling through
  to the normal fresh-join code (version check still happens first, as
  before). A match restores `id`/`name`/`company`/`assignmentResolved`/
  `token` onto a brand new `Client` (with a brand new `NetworkConnection` -
  the reconnecting socket has a different local port, so the server cannot
  recognise it by endpoint, only by the token in its `ConnectPacket`), erases
  the reserved seat, sends the ordinary success `ConnectResponsePacket`, and
  logs `Client '<name>' reclaimed its reserved seat (company N)`. No new
  branching was needed in `onReceiveStateRequestPacket`: it already resends
  the existing assignment whenever `client.assignmentResolved == true`
  (added for desync resyncs), and a reclaimed client's restored
  `assignmentResolved` flag drives that same branch. An unrecognised/stale
  token falls through to the ordinary fresh-join path with a log line,
  rather than rejecting the connection outright - a garbage token can never
  strand a client.
- **Deviation from the design doc's literal wording**: the doc says reclaim
  "skips join policy" unconditionally. In this implementation it skips join
  policy only when the reserved seat's `assignmentResolved` was `true` (the
  overwhelmingly common case - a client that had already joined before it
  timed out). If a client disconnects *before* ever being assigned a company
  (a narrow window during the initial join), its reserved
  `assignmentResolved` is `false`, and reclaiming it naturally re-enters the
  ordinary join-policy switch in `onReceiveStateRequestPacket` on reconnect -
  via the exact same restored-field mechanism, not a new special case. This
  was a deliberate choice to reuse the existing, already-correct machinery
  rather than add a parallel code path, and is arguably more correct for
  that edge case (an unassigned client reconnecting should still get
  assigned).
- Roster: `NetworkServer::buildRoster()` appends one `PlayerRosterEntry` per
  reserved seat (`reserved = true`) after the live clients. Rendered as
  `"<name> (disconnected)"` in two places: `Ui/Windows/PlayerList.cpp`'s
  `draw()` and `NetworkClient::receiveRosterUpdatePacket`'s log summary line
  (both checked ahead of the existing spectator/company branches).
- Client auto-retry state machine and where it lives (the "hardest part" per
  the task): `NetworkClient` (`NetworkClient.h`/`.cpp`) tracks three bools
  purely to answer `shouldAutoRetry()`:
  - `_eligibleForAutoRetry` - set only in `onUpdate()`'s `hasTimedOut()`
    branch, and only when `_status` was already `connected` or `resyncing`
    (an *established* connection, not a still-transferring initial join,
    which is excluded per the design doc - "not a failed initial connect").
  - `_isReconnectAttempt` - set by `setReconnectToken()`, called by the
    facade before `connect()` on a NetworkClient created specifically to
    retry; any close of such an instance is retry-worthy (bounded by the
    facade's attempt cap), covering both "still unreachable" (connect-phase
    timeout) and any other close reason for that attempt.
  - `_suppressAutoRetry` - always wins over both of the above. Set in
    `receiveServerClosingPacket` (graceful shutdown must never auto-retry -
    this is what the `-TestShutdown` smoke assertion checks for), in
    `onCancel()`'s connecting-state branch (user-initiated cancel), and in
    `receiveConnectionResponsePacket`'s rejection branch (e.g. a version
    mismatch will never resolve itself by retrying).
  - `shouldAutoRetry()` = `!_suppressAutoRetry && (_eligibleForAutoRetry ||
    _isReconnectAttempt)`.
  - `_token`/`getToken()`/`setReconnectToken()`: `_token` is updated on
    every `CompanyAssignmentPacket` (even a spectator gets one - it's what
    lets *any* client reconnect, not just company owners) and sent on every
    outgoing `ConnectPacket` in `sendConnectPacket()`.
- **Facade-owned retry state** (`Network.cpp`), the architectural answer to
  "`NetworkClient` is destroyed by `Network::close()` on timeout - where does
  retry state live": a `NetworkClient` object cannot remember anything across
  its own destruction, so the facade - which outlives any individual
  instance - owns it instead:
  - `_joinHost`/`_joinPort` (plain statics): the address most recently passed
    to `joinServer()`. Kept for the whole session (cleared only by a real
    `close()`), since they identify "the server we're joined to" independent
    of any one reconnect episode - unlike the token/attempt count, they must
    not reset just because an episode ended.
  - `ReconnectState _reconnect { active, token, attempts, nextAttemptTime }`:
    per-episode bookkeeping. `Network::tick()` is the state machine driver
    (called every frame from `OpenLoco.cpp`'s main loop, same place the
    pre-existing test hooks are driven from, well outside `GameScene::tick()`
    - see § Lockstep architecture facts): after `serverOrClient->update()`,
    it first checks whether a reconnect episode just succeeded (`_mode ==
    client && _client->getStatus() == connected`), clearing `_reconnect`
    if so (logging `Reconnected successfully`) so a later, unrelated
    disconnect starts a fresh episode at attempt 1. Then, if the
    client/server `isClosed()`: a retry-worthy close
    (`_client->shouldAutoRetry()`) captures `_client->getToken()` into
    `_reconnect.token`, destroys the client (`_client = nullptr`), and either
    gives up (`giveUpReconnecting()`, if the attempt cap - 5 - was already
    reached) or schedules the next attempt 5s later; any other close falls
    through to the pre-existing `close()`. When `_mode == none` and
    `_reconnect.active` and the schedule has elapsed, `attemptReconnect()`
    creates a fresh `NetworkClient`, calls `setReconnectToken(_reconnect.token)`
    **before** `connect(_joinHost, _joinPort)` (ordering matters -
    `sendConnectPacket()` reads `_token` at the end of `connect()`), logs
    `Reconnecting (attempt N)...`, and shows/updates progress via the
    `NetworkStatus` window (`showReconnectStatus()` - opens the window if not
    already present, e.g. because gameplay was showing with no status window
    up, otherwise just updates its text/close-callback; the close button
    wires to `giveUpReconnecting`, letting a user cancel mid-retry).
    `giveUpReconnecting()` closes the status window, requests the title
    scene, and calls the ordinary `close()` (which also clears
    `_joinHost`/`_reconnect` - the session really is over at that point) -
    i.e. the pre-existing return-to-title behaviour, unchanged.
- Determinism: nothing above ever touches `GameState`, `CompanyManager`'s
  human-company mask, or the game command stream - reconnection is pure
  session/connection bookkeeping (server-side `Client`/`ReservedSeat`,
  client-side retry bools/token, facade-side endpoint/attempt bookkeeping).
  The one non-deterministic operation (`std::random_device` token
  generation) happens only on the server, only at accept/reclaim time, and
  its result is never fed into anything replicated.

## Reconnect test hook

- `--test_blackhole <start>,<duration>` (`CommandLine.h`/`.cpp`): a hidden,
  **client-side** test-only CLI option (same hidden-hook convention as
  `--test_rename`/`--test_host_load`/`--test_shutdown_after` - a single
  string arg, parsed here as `"<start>,<duration>"` in seconds, deliberately
  absent from `printHelp`). Once armed, `NetworkClient::onReceivePacket`
  (the per-packet callback from the receive thread, overridden from
  `NetworkBase`) silently returns without delegating to
  `_serverConnection->receivePacket(packet)` for every packet received while
  `isBlackholed()` is true.
- **Why this produces a REAL, two-sided timeout, not a scripted fake**:
  `NetworkConnection::receivePacket()` is what stamps
  `_timeOfLastReceivedPacket` (used by `hasTimedOut()`) *and* sends the ACK
  back to the sender. Skipping it entirely means: (a) this client's own
  `hasTimedOut()` goes true ~15s after blackhole start (its normal 15s
  `kConnectionTimeout`, `NetworkConnection.cpp`), driving the existing
  `onUpdate()` path that sets `_eligibleForAutoRetry` and calls `close()`;
  and (b) the client also stops sending anything back to the server (no ACKs
  for the server's frequent pings, which is the only regular traffic in a
  quiet headless test), so the *server's* per-client `NetworkConnection` also
  goes 15s without receiving anything and independently times out via
  `NetworkServer::removedTimedOutClients()`. Both timeouts are genuine
  consequences of a real (simulated) packet-loss window, not a scripted
  "pretend to disconnect" call - this is what the task required
  ("a REAL timeout + reconnect, not a scripted fake").
- The blackhole window is defined in absolute wall-clock terms, not relative
  to any one `NetworkClient` instance: `testBlackholeReferenceTime()`
  (`NetworkClient.cpp`, anonymous namespace) caches `Platform::getTime()` the
  first time it's called (a Meyer's-singleton-style `static` local), which in
  practice is this process's very first `connect()` call. Every subsequent
  reconnect attempt constructs a brand new `NetworkClient`, but
  `isBlackholed()` always measures elapsed time against this one cached
  reference, so the window fires exactly once across the whole run
  regardless of how many `NetworkClient` objects come and go - critical,
  since `Platform::getTime()` itself (`timeGetTime()` on Windows) is system
  uptime, not process- or object-relative, but nothing previously cached
  "when did this session's outage-testing clock start".
- `scripts\run_sync_smoke_test.ps1` gained `-TestReconnect`: passes
  `--test_blackhole 20,20` to the **client** (not the host), requires
  `-JoinPolicy own`/`coop` (need a company to compare before/after) and
  `-RunSeconds >= 80`. Asserts, in order: a `Reconnecting (attempt` line in
  the client log; 2+ `Assigned company N` lines with the post-outage one
  (found by `LineNumber` relative to the `Reconnecting` line, same technique
  as the pre-existing `-TestHostLoad` assertion) reporting the SAME `N` as
  the first; host log lines `Reserved seat for` and `reclaimed its reserved
  seat`; and the standard zero `[ERR]`/desync check across the whole run.
  `-TestShutdown` gained one more assertion in the same run: zero
  `Reconnecting (attempt` lines (a graceful shutdown must never auto-retry -
  this is what actually exercises `_suppressAutoRetry`'s effect end-to-end).
- Timing choice (blackhole 20s→40s relative to client connect, run 90s):
  chosen so the client's own 15s timeout fires mid-window (~35s, comfortably
  inside [20,40)) and the server's independent 15s timeout fires at
  essentially the same wall-clock time (both clocks start from "last real
  traffic", which stopped at the same moment for both sides) - so by the
  time the facade's first retry attempt fires (~5s after the client detects
  its own timeout, i.e. ~40s), the server has already reserved the seat.
  Even if a retry attempt happens to race the exact blackhole-end boundary,
  the next attempt (5s later) lands well past it - the design tolerates a
  fully sequential worst case since attempts are bounded (5) and spaced (5s)
  well within the 90s run window.
- Verified end-to-end headless (own policy, 90s run): **first attempt
  succeeded** in the observed run - host log: `Client timed out: Player #1`
  → `Reserved seat for 'Player #1' (company 1) pending reconnect` →
  `Client 'Player #1' reclaimed its reserved seat (company 1)`; client log:
  `Connection with server timed out` → `Disconnected from server` →
  `Reconnecting (attempt 1)...` → `Assigned company 1` (identical to the
  pre-outage assignment) → `Reconnected successfully` → `Scene transition:
  gameplay -> gameplay` (a legitimate no-op scene transition, same
  observation as the pre-existing `-TestHostLoad` note about
  `gameplay -> gameplay`); zero `[ERR]`/desync lines in either log for the
  whole 90s run. Regression battery: build clean (`App` + `OpenLocoTests`),
  `ctest -C Release` 145/145, `-TestRename` (own and coop), spectator
  (no test hook), and `-TestHostLoad` smoke tests all still PASS;
  `-TestShutdown` PASSes including the new no-auto-reconnect assertion.

## Session / environment

- Branch `multiplayer`; remotes: `origin` = github.com/RikPi/OpenLoco (the
  fork, SSH; push here), `upstream` = OpenLoco/OpenLoco (fetch only —
  never push). The branch tracks `origin/multiplayer`.
- User does not own Locomotion (not free; ~€6 GOG/Steam, frequent sales).
- Commit style: imperative subject, body explains why, trailer
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
