@echo off
rem Builds the MiusPlayer launcher: build\MiusPlayerLauncher.exe (one exe, .NET Framework 4.x, no other files).
rem Uses the C# compiler that comes with Windows' .NET Framework 4 (also on GitHub Actions windows-latest).
rem The exe carries the release's version (release.json next to this folder, or in ..\repo) and app.manifest
rem (an ordinary program for its user, Windows 7 to 11) - a program with real version details and a manifest is
rem less often taken for something it is not by virus scanners.
setlocal EnableExtensions
set "HERE=%~dp0"
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
  echo csc.exe of the .NET Framework 4 was not found.
  exit /b 1
)
if not exist "%HERE%build\" mkdir "%HERE%build" || exit /b 1
set "REL=%HERE%..\release.json"
if not exist "%REL%" set "REL=%HERE%..\repo\release.json"
set "VER=0.0.0"
if exist "%REL%" for /f "usebackq delims=" %%v in (`powershell -NoProfile -Command "(Get-Content -Raw '%REL%' | ConvertFrom-Json).version"`) do set "VER=%%v"
> "%HERE%build\Version.cs" (
  echo // made by build_launcher.bat from release.json
  echo using System.Reflection;
  echo [assembly: AssemblyVersion^("%VER%.0"^)]
  echo [assembly: AssemblyFileVersion^("%VER%.0"^)]
  echo [assembly: AssemblyInformationalVersion^("%VER%"^)]
)
"%CSC%" /nologo /target:winexe /platform:anycpu /optimize+ /warnaserror- /utf8output ^
  /out:"%HERE%build\MiusPlayerLauncher.exe" /win32manifest:"%HERE%app.manifest" ^
  /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll ^
  /r:System.Web.Extensions.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll ^
  "%HERE%src\*.cs" "%HERE%build\Version.cs"
if errorlevel 1 (
  echo Build FAILED.
  exit /b 1
)
echo Built "%HERE%build\MiusPlayerLauncher.exe" (version %VER%)
exit /b 0
