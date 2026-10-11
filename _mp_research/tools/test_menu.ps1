# Automated test of the multiplayer entry in the main menu, its panel and the lobby. Starts the game, walks
# through with real mouse clicks and key presses, and checks the result in the module's log and with a
# network probe. Frames are captured by the game itself into ..\shots (nothing else on screen is captured).
#   test_menu.ps1 [-NoBuild]
param([switch]$NoBuild)
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$mp = "$script:Game\_mp_research"
if (-not $NoBuild) {
    $build = & cmd.exe /c "`"$mp\basic_network\build.bat`"" 2>$null | Out-String
    if ($build -match 'error [A-Z]+\d+') { $build; exit 1 }
    & python "$PSScriptRoot\make_progpack.py" | Out-Null
}
& cmd.exe /c "`"$mp\enable_multiplayer.bat`"" | Out-Null

# Centres as fractions of the window. The panels are laid out for a 1080-line frame, so heights do not depend
# on the resolution; x is taken right of centre so a window overlapping the left side does not matter.
$EntryX = 0.5; $EntryY = 0.129                       # MULTIPLAYER in the main menu
$X = 0.56; $B1 = 0.331; $B2 = 0.424; $B3 = 0.517; $B4 = 0.609; $B5 = 0.702     # panel buttons, top to bottom
# panel: $B1 skirmish, $B2 mission, $B3 operation, $B4 join, $B5 back; join screen: $B2 connect, $B3 back
$R1 = 0.591; $R2 = 0.683; $R3 = 0.776                # lobby button rows
function Count-Log([string]$path, [string]$pattern) { @(Select-String -Path $path -Pattern $pattern).Count }

# ---- 1. the game starts as a normal single-player game with one more menu entry
$p = Start-Game
$log = Get-MpLog $p "opgame"
[void](Wait-Log $log 'render pipeline reached' 60)
Start-Sleep 6
[void](Save-Shot $p "menu_1_main")
Check "no panel is forced on the player at start" (-not (Select-String -Path $log -Pattern 'multiplayer menu opened' -Quiet))
Check "no network port is open before multiplayer is chosen" ((& python "$PSScriptRoot\probe_host.py") -eq 'none')
[void](Click-Game $p $EntryX $EntryY)
Check "the MULTIPLAYER entry of the main menu opens the panel" ([bool](Wait-Log $log 'multiplayer menu opened' 8))
Start-Sleep 1
[void](Save-Shot $p "menu_2_panel")

# ---- 2. join: typing an address, the lobby looks for the host, leaving
[void](Click-Game $p $X $B4)            # JOIN A GAME
Start-Sleep -Milliseconds 600
Press-Key $p 0x2E                       # Delete clears a remembered address
Type-Text $p "127.0.0.1"
Start-Sleep -Milliseconds 500
[void](Save-Shot $p "menu_3_join")
[void](Click-Game $p $X $B2)            # CONNECT
Check "CONNECT uses the typed address" ((Wait-Log $log "connecting to '" 8) -match "'127[.]0[.]0[.]1'")
Start-Sleep 2
[void](Save-Shot $p "menu_4_lobby_join")
[void](Click-Game $p $X $R3)            # LEAVE THE LOBBY
Start-Sleep -Milliseconds 600
[void](Click-Game $p $X $B3)            # BACK (join screen)
Start-Sleep -Milliseconds 600

# ---- 3. host a skirmish: lobby, swap, the stock screen, F9 back to the lobby, leave
[void](Click-Game $p $X $B1)            # HOST A SKIRMISH
Check "HOST A SKIRMISH starts hosting" ([bool](Wait-Log $log 'hosting skirmish chosen' 8))
Start-Sleep 2
[void](Save-Shot $p "menu_5_lobby_host")
Check "host answers on UDP 1943 from the lobby" ((& python "$PSScriptRoot\probe_host.py") -eq 'menus skirmish')
[void](Click-Game $p $X $R1)            # SWAP SIDES
Check "SWAP SIDES in the lobby" ([bool](Wait-Log $log 'lobby: sides swapped - the host commands the enemy' 8))
[void](Click-Game $p $X $R1)
Check "SWAP SIDES again puts it back" ([bool](Wait-Log $log 'lobby: sides swapped - the host commands the own' 8))
[void](Click-Game $p $X $R2)            # SET UP THE BATTLE
Check "SET UP THE BATTLE opens the stock skirmish screen" ([bool](Wait-Log $log 'opened the stock skirmish screen' 8))
Start-Sleep 2
[void](Save-Shot $p "menu_6_hosting_skirmish")
$before = Count-Log $log 'multiplayer menu opened'
Press-Key $p 0x78
Start-Sleep 2
Check "F9 brings the lobby back while hosting" ((Count-Log $log 'multiplayer menu opened') -gt $before)
[void](Click-Game $p $X $R3)            # LEAVE THE LOBBY
Check "LEAVE THE LOBBY stops hosting" ([bool](Wait-Log $log 'stopped hosting' 8))
Start-Sleep 9                            # the probe before this one still counts as a player for a few seconds
Check "no network port is open after leaving" ((& python "$PSScriptRoot\probe_host.py") -eq 'none')
Stop-Game $p
Start-Sleep 2

# ---- 4. host an operation
$p = Start-Game
$log = Get-MpLog $p "opgame"
[void](Wait-Log $log 'render pipeline reached' 60)
Start-Sleep 6
[void](Click-Game $p $EntryX $EntryY)
Start-Sleep -Milliseconds 800
[void](Click-Game $p $X $B3)            # HOST AN OPERATION
Check "HOST AN OPERATION starts hosting" ([bool](Wait-Log $log 'hosting operation chosen' 8))
Start-Sleep 3
[void](Save-Shot $p "menu_7_lobby_operation")
Check "host answers as an operation host" ((& python "$PSScriptRoot\probe_host.py") -eq 'menus operation')
[void](Click-Game $p $X $R2)            # CHOOSE THE OPERATION
Check "CHOOSE THE OPERATION opens the stock campaign screen" ([bool](Wait-Log $log 'opened the stock campaign screen' 8))
Start-Sleep 2
[void](Save-Shot $p "menu_8_hosting_operation")
Stop-Game $p
Start-Sleep 2

# ---- 5. host a custom mission
$p = Start-Game
$log = Get-MpLog $p "opgame"
[void](Wait-Log $log 'render pipeline reached' 60)
Start-Sleep 6
[void](Click-Game $p $EntryX $EntryY)
Start-Sleep -Milliseconds 800
[void](Click-Game $p $X $B2)            # HOST A CUSTOM MISSION
Check "HOST A CUSTOM MISSION starts hosting" ([bool](Wait-Log $log 'hosting mission chosen' 8))
Start-Sleep 3
[void](Save-Shot $p "menu_9_lobby_mission")
Check "host answers as a mission host" ((& python "$PSScriptRoot\probe_host.py") -eq 'menus mission')
[void](Click-Game $p $X $R2)            # CHOOSE THE MISSION
Check "CHOOSE THE MISSION opens the stock missions screen" ([bool](Wait-Log $log 'opened the stock missions screen' 8))
Start-Sleep 3
[void](Save-Shot $p "menu_10_hosting_mission")
Stop-Game $p
Show-Results
