# Operation test, one hosting round: a game hosts an operation through the menu and lobby and opens the
# operation at the bottom of the Operations list (in this copy: the saved Peleshi operation, which resumes
# at its pending battle); a second game joins through its menu and lobby; both press Start twice; after the
# battle has begun the host leaves it through the game's own "to the operation" exit.
#   test_operation.ps1 [-Extra @{ MP_SYNC_STATE = "0" }] [-Fight 30]
# Leaves the host's menu program (the operation after the battle) running and prints its process id.
param([hashtable]$Extra = @{}, [int]$Fight = 30, [double]$OpX = 0.584, [double]$OpY = 0.806, [double]$ZoneX = 0.375, [double]$ZoneY = 0.685)
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$mp = "$script:Game\_mp_research"
& cmd.exe /c "`"$mp\enable_multiplayer.bat`"" | Out-Null
$started = Get-Date
$dump = Get-Item "$script:Game\out\minidump.dmp" -ErrorAction SilentlyContinue
function Crashed { $d = Get-Item "$script:Game\out\minidump.dmp" -ErrorAction SilentlyContinue; [bool]($d -and (-not $dump -or $d.LastWriteTime -gt $started)) }
function Battles([int[]]$except) {
    Get-CimInstance Win32_Process -Filter "Name='starter_x64.exe'" |
        Where-Object { $_.CommandLine -match 'tacgame\.progpack' -and $except -notcontains $_.ProcessId } | ForEach-Object { Get-Process -Id $_.ProcessId }
}
function Menus([int[]]$except) {
    Get-CimInstance Win32_Process -Filter "Name='starter_x64.exe'" |
        Where-Object { $_.CommandLine -notmatch 'tacgame\.progpack' -and $except -notcontains $_.ProcessId } | ForEach-Object { Get-Process -Id $_.ProcessId }
}

# ---- player 1: host an operation, open it
$a = Start-Game "" $Extra
$alog = Get-MpLog $a "opgame"
[void](Wait-Log $alog 'render pipeline reached' 60); Start-Sleep 6
[void](Click-Game $a 0.5 0.129); Start-Sleep -Milliseconds 800
[void](Click-Game $a 0.56 0.517)          # HOST AN OPERATION
Check "P1: hosting an operation, in the lobby" ([bool](Wait-Log $alog 'hosting operation chosen' 8))
Start-Sleep 1
[void](Click-Game $a 0.56 0.683)          # CHOOSE THE OPERATION
Check "P1: Operations screen open" ([bool](Wait-Log $alog 'opened the stock campaign screen' 8))
[void][GtAuto]::SetCursorPos(40, 700); Start-Sleep 3

# ---- player 2: join the lobby, ready
$b = Start-Game "" $Extra
$blog = Get-MpLog $b "opgame"
[void](Wait-Log $blog 'render pipeline reached' 60); Start-Sleep 6
[void](Click-Game $b 0.5 0.129); Start-Sleep -Milliseconds 800
[void](Click-Game $b 0.56 0.609)          # JOIN A GAME
Start-Sleep -Milliseconds 600; Press-Key $b 0x2E; Type-Text $b "127.0.0.1"; Start-Sleep -Milliseconds 400
[void](Click-Game $b 0.56 0.424)          # CONNECT
Check "P2: in P1's lobby" ([bool](Wait-Log $alog 'net: player connected' 20))
Start-Sleep 2
[void](Click-Game $b 0.56 0.591)          # READY
Check "P2: ready" ([bool](Wait-Log $blog 'lobby: ready' 8))

# ---- player 1 opens the operation: its picture in the Operations list
[void](Click-Game $a $OpX $OpY); [void][GtAuto]::SetCursorPos(600, 900); Start-Sleep 10
# an operation that is between battles opens on the operational map; its pending battle is the marked zone
$at = Battles @() | Select-Object -First 1
if (-not $at) { $a.Refresh(); if (-not $a.HasExited) { [void](Click-Game $a $ZoneX $ZoneY); [void][GtAuto]::SetCursorPos(600, 900) } }
for ($i = 0; $i -lt 60 -and -not $at; $i++) { $at = Battles @() | Select-Object -First 1; if (-not $at) { Start-Sleep 1 } }
Check "P1: the operation started a battle" ([bool]$at)
if (-not $at) { [void](Save-Shot $a "operation_not_started"); Show-Results; "P1 menu pid $($a.Id), P2 menu pid $($b.Id)"; exit 1 }
$atlog = Get-MpLog $at "tacgame"
Check "P1: battle hosted" ([bool](Wait-Log $atlog 'net: hosting battle on' 180))
$line = Wait-Log $atlog 'this player commands' 5; if ($line) { "   P1 " + $line.Substring(22) }
Check "P1: operation saved before the battle" ([bool](Wait-Log $atlog 'operation saved before the battle' 10))

Check "P2: battle downloaded" ([bool](Wait-Log $blog 'battle received' 120))
$bt = $null; for ($i = 0; $i -lt 60 -and -not $bt; $i++) { $bt = Battles @($at.Id) | Select-Object -First 1; if (-not $bt) { Start-Sleep 1 } }
Check "P2: battle started" ([bool]$bt)
if (-not $bt) { Show-Results; exit 1 }
$btlog = Get-MpLog $bt "tacgame"
$line = Wait-Log $btlog 'this player commands' 180; if ($line) { "   P2 " + $line.Substring(22) }
Check "P2: connected to P1 inside the battle" ([bool](Wait-Log $btlog 'net: connected to host' 120))
Check "P1: sees P2 inside the battle" ([bool](Wait-Log $atlog 'net: versus' 30))
Start-Sleep 20

# ---- both press Start: deployment, then the initial orders
Send-Ready $at; Send-Ready $bt
Check "deployment ended in both games" ([bool](Wait-Log $atlog 'phase: initial orders' 40) -and [bool](Wait-Log $btlog 'phase: initial orders' 40))
Start-Sleep 4
Send-Ready $at; Send-Ready $bt
Check "the battle began in both games" ([bool](Wait-Log $atlog 'phase: battle$' 40) -and [bool](Wait-Log $btlog 'phase: battle$' 40))
Start-Sleep $Fight
$at.Refresh(); $bt.Refresh()
Check "P1 still running $Fight s into the battle" (-not $at.HasExited)
Check "P2 still running $Fight s into the battle" (-not $bt.HasExited)
Check "no crash dump so far" (-not (Crashed))
"--- trenches"; & python "$PSScriptRoot\trench_diff.py" $at.Id $bt.Id
if (Crashed) { & python "$PSScriptRoot\crash_info.py" | Select-Object -First 8 }
foreach ($pair in @(@("P1", $atlog), @("P2", $btlog))) {
    $loaded = 0; $refused = 0
    foreach ($m in Select-String -Path $pair[1] -Pattern 'loaded (\d+) \(.*?(\d+) not loaded') { $loaded += [int]$m.Matches[0].Groups[1].Value; $refused += [int]$m.Matches[0].Groups[2].Value }
    "   $($pair[0]): unit states loaded $loaded, refused $refused"
}

# ---- player 1 leaves the battle
if (-not $at.HasExited) {
    Send-Resign $at
    Check "P1: left the battle through the game's exit" ([bool](Wait-Log $atlog 'leaving the battle through' 10))
    for ($i = 0; $i -lt 40 -and -not $at.HasExited; $i++) { Start-Sleep 1; $at.Refresh() }
    Check "P1: battle program ended" $at.HasExited
    $am = $null; for ($i = 0; $i -lt 40 -and -not $am; $i++) { $am = Menus @($b.Id) | Select-Object -First 1; if (-not $am) { Start-Sleep 1 } }
    Check "P1: back in the menu program (the operation)" ([bool]$am)
    Check "P2: told that the host left" ([bool](Wait-Log $btlog 'has left|game closed|connection lost' 20))
    for ($i = 0; $i -lt 30 -and -not $bt.HasExited; $i++) { Start-Sleep 1; $bt.Refresh() }
    Check "P2: battle program ended" $bt.HasExited
    Start-Sleep 8
    $menus = @(Menus @())
    Check "both players are back in menu programs" ($menus.Count -ge 2) "$($menus.Count) running"
    Check "no crash dump" (-not (Crashed))
    if ($am) { Start-Sleep 6; [void](Save-Shot $am "operation_p1_after_battle") }
    "menu programs running: " + (($menus | ForEach-Object { $_.Id }) -join ', ')
}
Show-Results
