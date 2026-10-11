# Checks the mpmod1 package the way a player uses it: the folder is put into a game folder, multiplayer is
# switched on with Enable_Multiplayer.bat and off with Disable_Multiplayer.bat.
#   test_mpmod1.ps1 [-Package <mpmod1 folder>] [-Real]
# Without -Real everything happens in throwaway game folders (the stock files of this game copied to a
# temp folder whose path has spaces and brackets, like "Program Files (x86)"). With -Real the package is
# also enabled in this copy of the game, the game is started the way a player starts it, and the package
# is disabled again; this copy then gets its usual test install (enable_multiplayer.bat) back.
param([string]$Package = "", [switch]$Real)
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$game = $script:Game
$mp = "$game\_mp_research"
if (-not $Package) { $Package = Join-Path (Split-Path $game -Parent) "mpmod1" }
if (-not (Test-Path "$Package\Enable_Multiplayer.bat")) { "no mpmod1 package at $Package (tools\pack_mpmod1.ps1 makes it)"; exit 1 }
if (Test-Path "$Package\state") { "the package at $Package is enabled in a game; test skipped"; exit 1 }
$payload = @(Get-ChildItem "$Package\files" -Recurse -File | ForEach-Object { $_.FullName.Substring("$Package\files".Length + 1) })
$stock = @(Get-Content "$Package\game_build_3598.sha256" | ForEach-Object { ($_ -split '\s+', 2)[1] })
$root = Join-Path $env:TEMP "mpmod1 test (x86)"

# what is in a game folder, mpmod1 left out: relative path -> hash
function Snapshot([string]$g) {
    $h = @{}
    Get-ChildItem $g -Recurse -File | Where-Object { $_.FullName -notlike "$g\mpmod1\*" } | ForEach-Object { $h[$_.FullName.Substring($g.Length + 1)] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
    $h
}
function Same($a, $b) {
    if ($a.Count -ne $b.Count) { return $false }
    foreach ($k in $a.Keys) { if ($b[$k] -ne $a[$k]) { return $false } }
    $true
}
function Difference($a, $b) { (@($a.Keys | Where-Object { $b[$_] -ne $a[$_] }) + @($b.Keys | Where-Object { -not $a.ContainsKey($_) }) | Sort-Object -Unique) -join ', ' }
function New-FakeGame([string]$name) {
    $g = Join-Path $root $name
    if (Test-Path $g) { Remove-Item $g -Recurse -Force }
    foreach ($f in $stock) { [void](New-Item -ItemType Directory -Force (Split-Path "$g\$f" -Parent)); Copy-Item "$game\$f" "$g\$f" }
    [void](New-Item -ItemType Directory -Force "$g\users")
    Copy-Item $Package "$g\mpmod1" -Recurse
    $g
}
function Run([string]$g, [string]$bat, [string]$arg = "/quiet", [string]$answer = $null) {
    $out = if ($null -ne $answer) { $answer | & cmd.exe /c "`"$g\mpmod1\$bat`" $arg" 2>&1 } else { & cmd.exe /c "`"$g\mpmod1\$bat`" $arg" 2>&1 }
    [pscustomobject]@{ Code = $LASTEXITCODE; Text = ($out | Out-String) }
}
function Installed([string]$g) {
    foreach ($f in $payload) { if (-not (Test-Path "$g\$f") -or (Get-FileHash "$g\$f").Hash -ne (Get-FileHash "$Package\files\$f").Hash) { return $false } }
    $true
}

if (Test-Path $root) { Remove-Item $root -Recurse -Force }
"--- A. a clean game"
$g = New-FakeGame "Graviteam Tactics Mius-Front"
$before = Snapshot $g
$r = Run $g "Enable_Multiplayer.bat"
Check "A: enable succeeds in a clean game (path with spaces and brackets)" ($r.Code -eq 0 -and $r.Text -match 'Multiplayer is ENABLED') ($r.Text.Trim() -split "`r?`n" | Select-Object -Last 1)
$after = Snapshot $g
$added = @($after.Keys | Where-Object { -not $before.ContainsKey($_) } | Sort-Object)
Check "A: exactly the five mod files were added" (-not (Compare-Object $added ($payload | Sort-Object))) ($added -join ', ')
Check "A: the added files are the package's" (Installed $g)
Check "A: no file of the game was changed" (-not @($before.Keys | Where-Object { $after[$_] -ne $before[$_] }))
$r = Run $g "Enable_Multiplayer.bat"
Check "A: enabling twice is harmless" ($r.Code -eq 0 -and (Same $after (Snapshot $g)))
Set-Content "$g\users\mp_last_host.txt" "127.0.0.1"
[void](New-Item -ItemType Directory -Force "$g\users\mp_saves\a_vs_b"); Set-Content "$g\users\mp_saves\a_vs_b\x.opsave" "save"
$r = Run $g "Disable_Multiplayer.bat"
$left = Snapshot $g
$kept = $left.ContainsKey("users\mp_saves\a_vs_b\x.opsave"); $left.Remove("users\mp_saves\a_vs_b\x.opsave")
Check "A: disable succeeds" ($r.Code -eq 0 -and $r.Text -match 'Multiplayer is DISABLED')
Check "A: after disable the game folder is what it was before" (Same $before $left) (Difference $before $left)
Check "A: the multiplayer operation saves were kept" $kept
Check "A: no root\programs folder and no state folder are left" (-not (Test-Path "$g\root\programs") -and -not (Test-Path "$g\mpmod1\state"))
$r = Run $g "Disable_Multiplayer.bat"
Check "A: disabling twice is harmless" ($r.Code -eq 0 -and $r.Text -match 'not enabled')

"--- B. the game has a steam_appid.txt of its own"
$g = New-FakeGame "B"
Set-Content "$g\steam_appid.txt" "312980" -NoNewline
$before = Snapshot $g
[void](Run $g "Enable_Multiplayer.bat"); [void](Run $g "Enable_Multiplayer.bat")
Check "B: enabled" (Installed $g)
$r = Run $g "Disable_Multiplayer.bat"
$left = Snapshot $g
Check "B: disable leaves the player's own steam_appid.txt, and nothing else" ($r.Code -eq 0 -and (Same $before $left)) (Difference $before $left)

"--- C. the game has program files of the same names from something else"
$g = New-FakeGame "C"
[void](New-Item -ItemType Directory -Force "$g\root\programs")
$bytes = New-Object byte[] 5000; (New-Object Random 7).NextBytes($bytes)
[IO.File]::WriteAllBytes("$g\root\programs\opgame.progpack", $bytes); [IO.File]::WriteAllBytes("$g\root\programs\tacgame.progpack", $bytes[100..4000])
$before = Snapshot $g
$r = Run $g "Enable_Multiplayer.bat"
Check "C: enabled, and the player is told his files are kept" ((Installed $g) -and $r.Text -match 'kept in' -and (Test-Path "$g\mpmod1\state\backup\opgame.progpack"))
$r = Run $g "Disable_Multiplayer.bat"
$left = Snapshot $g
Check "C: disable puts the other program files back unchanged" ($r.Code -eq 0 -and (Same $before $left)) (Difference $before $left)

"--- D. the older drop-in packaging is installed"
$g = New-FakeGame "D"
$vanilla = Snapshot $g
Copy-Item "$mp\Multiplayer_DropIn\*" $g -Recurse -Force
$r = Run $g "Enable_Multiplayer.bat"
Check "D: enable replaces the older install" ($r.Code -eq 0 -and (Installed $g) -and -not (Test-Path "$g\Remove_Multiplayer.bat") -and -not (Test-Path "$g\Multiplayer_README.txt"))
$r = Run $g "Disable_Multiplayer.bat"
$left = Snapshot $g
Check "D: disable leaves the original game" ($r.Code -eq 0 -and (Same $vanilla $left)) (Difference $vanilla $left)

"--- E. the mpmod1 folder was replaced while multiplayer was enabled"
$g = New-FakeGame "E"
$vanilla = Snapshot $g
[void](Run $g "Enable_Multiplayer.bat")
Remove-Item "$g\mpmod1" -Recurse -Force; Copy-Item $Package "$g\mpmod1" -Recurse
[IO.File]::WriteAllBytes("$g\root\programs\opgame.progpack", [IO.File]::ReadAllBytes("$g\root\programs\opgame.progpack")[0..30000])    # "an older version"
$r = Run $g "Enable_Multiplayer.bat"
Check "E: enable from the new folder" ($r.Code -eq 0 -and (Installed $g) -and -not (Test-Path "$g\mpmod1\state\backup"))
$r = Run $g "Disable_Multiplayer.bat"
$left = Snapshot $g
Check "E: disable leaves the original game" ($r.Code -eq 0 -and (Same $vanilla $left)) (Difference $vanilla $left)
[void](Run $g "Enable_Multiplayer.bat")
Remove-Item "$g\mpmod1" -Recurse -Force; Copy-Item $Package "$g\mpmod1" -Recurse
$r = Run $g "Disable_Multiplayer.bat"
$left = Snapshot $g
Check "E: disable from a fresh folder (its notes lost) leaves the original game too" ($r.Code -eq 0 -and (Same $vanilla $left)) (Difference $vanilla $left)

"--- F. another game version"
$g = New-FakeGame "F"
$b = [IO.File]::ReadAllBytes("$g\bin\rel\x64\adv_physic.codelib"); $b[5000] = $b[5000] -bxor 1; [IO.File]::WriteAllBytes("$g\bin\rel\x64\adv_physic.codelib", $b)
Remove-Item "$g\bin\rel\x64\basic_sound.codelib"
$before = Snapshot $g
$r = Run $g "Enable_Multiplayer.bat"
Check "F: /quiet refuses, names the two files, changes nothing" ($r.Code -eq 2 -and $r.Text -match 'not as expected: bin\\rel\\x64\\adv_physic\.codelib' -and $r.Text -match 'basic_sound' -and $r.Text -notmatch 'adv_game' -and (Same $before (Snapshot $g)) -and -not (Test-Path "$g\mpmod1\state"))
$r = Run $g "Enable_Multiplayer.bat" "" "n"
Check "F: asked, answer n: nothing changed" ($r.Code -eq 2 -and $r.Text -match 'Enable it anyway' -and (Same $before (Snapshot $g)))
$r = Run $g "Enable_Multiplayer.bat" "" "y"
Check "F: asked, answer y: enabled" ($r.Code -eq 0 -and (Installed $g))
[void](Run $g "Disable_Multiplayer.bat")
Check "F: and disabled again" (Same $before (Snapshot $g))

"--- G. the folder is not inside a game"
$loose = Join-Path $root "somewhere else"
[void](New-Item -ItemType Directory -Force $loose); Copy-Item $Package "$loose\mpmod1" -Recurse
$r = Run $loose "Enable_Multiplayer.bat"
Check "G: enable says where the folder has to go" ($r.Code -eq 3 -and $r.Text -match 'starter_x64\.exe' -and -not (Test-Path "$loose\mpmod1\state"))
$r = Run $loose "Disable_Multiplayer.bat"
Check "G: disable says so too" ($r.Code -eq 3)
Remove-Item $root -Recurse -Force

if ($Real) {
    "--- R. in this copy of the game, started the way a player starts it"
    function Game-Files { Get-ChildItem $game -Recurse -File | Where-Object { $_.FullName -notlike "$mp\*" -and $_.FullName -notlike "$game\out\*" -and $_.FullName -notlike "$game\mpmod1\*" } | ForEach-Object { $_.FullName.Substring($game.Length + 1) } }
    function Stock-Changed { @(Get-Content "$Package\game_build_3598.sha256" | Where-Object { $h, $f = $_ -split '\s+', 2; (Get-FileHash "$game\$f" -Algorithm SHA256).Hash -ne $h }).Count }
    if (Test-Path "$game\mpmod1") { "there already is an mpmod1 folder in $game; -Real skipped"; Show-Results; exit 1 }
    & cmd.exe /c "`"$mp\disable_multiplayer.bat`"" | Out-Null
    $before = @(Game-Files)
    Check "R: starting point is the original game" ((Stock-Changed) -eq 0 -and -not (Test-Path "$game\bin\rel\x64\basic_network.codelib"))
    Copy-Item $Package "$game\mpmod1" -Recurse
    $r = & cmd.exe /c "`"$game\mpmod1\Enable_Multiplayer.bat`" /quiet" 2>&1 | Out-String
    Check "R: enable: build check passes, multiplayer enabled" ($LASTEXITCODE -eq 0 -and $r -notmatch 'not as expected')
    $added = @(Compare-Object $before (Game-Files) | Where-Object SideIndicator -eq '=>' | ForEach-Object InputObject | Sort-Object)
    Check "R: exactly the five mod files were added" (-not (Compare-Object $added ($payload | Sort-Object))) ($added -join ', ')
    Check "R: no stock file was changed" ((Stock-Changed) -eq 0)
    $p = Start-Game
    $log = Get-MpLog $p "opgame"
    Check "R: the game starts and the multiplayer module is in it" ([bool](Wait-Log $log 'render pipeline reached' 90))
    Start-Sleep 6
    [void](Click-Game $p 0.5 0.129)
    Check "R: the MULTIPLAYER entry of the main menu opens its panel" ([bool](Wait-Log $log 'multiplayer menu opened' 8))
    [void](Click-Game $p 0.56 0.331)        # HOST A SKIRMISH
    Check "R: HOST A SKIRMISH opens a lobby that is hosting" ([bool](Wait-Log $log 'net: hosting on' 10))
    & cmd.exe /c "`"$game\mpmod1\Enable_Multiplayer.bat`" /quiet" | Out-Null; $e = $LASTEXITCODE
    & cmd.exe /c "`"$game\mpmod1\Disable_Multiplayer.bat`" /quiet" | Out-Null
    Check "R: enable and disable refuse while the game runs" ($e -eq 4 -and $LASTEXITCODE -eq 4 -and (Test-Path "$game\bin\rel\x64\basic_network.codelib"))
    Stop-Game $p
    for ($i = 0; $i -lt 20 -and (Get-Process starter_x64 -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
    $r = & cmd.exe /c "`"$game\mpmod1\Disable_Multiplayer.bat`" /quiet" 2>&1 | Out-String
    Check "R: disable succeeds" ($LASTEXITCODE -eq 0 -and $r -match 'Multiplayer is DISABLED')
    $left = @(Compare-Object $before (Game-Files) | ForEach-Object { $_.SideIndicator + ' ' + $_.InputObject })
    Check "R: afterwards the game folder holds the files it held before" ($left.Count -eq 0) ($left -join ', ')
    Check "R: stock files still match" ((Stock-Changed) -eq 0)
    if (-not (Test-Path "$game\mpmod1\state")) { Remove-Item "$game\mpmod1" -Recurse -Force }
    Check "R: the test's copy of mpmod1 is gone from this game" (-not (Test-Path "$game\mpmod1"))
    & cmd.exe /c "`"$mp\enable_multiplayer.bat`"" | Out-Null       # this copy's usual test install
}
Show-Results
