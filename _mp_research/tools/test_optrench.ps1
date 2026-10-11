# Trenches in an operation battle, two games on one PC. Needs an operation save with a battle pending:
#   test_optrench.ps1 -Save <file.opsave> -Operation 1943_08_Mius:08_28_saur_rus -Turns 7 -Zone 0.324,0.751 [-NoSync]
# P1 hosts the operation on its map and starts the pending battle (a click on its zone); P2 joins the
# battle. During deployment both players put some of their squads somewhere else (the game digs a squad in
# again where it is put; the test makes that call through the test command "dig"). The trenches of the two
# games are compared at three moments: when both are in deployment, after the squads were moved, and in
# the battle. -NoSync: each game digs for itself (MP_SYNC_TRENCH=0).
param([Parameter(Mandatory = $true)][string]$Save, [Parameter(Mandatory = $true)][string]$Operation, [int]$Turns = 7,
      [double[]]$Zone = @(0.324, 0.751), [switch]$NoSync, [int]$Squads = 4, [double]$Shift = 25, [hashtable]$Extra = @{}, [int]$Fight = 20)
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$mp = "$script:Game\_mp_research"
& cmd.exe /c "`"$mp\enable_multiplayer.bat`"" | Out-Null
Copy-Item $Save ("$script:Game\users\profiles\cloud\" + ($Operation -replace ':', '_') + ".opsave") -Force
$started = Get-Date
$vars = @{ MP_LOG_TRENCH = "1" } + $Extra
if ($NoSync) { $vars["MP_SYNC_TRENCH"] = "0" }
function Battles([int[]]$except) {
    Get-CimInstance Win32_Process -Filter "Name='starter_x64.exe'" | Where-Object { $_.CommandLine -match 'tacgame\.progpack' -and $except -notcontains $_.ProcessId } |
        Sort-Object CreationDate | ForEach-Object { Get-Process -Id $_.ProcessId }
}
function Compare-Trenches([string]$when) {
    $d = & python "$PSScriptRoot\trench_diff.py" $bt.Id $bt2.Id
    Write-Host "--- trenches $when"
    $d | Where-Object { $_ -match 'host game|10 cm|trenches:' } | ForEach-Object { Write-Host $_ }
    ($d | Out-String)
}

$p1 = Start-Game "root\programs\opgame.progpack, cloud, $Operation, 0, $Turns" (@{ MP_ROLE = "host"; MP_HOST_MODE = "operation" } + $vars)
$p1log = Get-MpLog $p1 "opgame"
Check "P1: hosting, on the operational map" ([bool](Wait-Log $p1log 'net: hosting on' 90))
Start-Sleep 12
[void](Click-Game $p1 $Zone[0] $Zone[1]); [void][GtAuto]::SetCursorPos(3000, 1300)
$bt = $null; for ($i = 0; $i -lt 90 -and -not $bt; $i++) { $bt = Battles @() | Select-Object -First 1; if (-not $bt) { Start-Sleep 1 } }
Check "P1: battle started from the map" ([bool]$bt)
if (-not $bt) { Stop-Game $p1; Show-Results; exit 1 }
$btlog = Get-MpLog $bt "tacgame"
Check "P1: battle hosted" ([bool](Wait-Log $btlog 'net: hosting battle on' 240))
$env:MP_TEST = "1"
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k]) }
$join = & "$script:Game\mp_join.exe" 127.0.0.1 | Out-String
$env:MP_TEST = $null
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $null) }
$bt2 = $null; for ($i = 0; $i -lt 90 -and -not $bt2; $i++) { $bt2 = Battles @($bt.Id) | Select-Object -First 1; if (-not $bt2) { Start-Sleep 1 } }
Check "P2: in the battle" ([bool]$bt2)
if (-not $bt2) { Stop-Game $bt; Show-Results; exit 1 }
$bt2log = Get-MpLog $bt2 "tacgame"
Check "P2: connected inside the battle" ([bool](Wait-Log $bt2log 'net: connected to host' 240))
foreach ($l in @($btlog, $bt2log)) { (Select-String -Path $l -Pattern 'this player commands').Line | ForEach-Object { "   " + $_.Substring(22) } }
Start-Sleep 25
$first = Compare-Trenches "with both games in deployment"
"--- both players put $Squads squads somewhere else ($Shift m away)"
Send-Command $bt "dig $Squads $Shift"; Send-Command $bt2 "dig $Squads $Shift"
foreach ($l in @($btlog, $bt2log)) { $line = Wait-Log $l 'test: \d+ squads of this player dug in again' 15; if ($line) { "   " + $line.Substring(22) } }
Start-Sleep 8
$moved = Compare-Trenches "after the squads were moved"
Send-Ready $bt; Send-Ready $bt2
Check "deployment ended in both games" ([bool](Wait-Log $btlog 'phase: initial orders' 40) -and [bool](Wait-Log $bt2log 'phase: initial orders' 40))
Start-Sleep 4
Send-Ready $bt; Send-Ready $bt2
Check "the battle began in both games" ([bool](Wait-Log $btlog 'phase: battle$' 40) -and [bool](Wait-Log $bt2log 'phase: battle$' 40))
Start-Sleep $Fight
$last = Compare-Trenches "in the battle"
Write-Host "--- descriptions"; & python "$PSScriptRoot\desc_diff.py" $bt.Id $bt2.Id | ForEach-Object { Write-Host $_ }
if (-not $NoSync) {
    Check "same trenches after the squads were moved" ($moved -match 'the same in both games')
    Check "same trenches in the battle" ($last -match 'the same in both games')
}
foreach ($l in @($btlog, $bt2log)) {
    (Select-String -Path $l -Pattern 'trench: \d+ squads').Line | Select-Object -Last 1 | ForEach-Object { Write-Host ("   " + $_.Substring(22, [Math]::Min(200, $_.Length - 22))) }
    $m = @(Select-String -Path $l -Pattern 'mean correction ([0-9.]+) m, largest ([0-9.]+) m') | Select-Object -Last 4
    if ($m) { Write-Host ("   how far this game's own simulation puts the other player's units from where they are, last 20 s: mean " + (($m | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Measure-Object -Average).Average.ToString('0.00')) + " m, largest " + (($m | ForEach-Object { [double]$_.Matches[0].Groups[2].Value } | Measure-Object -Maximum).Maximum.ToString('0.00')) + " m") }
}
$bt.Refresh(); $bt2.Refresh()
Check "both games still running" (-not $bt.HasExited -and -not $bt2.HasExited)
$dump = Get-Item "$script:Game\out\minidump.dmp" -ErrorAction SilentlyContinue
Check "no crash dump written" (-not ($dump -and $dump.LastWriteTime -gt $started))
foreach ($g in @(Get-Process starter_x64 -ErrorAction SilentlyContinue)) { Stop-Game $g }
Show-Results
