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
- `serverClosing` could not be exercised end-to-end headless: `--headless`
  has no clean-quit trigger (`Network::close()` is only ever reached via UI
  quit flows; see the now-narrowed "Graceful shutdown for `--headless`"
  backlog item in `TASKS.md`) - verified by code review plus the fact the
  build/tests/existing smoke test still pass. What *was* verified headless
  is the deliberately-different hard-kill path: `Stop-Process` on the host
  produces no `serverClosing` (expected - the process never runs its
  destructor chain), and the client's pre-existing 15s connection-timeout
  path still fires correctly (`Connection with server timed out` /
  `Disconnected from server` in the client log, process stays alive
  afterwards, no crash).

## Session / environment

- Branch `multiplayer`; remotes: `origin` = github.com/RikPi/OpenLoco (the
  fork, SSH; push here), `upstream` = OpenLoco/OpenLoco (fetch only —
  never push). The branch tracks `origin/multiplayer`.
- User does not own Locomotion (not free; ~€6 GOG/Steam, frequent sales).
- Commit style: imperative subject, body explains why, trailer
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
