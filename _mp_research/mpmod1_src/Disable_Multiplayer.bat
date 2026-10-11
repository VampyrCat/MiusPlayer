@echo off
rem Graviteam Tactics: Mius-Front multiplayer (mpmod1).
rem Takes the multiplayer files out of the game this mpmod1 folder sits in. The game's own files were
rem never changed, so the game is exactly the original again afterwards.
rem     Disable_Multiplayer.bat [/quiet]       /quiet: no "press a key"
setlocal EnableExtensions DisableDelayedExpansion
set "MOD=%~dp0"
set "MOD=%MOD:~0,-1%"
for %%I in ("%MOD%\..") do set "GAME=%%~fI"
set "QUIET="
if /i "%~1"=="/quiet" set "QUIET=1"
set "RC=0"
echo.
echo  Graviteam Tactics: Mius-Front - multiplayer
echo.
if not exist "%GAME%\starter_x64.exe" goto :no_game
tasklist /fi "imagename eq starter_x64.exe" /nh 2>nul | find /i "starter_x64.exe" >nul && goto :running

set "HAD="
if exist "%GAME%\bin\rel\x64\basic_network.codelib" set "HAD=1"
findstr /m /c:"basic_network.codelib" "%GAME%\root\programs\tacgame.progpack" >nul 2>nul && set "HAD=1"
if exist "%MOD%\state\enabled.txt" set "HAD=1"
if not defined HAD goto :nothing

rem (the programs that ask for the module first, the module last: stopping half-way leaves a working game)
call :gone "root\programs\tacgame.progpack" || goto :failed
call :gone "root\programs\opgame.progpack" || goto :failed
call :gone "bin\rel\x64\basic_network.codelib" || goto :failed
call :gone "mp_join.exe" || goto :failed
call :gone "users\mp_last_host.txt" || goto :failed
rem (loose files of the older "drop-in" packaging)
call :gone "Remove_Multiplayer.bat" || goto :failed
call :gone "Multiplayer_README.txt" || goto :failed
rem steam_appid.txt goes if the multiplayer brought it. Without this folder's notes (the folder was replaced
rem while multiplayer was enabled, or the older packaging was installed) it did, if it is the same file.
if exist "%MOD%\state\steam_appid_added.txt" goto :appid_gone
if exist "%MOD%\state\enabled.txt" goto :appid_done
fc /b "%MOD%\files\steam_appid.txt" "%GAME%\steam_appid.txt" >nul 2>nul || goto :appid_done
:appid_gone
call :gone "steam_appid.txt" || goto :failed
:appid_done
call :back opgame.progpack || goto :failed
call :back tacgame.progpack || goto :failed
rmdir "%GAME%\root\programs" 2>nul
if exist "%MOD%\state\steam_appid_added.txt" del /f /q "%MOD%\state\steam_appid_added.txt"
if exist "%MOD%\state\enabled.txt" del /f /q "%MOD%\state\enabled.txt"
rmdir "%MOD%\state\backup" 2>nul
rmdir "%MOD%\state" 2>nul

echo  Multiplayer is DISABLED in
echo    "%GAME%"
echo.
echo  The game is the original single-player game again.
if exist "%GAME%\users\mp_saves\" echo  Your multiplayer operation saves in users\mp_saves were kept.
echo  You can delete the mpmod1 folder now, or run Enable_Multiplayer.bat again later.
goto :finish

rem ---- %1 = a file of the multiplayer, relative to the game folder
:gone
if not exist "%GAME%\%~1" exit /b 0
del /f /q "%GAME%\%~1" >nul 2>nul
if exist "%GAME%\%~1" exit /b 1
exit /b 0

rem ---- %1 = a program file the game had before the multiplayer was enabled
:back
if not exist "%MOD%\state\backup\%~1" exit /b 0
if not exist "%GAME%\root\programs\" mkdir "%GAME%\root\programs" || exit /b 1
move /y "%MOD%\state\backup\%~1" "%GAME%\root\programs\%~1" >nul || exit /b 1
echo    put back the root\programs\%~1 the game had before
exit /b 0

:nothing
echo  Multiplayer is not enabled in this game: nothing to take out.
goto :finish

:no_game
echo  This folder is not inside a game folder (no starter_x64.exe next to it):
echo    "%GAME%"
echo  Run this from the mpmod1 folder inside the game in which multiplayer was enabled.
set "RC=3"
goto :finish

:running
echo  The game is running. Close it, then run this again.
set "RC=4"
goto :finish

:failed
echo.
echo  A file could not be removed. Is the game still running, or is the folder
echo  write-protected? Close the game and run this again.
set "RC=1"
goto :finish

:finish
echo.
if not defined QUIET pause
exit /b %RC%
