@echo off
rem mpmod2: builds the multiplayer module and the join launcher into .\build (needs the Visual Studio C++ tools,
rem 2019 or later - found with vswhere, as on GitHub's build machines). In the modding copy the results are also
rem copied into the drop-in folder ..\dropin (installed into a game with ..\enable_mpmod2.bat).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || (echo No Visual Studio C++ tools found & exit /b 1)
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /LD /MD /O1 /EHa /W3 /Fo"build\\" basic_network.cpp build.cpp session.cpp ui.cpp ui_d3d11.cpp state.cpp statesync.cpp shots.cpp trench.cpp xlate.cpp anim.cpp hits.cpp endgame.cpp steam.cpp orders.cpp vdamage.cpp stats.cpp guns.cpp fires.cpp /link user32.lib gdi32.lib ws2_32.lib ole32.lib oleaut32.lib /OUT:build\basic_network.codelib /IMPLIB:build\basic_network.lib || exit /b 1
cl /nologo /MT /O1 /EHsc /W3 /Fo"build\\" mp_join.cpp /link ws2_32.lib /OUT:build\mp_join.exe || exit /b 1
if not exist "..\dropin" exit /b 0
if not exist "..\dropin\bin\rel\x64" mkdir "..\dropin\bin\rel\x64"
copy /y "build\basic_network.codelib" "..\dropin\bin\rel\x64\" >nul
copy /y "build\mp_join.exe" "..\dropin\" >nul
