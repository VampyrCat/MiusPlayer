@echo off
rem Removes the multiplayer files from this copy of the game, using the drop-in folder's own uninstaller.
rem Stock files were never changed.
set G=%~dp0..
copy /y "%~dp0Multiplayer_DropIn\Remove_Multiplayer.bat" "%G%\Remove_Multiplayer.bat" >nul
call "%G%\Remove_Multiplayer.bat" /quiet
del "%G%\Remove_Multiplayer.bat"
