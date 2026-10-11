@echo off
rem Builds the multiplayer module and the join launcher into .\build and copies them into the drop-in folder
rem ..\Multiplayer_DropIn (needs Visual Studio 2022 C++ tools). The program definitions in that folder come
rem from ..\tools\make_progpack.py. Install into this copy of the game with ..\enable_multiplayer.bat.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /LD /MD /O1 /EHa /W3 /Fo"build\\" basic_network.cpp session.cpp ui.cpp state.cpp statesync.cpp shots.cpp trench.cpp xlate.cpp /link user32.lib gdi32.lib ws2_32.lib ole32.lib oleaut32.lib /OUT:build\basic_network.codelib /IMPLIB:build\basic_network.lib || exit /b 1
cl /nologo /MT /O1 /EHsc /W3 /Fo"build\\" mp_join.cpp /link ws2_32.lib /OUT:build\mp_join.exe || exit /b 1
if not exist "..\Multiplayer_DropIn\bin\rel\x64" mkdir "..\Multiplayer_DropIn\bin\rel\x64"
copy /y "build\basic_network.codelib" "..\Multiplayer_DropIn\bin\rel\x64\" >nul
copy /y "build\mp_join.exe" "..\Multiplayer_DropIn\" >nul
