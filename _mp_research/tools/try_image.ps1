# Starts a battle from data\common\shared\config\tacgame_image_swapped.cfgpack, waits for deployment,
# saves one frame as shots\<name>.png and closes the game again. For battle-description experiments.
param([string]$Name = "try", [int]$Wait = 28)
. "$PSScriptRoot\auto.ps1"
Assert-NoGameRunning
$p = Start-Game "root\programs\tacgame.progpack, data\common\shared\config\tacgame_image_swapped.cfgpack, users\profiles, cloud, *:*, 0, 1" @{ MP_DEBUG = "1" }
$log = Get-MpLog $p "tacgame"
if (-not (Wait-Log $log 'first tick' 60)) { "the battle did not start"; Stop-Game $p; exit 1 }
Start-Sleep $Wait
Save-Shot $p $Name
Stop-Game $p
