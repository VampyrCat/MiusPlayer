# Saves a half-size screenshot of each running game window: shot.ps1 <output folder>
param([string]$OutDir = "$PSScriptRoot\..\shots")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class GtWin {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }
}
"@
[void][GtWin]::SetProcessDPIAware()
New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ($p in Get-Process starter_x64 -ErrorAction SilentlyContinue) {
    if ($p.MainWindowHandle -eq 0) { "pid $($p.Id): no window"; continue }
    [void][GtWin]::ShowWindow($p.MainWindowHandle, 9)
    [void][GtWin]::SetForegroundWindow($p.MainWindowHandle)
    Start-Sleep -Milliseconds 1200
    $r = New-Object GtWin+RECT
    [void][GtWin]::GetWindowRect($p.MainWindowHandle, [ref]$r)
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { "pid $($p.Id): empty rect"; continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $gr = [System.Drawing.Graphics]::FromImage($bmp)
    $gr.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w / 2)), ([int]($h / 2))
    $out = Join-Path $OutDir "game_$($p.Id).png"
    $small.Save($out)
    "pid $($p.Id): $($r.L),$($r.T) ${w}x${h} -> $out"
}
