# Checks that a battle keeps running at full speed when the game window is not in the foreground.
# Starts one instance (normal borderless window, normal game speed), waits for the battle to run,
# then puts another window in front and compares frames and simulated time per real second.
param([int]$Warmup = 80)
Add-Type @"
using System; using System.Runtime.InteropServices;
public class GtFocus {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  public struct RECT { public int L, T, R, B; }
  // Bring a window to the front the way alt-tab would, from a process that is not in the foreground.
  public static void Front(IntPtr target) {
    uint me = GetCurrentThreadId(), fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
    AttachThreadInput(me, fg, true); SetForegroundWindow(target); AttachThreadInput(me, fg, false);
  }
}
"@
$game = (Resolve-Path "$PSScriptRoot\..\..").Path
if (Get-Process starter_x64 -ErrorAction SilentlyContinue) { "A game instance is already running; test skipped."; exit 2 }
Get-ChildItem "$game\out" -Filter "mp_trace*.txt" | ForEach-Object { [IO.File]::Delete($_.FullName) }
Copy-Item "$game\_mp_research\test_battle_image.cfgpack" "$game\data\common\shared\config\tacgame_image.cfgpack" -Force
$env:MP_DEBUG = "1"; $env:MP_WINDOW = $null; $env:MP_LOCKSTEP = $null
$p = Start-Process "$game\starter_x64.exe" -WorkingDirectory $game -PassThru -ArgumentList "root\programs\tacgame.progpack, data\common\shared\config\tacgame_image.cfgpack, users\profiles, cloud, *:*, 0, 1"
$env:MP_DEBUG = $null
Start-Sleep $Warmup
$p.Refresh()
$trace = "$game\out\mp_trace_tacgame_$($p.Id).txt"
function Sample {
    $last = Get-Content $trace -Tail 1
    if ($last -match 'sim=(\d+) step=\d+ frame=(\d+)') { @{ sim = [double]$Matches[1] / 1000; frame = [double]$Matches[2] } } else { @{ sim = 0; frame = 0 } }
}
function Measure-Run($label) {
    $a = Sample; Start-Sleep 20; $b = Sample
    "{0}: game in foreground = {1}, {2:N0} frames/s, {3:N1} simulated s per real s" -f $label,
        ([GtFocus]::GetForegroundWindow() -eq $p.MainWindowHandle), (($b.frame - $a.frame) / 20), (($b.sim - $a.sim) / 20)
}
$r = New-Object GtFocus+RECT; [void][GtFocus]::GetWindowRect($p.MainWindowHandle, [ref]$r)
$style = [GtFocus]::GetWindowLong($p.MainWindowHandle, -16)
"window: $($r.L),$($r.T) $($r.R - $r.L)x$($r.B - $r.T), title bar: $(($style -band 0x00C00000) -eq 0x00C00000), borderless popup: $(($style -band 0x80000000) -ne 0)"
[GtFocus]::Front($p.MainWindowHandle); Start-Sleep 2
Measure-Run "focused  "
$other = Start-Process notepad.exe -PassThru
Start-Sleep 2; $other.Refresh()
[GtFocus]::Front($other.MainWindowHandle); Start-Sleep 2
Measure-Run "alt-tabbed"
if (-not $other.HasExited) { $other.Kill() }
if (-not $p.HasExited) { [void]$p.CloseMainWindow(); Start-Sleep 5; $p.Refresh(); if (-not $p.HasExited) { $p.Kill() } }
