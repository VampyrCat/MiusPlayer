# Versus test on one PC: game A hosts the saved test battle (its player commands the Soviets), game B joins
# through mp_join.exe asking for a side. Checks who commands what, that each game owns its own units, and
# compares unit existence, status and position between the two games during the fight.
#   test_versus.ps1 [-Side germans|soviets] [-Warmup 60] [-Samples 6] [-Every 20] [-KeepRunning] [-Details]
param([ValidateSet("germans", "soviets")][string]$Side = "germans", [int]$Warmup = 60, [int]$Samples = 6, [int]$Every = 20,
      [switch]$KeepRunning, [switch]$Details, [hashtable]$Extra = @{})
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$mp = "$script:Game\_mp_research"
& cmd.exe /c "`"$mp\enable_multiplayer.bat`"" | Out-Null
Get-ChildItem "$script:Game\out" -Filter "basic_network*.log" | ForEach-Object { [IO.File]::Delete($_.FullName) }
Get-ChildItem "$script:Game\out" -Filter "mp_trace*.txt" | ForEach-Object { [IO.File]::Delete($_.FullName) }
$image = "$script:Game\data\common\shared\config\tacgame_image.cfgpack"
Copy-Item "$mp\test_battle_image.cfgpack" $image -Force
$started = Get-Date
$dump = Get-Item "$script:Game\out\minidump.dmp" -ErrorAction SilentlyContinue
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$half = [int]($screen.Width / 2)

function Find-Battle([int[]]$except) {
    Get-CimInstance Win32_Process -Filter "Name='starter_x64.exe'" |
        Where-Object { $_.CommandLine -match 'tacgame\.progpack' -and $except -notcontains $_.ProcessId } |
        Select-Object -First 1 | ForEach-Object { Get-Process -Id $_.ProcessId }
}

# ---- A: host
$a = Start-Game "root\programs\tacgame.progpack, data\common\shared\config\tacgame_image.cfgpack, users\profiles, cloud, *:*, 0, 1" (@{
    MP_ROLE = "host"; MP_WINDOW = "0,0,$half,$($screen.Height)" } + $Extra)
$alog = Get-MpLog $a "tacgame"
Check "A: hosting the battle" ([bool](Wait-Log $alog 'net: hosting battle on' 90))
$line = Wait-Log $alog 'this player commands' 5
Check "A: its player commands the Soviets" ($line -match 'commands Soviets') $(if ($line) { $line.Substring(22) })

# ---- B: joins with mp_join.exe, which downloads the battle and starts the game
$env:MP_TEST = "1"; $env:MP_WINDOW = "$half,0,$half,$($screen.Height)"
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }
$join = & "$script:Game\mp_join.exe" 127.0.0.1 $Side | Out-String
$env:MP_TEST = $null; $env:MP_WINDOW = $null
foreach ($k in $Extra.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
Check "B: mp_join received the battle" ($join -match 'Battle received') ($join.Trim() -replace '\s+', ' ')
$b = $null
for ($i = 0; $i -lt 30 -and -not $b; $i++) { $b = Find-Battle @($a.Id); if (-not $b) { Start-Sleep 1 } }
Check "B: battle started" ([bool]$b)
if (-not $b) { Stop-Game $a; Show-Results; exit 1 }
$blog = Get-MpLog $b "tacgame"
$versus = $Side -eq "germans"
$want = if ($versus) { "Germans" } else { "Soviets" }
$line = Wait-Log $blog 'this player commands' 90
Check "B: its player commands the $want" ($line -match "commands $want") $(if ($line) { $line.Substring(22) })
Check "B: connected to A" ([bool](Wait-Log $blog 'net: connected to host' 90))
if ($versus) {
    Check "A: versus, AI commander off" ([bool](Wait-Log $alog 'AI commander switched off' 20))
    Check "B: versus, AI commander off" ([bool](Wait-Log $blog 'AI commander switched off' 20))
} else {
    Check "A: same side, host owns every unit" (-not (Select-String -Path $alog -Pattern 'versus: each game' -Quiet))
}
# ---- both players have to press Start to end deployment, and again to end the initial orders
Check "Start button is shared" ([bool](Wait-Log $alog 'Start button shared' 30) -and [bool](Wait-Log $blog 'Start button shared' 30))
Start-Sleep 3
Send-Ready $a
Check "A pressed Start: A waits for B" ([bool](Wait-Log $alog 'ready: this player has finished deployment, waiting' 10))
Start-Sleep 5
Check "deployment does not end on A's press alone" (-not (Select-String -Path $alog, $blog -Pattern 'phase: initial orders' -Quiet))
Send-Ready $b
Check "B pressed Start: A reaches the initial orders" ([bool](Wait-Log $alog 'phase: initial orders' 30))
Check "B pressed Start: B reaches the initial orders" ([bool](Wait-Log $blog 'phase: initial orders' 30))
Start-Sleep 3
Send-Ready $b
Check "B pressed Start: B waits for A" ([bool](Wait-Log $blog 'ready: this player has finished the initial orders, waiting' 10))
Start-Sleep 5
Check "the battle does not start on B's press alone" (-not (Select-String -Path $alog, $blog -Pattern 'phase: battle$' -Quiet))
Send-Ready $a
Check "A: battle phase reached" ([bool](Wait-Log $alog 'phase: battle$' 30))
Check "B: battle phase reached" ([bool](Wait-Log $blog 'phase: battle$' 30))
Start-Sleep $Warmup
[void](Save-Shot $a "versus_a")
[void](Save-Shot $b "versus_b")

"--- combat comparison, A against B"
$diff = & python "$PSScriptRoot\combat_diff.py" $a.Id $b.Id $Samples $Every
$diff | Where-Object { $_ -match 'sample|disagree|worst|Traceback|Error' -or ($Details -and $_ -match '^      (i_|far)') }
$worst = if (($diff | Out-String) -match 'worst sample: (\d+)') { [int]$Matches[1] } else { 999 }
Check "units agree between the games (at most 5 disagreeing in the worst sample)" ($worst -le 5) "worst sample: $worst"
if ($versus) {
    $view = & python "$PSScriptRoot\view_check.py" $a.Id $b.Id
    $view
    Check "each game counts only its own player's units as its own (fog of war)" (($view | Out-String) -match 'view: ok')
}
foreach ($g in @($a, $b)) { $g.Refresh() }
Check "A: still running" (-not $a.HasExited)
Check "B: still running" (-not $b.HasExited)
$newDump = Get-Item "$script:Game\out\minidump.dmp" -ErrorAction SilentlyContinue
$crashed = $newDump -and (-not $dump -or $newDump.LastWriteTime -gt $started)
Check "no crash dump written" (-not $crashed)
if ($crashed) { & python "$PSScriptRoot\crash_info.py" | Select-Object -First 12 }
if ($versus) {
    $asent = (Select-String -Path $alog -Pattern 'net: 5 s: sent ([0-9]+) and applied ([0-9]+)' | Select-Object -Last 6)
    $bsent = (Select-String -Path $blog -Pattern 'net: 5 s: sent ([0-9]+) and applied ([0-9]+)' | Select-Object -Last 6)
    $bout = ($bsent | ForEach-Object { [int]$_.Matches[0].Groups[1].Value } | Measure-Object -Sum).Sum
    $ain = ($asent | ForEach-Object { [int]$_.Matches[0].Groups[2].Value } | Measure-Object -Sum).Sum
    Check "B sends its own units, A applies them" ($bout -gt 0 -and $ain -gt 0) "B sent $bout, A applied $ain transforms in the last 30 s"
}
foreach ($pair in @(@("host", $alog), @("join", $blog))) {
    "--- $($pair[0])"
    (Select-String -Path $pair[1] -Pattern 'state: 5 s|net: 5 s').Line | Select-Object -Last 4 | ForEach-Object { $_.Substring(22) }
}
if (-not $KeepRunning) { Stop-Game $b; Stop-Game $a }
Show-Results
