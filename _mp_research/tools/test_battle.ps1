# Rebuilds the module, starts N instances of the saved test battle side by side and waits.
#   test_battle.ps1 [-Instances 2] [-Seconds 90] [-NoBuild] [-KeepRunning]
# Only the instances this script starts are ever closed; a game you are playing is left alone
# (but the module cannot be rebuilt while any instance of the modded copy is running).
param([int]$Instances = 2, [int]$Seconds = 90, [switch]$NoBuild, [switch]$KeepRunning, [int]$Affinity = 0, [switch]$Net, [string]$Check = "")
Add-Type -AssemblyName System.Windows.Forms
$game = (Resolve-Path "$PSScriptRoot\..\..").Path
$mp = "$game\_mp_research"

$others = @(Get-Process starter_x64 -ErrorAction SilentlyContinue)
if ($others.Count -gt 0) {
    "A game instance is already running (pid $($others.Id -join ', ')). Not touching it; test skipped."
    exit 2
}

if (-not $NoBuild) {
    $build = & cmd.exe /c "`"$mp\basic_network\build.bat`"" 2>$null | Out-String
    if ($build -match 'error [A-Z]+\d+') { $build; exit 1 }
    Copy-Item "$mp\basic_network\build\basic_network.codelib" "$game\bin\rel\x64\" -Force
}
Get-ChildItem "$game\out" -Filter "basic_network*.log" | ForEach-Object { [IO.File]::Delete($_.FullName) }
Get-ChildItem "$game\out" -Filter "mp_trace*.txt" | ForEach-Object { [IO.File]::Delete($_.FullName) }
Get-ChildItem "$game\out" -Filter "mp_units*.txt" | ForEach-Object { [IO.File]::Delete($_.FullName) }
Copy-Item "$mp\test_battle_image.cfgpack" "$game\data\common\shared\config\tacgame_image.cfgpack" -Force

$cmdline = "root\programs\tacgame.progpack, data\common\shared\config\tacgame_image.cfgpack, users\profiles, cloud, *:*, 0, 1"
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$w = [int]($screen.Width / $Instances)
$started = @()
for ($i = 0; $i -lt $Instances; $i++) {
    # side by side, so neither window covers the other
    $env:MP_WINDOW = "$($i * $w),0,$w,$($screen.Height)"
    if ($Net) {
        # first instance hosts, the others join it over the loopback address
        if ($i -eq 0) { $env:MP_ROLE = "host"; $env:MP_PEER = $null } else { $env:MP_ROLE = "join"; $env:MP_PEER = "127.0.0.1" }
    }
    $proc = Start-Process "$game\starter_x64.exe" -ArgumentList $cmdline -WorkingDirectory $game -PassThru
    if ($Affinity) { $proc.ProcessorAffinity = [IntPtr]($Affinity -shl ($i * 2)) }   # pin each instance to its own core(s)
    $started += $proc
    Start-Sleep 3
}
$env:MP_WINDOW = $null; $env:MP_ROLE = $null; $env:MP_PEER = $null
Start-Sleep $Seconds
if ($Check) {
    # "tool.py extra args": the tool gets the process ids first, then the extra arguments
    $parts = $Check -split ' '
    $extra = @($parts | Select-Object -Skip 1)
    & python "$PSScriptRoot\$($parts[0])" @($started | ForEach-Object { $_.Id }) @extra
}
foreach ($p in $started) { $p.Refresh(); if ($p.HasExited) { "pid $($p.Id): already exited" } else { "pid $($p.Id): running" } }
if (-not $KeepRunning) {
    foreach ($p in $started) { if (-not $p.HasExited) { [void]$p.CloseMainWindow() } }
    Start-Sleep 6
    foreach ($p in $started) { $p.Refresh(); if (-not $p.HasExited) { $p.Kill() } }
}
Get-ChildItem "$game\out" -Filter "mp_trace*.txt" | ForEach-Object { "$($_.Name): $((Get-Content $_.FullName).Count) lines" }
