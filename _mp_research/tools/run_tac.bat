@echo off
rem Starts the tactical battle directly from the battle description the operational phase last wrote
rem (data\common\shared\config\tacgame_image.cfgpack). Run it twice for two instances of the same battle.
cd /d "%~dp0..\.."
start "" starter_x64.exe root\programs\tacgame.progpack, data\common\shared\config\tacgame_image.cfgpack, users\profiles, cloud, *:*, 0, 1
