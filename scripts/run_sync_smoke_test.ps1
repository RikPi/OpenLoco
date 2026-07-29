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

Requires: a built tree (windows-release preset), the stub install dir
(fake-locomotion with Data\g1.DAT), allow_multiple_instances: true in the
OpenLoco config, and the competitor fixture object for the company-grant
outcome (scripts\gen_competitor_object.py). See KNOWLEDGEBASE.md § Headless.

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
    [switch]$TestReconnect
)

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

$testHookFlags = @($TestRename.IsPresent, $TestHostLoad.IsPresent, $TestShutdown.IsPresent, $TestReconnect.IsPresent)
$exclusiveSwitchCount = ($testHookFlags | Where-Object { $_ }).Count
if ($exclusiveSwitchCount -gt 1)
{
    Fail 'TestRename, TestHostLoad, TestShutdown and TestReconnect are mutually exclusive -- run each in its own invocation'
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

Push-Location $BuildDir
try
{
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
    $hostArgs += @('host', $fixture)
    $hostProc = Start-Process $exe -ArgumentList $hostArgs -PassThru -NoNewWindow
    Start-Sleep 8

    Write-Host 'Starting client...'
    $testRenameName = 'SyncTest'
    $clientArgs = @('--locomotion_path', $LocomotionPath, '--headless', '--log_levels', 'all', 'join', '127.0.0.1')
    if ($TestRename)
    {
        $clientArgs += @('--test_rename', $testRenameName)
    }
    if ($TestReconnect)
    {
        $clientArgs += @('--test_blackhole', '20,20')
    }
    $clientProc = Start-Process $exe -ArgumentList $clientArgs -PassThru -NoNewWindow

    Write-Host "Running for $RunSeconds seconds..."
    Start-Sleep $RunSeconds

    $hostAlive = -not $hostProc.HasExited
    $clientAlive = -not $clientProc.HasExited
    Stop-Process -Id $hostProc.Id, $clientProc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep 2

    if (-not $hostAlive) { Fail 'host process died during the run' }
    if (-not $clientAlive) { Fail 'client process died during the run' }

    $logs = Get-ChildItem $logDir -File | Sort-Object LastWriteTime
    if ($logs.Count -lt 2) { Fail "expected 2 log files, found $($logs.Count)" }
    $hostLog = Get-Content $logs[0].FullName
    $clientLog = Get-Content $logs[1].FullName

    $accepts = ($hostLog | Select-String 'Accepted new client').Count
    if ($accepts -ne 1) { Fail "host accepted $accepts clients (expected 1)" }

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

    $badLines = @($hostLog + $clientLog | Select-String '\[ERR\]|[Dd]esync')
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
    Write-Host "PASS: lockstep held for $RunSeconds s (policy=$JoinPolicy, expect=$Expect$extraNote)" -ForegroundColor Green
    exit 0
}
finally
{
    Pop-Location
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
