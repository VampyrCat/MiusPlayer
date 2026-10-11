# Combat synchronisation test: a host and a joining game run the same battle on this PC, and the units'
# existence, status and position are compared between the two games several times during the fight.
#   test_combat.ps1 [-PositionsOnly] [-NoBuild] [-Warmup 75] [-Samples 6] [-Every 20]
# -PositionsOnly switches full-state replication off, as the baseline to compare against.
param([switch]$PositionsOnly, [switch]$NoBuild, [int]$Warmup = 75, [int]$Samples = 6, [int]$Every = 20, [string]$World = "", [switch]$Details)
$game = (Resolve-Path "$PSScriptRoot\..\..").Path
$env:MP_DEBUG = "1"
$env:MP_SYNC_STATE = if ($PositionsOnly) { "0" } else { "1" }
if ($World) { $env:MP_SYNC_WORLD = $World }
$started = Get-Date
$dump = Get-Item "$game\out\minidump.dmp" -ErrorAction SilentlyContinue
$out = & "$PSScriptRoot\test_battle.ps1" -Instances 2 -Seconds $Warmup -Net -NoBuild:$NoBuild -Check "combat_diff.py $Samples $Every"
$env:MP_DEBUG = $null; $env:MP_SYNC_STATE = $null; $env:MP_SYNC_WORLD = $null
"mode: " + $(if ($PositionsOnly) { "positions only" } else { "full state replication" }) + $(if ($World) { ", world objects $World" } else { "" })
$out | Where-Object { $_ -match 'sample|disagree|worst|exited|Traceback|Error' -or ($Details -and $_ -match '^      i_') }
$newDump = Get-Item "$game\out\minidump.dmp" -ErrorAction SilentlyContinue
if ($newDump -and (-not $dump -or $newDump.LastWriteTime -gt $started)) { "CRASH: a game wrote a crash dump during this run"; & python "$PSScriptRoot\crash_info.py" | Select-Object -First 12 }
Get-ChildItem "$game\out" -Filter "basic_network_tacgame_*.log" | Where-Object { $_.LastWriteTime -gt $started } | ForEach-Object {
    $role = if (Select-String -Path $_.FullName -Pattern 'net: hosting' -Quiet) { "host" } else { "join" }
    $lines = (Select-String -Path $_.FullName -Pattern 'state: 5 s|net: 5 s').Line | Select-Object -Last 4 | ForEach-Object { $_.Substring(22) }
    "--- $role"; $lines
}
