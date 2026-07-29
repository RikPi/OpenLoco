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
      `-TestRename` smoke PASS. Runtime load path itself was at the time
      compile/review-verified only — it opens a file-browse dialog, which
      headless can't drive — but is now exercised end-to-end headless via
      the `--test_host_load <seconds>` hook (see the completed backlog item
      below and KNOWLEDGEBASE.md § Host-driven mid-session load test hook).
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
      UI-initiated hosting bind/port: **now resolved**, see the completed
      "Host bind/port via config" item below.
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
      (144/144, `-TestRename` smoke test PASS); headless had no exposed
      clean-quit trigger at the time (separate backlog item below), so the
      packet-send path itself could not be exercised end-to-end headless -
      the hard-kill path (which deliberately does NOT send `serverClosing`)
      was verified instead: client log shows `Connection with server timed
      out` / `Disconnected from server` ~15s after `Stop-Process` on the
      host, no crash. **Now superseded**: the `--test_shutdown_after
      <seconds>` headless hook (see the completed "Graceful shutdown for
      `--headless`" backlog item) exercises the graceful packet-send path
      end-to-end too.
- [x] Options UI checkbox for `network.enabled`: "Enable multiplayer"
      checkbox added to the Options window's Miscellaneous tab, in a new
      "Multiplayer" group box between "Vehicle behaviour" and "Save options"
      (window height grown 266 -> 302 to fit; groupSaveOptions/autosave/
      export-objects widgets all shifted down 36px to make room). Bound to
      `Config::get().network.enabled` + `Config::write()`, matching the
      existing checkbox pattern (e.g. `disableTownExpansionMouseUp`);
      toggling also calls `WindowManager::invalidate(WindowType::titleMenu)`
      so the title screen's multiplayer button (gated on this same flag in
      `TitleMenu.cpp`'s `prepareDraw`) appears/disappears without needing an
      unrelated redraw to trigger it. Localisation: no brand-new string was
      needed for the group box title - vanilla's dead "Two Player Game"
      setup-dialog title (StringId 1466, unused by any code on this branch,
      already translated into all 17 shipped languages, e.g. nl-NL already
      renders it as "Multiplayer") was repurposed and named
      `StringIds::multiplayer_group_title`. The checkbox's own label needed
      wording no existing string had, so one new string was appended
      (`StringIds::option_enable_multiplayer` = 2458, "Enable multiplayer",
      en-GB.yml only - confirmed safe: `Localisation::loadLanguageFile()`
      always loads en-GB first as the fallback table, then overlays the
      selected language on top, so an id present only in en-GB simply falls
      back to English text in every other language, exactly like every
      other not-yet-translated addition). The tooltip reuses the existing,
      already-in-use `StringIds::title_multiplayer_toggle_tooltip` ("Toggle
      between single player and two player mode" - already the TitleMenu
      button's own tooltip). Verified: build clean (App + OpenLocoTests),
      ctest 145/145, `-TestRename` smoke test PASS (regression). The
      checkbox's live toggle behaviour itself could not be exercised
      headless (windowed UI, no file-browse-style hook exists or was added)
      - verified by code review plus the existing `network.enabled` runtime
      behaviour (already headless-tested via the join-policy smoke runs)
      being unchanged by this purely-additive UI change.
- [x] Host bind/port via config: `Config::Network` gained `std::string bind`
      and `uint16_t port{ 11754 }` (Config.h), read/written under
      `network.bind`/`network.port` (Config.cpp, same pattern as
      `network.enabled`). `Network::openServer()` (Network.cpp) now prefers
      the CLI `--bind`/`--port` options when supplied (`cmdlineOptions.bind`
      non-empty / `cmdlineOptions.port` present) and otherwise falls back to
      the config values (config port of 0 - not a realistic setting, but
      defensive - falls back further to `kDefaultPort`); CLI-driven hosting
      (headless test hosts, the smoke test) is byte-for-byte unchanged
      since neither fallback path is taken when the CLI actually passes
      `--bind`/`--port`. No UI was added to edit `network.bind`/`port`
      directly (out of scope for this pass - only the config plumbing +
      server fallback were requested); a future Options/host-dialog field
      can now just read/write these two config values. Verified: build
      clean, ctest 145/145, `-TestRename` smoke test PASS (this test always
      passes `--bind`/`--port` implicitly via defaults - CLI path - so it
      also regression-covers that the CLI-wins branch still works
      unchanged).
- [x] Chat window scrolling/history: `Ui/Windows/Chat.cpp`'s history cap
      raised from 12 to 100 (`kMaxMessages`); a new `kVisibleMessages = 12`
      keeps the same fixed number of lines actually drawn (the window has no
      scrollbar/viewport widget). `draw()` now renders only the tail of
      `_history` (`_history.size() - kVisibleMessages` onward) instead of
      the whole (previously always <= 12 entry) deque, so the window always
      shows the most recent messages, bottom-anchored in time (oldest-of-
      the-shown-batch at the top, newest at the bottom), while up to 100
      messages of scrollback are retained in memory for a future real
      `ScrollView`-based history view. A real `ScrollView` was judged a
      bigger change than this pass's scope (the window has no scroll
      widget/viewport machinery today) - noted as a follow-up rather than
      built. Also added a "Players" button (`Widx::kPlayersBtn`, new string
      `StringIds::chat_players_button` = 2459, "Players", en-GB.yml only,
      same fallback reasoning as `option_enable_multiplayer` above) that
      calls `PlayerList::open()`, splitting the former full-width Send
      button row (8,153 / 304x14) into a Players button (8,153 / 70x14) and
      a narrower Send button (82,153 / 230x14) - giving the roster window a
      second reachable trigger alongside the existing "opens alongside Chat"
      one in `TimePanel::beginSendChatMessage`. Verified: build clean, ctest
      145/145, `-TestRename` smoke test PASS (unaffected by this change,
      chat itself isn't exercised by the smoke test beyond the pre-existing
      "Server is shutting down" chat-post path, which is unrelated to
      history rendering).
- [ ] Fix `Utility::nullTerminatedView` upstream-style: both loop branches
      return the same full-length view (latent bug found during the roster
      work; currently worked around by `Network::resolveDisplayName`)
- [x] `--test_host_load <seconds>` headless hook (mirror the `--test_rename`
      pattern) so the mid-session Load flow can be exercised in the smoke
      test without the file-browse dialog. Ended up seconds-based (not
      path-based) - the host reloads the exact fixture it was started with
      (`getCommandLineOptions().path`), so no extra CLI argument was needed.
      Driven from `NetworkServer::onUpdate()` (mirrors the client hook's
      pattern of driving itself from its own `onUpdate()`, outside
      `GameScene::tick()`): once uptime exceeds the threshold AND at least
      one client's join assignment has resolved, it calls
      `S5::importSaveToGameState` + `SceneManager::requestScene(gameplay)` +
      `Network::requestAllClientsResync()` - the exact sequence
      `Game::loadGame`'s networked-host branch uses, minus the file-browse
      dialog. `scripts\run_sync_smoke_test.ps1` gained `-TestHostLoad`
      (passes `--test_host_load 20`, asserts `Host loaded a new game`
      followed by a second `Assigned company` line in the client log, plus
      the standard zero-error assertion; requires `-RunSeconds >= 50` and
      `own`/`coop` join policy, enforced). See KNOWLEDGEBASE.md § Host-driven
      mid-session load test hook. Verified end-to-end headless: host logs
      `[TEST] host reloaded save` + `Requested resync from 1 client(s) after
      state reload`; client logs `Host loaded a new game; resyncing` then a
      fresh `Assigned company 1`; zero `[ERR]`/desync lines for the rest of
      the run; build clean; ctest 145/145.
- [x] **Reconnect** (design: `docs/multiplayer.md` § Reconnect). Network
      version bumped to 6. A client that loses its connection can rejoin the
      same session and reclaim its company instead of being treated as a new
      player:
      - Session token: the server generates a random (`std::random_device`,
        non-deterministic by design - session bookkeeping, not game state)
        `uint64_t` per accepted client, sent alongside every
        `CompanyAssignmentPacket` (new `token` field). `ConnectPacket` grows
        a matching `token` field (0 = fresh join).
      - Seat reservation: `NetworkServer::removedTimedOutClients()` now moves
        a timed-out client's `{token, id, name, company, assignmentResolved}`
        into a new `_reservedSeats` list instead of dropping it (kept for the
        session's lifetime - no expiry policy yet). Never touches
        `GameState`/the human-company mask.
      - Reclaim on connect: `NetworkServer::createNewClient()` checks a
        non-zero `ConnectPacket::token` against `_reservedSeats` first; a
        match restores the client's identity/company/assignment and skips
        the fresh-join path (join policy included) entirely - the existing
        `assignmentResolved == true` branch in `onReceiveStateRequestPacket`
        (already used by desync resyncs) resends the snapshot + assignment,
        which is all a reclaim needs.
      - Roster: `RosterEntry`/`PlayerRosterEntry` gained a `reserved` flag;
        `NetworkServer::buildRoster()` includes reserved seats, rendered as
        "`<name> (disconnected)`" by `PlayerList.cpp` and the client's roster
        log summary.
      - Client auto-retry: `NetworkClient` tracks `_eligibleForAutoRetry`
        (set only when an already-`connected`/`resyncing` connection times
        out - not a failed initial connect), `_isReconnectAttempt` (this
        instance is itself a retry) and `_suppressAutoRetry` (graceful
        `serverClosing`, user cancel, or explicit rejection - always wins).
        Since `Network::close()` destroys the `NetworkClient` object on any
        disconnect, the retry loop's state - `{host, port, token, attempts}`
        - lives one level up, in the `Network.cpp` facade (`_joinHost`/
        `_joinPort`/`_reconnect`), and survives across the destroyed/
        recreated `NetworkClient` instances. `Network::tick()` recognises a
        retry-worthy close via `NetworkClient::shouldAutoRetry()`, tears the
        client down, and schedules the next attempt (up to 5, ~5s apart,
        logged `Reconnecting (attempt N)...`, surfaced via the
        `NetworkStatus` window); giving up returns to the title scene (the
        existing behaviour). A successful reconnect (fresh `NetworkClient`,
        same token) re-triggers the ordinary snapshot+assignment-resend flow
        and clears the retry bookkeeping.
      - New hidden client-side test hook `--test_blackhole <start>,<duration>`
        makes `NetworkClient::onReceivePacket` silently discard every
        incoming packet for a real time window, producing a genuine
        two-sided timeout (both peers stop hearing from each other) instead
        of a scripted fake. `scripts\run_sync_smoke_test.ps1` gained
        `-TestReconnect` (blackhole 20s-40s, `-RunSeconds >= 80`); asserts a
        `Reconnecting (attempt` line, a second `Assigned company N` line
        with the SAME `N` as before the outage, host `Reserved seat for`
        and `reclaimed its reserved seat` lines, and the usual zero-error/
        desync check. `-TestShutdown` gained an added assertion that no
        `Reconnecting (attempt` line appears (graceful shutdown must never
        auto-retry).
      - Verified end-to-end headless (own policy, 90s run, real blackhole):
        host logs `Client timed out` -> `Reserved seat for 'Player #1'
        (company 1) pending reconnect` -> `Client 'Player #1' reclaimed its
        reserved seat (company 1)`; client logs `Connection with server
        timed out` -> `Disconnected from server` -> `Reconnecting (attempt
        1)...` -> `Assigned company 1` (same company as the original
        assignment) -> `Reconnected successfully` -> `Scene transition:
        gameplay -> gameplay`; zero `[ERR]`/desync lines in either log.
        Regressions: build clean (App + OpenLocoTests), ctest 145/145,
        `-TestRename` (own + coop), spectator, and `-TestHostLoad` smoke
        tests all still PASS; `-TestShutdown` PASSes with the new
        no-auto-reconnect assertion. See KNOWLEDGEBASE.md § Reconnect
        implementation notes and § Reconnect test hook.
      - Deviation from the design doc's literal wording: reclaim "skips join
        policy" only in the common case (the seat's `assignmentResolved` was
        already `true` when reserved). If a client disconnects before ever
        being assigned, its reserved `assignmentResolved` is `false`, and a
        reclaim naturally re-enters the ordinary join-policy switch on
        reconnect (via the same restored-field mechanism) rather than being
        specially skipped - a deliberate reuse of existing code rather than a
        new branch, and arguably the more correct behaviour for that edge
        case. Host migration remains explicitly out of scope (per the design
        doc) and was not addressed.

## Milestone 3: lobby (phase 1 — LAN server discovery)

- [x] **LAN server discovery** (design: `docs/multiplayer.md` § LAN server
      discovery). Network version bumped to 7. Two new connectionless packet
      kinds, `discoveryRequest`/`discoveryResponse` (`Packet.h`), handled
      directly in `NetworkServer::onReceivePacket` for endpoints that are not
      an established client (alongside the existing connect handling) —
      bypasses `NetworkConnection` sequencing entirely (no acks; a raw
      `IUdpSocket::sendData` reply). The server answers ANY discoveryRequest
      regardless of the requester's version (the request carries none) and
      self-describes its own version, so a browser can grey out incompatible
      servers rather than the version bump gating discoverability itself.
      `Socket.cpp`'s previously dormant `SO_BROADCAST` enable was turned on
      (unconditionally, on every UDP socket — smallest viable change, and
      harmless for the pre-existing game-connection sockets which never send
      to a broadcast address).
      Client side: `Network/ServerDiscovery.{h,cpp}` — a small class reusing
      `NetworkBase`'s background receive-thread plumbing (not a real
      session), probing `255.255.255.255:<port>` and `127.0.0.1:<port>`
      roughly once a second (both target `kDefaultPort` only — a
      non-default-port server isn't found this way, but stays reachable via
      "Join by address"), deduplicating responses by endpoint with a ~5s
      expiry. Facade: `Network::beginServerDiscovery()`/`endServerDiscovery()`/
      `getDiscoveredServers()`, plus a shared `Network::parseServerAddress()`
      helper (moved out of `TitleMenu::multiplayerConnect`, unchanged, so the
      browser's "Join by address" prompt and the browser's row-click path
      agree on host:port/[ipv6]:port parsing).
      UI: `Ui/Windows/ServerBrowser.cpp` (`WindowType::serverBrowser`, first
      free slot past `playerList`), modeled on `PlayerList.cpp` for the
      facade/registration/CMakeLists pattern with a trimmed-down
      `CompanyList.cpp`-style `ScrollView` for the clickable row list.
      TitleMenu's multiplayer button now opens the browser instead of the
      raw address prompt directly; the prompt itself moved into the
      browser's "Join by address" button (new string
      `StringIds::server_browser_join_by_address` = 2460, en-GB-only, same
      fallback pattern as the two strings before it). Discovery runs only
      while the window is open (`Network::beginServerDiscovery()` in
      `open()`, `endServerDiscovery()` in `onClose()`).
      Headless test hook: hidden client CLI flag `--test_discover`
      (`CommandLine.h`/`.cpp`) drives a small state machine in `Network.cpp`
      (`updateTestDiscoverHook()`, called from `Network::tick()`, which
      already runs every frame regardless of mode/scene): instead of
      joining, starts discovery, logs `[TEST] discovered server: '<name>'
      <address>:<port> players=N/M version=V` for each unique server found,
      then stops probing after ~10s (the process itself keeps running —
      headless has no exit path, same as every other test hook here).
      `scripts\run_sync_smoke_test.ps1` gained `-TestDiscovery` (passes
      `--test_discover` to the second process instead of `join
      127.0.0.1`; asserts the discovered-server line with the expected
      name/port/version, asserts the host accepted 0 clients — proving
      discovery never joins — and skips the gameplay-transition/company-
      assignment assertions, which don't apply since this process never
      joins; the standard zero-`[ERR]`/desync and both-processes-alive
      assertions still apply and still run). Verified end-to-end headless
      (20s run): client log shows `[TEST] discovery started` →
      `[TEST] discovered server: 'Player #0' 127.0.0.1:11754 players=1/32
      version=7` → `[TEST] discovery finished`; host log has zero
      `[TEST]`/`[ERR]`/`Accepted new client` lines (confirms discovery never
      triggers the join path at all); build clean; ctest 145/145;
      `-TestRename` (own policy) regression PASSes unchanged.
      Design-doc note only (no code): `docs/multiplayer.md` § Master server
      (phase 2 — constraints) records that a future internet-wide server
      list needs only a tiny stateless HTTP announce/list service (TTL'd
      periodic announces, a config-value master URL, in-game browser merges
      master + LAN results) — explicitly not implemented.

## Milestone: master server (phase 2 — service half done)

- [x] **Master server service** (design: `docs/multiplayer.md` § Master
      server (phase 2 — design)). Standalone Go service, stdlib only,
      `tools/master-server/` (`go.mod` — no `require` entries):
      `main.go` (flags, UDP/HTTP server wiring, TTL sweeper ticker, graceful
      SIGINT/SIGTERM shutdown), `protocol.go` (wire framing + payload
      encode/decode), `registry.go` (in-memory TTL registry, per-IP cap,
      overall bound), `udp.go` (the announce/query request loop).
      Speaks the game's own `Packet.h` framing rather than HTTP — three new
      connectionless `PacketKind` values appended immediately after the
      existing `discoveryResponse` (17), matching the same
      no-`NetworkConnection`/no-acks/`sequence=0` pattern discovery already
      uses: `masterAnnounce = 18`, `masterQuery = 19`,
      `masterServerList = 20`. **These numeric values are load-bearing for
      the not-yet-done game-side change** — Packet.h must append the three
      kinds in exactly this order so its enum values match what this
      service hard-codes.
      `masterServerList`'s entry cap is derived, not guessed:
      `kMaxPacketDataSize` (4090) minus the payload's own cookie+count
      header (5) leaves 4085 bytes; each entry is 43 bytes
      (`4+2+2+1+1+1+1+31`); `floor(4085/43) = 95` entries, and 95×43 = 4085
      exactly (no wasted/overflowing remainder) — enforced both where the
      list is built and again inside the encoder as a hard backstop, so a
      reply can never exceed a small fixed bound regardless of registry
      size (no amplification).
      Registry: keyed by (observed source IP, advertised `gamePort`) so one
      host can announce multiple game servers on different ports; a repeat
      announce for the same key refreshes the TTL instead of duplicating;
      per-IP cap and overall registry bound are enforced only on *new* keys
      (a refresh of an existing entry never gets rejected by either cap).
      IPv4 only v1 — IPv6 sources are ignored (logged only under
      `-verbose`).
      Tests: `protocol_test.go` (framing round-trip, malformed/truncated
      packet rejection, `masterAnnounce`/`masterQuery`/`masterServerList`
      encode/decode round-trips, name trimming/rejection, the 95-entry cap
      boundary), `registry_test.go` (upsert/refresh, per-IP cap, overall
      bound, TTL expiry incl. refresh-resets-the-clock, deterministic
      `List()` ordering), `integration_test.go` (spins a real UDP listener
      on an ephemeral loopback port, drives announce → announce (refresh) →
      query → `masterServerList` over an actual socket, plus a
      malformed/unknown-kind-packets-don't-crash-the-loop case). All green:
      `go vet ./...` clean, `go test ./...` 26/26 PASS, `go build` produces
      `master-server.exe`.
      Manually verified end-to-end on Windows (native binary, no Docker
      available to test with — Dockerfile is written but unverified):
      built and ran the exe on ephemeral ports with a throwaway Go client
      script — two announces from the same source (refresh, not a
      duplicate) followed by a query returned a `masterServerList` with
      exactly one entry and the correct name/port/version/counts; `GET
      /servers` showed the same entry as JSON immediately after the
      announce and an empty array again once a short `-ttl` (6s) elapsed;
      a `CTRL_BREAK_EVENT` (the Windows equivalent this environment can
      actually deliver to a child console process, mapped by the Go
      runtime to `os.Interrupt`) produced the "shutting down..." /
      "shutdown complete" log lines and a clean exit, confirming graceful
      shutdown.
      `README.md`: protocol tables, flags, `GET /servers` JSON shape,
      systemd unit example, Docker build/run, and a Fly.io `fly.toml`
      sketch (UDP + HTTP service blocks) — noting the in-memory registry
      means a redeploy just drops entries, which self-heals within one TTL
      window as hosts re-announce.
- [x] **Game-side master server integration (network v8)** — phase 2
      complete. `Packet.h` gains masterAnnounce/masterQuery/masterServerList
      (18/19/20, matching the service); `network.masterServer` config value
      plus a documented `--master_server <host[:port]>` CLI override (wins
      over config; useful for dedicated servers). While hosting with a
      master configured, the server announces every ~30s and on roster
      changes from its existing socket; the browser/discovery component
      queries the master ~every 2s and merges results with LAN probes
      (dedupe by endpoint, LAN wins; master entries tagged in the browser
      and in `--test_discover` output as "(master)"). Verified by the new
      `-TestMaster` smoke mode: builds the real Go service, runs it as a
      third loopback process, asserts host "Announcing to master server",
      client `[TEST] discovered server (master): ...` with correct
      name/port/version, all processes alive, no errors. Also fixed a
      latent smoke-script bug found here: host/client log attribution
      sorted by LastWriteTime (unstable — whoever flushed last); now
      sorted by filename (embedded creation timestamp).

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
- [x] Typed serialization for the 3 complex-type commands
      (`changeCompanyFace`, `updateOwnerStatus`, `vehicleRepaint`) — typed
      coverage now 79/85 (70 generic + 6 rename-chunk + these 3); 6 raw-
      fallback stubs remain (`loadMultiplayerMap`, `gc_unk_34`, `gc_unk_68`,
      `gc_unk_70`, `sendChatMessage`, `multiplayerSave`). All 3 verified
      symmetric (`Args(registers(X)) == X`, unlike the renames) so all use
      the plain generic `makeTypedCodec<T>`; see KNOWLEDGEBASE.md § Complex-
      type typed serialization for the per-command reasoning and the
      archive extensions (explicit `ObjectHeader` branch, explicit
      `OwnerStatus` branch, generic `std::array<T, N>` branch + a small
      `ColourScheme` branch). Added 7 new tests (2 archive-level + 5 typed
      round-trip) to `CommandSerializationTests.cpp`; re-pointed the
      generic `rawFallbackRoundTrip` test at `sendChatMessage` since
      `changeCompanyFace` is no longer on the fallback path. Verified: full
      clean build, ctest 152/152 (was 145), `-TestRename` smoke test PASS
      (regression).
- [x] Player names in chat (needs client→name registry at Network layer) —
      done as part of the player roster work: `Network::receiveChatMessage`
      resolves the sender's display name via `Network::getPlayerRoster()`,
      falling back to "Player #N" only if the roster doesn't have the
      sender yet.
- [x] Reconnect - done, see Milestone 2 above and KNOWLEDGEBASE.md § Reconnect
      implementation notes / § Reconnect test hook. Host migration remains
      explicitly out of scope per the design doc and is not addressed.
- [ ] Lobby & server browser — phase 1 (LAN discovery) done, see the
      completed "LAN server discovery" item below. Phase 2 (internet master
      server): COMPLETE — Go service + game-side integration both done (see
      the master server milestone section above). Deployment to a real
      host (user's VPS or Fly.io) is an ops task for whenever wanted; the
      game points at it via `network.masterServer` or `--master_server`.
- [ ] Interpolate/harden `NetworkStatus` UX (connect progress, errors)
- [x] Graceful shutdown for `--headless`: hidden `--test_shutdown_after
      <seconds>` CLI-driven hook (`CommandLine.{h,cpp}`, deliberately not in
      `printHelp`, same hidden-test-hook pattern as `--test_rename`) plus a
      headless hook in `NetworkServer` (`updateTestShutdownHook()`, driven
      from `onUpdate()` like the client-side test hooks) that, once the
      server has been up for `<seconds>` and at least one client is
      assigned, logs `[TEST] closing server` and calls the inherited
      `NetworkBase::close()` (unqualified `close()` resolves to the member,
      not the free `Network::close()` facade, by ordinary C++ name hiding -
      the already-established pattern `NetworkClient::
      receiveServerClosingPacket` uses for the same reason). The host
      process is not exited: with the networked scene flags gone it just
      keeps running as single-player, which is the point - this hook tests
      the *client's* reaction, not host teardown.
      `scripts\run_sync_smoke_test.ps1` gained `-TestShutdown` (passes
      `--test_shutdown_after 25`, asserts `Server is shutting down` present
      and `timed out` absent in the client log, plus the standard
      zero-error/both-alive assertions, which need no changes since neither
      process actually exits under this hook; enforces `-RunSeconds >= 35`).
      See KNOWLEDGEBASE.md § Graceful shutdown test hook. **Behavioural
      discovery along the way**: this is the first headless exercise of the
      client's post-shutdown `SceneManager::requestScene(title)` path, and
      it uncovered that `Title::loadTitle()` unconditionally reads
      `Data/title.dat` from the Locomotion install path — a file the stub
      `fake-locomotion` fixture never provided — which crashed the client
      process outright (an uncaught `FileStream` open failure; its error
      message is also mislabeled "for writing" even on a read failure, a
      separate latent bug in `FileStream.cpp`, not fixed here). Fixed
      test-fixture-only, no production code touched: the smoke script now
      copies its `gensave` fixture to `Data/title.dat` and patches the S5
      Header's `isTitleSequence` bit in place (`Set-TitleSequenceFlag` in
      `run_sync_smoke_test.ps1`, reverse-engineered the same way as the
      Competitor object fixture - see KNOWLEDGEBASE.md § Title sequence
      fixture for the byte layout/checksum details). Verified end-to-end
      headless: host logs `[TEST] closing server` / `Server closed`; client
      logs `Server is shutting down` / `Disconnected from server` / `Scene
      transition: gameplay -> title`, no crash, no timeout message, zero
      `[ERR]`/desync lines; build clean; ctest 145/145.
- [ ] Upstream grooming: split branch into reviewable PRs

## Blocked / external

- Real-asset play testing: user does not own Locomotion yet (~€6 Steam/GOG)
