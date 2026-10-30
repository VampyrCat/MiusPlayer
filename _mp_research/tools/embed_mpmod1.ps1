# Makes mp_launcher\mpmod1.zip, the mpmod1 package that mp_launcher.exe carries and installs: a released
# package with the current Enable / Disable files and README head. mp_launcher.exe itself is not in it (it
# would have to contain itself); it adds a copy of itself when it installs.
#   embed_mpmod1.ps1 [-Release <zip with a folder mpmod1>]          default: GTMPRelease1.zip
param([string]$Release = "")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$mp = (Resolve-Path "$PSScriptRoot\..").Path
if (-not $Release) { $Release = Join-Path (Split-Path $mp -Parent) "GTMPRelease1.zip" }
$out = "$mp\mp_launcher\mpmod1.zip"

function Text-Of($entry) {
    $reader = New-Object IO.StreamReader($entry.Open(), (New-Object Text.ASCIIEncoding))
    try { $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Replace-Once([string]$text, [string]$old, [string]$new) {
    if (-not $text.Contains($old)) { throw "not in the release's README: $old" }
    $text.Replace($old, $new)
}

$from = [IO.Compression.ZipFile]::OpenRead($Release)
if (Test-Path $out) { Remove-Item $out }
$to = [IO.Compression.ZipFile]::Open($out, "Create")
try {
    foreach ($e in $from.Entries) {
        $name = $e.FullName.Replace("\", "/")
        if ($name.EndsWith("/")) { continue }
        if ($name -notlike "mpmod1/*") { throw "not an mpmod1 package: $name" }
        $bat = [IO.Path]::GetFileName($name)
        if ($name -eq "mpmod1/$bat" -and $bat -like "*.bat") {
            $data = [IO.File]::ReadAllBytes("$mp\mpmod1_src\$bat")
        } elseif ($name -eq "mpmod1/README.txt") {
            $text = Text-Of $e
            $text = Replace-Once $text "copies five new files" "copies six new files"
            $text = Replace-Once $text "build 3598 (the final Steam version)." "build 3598, on Steam's public_beta branch (Steam: Properties, Betas)."
            $line = "      mp_join.exe                         optional: join from a command prompt`r`n"
            $text = Replace-Once $text $line ($line + "      mp_launcher.exe                     optional: a small window to host a mission or join without the menus`r`n")
            $data = (New-Object Text.ASCIIEncoding).GetBytes($text)
        } else {
            $copy = New-Object IO.MemoryStream
            $in = $e.Open(); $in.CopyTo($copy); $in.Dispose()
            $data = $copy.ToArray()
        }
        $stream = $to.CreateEntry($name, "Optimal").Open()
        $stream.Write($data, 0, $data.Length)
        $stream.Dispose()
    }
} finally {
    $to.Dispose()
    $from.Dispose()
}
"embedded $Release -> $out  ($((Get-Item $out).Length) bytes)"
