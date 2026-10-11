@echo off
rem Copies the drop-in folder into this copy of the game (the same thing a player does by hand).
xcopy /y /e /q "%~dp0Multiplayer_DropIn\*" "%~dp0..\" >nul || (echo Install failed - is the game running? & exit /b 1)
echo Multiplayer installed.
