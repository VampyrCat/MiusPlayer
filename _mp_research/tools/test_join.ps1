# End-to-end test of the join flow on one PC:
#   1. start a battle in host mode (left half of the screen)
#   2. run mp_join.exe 127.0.0.1, which fetches the battle and starts a second game (right half)
#   3. compare the unit positions of the two games while they run
param([int]$Seconds = 110, [switch]$KeepRunning)
Add-Type -AssemblyName System.Windows.Forms
$game = (Resolve-Path "$PSScriptRoot\..\..").Path
$mp = "$game\_mp_research"
if (Get-Process starter_x64 -ErrorAction SilentlyContinue) { "A game instance is already running; test skipped."; exit 2 }

$build = & cmd.exe /c "`"$mp\basic_network\build.bat`"" 2>$null | Out-String
if ($build -match 'error [A-Z]+\d+') { $build; exit 1 }
& cmd.exe /c "`"$mp\enable_multiplayer.bat`""
Get-ChildItem "$game\out" -Filter "basic_network*.log" | ForEach-Object { [IO.File]::Delete($_.FullName) }
Copy-Item "$mp\test_battle_image.cfgpack" "$game\data\common\shared\config\tacgame_image.cfgpack" -Force

$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$half = [int]($screen.Width / 2)
$env:MP_DEBUG = "1"
$env:MP_ROLE = "host"; $env:MP_WINDOW = "0,0,$half,$($screen.Height)"
$hostGame = Start-Process "$game\starter_x64.exe" -WorkingDirectory $game -PassThru -ArgumentList "root\programs\tacgame.progpack, data\common\shared\config\tacgame_image.cfgpack, users\profiles, cloud, *:*, 0, 1"
Start-Sleep 25      # the host starts listening once its battle has loaded
$env:MP_ROLE = $null; $env:MP_WINDOW = "$half,0,$half,$($screen.Height)"
"--- mp_join output"
& "$game\mp_join.exe" 127.0.0.1
$env:MP_WINDOW = $null; $env:MP_DEBUG = $null
Start-Sleep $Seconds
$games = @(Get-Process starter_x64 -ErrorAction SilentlyContinue)
"--- running games: $($games.Count)"
$joined = $games | Where-Object { $_.Id -ne $hostGame.Id } | Select-Object -First 1
if ($joined) { & python "$PSScriptRoot\live_diff.py" $hostGame.Id $joined.Id 3 }
foreach ($g in $games) { "pid $($g.Id): $($g.MainWindowTitle)" }
if (-not $KeepRunning) {
    foreach ($g in $games) { [void]$g.CloseMainWindow() }
    Start-Sleep 6
    foreach ($g in $games) { $g.Refresh(); if (-not $g.HasExited) { $g.Kill() } }
}
Get-ChildItem "$game\out" -Filter "basic_network_tacgame_*.log" | ForEach-Object {
    "== $($_.Name)"
    $net = (Select-String -Path $_.FullName -Pattern 'net:').Line
    $net | Select-Object -First 5
    $net | Select-Object -Last 3
}
