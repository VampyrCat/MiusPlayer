# Operational map test, two games on one PC. Needs an operation save to work on:
#   test_opmap.ps1 -Save <file.opsave> -Operation 1942_07_Oskol:07_02_odintsov_rus -Turns 6 [-KeepRunning]
#                  [-Select x,y] [-Targets x,y,x,y,...] [-Zone x,y]
# The save is copied into the profile as that operation's save. Then:
#   P1 hosts the operation, directly on its map; P2 joins the lobby and is ready
#   P1's game shares the map by itself      -> P2's game opens the operation from the other side
#   P2 orders one of its battle groups to move (real mouse clicks; the order is counted in P2's own save)
#   P2 presses next turn                    -> nothing ends: P1 has not
#   P1 presses next turn                    -> P2's moves are collected and merged, P1's map reloads and the
#                                              turn is carried out
#   then either the next turn's map reaches P2, whose game reloads it and stays on the map, or (with -Zone,
#   the place of the pending battle on P1's map) P1 starts the battle and P2's game enters it from the map.
param([Parameter(Mandatory = $true)][string]$Save, [Parameter(Mandatory = $true)][string]$Operation, [int]$Turns = 6,
      [switch]$KeepRunning, [double[]]$Select = @(0.312, 0.475), [double[]]$Targets = @(0.355, 0.475, 0.335, 0.44, 0.335, 0.51, 0.29, 0.475),
      [double[]]$Zone = @(), [int]$Fight = 25, [switch]$ReloadOnly, [double[]]$Menu = @(), [hashtable]$Extra = @{}, [switch]$StopAtMap)
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$mp = "$script:Game\_mp_research"
& cmd.exe /c "`"$mp\enable_multiplayer.bat`"" | Out-Null
$file = ($Operation -replace ':', '_') + ".opsave"
$prof = "$script:Game\users\profiles\cloud"
Copy-Item $Save "$prof\$file" -Force
$started = Get-Date
function Crashed { $d = Get-Item "$script:Game\out\minidump.dmp" -ErrorAction SilentlyContinue; [bool]($d -and $d.LastWriteTime -gt $started) }
function Programs([string]$kind, [int[]]$except) {
    Get-CimInstance Win32_Process -Filter "Name='starter_x64.exe'" | Where-Object {
        $tac = $_.CommandLine -match 'tacgame\.progpack'; $op = $_.CommandLine -match 'opgame\.progpack'
        $except -notcontains $_.ProcessId -and (($kind -eq 'battle' -and $tac) -or ($kind -eq 'map' -and $op) -or ($kind -eq 'menu' -and -not $tac -and -not $op))
    } | ForEach-Object { Get-Process -Id $_.ProcessId }
}
function Wait-Program([string]$kind, [int[]]$except, [int]$timeout = 60) {
    for ($i = 0; $i -lt $timeout; $i++) { $p = Programs $kind $except | Select-Object -First 1; if ($p) { return $p }; Start-Sleep 1 }
    $null
}
function Orders($proc, [string]$log) {
    $before = @(Select-String -Path $log -Pattern 'test: operation saved').Count
    Send-Command $proc "savemap"
    for ($i = 0; $i -lt 20; $i++) {
        $m = @(Select-String -Path $log -Pattern 'test: operation saved.* (\d+) orders')
        if ($m.Count -gt $before) { return [int]$m[-1].Matches[0].Groups[1].Value }
        Start-Sleep -Milliseconds 500
    }
    -1
}

if ($Menu.Count -ge 2) {
    # ---- P1: host through the menus, the way a player does: MULTIPLAYER, HOST AN OPERATION, CHOOSE THE
    #      OPERATION, then the picture of the (saved) operation at -Menu x,y on the Operations screen
    $p1 = Start-Game "" $Extra
    $p1log = Get-MpLog $p1 "opgame"
    [void](Wait-Log $p1log 'render pipeline reached' 60); Start-Sleep 6
    [void](Click-Game $p1 0.5 0.129); Start-Sleep -Milliseconds 800
    [void](Click-Game $p1 0.56 0.517)
    Check "P1: hosting an operation, in the lobby" ([bool](Wait-Log $p1log 'hosting operation chosen' 8))
    Start-Sleep 1
    [void](Click-Game $p1 0.56 0.683)
    Check "P1: Operations screen open" ([bool](Wait-Log $p1log 'opened the stock campaign screen' 8))
    [void][GtAuto]::SetCursorPos(40, 700); Start-Sleep 3
    [void](Click-Game $p1 $Menu[0] $Menu[1]); [void][GtAuto]::SetCursorPos(3000, 1300); Start-Sleep 20
    Check "P1: the operation is open (its save was read)" ((Orders $p1 $p1log) -ge 0)
} else {
    # ---- P1: host, straight on the operation's map
    $p1 = Start-Game "root\programs\opgame.progpack, cloud, $Operation, 0, $Turns" (@{ MP_ROLE = "host"; MP_HOST_MODE = "operation" } + $Extra)
    $p1log = Get-MpLog $p1 "opgame"
    Check "P1: hosting, on the operational map" ([bool](Wait-Log $p1log 'net: hosting on' 90))
    Start-Sleep 10
}

# ---- P2: lobby, ready
$p2 = Start-Game "" $Extra
$p2log = Get-MpLog $p2 "opgame"
[void](Wait-Log $p2log 'render pipeline reached' 60); Start-Sleep 6
[void](Click-Game $p2 0.5 0.129); Start-Sleep -Milliseconds 800
[void](Click-Game $p2 0.56 0.609)
Start-Sleep -Milliseconds 600; Press-Key $p2 0x2E; Type-Text $p2 "127.0.0.1"; Start-Sleep -Milliseconds 400
[void](Click-Game $p2 0.56 0.424)
Check "P2: in P1's lobby" ([bool](Wait-Log $p1log 'net: player connected' 20))
Start-Sleep 2
[void](Click-Game $p2 0.56 0.591)
Check "P2: ready" ([bool](Wait-Log $p2log 'lobby: ready' 8))

# ---- the map is shared without anybody asking
$line = Wait-Log $p1log 'map: shared with' 30
Check "P1: map shared as soon as P2 is ready" ([bool]$line) $(if ($line) { $line.Substring(22) })
Check "P2: map received" ([bool](Wait-Log $p2log 'operational map received' 60))
$p2map = Wait-Program 'map' @($p1.Id) 60
Check "P2: operation program started on its copy" ([bool]$p2map)
if (-not $p2map) { Show-Results; exit 1 }
$cmd = (Get-CimInstance Win32_Process -Filter "ProcessId=$($p2map.Id)").CommandLine
Check "P2: plays the operation from its own side (the counterpart operation)" ($cmd -notmatch [regex]::Escape($Operation)) ($cmd.Substring($cmd.IndexOf('opgame')))
$p2maplog = Get-MpLog $p2map "opgame"
Check "P2: its side of the map" ([bool](Wait-Log $p2maplog "this player's side of the operational map" 90))
Check "P2: connected to P1 from the map" ([bool](Wait-Log $p2maplog 'net: connected to host' 60))
Start-Sleep 12
[void][GtAuto]::SetCursorPos(3000, 1300)
[void](Save-Shot $p1 "opmap_p1_own")
[void](Save-Shot $p2map "opmap_p2_own")
if ($StopAtMap) { Show-Results; "P1 $($p1.Id), P2 map program $($p2map.Id)"; exit 0 }

# ---- P2 orders a battle group to move: select it, click one of the squares next to it; the order is
#      looked for in P2's own save, and other squares are tried until one takes
$orders = Orders $p2map $p2maplog
"P2 orders before: $orders"
for ($t = 0; $t + 1 -lt $Targets.Count -and $orders -lt 1; $t += 2) {
    [void](Click-Game $p2map $Select[0] $Select[1]); Start-Sleep 3
    [void](Click-Game $p2map $Targets[$t] $Targets[$t + 1]); Start-Sleep 3
    $orders = Orders $p2map $p2maplog
    "P2 orders after clicking $($Targets[$t]),$($Targets[$t + 1]): $orders"
}
[void][GtAuto]::SetCursorPos(2600, 1300); Start-Sleep 1
[void](Save-Shot $p2map "opmap_p2_ordered")
Check "P2: gave an order on its map (counted in its own save)" ($orders -ge 1) "$orders orders"

if ($ReloadOnly) {
    # the host's game declares the operation to have moved on: P2's copy is outdated, a new one is offered
    Send-Command $p1 "newmap"
    Check "P1: shares the map again" ([bool](Wait-Log $p1log 'operation is declared to have moved on' 10) -and @(Select-String -Path $p1log -Pattern 'map: shared with').Count -ge 1)
    Check "P2: the new map is offered" ([bool](Wait-Log $p2maplog 'offers the map of the next turn' 40))
    Check "P2: reloads its map" ([bool](Wait-Log $p2maplog 'the new turn has arrived' 60))
    $p2c = Wait-Program 'map' @($p1.Id, $p2map.Id) 60
    Check "P2: operation program running on the new map" ([bool]$p2c)
    if ($p2c) {
        $p2clog = Get-MpLog $p2c "opgame"
        Check "P2: its side of the map again" ([bool](Wait-Log $p2clog "this player's side of the operational map" 90))
        Check "P2: connected to P1 again" ([bool](Wait-Log $p2clog 'net: connected to host' 60))
        Start-Sleep 12
        Check "P2: does not reload a second time" (-not (Select-String -Path $p2clog -Pattern 'offers the map of the next turn' -Quiet))
        Check "P1: counts P2 as on the map again" ((Orders $p1 $p1log) -ge 0 -and -not (Select-String -Path $p1log -Pattern 'gone' -Quiet))
        [void](Save-Shot $p2c "opmap_p2_reloaded")
    }
    Check "P2: never left for the menu" (-not (Programs 'menu' @($p2.Id, $p1.Id)))
    Check "no crash" (-not (Crashed))
    if (-not $KeepRunning) { foreach ($g in @(Get-Process starter_x64 -ErrorAction SilentlyContinue)) { Stop-Game $g } }
    Show-Results
    exit 0
}

# ---- next turn takes both players
Send-Command $p2map "endturn"
Check "P2 pressed next turn: P2 waits for P1" ([bool](Wait-Log $p2maplog 'this player has ended the turn, waiting' 10))
Start-Sleep 6
Check "the turn does not end on P2's press alone" (-not (Select-String -Path $p1log -Pattern 'asking .* for his moves|map: ending the turn' -Quiet))
Send-Command $p1 "endturn"
Check "P1 pressed next turn: P2's moves are asked for" ([bool](Wait-Log $p1log 'asking .* for his moves' 15))
Check "P2: sends its moves" ([bool](Wait-Log $p2maplog "sending this player's moves" 20))
Check "P1: P2's map received" ([bool](Wait-Log $p1log "the other player's map received" 60))
$line = Wait-Log $p1log 'moves are in' 20
Check "P1: P2's moves merged into the operation" ([bool]$line) $(if ($line) { $line.Substring(22) })
$merged = if ($line -match '\((\d+) orders') { [int]$Matches[1] } else { -1 }
Check "P1: the merge carries P2's order" ($merged -ge 1) "$merged orders merged"
Check "P2: stays on its map while the turn is carried out" ([bool](Wait-Log $p2maplog 'waiting for the turn to be carried out' 20))
$p1b = Wait-Program 'map' @($p1.Id, $p2map.Id) 60
Check "P1: operation program restarted on the merged save" ([bool]$p1b)
if (-not $p1b) { Show-Results; exit 1 }
$p1blog = Get-MpLog $p1b "opgame"
Check "P1: turn ended" ([bool](Wait-Log $p1blog 'map: ending the turn' 90))
$line = Wait-Log $p1blog 'turn processed with both players' 40
Check "P1: command tags put back after the turn" ([bool]$line) $(if ($line) { $line.Substring(22) })
$p1c = Wait-Program 'map' @($p1.Id, $p2map.Id, $p1b.Id) 60
Check "P1: operation program reloaded for the next phase" ([bool]$p1c)
if (-not $p1c) { Show-Results; exit 1 }
$p1clog = Get-MpLog $p1c "opgame"
Check "P1: hosting again" ([bool](Wait-Log $p1clog 'net: hosting on' 90))
$tags = (& python "$PSScriptRoot\mirror_opsave.py" "$prof\$file" "$env:TEMP\mp_tags_probe.opsave" | Select-Object -First 1)
Check "P1's operation: only its own side is player-led again in the save" ($tags -match "USSR.: .{1,3}USER" -and $tags -notmatch "GER.: .{1,3}USER") $tags
$p2map.Refresh()
Check "P2: never left for the menu" (-not (Programs 'menu' @($p2.Id, $p1.Id)))
Start-Sleep 14
[void][GtAuto]::SetCursorPos(2600, 1300); [void](Save-Shot $p1c "opmap_p1_after_turn")
$shared = Select-String -Path $p1clog -Pattern 'map: shared with' -Quiet
if ($shared) {
    "the turn led to a new orders phase"
    Check "P2: the map of the next turn is offered" ([bool](Wait-Log $p2maplog 'offers the map of the next turn' 30))
    Check "P2: reloads its map" ([bool](Wait-Log $p2maplog 'the new turn has arrived' 60))
    $p2c = Wait-Program 'map' @($p1.Id, $p2map.Id, $p1b.Id, $p1c.Id) 60
    Check "P2: operation program running on the new turn" ([bool]$p2c)
    if ($p2c) {
        $p2clog = Get-MpLog $p2c "opgame"
        Check "P2: its side of the map again" ([bool](Wait-Log $p2clog "this player's side of the operational map" 90))
        Check "P2: connected to P1 again" ([bool](Wait-Log $p2clog 'net: connected to host' 60))
        Start-Sleep 10; [void](Save-Shot $p2c "opmap_p2_next_turn")
    }
} elseif ($Zone.Count -ge 2) {
    "the turn led to a battle; P1 starts it"
    [void](Click-Game $p1c $Zone[0] $Zone[1]); [void][GtAuto]::SetCursorPos(2600, 1300)
    $bt = Wait-Program 'battle' @() 90
    Check "P1: battle started from the map" ([bool]$bt)
    if ($bt) {
        $btlog = Get-MpLog $bt "tacgame"
        Check "P1: battle hosted" ([bool](Wait-Log $btlog 'net: hosting battle on' 180))
        Check "P2: its map program enters the battle" ([bool](Wait-Log $p2maplog 'the host has started a battle; entering it' 60))
        $bt2 = Wait-Program 'battle' @($bt.Id) 120
        Check "P2: in the battle" ([bool]$bt2)
        if ($bt2) {
            $bt2log = Get-MpLog $bt2 "tacgame"
            Check "P2: connected inside the battle" ([bool](Wait-Log $bt2log 'net: connected to host' 180))
            Start-Sleep 20
            Send-Ready $bt; Send-Ready $bt2
            Check "deployment ended in both games" ([bool](Wait-Log $btlog 'phase: initial orders' 40) -and [bool](Wait-Log $bt2log 'phase: initial orders' 40))
            Start-Sleep 4
            Send-Ready $bt; Send-Ready $bt2
            Check "the battle began in both games" ([bool](Wait-Log $btlog 'phase: battle$' 40) -and [bool](Wait-Log $bt2log 'phase: battle$' 40))
            Start-Sleep $Fight
            $bt.Refresh(); $bt2.Refresh()
            Check "both battle programs running $Fight s into the battle" (-not $bt.HasExited -and -not $bt2.HasExited)
            Send-Resign $bt
            for ($i = 0; $i -lt 40 -and -not $bt.HasExited; $i++) { Start-Sleep 1; $bt.Refresh() }
            Check "P1: left the battle for the operation" $bt.HasExited
            $p1d = Wait-Program 'map' @($p1.Id, $p2map.Id, $p1b.Id, $p1c.Id) 90
            Check "P1: back on the operational map" ([bool]$p1d)
            for ($i = 0; $i -lt 40 -and -not $bt2.HasExited; $i++) { Start-Sleep 1; $bt2.Refresh() }
            Check "P2: battle program ended" $bt2.HasExited
            $p2m = Wait-Program 'menu' @($p2.Id) 60
            Check "P2: back with the host by itself" ([bool]$p2m -and [bool](Wait-Log (Get-MpLog $p2m "opgame") 'rejoining the lobby' 90))
        }
    }
} else {
    "the turn led to a battle (see shots\opmap_p1_after_turn.png; pass -Zone x,y to have P1 start it)"
}
Check "no crash" (-not (Crashed))
if (-not $KeepRunning) { foreach ($g in @(Get-Process starter_x64 -ErrorAction SilentlyContinue)) { Stop-Game $g } }
Show-Results
