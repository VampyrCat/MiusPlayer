# Trenches, two games on one PC: A hosts the saved test battle, B joins as the opponent. During deployment
# B's player puts some of his squads somewhere else (the game digs a squad in again where it is put; the
# test makes that call); both press Start; then the trenches of the two games are compared.
#   test_trench.ps1 [-NoSync] [-Squads 3] [-Shift 18]
# -NoSync: each game digs for itself (MP_SYNC_TRENCH=0), to see what that gives.
param([switch]$NoSync, [int]$Squads = 3, [double]$Shift = 18, [switch]$KeepRunning)
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$mp = "$script:Game\_mp_research"
& cmd.exe /c "`"$mp\enable_multiplayer.bat`"" | Out-Null
Copy-Item "$mp\test_battle_image.cfgpack" "$script:Game\data\common\shared\config\tacgame_image.cfgpack" -Force
$started = Get-Date
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$half = [int]($screen.Width / 2)
$vars = @{ MP_LOG_TRENCH = "1" }
if ($NoSync) { $vars["MP_SYNC_TRENCH"] = "0" }
$a = Start-Game "root\programs\tacgame.progpack, data\common\shared\config\tacgame_image.cfgpack, users\profiles, cloud, *:*, 0, 1" (@{
    MP_ROLE = "host"; MP_WINDOW = "0,0,$half,$($screen.Height)" } + $vars)
$alog = Get-MpLog $a "tacgame"
Check "A: hosting the battle" ([bool](Wait-Log $alog 'net: hosting battle on' 90))
$env:MP_TEST = "1"; $env:MP_WINDOW = "$half,0,$half,$($screen.Height)"
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k]) }
$join = & "$script:Game\mp_join.exe" 127.0.0.1 germans | Out-String
$env:MP_TEST = $null; $env:MP_WINDOW = $null
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $null) }
$b = $null
for ($i = 0; $i -lt 40 -and -not $b; $i++) {
    $b = Get-CimInstance Win32_Process -Filter "Name='starter_x64.exe'" | Where-Object { $_.CommandLine -match 'tacgame\.progpack' -and $_.ProcessId -ne $a.Id } |
         Select-Object -First 1 | ForEach-Object { Get-Process -Id $_.ProcessId }
    if (-not $b) { Start-Sleep 1 }
}
Check "B: battle started" ([bool]$b)
if (-not $b) { Stop-Game $a; Show-Results; exit 1 }
$blog = Get-MpLog $b "tacgame"
Check "B: connected to A" ([bool](Wait-Log $blog 'net: connected to host' 90))
Check "Start button is shared" ([bool](Wait-Log $alog 'Start button shared' 30) -and [bool](Wait-Log $blog 'Start button shared' 30))
Start-Sleep 14
"--- B's player puts $Squads of his squads somewhere else during deployment (dug in again $Shift m away)"
Send-Command $b "dig $Squads $Shift"
$line = Wait-Log $blog 'test: \d+ squads of this player dug in again' 15
Check "B: squads dug in again at their new places" ($line -match 'test: [1-9]') $(if ($line) { $line.Substring(22) })
Start-Sleep 6
Send-Ready $a; Send-Ready $b
Check "initial orders reached" ([bool](Wait-Log $alog 'phase: initial orders' 30) -and [bool](Wait-Log $blog 'phase: initial orders' 30))
Start-Sleep 3
Send-Ready $a; Send-Ready $b
Check "battle reached" ([bool](Wait-Log $alog 'phase: battle$' 30) -and [bool](Wait-Log $blog 'phase: battle$' 30))
Start-Sleep 12
"--- trenches"
$diff = & python "$PSScriptRoot\trench_diff.py" $a.Id $b.Id
$diff
if (-not $NoSync) { Check "both games have the same trenches" (($diff | Out-String) -match 'the same in both games') }
foreach ($log in @($alog, $blog)) { (Select-String -Path $log -Pattern 'trench: \d+ squads').Line | Select-Object -Last 1 | ForEach-Object { $_.Substring(22) } }
foreach ($g in @($a, $b)) { $g.Refresh() }
Check "both games still running" (-not $a.HasExited -and -not $b.HasExited)
$dump = Get-Item "$script:Game\out\minidump.dmp" -ErrorAction SilentlyContinue
Check "no crash dump written" (-not ($dump -and $dump.LastWriteTime -gt $started))
if (-not $KeepRunning) { Stop-Game $b; Stop-Game $a }
Show-Results
