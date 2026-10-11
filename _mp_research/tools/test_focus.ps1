# Focus test: with a host and a joining game running (started by test_versus.ps1 -KeepRunning), puts one in
# front for a while, then the other, and prints each game's own load and clock reports for both periods.
#   test_focus.ps1 [-Seconds 20] [-Close]
param([int]$Seconds = 20, [switch]$Close)
. "$PSScriptRoot\auto.ps1"
$games = @(Get-CimInstance Win32_Process -Filter "Name='starter_x64.exe'" | Where-Object { $_.CommandLine -match 'tacgame\.progpack' } |
    ForEach-Object { Get-Process -Id $_.ProcessId })
if ($games.Count -ne 2) { "needs exactly two running battles, found $($games.Count)"; exit 2 }
$logs = $games | ForEach-Object { Get-MpLog $_ "tacgame" }
$host_first = if (Select-String -Path $logs[0] -Pattern 'net: hosting' -Quiet) { 0 } else { 1 }
$order = @($host_first, (1 - $host_first))
foreach ($front in $order) {
    $name = if ($front -eq $host_first) { "HOST" } else { "JOINING GAME" }
    Focus-Game $games[$front]
    "===== $name in front for $Seconds s"
    Start-Sleep $Seconds
    foreach ($i in $order) {
        "--- " + $(if ($i -eq $host_first) { "host" } else { "join" })
        (Select-String -Path $logs[$i] -Pattern 'load: 5 s|clock:|net: 5 s').Line | Select-Object -Last 6 | ForEach-Object { $_.Substring(22) }
    }
}
if ($Close) { foreach ($g in $games) { Stop-Game $g } }
