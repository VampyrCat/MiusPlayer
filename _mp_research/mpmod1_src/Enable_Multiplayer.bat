@echo off
rem Graviteam Tactics: Mius-Front multiplayer (mpmod1).
rem Switches multiplayer on in the game this mpmod1 folder was put into: five new files are copied into
rem the game. No file of the game itself is changed or replaced, and Disable_Multiplayer.bat takes the
rem five out again.
rem     Enable_Multiplayer.bat [/quiet]        /quiet: no questions, no "press a key"
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
if not exist "%MOD%\files\bin\rel\x64\basic_network.codelib" goto :incomplete
if not exist "%MOD%\files\root\programs\tacgame.progpack" goto :incomplete
tasklist /fi "imagename eq starter_x64.exe" /nh 2>nul | find /i "starter_x64.exe" >nul && goto :running

rem ---- is this the game version the multiplayer was made for?
set "ODD=0"
for /f "usebackq tokens=1,*" %%A in ("%MOD%\game_build_3598.sha256") do call :check %%A "%%B"
if "%ODD%"=="0" goto :install
echo.
echo  The files listed above are not those of game version 6.00, build 3598 (the final
echo  Steam version). This multiplayer was made for that build only and will most
echo  likely not work with this game.
if defined QUIET goto :cancelled
set "ANS="
set /p "ANS=  Enable it anyway? (y/n) "
if /i not "%ANS%"=="y" goto :cancelled

:install
rem An earlier version of this multiplayer in the game is simply replaced. Anything else that already
rem has one of our file names is somebody else's: it is kept and put back by Disable_Multiplayer.bat.
set "HAD="
if exist "%GAME%\bin\rel\x64\basic_network.codelib" set "HAD=1"
findstr /m /c:"basic_network.codelib" "%GAME%\root\programs\tacgame.progpack" >nul 2>nul && set "HAD=1"
if not exist "%MOD%\state\" mkdir "%MOD%\state" || goto :failed
if defined HAD call :earlier
call :keep opgame.progpack || goto :failed
call :keep tacgame.progpack || goto :failed
rem (the module first, the programs that ask for it last: a copy that stops half-way leaves a working game)
copy /y "%MOD%\files\bin\rel\x64\basic_network.codelib" "%GAME%\bin\rel\x64\basic_network.codelib" >nul || goto :failed
copy /y "%MOD%\files\mp_join.exe" "%GAME%\mp_join.exe" >nul || goto :failed
if exist "%GAME%\steam_appid.txt" goto :programs
copy /y "%MOD%\files\steam_appid.txt" "%GAME%\steam_appid.txt" >nul || goto :failed
>"%MOD%\state\steam_appid_added.txt" echo mpmod1 put steam_appid.txt into the game folder; Disable_Multiplayer.bat removes it.
:programs
if not exist "%GAME%\root\programs\" mkdir "%GAME%\root\programs" || goto :failed
copy /y "%MOD%\files\root\programs\opgame.progpack" "%GAME%\root\programs\opgame.progpack" >nul || goto :failed
copy /y "%MOD%\files\root\programs\tacgame.progpack" "%GAME%\root\programs\tacgame.progpack" >nul || goto :failed
>"%MOD%\state\enabled.txt" echo Multiplayer (mpmod1) is enabled in this game. Run Disable_Multiplayer.bat before you delete or replace the mpmod1 folder.

echo  Multiplayer is ENABLED in
echo    "%GAME%"
echo.
echo  Start the game as usual: the main menu has a new entry at the top, MULTIPLAYER.
echo  Whoever you play with needs this same mpmod1 folder, enabled the same way.
echo  How to host and join, and what does not work yet: README.txt in this folder.
echo  Back to the original game: Disable_Multiplayer.bat in this folder.
goto :finish

rem ---- %1 = SHA-256 the file must have, %2 = file, relative to the game folder
:check
set "H="
if not exist "%GAME%\%~2" goto :check_odd
for /f "skip=1 delims=" %%X in ('certutil -hashfile "%GAME%\%~2" SHA256 2^>nul') do if not defined H set "H=%%X"
if defined H set "H=%H: =%"
if /i "%H%"=="%~1" exit /b 0
:check_odd
set /a ODD+=1
echo    not as expected: %~2
exit /b 0

rem ---- an earlier version of this multiplayer is installed (also the older "drop-in" packaging)
:earlier
echo    an earlier version of this multiplayer is in the game: it is replaced
rem (it brought its own steam_appid.txt, unless this folder's notes say the file was there before it)
if exist "%MOD%\state\enabled.txt" goto :earlier_loose
if exist "%GAME%\steam_appid.txt" >"%MOD%\state\steam_appid_added.txt" echo mpmod1 put steam_appid.txt into the game folder; Disable_Multiplayer.bat removes it.
:earlier_loose
if exist "%GAME%\Remove_Multiplayer.bat" del /f /q "%GAME%\Remove_Multiplayer.bat"
if exist "%GAME%\Multiplayer_README.txt" del /f /q "%GAME%\Multiplayer_README.txt"
exit /b 0

rem ---- %1 = a program file of ours; one of that name in the game that is not ours is kept
:keep
if defined HAD exit /b 0
if not exist "%GAME%\root\programs\%~1" exit /b 0
fc /b "%MOD%\files\root\programs\%~1" "%GAME%\root\programs\%~1" >nul 2>nul && exit /b 0
if exist "%MOD%\state\backup\%~1" exit /b 0
if not exist "%MOD%\state\backup\" mkdir "%MOD%\state\backup" || exit /b 1
copy /y "%GAME%\root\programs\%~1" "%MOD%\state\backup\%~1" >nul || exit /b 1
echo    the game already had a root\programs\%~1 of its own: it is kept in
echo    mpmod1\state\backup and Disable_Multiplayer.bat puts it back
exit /b 0

:no_game
echo  This folder is not inside a game folder.
echo.
echo  Put the whole "mpmod1" folder into the folder of Graviteam Tactics: Mius-Front - the
echo  one that contains starter_x64.exe - and run Enable_Multiplayer.bat from there.
echo  (Steam: right-click the game, Manage, Browse local files.)
echo.
echo  It is now in: "%GAME%"
set "RC=3"
goto :finish

:incomplete
echo  The mpmod1 folder is incomplete: its "files" folder is missing files. Copy the whole
echo  folder again.
set "RC=1"
goto :finish

:running
echo  The game is running. Close it, then run this again.
set "RC=4"
goto :finish

:cancelled
echo.
echo  Nothing was changed.
set "RC=2"
goto :finish

:failed
echo.
echo  A file could not be copied into the game folder. Is the game still running, or is
echo  the folder write-protected? Multiplayer may be only partly installed now: run
echo  Disable_Multiplayer.bat to take it out again, or this file again to complete it.
set "RC=1"
goto :finish

:finish
echo.
if not defined QUIET pause
exit /b %RC%
