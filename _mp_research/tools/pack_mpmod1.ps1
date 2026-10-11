# Packs the multiplayer for players: a folder "mpmod1" that is put into a game folder and switched on and
# off with its own Enable_Multiplayer.bat / Disable_Multiplayer.bat.
#   pack_mpmod1.ps1 [-Out <folder>]          default: mpmod1 next to this copy of the game
# The five mod files come from Multiplayer_DropIn (what every test installs), the two .bat files and the
# head of the README from mpmod1_src, the rest of the README from the drop-in's Multiplayer_README.txt,
# and the list of stock file hashes (the build check of Enable_Multiplayer.bat) from vanilla_hashes.sha256.
param([string]$Out = "")
$ErrorActionPreference = "Stop"
$mp = (Resolve-Path "$PSScriptRoot\..").Path
$game = (Resolve-Path "$mp\..").Path
if (-not $Out) { $Out = Join-Path (Split-Path $game -Parent) "mpmod1" }
$drop = "$mp\Multiplayer_DropIn"
$src = "$mp\mpmod1_src"
$files = "bin\rel\x64\basic_network.codelib", "root\programs\opgame.progpack", "root\programs\tacgame.progpack", "steam_appid.txt", "mp_join.exe"

if ((Split-Path $Out -Leaf) -ne "mpmod1") { throw "the folder has to be called mpmod1: $Out" }
if (Test-Path $Out) {
    # only a folder this script made is replaced, and never one that is switched on in a game
    if (-not (Test-Path "$Out\Enable_Multiplayer.bat")) { throw "$Out exists and is not an mpmod1 package" }
    if (Test-Path "$Out\state") { throw "$Out is enabled in a game (it has a state folder): disable it first" }
    Remove-Item $Out -Recurse -Force
}
function Write-Crlf([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, (($text -replace "`r`n", "`n") -replace "`n", "`r`n"), (New-Object Text.ASCIIEncoding))
}

foreach ($f in $files) {
    $to = Join-Path "$Out\files" $f
    [void](New-Item -ItemType Directory -Force (Split-Path $to -Parent))
    Copy-Item "$drop\$f" $to
}
foreach ($bat in "Enable_Multiplayer.bat", "Disable_Multiplayer.bat") { Write-Crlf "$Out\$bat" (Get-Content "$src\$bat" -Raw) }

# stock files of build 3598, by their place in the game folder
$lines = foreach ($line in Get-Content "$mp\vanilla_hashes.sha256") {
    $hash, $name = $line -split '\s+', 2
    $rel = "bin\rel\x64\$name", "root\$name", $name | Where-Object { Test-Path "$game\$_" } | Select-Object -First 1
    if (-not $rel) { throw "stock file not found: $name" }
    "$hash  $rel"
}
Write-Crlf "$Out\game_build_3598.sha256" (($lines -join "`n") + "`n")

$version = [regex]::Match((Get-Content "$mp\basic_network\protocol.h" -Raw), 'k_version\s*=\s*(\d+)').Groups[1].Value
$module = Get-Item "$drop\bin\rel\x64\basic_network.codelib"
$stamp = "network version $version, module built " + $module.LastWriteTime.ToString("yyyy-MM-dd HH:mm")
$body = Get-Content "$drop\Multiplayer_README.txt"
$play = [Array]::IndexOf($body, "PLAY")
if ($play -lt 0) { throw "no PLAY section in Multiplayer_README.txt" }
$head = (Get-Content "$src\README_head.txt" -Raw).Replace("@VERSION@", $stamp)
Write-Crlf "$Out\README.txt" ($head.TrimEnd() + "`n`n" + (($body[$play..($body.Count - 1)]) -join "`n") + "`n")

"packed $Out  ($stamp)"
Get-ChildItem $Out -Recurse -File | ForEach-Object { "   {0,-48} {1,9:N0}" -f $_.FullName.Substring($Out.Length + 1), $_.Length }
