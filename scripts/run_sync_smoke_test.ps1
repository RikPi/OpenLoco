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
    [string]$Expect = ''
)

$ErrorActionPreference = 'Stop'

if ($Expect -eq '')
{
    $Expect = if ($JoinPolicy -eq 'own') { 'company' } else { 'spectator' }
}

$exe = Join-Path $BuildDir 'OpenLoco.exe'
if (-not (Test-Path $exe)) { Write-Error "OpenLoco.exe not found at $exe" }

$work = Join-Path ([IO.Path]::GetTempPath()) "openloco-sync-smoke-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
$fixture = Join-Path $work 'fixture.sv5'
$logDir = Join-Path $env:APPDATA 'OpenLoco\logs'

function Fail([string]$reason)
{
    Write-Host "FAIL: $reason" -ForegroundColor Red
    exit 1
}

Push-Location $BuildDir
try
{
    Write-Host "Generating fixture (seed $Seed)..."
    & $exe --locomotion_path $LocomotionPath gensave $fixture --seed $Seed | Out-Null
    if (-not (Test-Path $fixture)) { Fail 'gensave produced no fixture' }

    # Fresh log dir so assertions only see this run
    Get-ChildItem $logDir -File -ErrorAction SilentlyContinue | Remove-Item -Force

    Write-Host "Starting host (join policy: $JoinPolicy)..."
    $hostArgs = @('--locomotion_path', $LocomotionPath, '--headless', '--log_levels', 'all', '--join_policy', $JoinPolicy, 'host', $fixture)
    $hostProc = Start-Process $exe -ArgumentList $hostArgs -PassThru -NoNewWindow
    Start-Sleep 8

    Write-Host 'Starting client...'
    $clientArgs = @('--locomotion_path', $LocomotionPath, '--headless', '--log_levels', 'all', 'join', '127.0.0.1')
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

    $badLines = @($hostLog + $clientLog | Select-String '\[ERR\]|[Dd]esync')
    if ($badLines.Count -gt 0)
    {
        $badLines | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }
        Fail "$($badLines.Count) error/desync line(s) in the logs"
    }

    Write-Host "PASS: lockstep held for $RunSeconds s (policy=$JoinPolicy, expect=$Expect)" -ForegroundColor Green
    exit 0
}
finally
{
    Pop-Location
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
