# Shots and fog of war, two games on one PC: A hosts the saved test battle, B joins through mp_join.exe.
# Both press Start twice, the battle runs for -Seconds of real time (at -Speed times real time), then the
# two launch logs are compared and the fog of war is read from both games.
#   test_shots.ps1 [-Side germans|soviets] [-Seconds 120] [-Speed 3] [-NoShotSync] [-NoStateSync] [-KeepRunning]
# -Side soviets: both players on the host's side, the computer commands the opponent (plenty of fighting).
# -NoShotSync: each game fires for itself, as before shot replication - to measure what that looked like.
param([ValidateSet("germans", "soviets")][string]$Side = "germans", [int]$Seconds = 120, [double]$Speed = 3,
      [switch]$NoShotSync, [switch]$NoStateSync, [switch]$KeepRunning, [hashtable]$Extra = @{})
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$mp = "$script:Game\_mp_research"
& cmd.exe /c "`"$mp\enable_multiplayer.bat`"" | Out-Null
Get-ChildItem "$script:Game\out" -Filter "basic_network*.log" | ForEach-Object { [IO.File]::Delete($_.FullName) }
Get-ChildItem "$script:Game\out" -Filter "mp_shots_*.txt" | ForEach-Object { [IO.File]::Delete($_.FullName) }
Copy-Item "$mp\test_battle_image.cfgpack" "$script:Game\data\common\shared\config\tacgame_image.cfgpack" -Force
$started = Get-Date
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$half = [int]($screen.Width / 2)
$vars = @{ MP_LOG_SHOTS = "1"; MP_TEST_SPEED = "$Speed" } + $Extra
if ($NoShotSync) { $vars["MP_SYNC_SHOTS"] = "0" }
if ($NoStateSync) { $vars["MP_SYNC_STATE"] = "0" }

$a = Start-Game "root\programs\tacgame.progpack, data\common\shared\config\tacgame_image.cfgpack, users\profiles, cloud, *:*, 0, 1" (@{
    MP_ROLE = "host"; MP_WINDOW = "0,0,$half,$($screen.Height)" } + $vars)
$alog = Get-MpLog $a "tacgame"
Check "A: hosting the battle" ([bool](Wait-Log $alog 'net: hosting battle on' 90))
$env:MP_TEST = "1"; $env:MP_WINDOW = "$half,0,$half,$($screen.Height)"
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k]) }
$join = & "$script:Game\mp_join.exe" 127.0.0.1 $Side | Out-String
$env:MP_TEST = $null; $env:MP_WINDOW = $null
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $null) }
$b = $null
for ($i = 0; $i -lt 40 -and -not $b; $i++) {
    $b = Get-CimInstance Win32_Process -Filter "Name='starter_x64.exe'" | Where-Object { $_.CommandLine -match 'tacgame\.progpack' -and $_.ProcessId -ne $a.Id } |
         Select-Object -First 1 | ForEach-Object { Get-Process -Id $_.ProcessId }
    if (-not $b) { Start-Sleep 1 }
}
Check "B: battle started" ([bool]$b) ($join.Trim() -replace '\s+', ' ')
if (-not $b) { Stop-Game $a; Show-Results; exit 1 }
$blog = Get-MpLog $b "tacgame"
Check "B: connected to A" ([bool](Wait-Log $blog 'net: connected to host' 90))
Check "Start button is shared" ([bool](Wait-Log $alog 'Start button shared' 30) -and [bool](Wait-Log $blog 'Start button shared' 30))
Start-Sleep 3
Send-Ready $a; Send-Ready $b
Check "initial orders reached" ([bool](Wait-Log $alog 'phase: initial orders' 30) -and [bool](Wait-Log $blog 'phase: initial orders' 30))
Start-Sleep 3
Send-Ready $a; Send-Ready $b
Check "battle reached" ([bool](Wait-Log $alog 'phase: battle$' 30) -and [bool](Wait-Log $blog 'phase: battle$' 30))
"running the battle for $Seconds s at $Speed times real time..."
Start-Sleep $Seconds
foreach ($g in @($a, $b)) { $g.Refresh() }
Check "both games still running" (-not $a.HasExited -and -not $b.HasExited)
$dump = Get-Item "$script:Game\out\minidump.dmp" -ErrorAction SilentlyContinue
Check "no crash dump written" (-not ($dump -and $dump.LastWriteTime -gt $started))

"--- launches"
$diff = & python "$PSScriptRoot\shots_diff.py" "$script:Game\out\mp_shots_$($a.Id).txt" "$script:Game\out\mp_shots_$($b.Id).txt"
$diff
if (-not $NoShotSync) { Check "every launch of one game was carried out by the other" (($diff | Out-String) -match 'every launch arrived') }
"--- fog of war"
$fog = & python "$PSScriptRoot\fog_table.py" $a.Id $b.Id
$fog
"--- clocks"
foreach ($log in @($alog, $blog)) { (Select-String -Path $log -Pattern 'clock: ').Line | Select-Object -Last 2 | ForEach-Object { $_.Substring(22) } }
"--- shots"
foreach ($log in @($alog, $blog)) { (Select-String -Path $log -Pattern 'shots: 5 s').Line | Select-Object -Last 2 | ForEach-Object { $_.Substring(22) } }
[void](Save-Shot $a "shots_a"); [void](Save-Shot $b "shots_b")
if (-not $KeepRunning) { Stop-Game $b; Stop-Game $a }
Show-Results
