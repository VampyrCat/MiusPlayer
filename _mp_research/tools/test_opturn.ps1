# Ends one turn of an operation from a given save, alone (no second player), and keeps the save the game
# writes afterwards. Used to see what the game does with a save, e.g. one with another player's orders merged in.
#   test_opturn.ps1 -Save <file.opsave> -Operation 1942_07_Oskol:07_02_odintsov_rus -Turns 6 -Out <after.opsave>
param([Parameter(Mandatory = $true)][string]$Save, [Parameter(Mandatory = $true)][string]$Operation, [int]$Turns = 6, [Parameter(Mandatory = $true)][string]$Out)
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
& cmd.exe /c "`"$script:Game\_mp_research\enable_multiplayer.bat`"" | Out-Null
$file = "$script:Game\users\profiles\cloud\" + ($Operation -replace ':', '_') + ".opsave"
Copy-Item $Save $file -Force
$started = Get-Date
$p = Start-Game "root\programs\opgame.progpack, cloud, $Operation, 0, $Turns" @{ MP_ROLE = "host"; MP_HOST_MODE = "operation" }
$log = Get-MpLog $p "opgame"
Check "on the operational map" ([bool](Wait-Log $log 'net: hosting on' 90))
Start-Sleep 12
Send-Command $p "endturn"
Check "turn ended" ([bool](Wait-Log $log 'map: ending the turn' 15))
Start-Sleep 12
Send-Command $p "savemap"
$line = Wait-Log $log 'test: operation saved' 15
Check "operation saved after the turn" ([bool]$line) $(if ($line) { $line.Substring(22) })
$p.Refresh()
Check "game still running" (-not $p.HasExited)
$d = Get-Item "$script:Game\out\minidump.dmp" -ErrorAction SilentlyContinue
Check "no crash" (-not ($d -and $d.LastWriteTime -gt $started))
if ($line) { Copy-Item $file $Out -Force }
[void][GtAuto]::SetCursorPos(2600, 1300); [void](Save-Shot $p ("opturn_" + [IO.Path]::GetFileNameWithoutExtension($Out)))
Stop-Game $p
Show-Results
