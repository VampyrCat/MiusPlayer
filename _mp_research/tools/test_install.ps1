# Checks the drop-in folder the way a player uses it: copy it into a vanilla game, then remove it again.
# Verifies that only new files appear, no stock file changes, and that removal leaves nothing behind.
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$game = $script:Game
$mp = "$game\_mp_research"
$drop = "$mp\Multiplayer_DropIn"

function Stock-Changed {
    $bad = 0
    foreach ($line in Get-Content "$mp\vanilla_hashes.sha256") {
        $hash, $name = $line -split '\s+', 2
        $file = Get-ChildItem "$game\bin\rel\x64\$name", "$game\root\$name", "$game\$name" -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $file -or (Get-FileHash $file.FullName -Algorithm SHA256).Hash -ne $hash) { $bad++ }
    }
    $bad
}
function Game-Files { Get-ChildItem $game -Recurse -File | Where-Object { $_.FullName -notlike "$mp\*" -and $_.FullName -notlike "$game\out\*" } | ForEach-Object { $_.FullName.Substring($game.Length + 1) } }

& cmd.exe /c "`"$mp\disable_multiplayer.bat`"" | Out-Null
$before = Game-Files
Check "starting point is a vanilla game (stock files match their recorded hashes)" ((Stock-Changed) -eq 0)

Copy-Item "$drop\*" $game -Recurse -Force            # what a player does: drop the folder's contents into the game
$added = @(Compare-Object $before (Game-Files) | Where-Object SideIndicator -eq '=>' | ForEach-Object InputObject)
$expected = @(Get-ChildItem $drop -Recurse -File | ForEach-Object { $_.FullName.Substring($drop.Length + 1) })
Check "dropping the folder in adds exactly its own files" (-not (Compare-Object $added $expected)) "$($added.Count) files added"
Check "no stock file was changed by installing" ((Stock-Changed) -eq 0)

& cmd.exe /c "`"$game\Remove_Multiplayer.bat`" /quiet" | Out-Null
$left = @(Compare-Object $before (Game-Files) | ForEach-Object InputObject)
Check "Remove_Multiplayer.bat leaves only itself behind" (($left -join ',') -eq 'Remove_Multiplayer.bat') ($left -join ', ')
Remove-Item "$game\Remove_Multiplayer.bat" -ErrorAction SilentlyContinue
Check "stock files still match after removal" ((Stock-Changed) -eq 0)
Show-Results
