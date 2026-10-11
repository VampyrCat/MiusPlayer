@echo off
rem Removes the multiplayer files from the game folder this file sits in. The stock game files were never
rem changed, so the game is exactly the original again afterwards.
cd /d "%~dp0"
if not exist "starter_x64.exe" (echo Run this from the game folder, next to starter_x64.exe. & pause & exit /b 1)
for %%F in ("bin\rel\x64\basic_network.codelib" "root\programs\tacgame.progpack" "root\programs\opgame.progpack" "steam_appid.txt" "mp_join.exe" "Multiplayer_README.txt" "users\mp_last_host.txt") do if exist "%%~F" del "%%~F"
rmdir "root\programs" 2>nul
echo Multiplayer removed. You can delete Remove_Multiplayer.bat too.
if not "%1"=="/quiet" pause
