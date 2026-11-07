# Makes a release of MiusPlayer from what src\build.bat and launcher\build_launcher.bat built:
#   <Out>\MiusPlayer-<version>.zip    mod\bin\rel\x64\basic_network.codelib, mod\mp_join.exe, mod\steam_appid.txt,
#                                     mod\manifest.json (version, network version, supported game builds, SHA-256 of each file)
#   <Out>\MiusPlayerLauncher.exe      the launcher, which installs and updates the mod from the latest release
# The game's own program files (root\programs\*.progpack) are not in it: the launcher makes them from the
# player's own game. Works in Windows PowerShell 5.1 and PowerShell 7.
#   pack_release.ps1 [-Out dist] [-Root <folder with src\ and launcher\>] [-Release <release.json>]
param([string]$Out = "dist", [string]$Root = "", [string]$Release = "")
$ErrorActionPreference = "Stop"
$root = if ($Root) { (Resolve-Path $Root).Path } else { Split-Path $PSScriptRoot -Parent }
$relFile = if ($Release) { $Release } else { Join-Path $root "release.json" }
$rel = Get-Content $relFile -Raw | ConvertFrom-Json
$version = [string]$rel.version
$out = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
New-Item -ItemType Directory -Force -Path $out | Out-Null

$files = [ordered]@{
    "bin/rel/x64/basic_network.codelib" = Join-Path $root "src\build\basic_network.codelib"
    "mp_join.exe"                       = Join-Path $root "src\build\mp_join.exe"
}
foreach ($f in $files.Values) { if (-not (Test-Path $f)) { throw "not built: $f" } }
$appid = [System.Text.Encoding]::ASCII.GetBytes("312980")

function Sha256([byte[]]$b) {
    $h = [System.Security.Cryptography.SHA256]::Create()
    try { -join ($h.ComputeHash($b) | ForEach-Object { $_.ToString("x2") }) } finally { $h.Dispose() }
}

$entries = [ordered]@{}
foreach ($k in $files.Keys) { $entries[$k] = [System.IO.File]::ReadAllBytes($files[$k]) }
$entries["steam_appid.txt"] = $appid
$list = @()
foreach ($k in $entries.Keys) { $list += [ordered]@{ path = $k; sha256 = (Sha256 $entries[$k]) } }
$manifest = [ordered]@{ version = $version; network = [int]$rel.network; builds = @($rel.builds); files = $list }
$json = $manifest | ConvertTo-Json -Depth 5
$entries["manifest.json"] = [System.Text.Encoding]::UTF8.GetBytes($json)

# entry names with forward slashes (Compress-Archive in Windows PowerShell writes backslashes)
Add-Type -AssemblyName System.IO.Compression
$zipPath = Join-Path $out "MiusPlayer-$version.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath }
$fs = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
$zip = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($k in $entries.Keys) {
        $e = $zip.CreateEntry("mod/" + $k, [System.IO.Compression.CompressionLevel]::Optimal)
        $s = $e.Open()
        try { $s.Write($entries[$k], 0, $entries[$k].Length) } finally { $s.Dispose() }
    }
} finally { $zip.Dispose(); $fs.Dispose() }

$launcher = Join-Path $root "launcher\build\MiusPlayerLauncher.exe"
if (-not (Test-Path $launcher)) { throw "not built: $launcher" }
Copy-Item $launcher (Join-Path $out "MiusPlayerLauncher.exe") -Force
"MiusPlayer $version -> $zipPath"
$json
