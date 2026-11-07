@echo off
rem Builds the MiusPlayer launcher: build\MiusPlayerLauncher.exe (one exe, .NET Framework 4.x, no other files).
rem Uses the C# compiler that comes with Windows' .NET Framework 4 (also on GitHub Actions windows-latest).
setlocal EnableExtensions
set "HERE=%~dp0"
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
  echo csc.exe of the .NET Framework 4 was not found.
  exit /b 1
)
if not exist "%HERE%build\" mkdir "%HERE%build" || exit /b 1
"%CSC%" /nologo /target:winexe /platform:anycpu /optimize+ /warnaserror- /utf8output ^
  /out:"%HERE%build\MiusPlayerLauncher.exe" ^
  /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll ^
  /r:System.Web.Extensions.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll ^
  "%HERE%src\*.cs"
if errorlevel 1 (
  echo Build FAILED.
  exit /b 1
)
echo Built "%HERE%build\MiusPlayerLauncher.exe"
exit /b 0
