# Multiplayer branch — task board

Working file for the multiplayer effort. Kept current as work proceeds.
Design details live in `docs/multiplayer.md`; operational knowledge in
`KNOWLEDGEBASE.md`.

## Done

- [x] Build toolchain on Windows (VS2022 `windows` preset, vcpkg static)
- [x] Fix cross-machine divergence sources (`updateOwnerStatus` throttle,
      terraform register sign-extension) — `3ffcd3e3`
- [x] Desync detection: per-tick PRNG verify vs server pings, S5 dumps to
      `save/desync/`, sim freeze + auto-resync via snapshot — `70b48594`
- [x] Portable wire format v2: typed Args serialization (74/85 commands),
      variable-length `GameCommandPacket`, server-side version check —
      `70b48594`, `78990139`
- [x] In-game chat window (packet-level chat by design) — `242757b0`
- [x] Headless enablement: stub-g1 tolerance (`4611e00f`), `gensave`
      deterministic fixture generator (`e589b8a8`), `--headless` mode +
      BootScene reconnect-flood fix (`9e794404`)
- [x] AI determinism: AI runs on every peer; in-tick command bypass —
      `051afb0d`
- [x] **Milestone 1: headless host+join lockstep session verified**
      (~100 s+, state transfer OK, desync detector silent)

## In progress

- [ ] **Session model** (design: `docs/multiplayer.md` § Session model)
  - [x] Phase A: server-side client→company mapping + enforcement
        (override company on received commands; drop spectator commands) —
        already in place (`Client::company` in `NetworkServer.h`,
        `onReceiveGameCommandPacket` drops spectators and overrides company)
  - [x] Phase B: join assignment — replicated `createPlayerCompany` game
        command (repurpose id 69), human-company set (deterministic,
        mirrored via command execution + snapshot `ExtraState`),
        `CompanyAssignment` packet, client controlling-id setup. Network
        version bumped to 3. Verified headless: command fails gracefully on
        the zero-competitor-object fixture (expected), no desync.
  - [x] Phase C: host `--join_policy <own|coop|spectator>`. coop assigns
        the host's company (no new command needed), spectator sends an
        explicit null assignment (also now sent when company creation
        fails). Verified headless: spectator policy end-to-end (host log
        "joins as spectator (host join policy)", client log "remaining a
        spectator", no desync). coop success path needs a fixture where
        the host owns a company (see backlog).

## Milestone 2: playable multiplayer

- [x] Networked save/load/quit made machine-local: `loadSaveQuitGame` no
      longer replicates (prompt only opens on the initiating machine),
      networked Save is an ordinary local S5 export, networked Load logs
      "not available", quit/return-to-title call `Network::close()`.
      Removed vanilla's dead two-player save handshake (MultiPlayer flags
      2/3/4 consumers, `do_69`/`do_70`/`do_72` helpers) — this also fixes
      the serious bug where networked Save spuriously broadcast a
      `createPlayerCompany` (repurposed vanilla id 69) to the session.
- [x] TitleMenu crash hazard: networked draw dereferenced raw vanilla
      address 0xF254D0 for the player name — now shows the configured
      local name. Multiplayer toggle while connected now disconnects
      instead of re-entering joinServer (assert/UB).
- [x] Host-driven mid-session Load (network v5): the host loads locally
      (importSaveToGameState + gameplay scene request), then
      `Network::requestAllClientsResync()` resets every client's join
      assignment, reseeds the human-company mask for the new world, and
      sends the new `resyncRequired` packet; clients discard state,
      re-request the snapshot ("Host loaded a new game" status) and get
      fresh assignments. Clients attempting Load get a log-only refusal.
      Also fixes a latent resync bug: `onReceiveStateRequestPacket` used to
      re-run join assignment on every state request, so a desync auto-resync
      would have created a second company — assignments are now tracked per
      client (`assignmentResolved`) and desync resyncs re-send the existing
      assignment instead. Verified: build clean, ctest 144/144, own+coop
      `-TestRename` smoke PASS. Runtime load path itself is
      compile/review-verified only — it opens a file-browse dialog, which
      headless can't drive (see backlog: `--test_host_load` hook).
- [x] Player roster window (client names + companies + spectators; roster
      broadcast packet). Network version bumped to 4. New `rosterUpdate`
      packet (`RosterUpdatePacket`/`RosterEntry` in `Packet.h`, variable-
      length like `SendChatMessage` - only `count` entries go on the wire),
      broadcast by the server whenever the roster changes (client accepted,
      timed out, company assigned/coop/spectator resolved). Server builds
      the roster from its live client list plus a synthetic host entry
      (`client_id_t 0`, same id already used for host chat messages);
      clients store the latest snapshot. Single facade call for UI code:
      `Network::getPlayerRoster()` (server rebuilds on demand, client
      returns its last received snapshot) — presentation data only, never
      touches `GameState` or the game command stream. New read-only window
      `Ui/Windows/PlayerList.cpp` (`WindowType::playerList = 62`, the first
      free slot past `debug`), modeled on `Chat.cpp`; opened alongside Chat
      from `TimePanel::beginSendChatMessage` (no other reachable trigger
      added — see KNOWLEDGEBASE.md § Player roster for the rationale).
      Display-name fix: `Network::resolveDisplayName` trims
      whitespace/NUL padding from the connect packet's fixed-size name
      buffer (and the host's `preferredOwnerName`) and falls back to
      "Player #<id>" when empty, fixing the blank-padding in "Accepted new
      client"/assignment log lines; chat now resolves the sender's display
      name via the roster instead of always printing "Player #N". Verified
      headless: client log shows `Roster: 2 players: 'Player #0' company 0,
      'Player #1' spectator` immediately after joining, then `Roster: 2
      players: 'Player #0' company 0, 'Player #1' company 1` once the
      company assignment resolves; zero `[ERR]`/desync lines; ctest 144/144;
      `-TestRename` smoke test still PASSes (regression).
- [x] Join dialog accepts `host`, `host:port` and `[ipv6]:port` (parsed in
      TitleMenu::multiplayerConnect; bare IPv6 passes through unchanged).
      Still open: UI-initiated hosting can't choose bind/port (openServer
      reads CLI options).
- [x] `network.enabled` defaults to true on this branch (Config.h struct
      default + yaml fallback) — multiplayer UI visible out of the box;
      upstream keeps it hidden while WIP.
- [x] Graceful disconnect notification for the clean-quit path (clients of
      a hard-killed/crashed host still time out - see KNOWLEDGEBASE.md §
      Graceful disconnect for that expected gap). New `serverClosing`
      packet (no payload), sent by `NetworkServer::onClose()` to every
      still-connected client before sockets are torn down (`sendPacket`
      writes to the socket synchronously, so this does not depend on the
      already-stopped receive thread). Client logs "Server is shutting
      down", posts it to the Chat window, requests the title scene, and
      closes its own connection. Verified by code review + build/tests
      (144/144, `-TestRename` smoke test PASS); headless has no exposed
      clean-quit trigger yet (separate backlog item below), so the
      packet-send path itself could not be exercised end-to-end headless -
      the hard-kill path (which deliberately does NOT send `serverClosing`)
      was verified instead: client log shows `Connection with server timed
      out` / `Disconnected from server` ~15s after `Stop-Process` on the
      host, no crash.
- [ ] Options UI checkbox for `network.enabled` (config-file-only today;
      branch default is now true so this is polish, not a blocker)
- [ ] Fix `Utility::nullTerminatedView` upstream-style: both loop branches
      return the same full-length view (latent bug found during the roster
      work; currently worked around by `Network::resolveDisplayName`)
- [ ] `--test_host_load <path>` headless hook (mirror the `--test_rename`
      pattern) so the mid-session Load flow can be exercised in the smoke
      test without the file-browse dialog

## Backlog

- [x] Scripted smoke test: `scripts/run_sync_smoke_test.ps1` (gensave →
      headless host+join → asserts accept count, assignment outcome,
      gameplay transition, zero error/desync lines; exit code 0/1).
      Verified for `own` and `spectator` policies.
- [x] Wire the smoke test into a CI workflow job:
      `.github/workflows/multiplayer-sync.yml`, a new standalone workflow
      (ci.yml untouched) triggered on push to `multiplayer` + manual dispatch.
      One `windows-2022` job mirrors ci.yml's Windows job (same `windows`
      configure preset, `windows-release` build preset, same
      `VCPKG_DEFAULT_BINARY_CACHE`/`actions/cache@v5` key shape so the vcpkg
      binary cache is shared/warmed with ci.yml runs on the branch instead of
      rebuilding from scratch), builds only the `App` target (Release), then
      sets up the headless fixtures in-job (stub `Data/g1.DAT`, `openloco.yml`
      with `allow_multiple_instances: true`, `gen_competitor_object.py`), then
      runs `scripts/run_sync_smoke_test.ps1` three times (own+`-TestRename`,
      coop+`-TestRename`, spectator; `-RunSeconds 45` each), uploading
      `%APPDATA%\OpenLoco\logs\*` on failure. Everything short of actually
      executing on Actions was validated locally (YAML parse, every
      referenced path/script/preset exists, the stub-creation PowerShell
      snippets run correctly against a scratch dir, `gen_competitor_object.py`
      accepts an explicit output-dir arg) — see KNOWLEDGEBASE.md § CI for
      what only the first real run can prove (loopback/firewall behavior on
      hosted runners, actual vcpkg cache hit, wall-clock time).
- [x] Fixture where the host owns a company: gensave now creates a player
      company when a competitor object is available, and
      `gen_competitor_object.py` emits 8 distinct competitors (a company
      consumes its competitor exclusively, so own-policy joins need spares).
      All three join policies pass the scripted smoke test.
- [x] Client-issued in-game command round-trip test: hidden `--test_rename
      <name>` CLI option (`CommandLine.{h,cpp}`, deliberately not in
      `printHelp`) plus a headless hook in `NetworkClient` (state machine
      driven from `onUpdate()`, i.e. outside `GameScene::tick()`) that, once
      the client is assigned a real company and is fully `connected`
      (`NetworkClientStatus`), issues a company rename via the exact
      chunked `doCommand` sequence the UI/CompanyManager use (bufferIndex
      1, 2, 0 — not 0, 1, 2; see KNOWLEDGEBASE.md § Client round-trip test
      hook) ~2s later, then reads the company's real name back via
      `StringManager::formatString` ~8s after that and logs `[TEST] rename
      verified: '<name>'` or `[TEST] rename FAILED: ...`. Since a client
      never applies its own queued commands locally
      (`GameCommands::doCommand`'s network branch only queues and returns),
      a verified line proves the full client -> server -> broadcast ->
      apply-on-both-peers round trip. `scripts\run_sync_smoke_test.ps1`
      gained `-TestRename` (asserts the verified line) and a corrected
      default `-Expect` (coop now defaults to `company`, matching its
      actual behaviour, not `spectator`). Verified: `own` and `coop`
      policies both PASS with the verified line; `spectator` (no
      `-TestRename`) still PASSes; zero `[ERR]`/desync lines; ctest
      144/144. One bug found and fixed along the way: the naive "arm on
      `CompanyAssignmentPacket`" design fired the rename before the state
      transfer finished and `Network::isConnected()` was still false,
      which made `doCommand` silently take its local-apply fallback
      instead of queuing to the server — fixed by gating the 2s countdown
      on `NetworkClientStatus::connected`, not just the assignment packet.
- [x] Fixture with a minimal custom competitor `.DAT` object, unlocking the
      `createPlayerCompany` success path in the smoke test — hand-crafted
      (`scripts/gen_competitor_object.py`, regeneratable; see KNOWLEDGEBASE.md
      § Competitor object fixture) since the OpenGraphics release zip ships
      zero Competitor objects. Verified: `gensave`/`simulate` still work,
      headless host+join now logs `Assigned company 0 to client '...'` /
      client `Assigned company 0` (instead of spectator fallback), zero
      `[ERR]`/desync lines over the run, ctest 144/144. Towns/industries
      still absent (unaffected — separate object types), so this only
      exercises the join-time company-assignment path, not full AI-active
      sync.
- [x] Trim/derive client display name (empty `preferredOwnerName` logs as
      blank padding in "Accepted new client"/assignment messages) — done as
      part of the player roster work (`Network::resolveDisplayName`); see
      Milestone 2.
- [ ] Typed serialization for the 3 complex-type commands
      (`changeCompanyFace`, `updateOwnerStatus`, `vehicleRepaint`)
- [x] Player names in chat (needs client→name registry at Network layer) —
      done as part of the player roster work: `Network::receiveChatMessage`
      resolves the sender's display name via `Network::getPlayerRoster()`,
      falling back to "Player #N" only if the roster doesn't have the
      sender yet.
- [ ] Reconnect / host migration
- [ ] Lobby & server browser
- [ ] Interpolate/harden `NetworkStatus` UX (connect progress, errors)
- [ ] Graceful shutdown for `--headless` (currently killed externally) — the
      server-side `serverClosing` notification now exists (see Milestone 2)
      but nothing in headless mode calls `Network::close()`/exits cleanly to
      trigger it; this item is now specifically about adding that headless
      trigger (e.g. a signal handler or a CLI-driven shutdown), not about
      the notification itself.
- [ ] Upstream grooming: split branch into reviewable PRs

## Blocked / external

- Real-asset play testing: user does not own Locomotion yet (~€6 Steam/GOG)
