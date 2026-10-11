# End-to-end test through the in-game menus, on one PC:
#   game A: main menu -> MULTIPLAYER -> HOST A SKIRMISH -> lobby -> SET UP THE BATTLE (stock skirmish screen)
#   game B: main menu -> MULTIPLAYER -> JOIN A GAME -> 127.0.0.1 -> CONNECT -> lobby -> [SWAP SIDES] -> READY
#   game A: starts the battle on the stock screen
# then checks that both are in the same battle on opposite sides, that both have to press Start, and that
# B's units are where A's are.
#   test_full.ps1 [-NoBuild] [-KeepRunning] [-Swap] [-Fight 60]
param([switch]$NoBuild, [switch]$KeepRunning, [switch]$Swap, [int]$Fight = 60)
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$mp = "$script:Game\_mp_research"
if (-not $NoBuild) {
    $build = & cmd.exe /c "`"$mp\basic_network\build.bat`"" 2>$null | Out-String
    if ($build -match 'error [A-Z]+\d+') { $build; exit 1 }
    & python "$PSScriptRoot\make_progpack.py" | Out-Null
}
& cmd.exe /c "`"$mp\enable_multiplayer.bat`"" | Out-Null
$EntryX = 0.5; $EntryY = 0.129
$X = 0.56; $B1 = 0.331; $B2 = 0.424; $B3 = 0.517
$R1 = 0.591; $R2 = 0.683; $SL = 0.44; $SR = 0.575

# the battle is a separate process started by the menu program; with swapped sides the host's battle is
# started a second time, so wait for the one that really hosts
function Wait-Battle([int[]]$except, [string]$pattern, [int]$timeout = 90) {
    $deadline = (Get-Date).AddSeconds($timeout)
    while ((Get-Date) -lt $deadline) {
        foreach ($c in Get-CimInstance Win32_Process -Filter "Name='starter_x64.exe'") {
            if ($c.CommandLine -notmatch 'tacgame\.progpack' -or $except -contains $c.ProcessId) { continue }
            $l = "$script:Game\out\basic_network_tacgame_$($c.ProcessId).log"
            if ((Test-Path $l) -and (Select-String -Path $l -Pattern $pattern -Quiet)) { return Get-Process -Id $c.ProcessId }
        }
        Start-Sleep 1
    }
    $null
}

# ---- game A hosts a skirmish and goes to the stock setup screen
$a = Start-Game
$alog = Get-MpLog $a "opgame"
[void](Wait-Log $alog 'render pipeline reached' 60); Start-Sleep 6
[void](Click-Game $a $EntryX $EntryY)
Check "A: MULTIPLAYER entry opens the panel" ([bool](Wait-Log $alog 'multiplayer menu opened' 8))
Start-Sleep -Milliseconds 800
[void](Click-Game $a $X $B1)
Check "A: hosting a skirmish, in the lobby" ([bool](Wait-Log $alog 'hosting skirmish chosen' 10))
Start-Sleep 1
[void](Click-Game $a $X $R2)            # SET UP THE BATTLE
Check "A: stock skirmish screen open" ([bool](Wait-Log $alog 'opened the stock skirmish screen' 10))

# ---- game B joins the lobby
$b = Start-Game
$blog = Get-MpLog $b "opgame"
[void](Wait-Log $blog 'render pipeline reached' 60); Start-Sleep 6
[void](Click-Game $b $EntryX $EntryY)
Check "B: MULTIPLAYER entry opens the panel" ([bool](Wait-Log $blog 'multiplayer menu opened' 8))
Start-Sleep -Milliseconds 800
[void](Click-Game $b $X 0.609)          # JOIN A GAME
Start-Sleep -Milliseconds 600
Press-Key $b 0x2E
Type-Text $b "127.0.0.1"
Start-Sleep -Milliseconds 400
[void](Click-Game $b $X $B2)            # CONNECT
Check "B: connecting" ([bool](Wait-Log $blog "connecting to '127" 8))
Check "A: sees B in the lobby" ([bool](Wait-Log $alog 'net: player connected' 15))
Start-Sleep 2
if ($Swap) {
    [void](Click-Game $b $SR $R1)       # SWAP SIDES, asked for by the joining player
    Check "A: sides swapped at B's request" ([bool](Wait-Log $alog 'lobby: sides swapped - the host commands the enemy' 8))
    Start-Sleep 1
}
[void](Save-Shot $b "full_b_lobby")
[void](Click-Game $b $SL $R1)           # READY
Check "B: ready" ([bool](Wait-Log $blog 'lobby: ready' 8))
Start-Sleep 2

# ---- A starts the battle on the stock screen
[void](Click-Game $a 0.988 0.979)       # the stock screen's "start battle" arrow, bottom right
$abattle = Wait-Battle @() 'net: hosting battle on' 120
Check "A: battle started and hosted" ([bool]$abattle)
if (-not $abattle) { [void](Save-Shot $a "full_a_no_battle"); Stop-Game $a; Stop-Game $b; Show-Results; exit 1 }
$ablog = Get-MpLog $abattle "tacgame"
if ($Swap) { Check "A: its battle was restarted from the turned description" ([bool](Select-String -Path "$script:Game\out\basic_network_tacgame_*.log" -Pattern 'restarting the battle from the turned description' -Quiet)) }

Check "B: battle downloaded from A" ([bool](Wait-Log $blog 'battle received' 60))
$bbattle = Wait-Battle @($abattle.Id) 'net: joining' 60
Check "B: battle started" ([bool]$bbattle)
if ($bbattle) {
    $bblog = Get-MpLog $bbattle "tacgame"
    Check "B: connected to A inside the battle" ([bool](Wait-Log $bblog 'net: connected to host' 90))
    $aside = if ((Wait-Log $ablog 'this player commands (\w+)' 5) -match 'commands (\w+)') { $Matches[1] }
    $bside = if ((Wait-Log $bblog 'this player commands (\w+)' 5) -match 'commands (\w+)') { $Matches[1] }
    Check "the players command different forces" ($aside -and $bside -and $aside -ne $bside) "A: $aside, B: $bside"
    Check "B: receives unit states" ([bool](Wait-Log $bblog 'net: 5 s: sent [0-9]+ and applied [1-9]' 120))
    [void](Save-Shot $abattle "full_a_deployment")

    # ---- both players press Start: A with a real click on the toolbar arrow, B through the test command
    Start-Sleep 3
    [void](Click-Game $abattle 0.493 0.021)
    $clicked = [bool](Wait-Log $ablog 'ready: this player has finished deployment' 8)
    Check "A: a click on the Start arrow marks A as ready" $clicked
    if (-not $clicked) { Send-Ready $abattle }
    Start-Sleep 3
    Check "deployment waits for B" (-not (Select-String -Path $ablog -Pattern 'phase: initial orders' -Quiet))
    Send-Ready $bbattle
    Check "A: deployment ended (initial orders)" ([bool](Wait-Log $ablog 'phase: initial orders' 20))
    Check "B: deployment ended (initial orders)" ([bool](Wait-Log $bblog 'phase: initial orders' 20))
    Start-Sleep 3
    Send-Ready $bbattle
    Start-Sleep 2
    [void](Click-Game $abattle 0.493 0.021)
    if (-not (Wait-Log $ablog 'ready: this player has finished the initial orders' 8)) { Send-Ready $abattle }
    Check "A: battle started" ([bool](Wait-Log $ablog 'phase: battle$' 20))
    Check "B: battle started" ([bool](Wait-Log $bblog 'phase: battle$' 20))
    Start-Sleep $Fight
    "--- unit positions, A against B"
    $diff = & python "$PSScriptRoot\live_diff.py" $abattle.Id $bbattle.Id 3
    $diff
    $last = ($diff | Where-Object { $_ -match 'i_human' } | Select-Object -Last 1)
    $median = if ($last -match 'median ([\d.]+) m') { [double]$Matches[1] } else { 999 }
    Check "B's soldiers are where A's are (median under 1 m)" ($median -lt 1.0) "median $median m"
    [void](Save-Shot $abattle "full_a_battle")
    [void](Save-Shot $bbattle "full_b_battle")
    (Select-String -Path $bblog -Pattern 'state: 5 s').Line | Select-Object -Last 1 | ForEach-Object { $_.Substring(22) }
}
if (-not $KeepRunning) { foreach ($g in @($bbattle, $abattle, $b, $a)) { Stop-Game $g } }
Show-Results
