<#
.SYNOPSIS
Headless multiplayer lockstep smoke test.

Generates a deterministic fixture save, starts a headless host and a headless
client on loopback, lets them run, then asserts from the file logs that:
  - the host accepted exactly one client
  - the join assignment resolved (company grant, or spectator when expected)
  - the client reached gameplay (state transfer completed)
  - no [ERR] / desync lines appeared (the desync detector runs continuously)
  - both processes survived the whole window
  - (with -TestRename, and a company-granting policy) a client-issued game
    command round-tripped through the server and was applied on the client
  - (with -TestHostLoad) the host's mid-session reload of its own fixture
    triggered a full client resync ending in a fresh company assignment
  - (with -TestShutdown) the host's graceful close was announced to the
    client, which returned to title cleanly (no timeout, no auto-reconnect)
  - (with -TestReconnect) a simulated network outage (client-side blackhole)
    caused a genuine two-sided timeout; the client auto-reconnected with its
    saved session token and reclaimed the SAME company it had before
  - (with -TestDiscovery) instead of joining, the second process runs LAN
    server discovery and logs the host it found via loopback (name/port/
    version) -- the host is never actually joined (no assignment/gameplay in
    this mode; see below for which standard assertions still apply)
  - (with -TestMaster) builds and runs tools/master-server as a third
    process on loopback; the host announces to it and the second process
    (also never joining) queries it and logs a master-sourced discovery
    line -- proves the internet-wide server list integration end-to-end
    without relying on LAN discovery (the host uses a non-default port)
  - (with -TestMigration) host + clientA + clientB (clientB joins ~5s after
    clientA); the host hard-crashes (--test_kill_after) ~25s in; both
    clients independently exhaust auto-retry against the dead host, elect
    the same successor (clientA - lower client id), clientA promotes itself
    to session host in-process (no new game process), and clientB reclaims
    its pre-crash company by name against clientA's new server -- proves
    host migration end-to-end (election, promotion, reclaim, no desync)

Requires: a built tree (windows-release preset), the stub install dir
(fake-locomotion with Data\g1.DAT), allow_multiple_instances: true in the
OpenLoco config, and the competitor fixture object for the company-grant
outcome (scripts\gen_competitor_object.py). See KNOWLEDGEBASE.md § Headless.
-TestMaster additionally requires Go (go.exe at the path hard-coded below).

Exit code 0 = pass, 1 = fail.
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\windows\Release'),
    [string]$LocomotionPath = (Join-Path $PSScriptRoot '..\build\fake-locomotion'),
    [int]$Seed = 42,
    [int]$RunSeconds = 60,
    [ValidateSet('own', 'coop', 'spectator')]
    [string]$JoinPolicy = 'own',
    # Expected assignment outcome: 'company' (client granted a company) or
    # 'spectator'. Default derives from the policy.
    [string]$Expect = '',
    # Exercise the client-issued game command round trip: the client fires a
    # company rename ~2s after being assigned a real company, then verifies
    # ~8s later that the name actually changed -- which can only happen via
    # the full client -> server -> broadcast -> apply round trip, since
    # clients never apply their own queued commands locally. Only meaningful
    # when the client is granted a company (own/coop policies); ignored
    # (and asserted against) for spectator.
    [switch]$TestRename,
    # Exercise the host-driven mid-session Load flow headlessly: passes
    # `--test_host_load 20` to the host, which reloads its own starting
    # fixture ~20s in (once at least one client is assigned) and resyncs
    # every client. Needs RunSeconds comfortably past 20s plus resync time;
    # enforced below (>= 50). Not meaningful combined with -TestRename or
    # -TestShutdown (see the combination guard below) -- each hook is
    # exercised in its own run.
    [switch]$TestHostLoad,
    # Exercise the graceful server-shutdown flow headlessly: passes
    # `--test_shutdown_after 25` to the host, which then closes its network
    # session (but keeps running as single-player, does not exit) ~25s in.
    # Needs RunSeconds comfortably past 25s; enforced below (>= 35). Not
    # meaningful combined with -TestRename or -TestHostLoad.
    [switch]$TestShutdown,
    # Exercise the client auto-reconnect flow headlessly: passes
    # `--test_blackhole 20,20` to the CLIENT, which silently discards every
    # incoming packet from t=20s to t=40s (relative to its first connect()
    # call), a genuine simulated network outage -- both the client (its own
    # 15s NetworkConnection timeout) and the server (which stops hearing
    # anything back from the client, since a blackholed client also stops
    # ACKing/sending) independently time the connection out around t=35s.
    # The client then auto-retries against the same endpoint with its saved
    # session token every ~5s (up to 5 attempts); once the blackhole window
    # closes (t=40s) a retry succeeds and the server reclaims the reserved
    # seat. Needs RunSeconds comfortably past all of this; enforced below
    # (>= 80). Requires -JoinPolicy own or coop (need a company assignment to
    # compare before/after). Not meaningful combined with the other -Test*
    # switches.
    [switch]$TestReconnect,
    # Exercise LAN server discovery headlessly (docs/multiplayer.md § LAN
    # server discovery): the second process runs `--test_discover` instead
    # of `join 127.0.0.1` -- it never joins the host at all, it just probes
    # for it (broadcast + loopback) and logs each unique server found for
    # ~10s, then stops probing (the process itself keeps running - headless
    # has no exit path). The host runs completely normally (it always
    # answers discoveryRequest packets; no special host flag is needed).
    # Needs enough time for the ~10s discovery window to complete; enforced
    # below (>= 15). Not meaningful combined with the other -Test* switches.
    [switch]$TestDiscovery,
    # Exercise the master server integration end-to-end (docs/multiplayer.md
    # § "Master server (phase 2 - design)"): builds tools/master-server (`go
    # build`) and runs it as a THIRD process on loopback, on a high UDP port
    # -- both the host and the second process are pointed at it via
    # --master_server (a CLI override, not the shared config file - see
    # CommandLine.h). The second process runs `--test_discover` (like
    # -TestDiscovery) rather than joining. The host is started on a
    # NON-default game port specifically so LAN discovery (which only ever
    # probes kDefaultPort) genuinely cannot find it -- the only way the
    # discover process can learn about it is via the master server, so a
    # "[TEST] discovered server (master): ..." line unambiguously proves the
    # master path (announce -> registry -> query -> masterServerList ->
    # merge) works end-to-end rather than being masked by a simultaneous LAN
    # discovery. Asserts: host log shows "Announcing to master server",
    # discover process logs the master-sourced discovery line with the
    # right name/port/version, the master-server.exe process itself stays
    # alive for the whole run (stopped in this script's `finally` block, not
    # inline), plus the standard zero-error/host+client-alive checks. Needs
    # enough time for the ~10s discovery window plus process startup;
    # enforced below (>= 20). Not meaningful combined with the other -Test*
    # switches.
    [switch]$TestMaster,
    # Exercise host migration end-to-end (docs/multiplayer.md § Host
    # migration): starts a THIRD process, clientB, ~5s after the normal
    # client (clientA) joins. Both clients run with --test_fast_retry
    # (shortens connection-timeout/retry/migration-plan/window constants for
    # test practicality only -- no wire/behavioural change beyond timing);
    # the host additionally gets --test_kill_after 25 (hard std::_Exit,
    # simulating a genuine crash -- deliberately NOT --test_shutdown_after,
    # which sends a graceful serverClosing that suppresses client
    # auto-retry/migration by design) and --test_fast_retry too (shortens
    # its migration-plan broadcast cadence so both clients have a fresh plan
    # well before it dies). Both clients independently exhaust ordinary
    # auto-retry against the dead host, then elect the same successor from
    # their last received migrationPlan (clientA, the lower client id, since
    # it joined first): clientA promotes itself to session host in-process
    # (the facade just swaps its NetworkClient for a NetworkServer -- no new
    # game process is spawned), and clientB reclaims its pre-crash company
    # by name against clientA's freshly seeded reserved seats. Needs enough
    # time for detection+retries+election+reclaim plus a stability margin
    # afterwards; enforced below (>= 120). Requires -JoinPolicy own or coop.
    # Not meaningful combined with the other -Test* switches.
    [switch]$TestMigration
)

# Only used when -TestMaster is passed. Not on PATH on this machine - see
# KNOWLEDGEBASE.md § Master server (phase 2 service).
$goExe = 'C:\Users\rikyt\AppData\Local\Programs\go\bin\go.exe'

$ErrorActionPreference = 'Stop'

function Fail([string]$reason)
{
    Write-Host "FAIL: $reason" -ForegroundColor Red
    exit 1
}

# Title::loadTitle() (Title.cpp) requires Data/title.dat to carry the S5
# Header's isTitleSequence bit (S5.h HeaderFlags, 1 << 2) -- importSaveToGameState
# rejects any file without it when loading with LoadFlags::titleSequence
# ("File was not a title sequence"), and no exposed SaveFlags option sets
# this bit on export, so a plain gensave/simulate fixture fails it as-is.
# Rather than touch S5.cpp/Title.cpp (out of scope) or hand-roll a second
# fixture format, this patches the bit directly into an existing S5 file
# in place, mirroring the reverse-engineered byte-patch approach already
# used for the Competitor object fixture (see gen_competitor_object.py /
# KNOWLEDGEBASE.md § Competitor object fixture).
#
# S5 file layout (S5.cpp exportGameState / SawyerStream.cpp): the first
# chunk is always the 32-byte Header, written as
# [encoding u8][length u32 LE][encoded payload], encoding = rotate (3).
# SawyerStreamWriter::encodeRotate applies a per-byte rotl(byte, code) with
# code cycling 1, 3, 5, 7, ... (code_i = (1 + 2*i) mod 8; the matching
# decodeRotate in the reader applies rotr with the same code sequence to
# invert it); Header's second byte (index 1) is HeaderFlags, so code = 3
# there. Verified against a real gensave fixture: decoding chunk-relative
# byte 1 with rotr(_, 3) yields 8 (HeaderFlags::hasSaveDetails) and bytes
# 4-7 decode to the exact kCurrentVersion constant (0x62262), confirming
# both the byte offsets and the rotate direction. The trailing 4 bytes of
# the whole file are a checksum: a plain additive sum of every preceding
# byte (SawyerStreamWriter::write/writeChecksum) -- no rotate/CRC involved
# -- so it must be recomputed after patching a byte anywhere in the file.
function Set-TitleSequenceFlag([string]$path)
{
    $bytes = [IO.File]::ReadAllBytes($path)

    if ($bytes[0] -ne 3)
    {
        throw "Set-TitleSequenceFlag: expected SawyerEncoding::rotate (3) as the first chunk's encoding in '$path', got $($bytes[0])"
    }

    $flagsByteIndex = 5 + 1 # 1 (encoding) + 4 (length) + 1 (chunk-relative HeaderFlags byte index)
    $code = 3               # code_i for chunk-relative index 1: (1 + 2*1) mod 8

    $encoded = [int]$bytes[$flagsByteIndex]
    $decoded = (($encoded -shr $code) -bor ($encoded -shl (8 - $code))) -band 0xFF     # rotr undoes the writer's rotl
    $decoded = $decoded -bor 0x04                                                     # HeaderFlags::isTitleSequence
    $reEncoded = (($decoded -shl $code) -bor ($decoded -shr (8 - $code))) -band 0xFF   # rotl, matching the writer
    $bytes[$flagsByteIndex] = [byte]$reEncoded

    [uint32]$checksum = 0
    for ($i = 0; $i -lt $bytes.Length - 4; $i++)
    {
        $checksum = ($checksum + $bytes[$i]) -band 0xFFFFFFFF
    }
    $checksumBytes = [BitConverter]::GetBytes([uint32]$checksum)
    [Array]::Copy($checksumBytes, 0, $bytes, $bytes.Length - 4, 4)

    [IO.File]::WriteAllBytes($path, $bytes)
}

$testHookFlags = @($TestRename.IsPresent, $TestHostLoad.IsPresent, $TestShutdown.IsPresent, $TestReconnect.IsPresent, $TestDiscovery.IsPresent, $TestMaster.IsPresent, $TestMigration.IsPresent)
$exclusiveSwitchCount = ($testHookFlags | Where-Object { $_ }).Count
if ($exclusiveSwitchCount -gt 1)
{
    Fail 'TestRename, TestHostLoad, TestShutdown, TestReconnect, TestDiscovery, TestMaster and TestMigration are mutually exclusive -- run each in its own invocation'
}

if ($TestHostLoad -and $RunSeconds -lt 50)
{
    Fail "-TestHostLoad fires at 20s and needs time to resync afterwards; use -RunSeconds >= 50 (got $RunSeconds)"
}

if ($TestHostLoad -and $JoinPolicy -eq 'spectator')
{
    Fail '-TestHostLoad requires -JoinPolicy own or coop -- a spectator client never gets an "Assigned company" line to check for a fresh one'
}

if ($TestShutdown -and $RunSeconds -lt 35)
{
    Fail "-TestShutdown fires at 25s and needs time to observe the client's reaction; use -RunSeconds >= 35 (got $RunSeconds)"
}

if ($TestReconnect -and $RunSeconds -lt 80)
{
    Fail "-TestReconnect's blackhole runs 20s-40s and needs time for both sides to time out, retry and resync afterwards; use -RunSeconds >= 80 (got $RunSeconds)"
}

if ($TestReconnect -and $JoinPolicy -eq 'spectator')
{
    Fail '-TestReconnect requires -JoinPolicy own or coop -- a spectator client never gets an "Assigned company" line to compare before/after'
}

if ($TestDiscovery -and $RunSeconds -lt 15)
{
    Fail "-TestDiscovery's discovery window runs for ~10s; use -RunSeconds >= 15 (got $RunSeconds)"
}

if ($TestMaster -and $RunSeconds -lt 20)
{
    Fail "-TestMaster's discovery window runs for ~10s, plus master-server/host startup; use -RunSeconds >= 20 (got $RunSeconds)"
}

if ($TestMigration -and $RunSeconds -lt 120)
{
    Fail "-TestMigration needs time for the host to die (~25s), both clients to detect+exhaust retries+elect+reclaim, plus a stability margin afterwards; use -RunSeconds >= 120 (got $RunSeconds)"
}

if ($TestMigration -and $JoinPolicy -eq 'spectator')
{
    Fail '-TestMigration requires -JoinPolicy own or coop -- a spectator client never gets an "Assigned company" line to compare before/after'
}

if ($Expect -eq '')
{
    # own: freshly created company. coop: the host's (shared) company --
    # also a "company" outcome, not spectator, per NetworkServer's join
    # policy handling. spectator: explicit null assignment.
    $Expect = if ($JoinPolicy -eq 'spectator') { 'spectator' } else { 'company' }
}

$exe = Join-Path $BuildDir 'OpenLoco.exe'
if (-not (Test-Path $exe)) { Write-Error "OpenLoco.exe not found at $exe" }

$work = Join-Path ([IO.Path]::GetTempPath()) "openloco-sync-smoke-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
$fixture = Join-Path $work 'fixture.sv5'
$logDir = Join-Path $env:APPDATA 'OpenLoco\logs'

# -TestMaster only: high, PID-derived loopback ports (some spread to reduce
# collision risk between concurrent smoke-test runs on the same machine).
# $hostGamePort is deliberately NOT kDefaultPort (11754) -- LAN discovery
# only ever probes kDefaultPort, so a server on this port is unreachable via
# LAN discovery, making a "(master)"-tagged discovery result unambiguous
# proof of the master server path (see the -TestMaster switch doc above).
$portOffset = $PID % 5000
$masterUdpPort = 40000 + $portOffset
$hostGamePort = 45000 + $portOffset
$masterServerProc = $null

Push-Location $BuildDir
try
{
    if ($TestMaster)
    {
        if (-not (Test-Path $goExe)) { Fail "go.exe not found at $goExe (required for -TestMaster)" }

        Write-Host 'Building master-server.exe...'
        $masterServerSrcDir = Join-Path $PSScriptRoot '..\tools\master-server'
        $masterServerExe = Join-Path $work 'master-server.exe'
        Push-Location $masterServerSrcDir
        try
        {
            & $goExe build -o $masterServerExe .
            if ($LASTEXITCODE -ne 0) { Fail 'go build for master-server failed' }
        }
        finally
        {
            Pop-Location
        }
        if (-not (Test-Path $masterServerExe)) { Fail 'go build produced no master-server.exe' }

        Write-Host "Starting master server on 127.0.0.1:$masterUdpPort..."
        $masterServerProc = Start-Process $masterServerExe -ArgumentList @('-udp-port', $masterUdpPort, '-http-port', '0', '-ttl', '90s') -PassThru -NoNewWindow
        Start-Sleep 1
        if ($masterServerProc.HasExited) { Fail 'master-server process exited immediately after starting' }
    }

    Write-Host "Generating fixture (seed $Seed)..."
    & $exe --locomotion_path $LocomotionPath gensave $fixture --seed $Seed | Out-Null
    if (-not (Test-Path $fixture)) { Fail 'gensave produced no fixture' }

    # Title::loadTitle() (Title.cpp) unconditionally reads Data/title.dat
    # from the Locomotion install path whenever the title scene is entered
    # (e.g. after a graceful server shutdown requests it). The stub install
    # dir only ships Data\g1.DAT, so without this the title transition
    # throws ("file ... could not be found" / FileStream open failure) and
    # the process crashes -- discovered exercising -TestShutdown end-to-end
    # for the first time (see KNOWLEDGEBASE.md). The fixture save is a valid
    # S5 file regardless of destination filename, so reusing it as the title
    # sequence stub is sufficient; harmless for runs that never reach title.
    $titleDatPath = Join-Path $LocomotionPath 'Data\title.dat'
    Copy-Item $fixture $titleDatPath -Force
    Set-TitleSequenceFlag $titleDatPath

    # Fresh log dir so assertions only see this run
    Get-ChildItem $logDir -File -ErrorAction SilentlyContinue | Remove-Item -Force

    Write-Host "Starting host (join policy: $JoinPolicy)..."
    $hostArgs = @('--locomotion_path', $LocomotionPath, '--headless', '--log_levels', 'all', '--join_policy', $JoinPolicy)
    if ($TestHostLoad)
    {
        $hostArgs += @('--test_host_load', '20')
    }
    if ($TestShutdown)
    {
        $hostArgs += @('--test_shutdown_after', '25')
    }
    if ($TestMaster)
    {
        # Non-default game port (see $hostGamePort above) plus the master
        # server override, taking precedence over network.masterServer in
        # the shared config file (CommandLine.h's --master_server).
        $hostArgs += @('--port', $hostGamePort, '--master_server', "127.0.0.1:$masterUdpPort")
    }
    if ($TestMigration)
    {
        # Hard-crash (not a graceful close -- see the -TestMigration switch
        # doc above) ~25s in, plus shortened retry/timeout/migration-plan
        # constants so the whole election+promotion+reclaim sequence
        # resolves in well under the RunSeconds window.
        $hostArgs += @('--test_kill_after', '25', '--test_fast_retry')
    }
    $hostArgs += @('host', $fixture)
    $hostProc = Start-Process $exe -ArgumentList $hostArgs -PassThru -NoNewWindow
    Start-Sleep 8

    Write-Host 'Starting client...'
    $testRenameName = 'SyncTest'
    $clientArgs = @('--locomotion_path', $LocomotionPath, '--headless', '--log_levels', 'all')
    if ($TestDiscovery -or $TestMaster)
    {
        # Instead of joining: probes for the host via LAN discovery
        # (broadcast + loopback) and logs what it finds. Never connects.
        $clientArgs += @('--test_discover')
        if ($TestMaster)
        {
            # Same override as the host - queries the same master server
            # instead of (or alongside) LAN probing.
            $clientArgs += @('--master_server', "127.0.0.1:$masterUdpPort")
        }
    }
    else
    {
        $clientArgs += @('join', '127.0.0.1')
        if ($TestRename)
        {
            $clientArgs += @('--test_rename', $testRenameName)
        }
        if ($TestReconnect)
        {
            $clientArgs += @('--test_blackhole', '20,20')
        }
        if ($TestMigration)
        {
            # Every process in this script shares the same %APPDATA% config
            # file, so preferredOwnerName is otherwise identical (empty)
            # across clientA and clientB -- which would break migration
            # reclaim's name-based matching (an empty name's "Player #<id>"
            # display fallback is id-dependent, not a stable identity). See
            # CommandLine.h's --test_owner_name doc.
            $clientArgs += @('--test_fast_retry', '--test_owner_name', 'ClientA')
        }
    }
    $clientProc = Start-Process $exe -ArgumentList $clientArgs -PassThru -NoNewWindow

    $clientBProc = $null
    if ($TestMigration)
    {
        Start-Sleep 5
        Write-Host 'Starting second client (clientB)...'
        $clientBArgs = @('--locomotion_path', $LocomotionPath, '--headless', '--log_levels', 'all', '--test_fast_retry', '--test_owner_name', 'ClientB', 'join', '127.0.0.1')
        $clientBProc = Start-Process $exe -ArgumentList $clientBArgs -PassThru -NoNewWindow
    }

    Write-Host "Running for $RunSeconds seconds..."
    Start-Sleep $RunSeconds

    $hostAlive = -not $hostProc.HasExited
    $clientAlive = -not $clientProc.HasExited
    $clientBAlive = $true
    if ($TestMigration) { $clientBAlive = -not $clientBProc.HasExited }
    $masterAlive = $true
    if ($TestMaster) { $masterAlive = -not $masterServerProc.HasExited }
    $stopIds = @($hostProc.Id, $clientProc.Id)
    if ($TestMigration) { $stopIds += $clientBProc.Id }
    Stop-Process -Id $stopIds -Force -ErrorAction SilentlyContinue
    Start-Sleep 2

    if ($TestMigration)
    {
        # The host is SUPPOSED to die (--test_kill_after hard-exits it,
        # simulating a crash) -- only clientA (the elected/promoted
        # successor) and clientB (which reclaims into it) must still be
        # alive at the end.
        if (-not $clientAlive) { Fail 'clientA process died during the run' }
        if (-not $clientBAlive) { Fail 'clientB process died during the run' }
    }
    else
    {
        if (-not $hostAlive) { Fail 'host process died during the run' }
        if (-not $clientAlive) { Fail 'client process died during the run' }
    }
    if ($TestMaster -and -not $masterAlive) { Fail 'master server process died during the run' }

    # Sort by name: the filename embeds the creation timestamp, so processes
    # sort in start order (host, then clientA, then clientB if present).
    # LastWriteTime is unreliable here - it reflects whichever process
    # happened to flush a log line last.
    $logs = Get-ChildItem $logDir -File | Sort-Object Name
    $expectedLogCount = if ($TestMigration) { 3 } else { 2 }
    if ($logs.Count -lt $expectedLogCount) { Fail "expected $expectedLogCount log files, found $($logs.Count)" }
    $hostLog = Get-Content $logs[0].FullName
    $clientLog = Get-Content $logs[1].FullName
    $clientBLog = if ($TestMigration) { Get-Content $logs[2].FullName } else { @() }

    $accepts = ($hostLog | Select-String 'Accepted new client').Count
    if ($TestDiscovery -or $TestMaster)
    {
        # Neither the discovery process nor the master-integration discover
        # process ever sends a ConnectPacket at all -- they only probe/query
        # and listen for replies. Confirms discovery really does bypass the
        # join flow entirely, not just skip logging.
        if ($accepts -ne 0) { Fail "host accepted $accepts clients (expected 0 -- -TestDiscovery/-TestMaster must never join)" }
    }
    elseif ($TestMigration)
    {
        # The ORIGINAL host must accept both clientA and clientB before it
        # dies -- the later migration reclaim is handled by clientA's
        # promoted server, a different process, and is asserted separately.
        if ($accepts -ne 2) { Fail "host accepted $accepts clients (expected 2 -- clientA and clientB before the host died)" }
    }
    else
    {
        if ($accepts -ne 1) { Fail "host accepted $accepts clients (expected 1)" }
    }

    # Gameplay-transition and join-assignment assertions are meaningless for
    # -TestDiscovery/-TestMaster: that process never joins, so it never
    # receives a snapshot or a company assignment, and never transitions to
    # gameplay. -TestMigration has its own dedicated assertion block below
    # (it needs to check clientB too, not just $clientLog).
    if (-not $TestDiscovery -and -not $TestMaster -and -not $TestMigration)
    {
        if (($clientLog | Select-String 'Scene transition: boot -> gameplay' -SimpleMatch).Count -lt 1)
        {
            Fail 'client never reached gameplay (state transfer incomplete)'
        }

        switch ($Expect)
        {
            'company'
            {
                if (($hostLog | Select-String 'Assigned (host )?company \d+ to client').Count -lt 1) { Fail 'host never assigned a company' }
                if (($clientLog | Select-String 'Assigned company \d+').Count -lt 1) { Fail 'client never received its company assignment' }
            }
            'spectator'
            {
                if (($clientLog | Select-String 'remaining a spectator').Count -lt 1) { Fail 'client never acknowledged spectator role' }
            }
            default { Fail "unknown expectation '$Expect'" }
        }
    }

    if ($TestRename -and $Expect -eq 'company')
    {
        if (($clientLog | Select-String "[TEST] rename verified: '$testRenameName'" -SimpleMatch).Count -lt 1)
        {
            Fail "client-issued rename round trip did not verify (expected `"[TEST] rename verified: '$testRenameName'`" in client log)"
        }
    }

    if ($TestHostLoad)
    {
        # The host reloads its own fixture ~20s in (once the client is
        # assigned), which must resync the client: it discards its state,
        # re-requests the snapshot, and gets a fresh company assignment. So
        # we need a "Host loaded a new game" line followed by a SECOND
        # "Assigned company" line (the first one is the initial join).
        $hostLoadMatch = $clientLog | Select-String 'Host loaded a new game' -SimpleMatch | Select-Object -First 1
        if (-not $hostLoadMatch)
        {
            Fail "client never logged 'Host loaded a new game' (--test_host_load hook did not fire or resync did not reach the client)"
        }

        $assignedMatches = @($clientLog | Select-String 'Assigned company \d+')
        if ($assignedMatches.Count -lt 2)
        {
            Fail "expected 2+ 'Assigned company' lines in client log (initial join + post-reload), found $($assignedMatches.Count)"
        }

        $freshAssignment = $assignedMatches | Where-Object { $_.LineNumber -gt $hostLoadMatch.LineNumber } | Select-Object -First 1
        if (-not $freshAssignment)
        {
            Fail "no 'Assigned company' line found after 'Host loaded a new game' in client log (fresh assignment missing)"
        }
    }

    if ($TestShutdown)
    {
        # The host closes its network session gracefully ~25s in (but keeps
        # running as single-player); the client should hear about it and
        # return to title cleanly, never falling back to its 15s connection
        # timeout path.
        if (($clientLog | Select-String 'Server is shutting down' -SimpleMatch).Count -lt 1)
        {
            Fail "client never logged 'Server is shutting down' (--test_shutdown_after hook did not fire or the notification was not received)"
        }

        if (($clientLog | Select-String 'timed out').Count -gt 0)
        {
            Fail "client log contains 'timed out' -- the shutdown should be graceful, not a connection timeout"
        }

        # A graceful shutdown must never trigger the auto-reconnect loop
        # (docs/multiplayer.md § Reconnect / NetworkClient::
        # receiveServerClosingPacket sets _suppressAutoRetry before closing).
        if (($clientLog | Select-String 'Reconnecting \(attempt').Count -gt 0)
        {
            Fail "client log contains 'Reconnecting (attempt' -- a graceful server shutdown must not trigger auto-reconnect"
        }
    }

    if ($TestReconnect)
    {
        # Client side: the blackhole must have caused a real timeout and at
        # least one genuine auto-retry attempt.
        $reconnectMatch = $clientLog | Select-String 'Reconnecting \(attempt' | Select-Object -First 1
        if (-not $reconnectMatch)
        {
            Fail "client never logged 'Reconnecting (attempt' (the blackhole did not cause an established-connection timeout, or auto-retry did not trigger)"
        }

        # There must be an "Assigned company N" line before the outage and
        # another one afterwards, for the SAME N -- proving the reconnect
        # reclaimed the original seat/company rather than joining fresh.
        $assignedMatches = @($clientLog | Select-String 'Assigned company (\d+)')
        if ($assignedMatches.Count -lt 2)
        {
            Fail "expected 2+ 'Assigned company' lines in client log (initial join + post-reconnect), found $($assignedMatches.Count)"
        }

        $initialCompany = $assignedMatches[0].Matches[0].Groups[1].Value
        $postReconnect = $assignedMatches | Where-Object { $_.LineNumber -gt $reconnectMatch.LineNumber } | Select-Object -First 1
        if (-not $postReconnect)
        {
            Fail "no 'Assigned company' line found after the 'Reconnecting' line (reconnect never completed a fresh state transfer)"
        }
        if ($postReconnect.Matches[0].Groups[1].Value -ne $initialCompany)
        {
            Fail "post-reconnect company ($($postReconnect.Matches[0].Groups[1].Value)) differs from the original ($initialCompany) -- the client rejoined as a different seat instead of reclaiming its own"
        }

        # Host side: the timeout must have reserved the seat, and the
        # reconnect must have reclaimed it (not been treated as a fresh
        # join).
        if (($hostLog | Select-String 'Reserved seat for' -SimpleMatch).Count -lt 1)
        {
            Fail "host never logged reserving a seat for the timed-out client (removedTimedOutClients did not fire, or did not reserve)"
        }
        if (($hostLog | Select-String 'reclaimed its reserved seat' -SimpleMatch).Count -lt 1)
        {
            Fail "host never logged the client reclaiming its reserved seat (reconnect was treated as a fresh join, or never arrived)"
        }

        # Reclaiming must not go through the fresh-join path: the earlier,
        # unconditional "$accepts -ne 1" check above already proves the host
        # only ever saw one "Accepted new client" for the whole run, i.e. the
        # reconnect above was a reclaim, not a second fresh join.
    }

    if ($TestMigration)
    {
        # Both clients must have reached gameplay and received their initial
        # company assignment before the host died.
        if (($clientLog | Select-String 'Scene transition: boot -> gameplay' -SimpleMatch).Count -lt 1) { Fail 'clientA never reached gameplay' }
        if (($clientBLog | Select-String 'Scene transition: boot -> gameplay' -SimpleMatch).Count -lt 1) { Fail 'clientB never reached gameplay' }

        $clientAInitial = @($clientLog | Select-String 'Assigned company (\d+)') | Select-Object -First 1
        $clientBInitial = @($clientBLog | Select-String 'Assigned company (\d+)') | Select-Object -First 1
        if (-not $clientAInitial) { Fail 'clientA never received its initial company assignment' }
        if (-not $clientBInitial) { Fail 'clientB never received its initial company assignment' }

        # Exactly one promotion, and it must be clientA (the lower client
        # id, since it joined first, and therefore plan[0] - the
        # deterministic successor per docs/multiplayer.md § Host migration).
        $promotions = @(($clientLog + $clientBLog) | Select-String 'Promoted to session host' -SimpleMatch)
        if ($promotions.Count -ne 1)
        {
            Fail "expected exactly 1 'Promoted to session host' line across both clients, found $($promotions.Count)"
        }
        if (($clientLog | Select-String 'Promoted to session host' -SimpleMatch).Count -ne 1)
        {
            Fail "clientA (expected successor) never logged 'Promoted to session host'"
        }
        if (($clientBLog | Select-String 'Promoted to session host' -SimpleMatch).Count -ne 0)
        {
            Fail 'clientB logged being promoted to session host -- it should have reclaimed into clientA instead'
        }

        # clientB must have reclaimed its seat by name into clientA's new
        # server and been re-assigned the SAME company it had before the
        # crash.
        if (($clientBLog | Select-String 'Host migration: reclaim successful' -SimpleMatch).Count -lt 1)
        {
            Fail 'clientB never logged a successful migration reclaim'
        }

        $clientBAllAssigned = @($clientBLog | Select-String 'Assigned company (\d+)')
        if ($clientBAllAssigned.Count -lt 2)
        {
            Fail "expected 2+ 'Assigned company' lines in clientB log (initial join + post-migration reclaim), found $($clientBAllAssigned.Count)"
        }
        $clientBInitialCompany = $clientBInitial.Matches[0].Groups[1].Value
        $clientBFinalCompany = $clientBAllAssigned[-1].Matches[0].Groups[1].Value
        if ($clientBFinalCompany -ne $clientBInitialCompany)
        {
            Fail "clientB's post-migration company ($clientBFinalCompany) differs from its pre-crash company ($clientBInitialCompany)"
        }
    }

    if ($TestDiscovery)
    {
        # The host's own display name falls back to "Player #0" under the
        # headless fixture (empty preferredOwnerName - see
        # Network::resolveDisplayName), and it always binds to the default
        # port (11754) here since neither the host nor client override
        # --bind/--port in this script. maxPlayers is always kMaxRosterEntries
        # (32); version must match this build's kNetworkVersion (9 as of
        # this writing -- bump alongside kNetworkVersion if it changes).
        $discoveredMatch = $clientLog | Select-String "\[TEST\] discovered server: 'Player #0' 127\.0\.0\.1:11754 players=\d+/32 version=9" | Select-Object -First 1
        if (-not $discoveredMatch)
        {
            Fail "client never logged discovering the host (expected a '[TEST] discovered server: ''Player #0'' 127.0.0.1:11754 players=N/32 version=9' line)"
        }
    }

    if ($TestMaster)
    {
        # The host must have logged that it's announcing to the configured
        # master server (--master_server, resolved at listen() time).
        if (($hostLog | Select-String 'Announcing to master server' -SimpleMatch).Count -lt 1)
        {
            Fail "host never logged 'Announcing to master server' (--master_server did not take effect)"
        }

        # The discover process must have learned about the host specifically
        # via the MASTER server, not LAN discovery (which cannot find it --
        # the host listens on $hostGamePort, a non-default port). name/port/
        # version must match; version bumps alongside kNetworkVersion.
        $discoveredMasterMatch = $clientLog | Select-String "\[TEST\] discovered server \(master\): 'Player #0' 127\.0\.0\.1:$hostGamePort players=\d+/32 version=9" | Select-Object -First 1
        if (-not $discoveredMasterMatch)
        {
            Fail "client never logged discovering the host via the master server (expected a '[TEST] discovered server (master): ...' line naming 127.0.0.1:$hostGamePort version=9)"
        }
    }

    $badLines = @($hostLog + $clientLog + $clientBLog | Select-String '\[ERR\]|[Dd]esync')
    if ($badLines.Count -gt 0)
    {
        $badLines | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }
        Fail "$($badLines.Count) error/desync line(s) in the logs"
    }

    $extraNote = ''
    if ($TestRename -and $Expect -eq 'company') { $extraNote = ', rename round trip verified' }
    elseif ($TestHostLoad) { $extraNote = ', host-load resync verified' }
    elseif ($TestShutdown) { $extraNote = ', graceful shutdown verified (no auto-reconnect)' }
    elseif ($TestReconnect) { $extraNote = ', auto-reconnect + seat reclaim verified' }
    elseif ($TestDiscovery) { $extraNote = ', LAN discovery verified (host found via loopback, never joined)' }
    elseif ($TestMaster) { $extraNote = ', master server integration verified (announce -> query -> merge, via loopback)' }
    elseif ($TestMigration) { $extraNote = ', host migration verified (election -> promotion -> reclaim, no desync)' }
    Write-Host "PASS: lockstep held for $RunSeconds s (policy=$JoinPolicy, expect=$Expect$extraNote)" -ForegroundColor Green
    exit 0
}
finally
{
    Pop-Location
    # Stopped here (rather than inline with the host/client above) so it is
    # torn down regardless of how the try block above exits, including an
    # early Fail().
    if ($masterServerProc -and -not $masterServerProc.HasExited)
    {
        Stop-Process -Id $masterServerProc.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
