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
  - [ ] Phase C: co-op (join existing company) + explicit spectator role

## Backlog

- [ ] CI job: gensave → headless host+join → assert no desync (script the
      proven smoke test)
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
- [ ] Trim/derive client display name (empty `preferredOwnerName` logs as
      blank padding in "Accepted new client"/assignment messages)
- [ ] Typed serialization for the 3 complex-type commands
      (`changeCompanyFace`, `updateOwnerStatus`, `vehicleRepaint`)
- [ ] Player names in chat (needs client→name registry at Network layer)
- [ ] Reconnect / host migration
- [ ] Lobby & server browser
- [ ] Interpolate/harden `NetworkStatus` UX (connect progress, errors)
- [ ] Graceful shutdown for `--headless` (currently killed externally)
- [ ] Upstream grooming: split branch into reviewable PRs

## Blocked / external

- Real-asset play testing: user does not own Locomotion yet (~€6 Steam/GOG)
