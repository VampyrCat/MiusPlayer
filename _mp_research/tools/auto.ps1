# Helpers for driving the game in automated tests: start it, bring it to the front, click, type, take
# screenshots and wait for lines in the multiplayer log. Dot-source this file:  . "$PSScriptRoot\auto.ps1"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public class GtAuto {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  public struct POINT { public int X, Y; }
  public struct RECT { public int L, T, R, B; }
  public static void Front(IntPtr target) {
    uint me = GetCurrentThreadId(), fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
    AttachThreadInput(me, fg, true); SetForegroundWindow(target); AttachThreadInput(me, fg, false);
  }
  public static void Key(byte vk, bool down) { keybd_event(vk, (byte)MapVirtualKey(vk, 0), down ? 0u : 2u, UIntPtr.Zero); }
}
"@
[void][GtAuto]::SetProcessDPIAware()
$script:Game = (Resolve-Path "$PSScriptRoot\..\..").Path
$script:Shots = "$PSScriptRoot\..\shots"
New-Item -ItemType Directory -Force $script:Shots | Out-Null

function Start-Game([string]$Arguments = "", [hashtable]$Env = @{}) {
    $Env = $Env.Clone(); $Env["MP_TEST"] = "1"
    foreach ($k in $Env.Keys) { Set-Item "env:$k" $Env[$k] }
    $p = if ($Arguments) { Start-Process "$script:Game\starter_x64.exe" -WorkingDirectory $script:Game -ArgumentList $Arguments -PassThru }
         else { Start-Process "$script:Game\starter_x64.exe" -WorkingDirectory $script:Game -PassThru }
    foreach ($k in $Env.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    $p
}

function Focus-Game($Process) { $Process.Refresh(); [GtAuto]::Front($Process.MainWindowHandle); Start-Sleep -Milliseconds 400 }

# Coordinates are fractions of the game window (0..1), so tests do not depend on the screen resolution.
function Click-Game($Process, [double]$fx, [double]$fy) {
    Focus-Game $Process
    $r = New-Object GtAuto+RECT; [void][GtAuto]::GetWindowRect($Process.MainWindowHandle, [ref]$r)
    $x = [int]($r.L + ($r.R - $r.L) * $fx); $y = [int]($r.T + ($r.B - $r.T) * $fy)
    $under = [GtAuto]::GetAncestor([GtAuto]::WindowFromPoint((New-Object GtAuto+POINT -Property @{ X = $x; Y = $y })), 2)
    if ($under -ne $Process.MainWindowHandle) { "click at $fx,$fy skipped: another window covers that spot"; return $false }
    [void][GtAuto]::SetCursorPos($x, $y); Start-Sleep -Milliseconds 250
    [GtAuto]::mouse_event(1, 2, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 120     # nudge, so raw input sees the move
    [GtAuto]::mouse_event(1, -2, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 120
    [GtAuto]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 120
    [GtAuto]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 300
    $true
}

function Press-Key($Process, [byte]$vk) {
    Focus-Game $Process
    if ([GtAuto]::GetForegroundWindow() -ne $Process.MainWindowHandle) { "key skipped: the game is not in the foreground"; return }
    [GtAuto]::Key($vk, $true); Start-Sleep -Milliseconds 90; [GtAuto]::Key($vk, $false); Start-Sleep -Milliseconds 120
}

function Type-Text($Process, [string]$text) {
    Focus-Game $Process
    if ([GtAuto]::GetForegroundWindow() -ne $Process.MainWindowHandle) { "typing skipped: the game is not in the foreground"; return }
    foreach ($ch in $text.ToCharArray()) {
        $vk = switch ($ch) { '.' { 0xBE } ':' { 0xBA } '-' { 0xBD } default { [byte][char]::ToUpper($ch) } }
        if ($ch -eq ':') { [GtAuto]::Key(0x10, $true) }
        [GtAuto]::Key([byte]$vk, $true); Start-Sleep -Milliseconds 60; [GtAuto]::Key([byte]$vk, $false); Start-Sleep -Milliseconds 60
        if ($ch -eq ':') { [GtAuto]::Key(0x10, $false) }
    }
}

# Asks the game itself for its current frame (needs MP_TEST=1 in the game's environment) and saves it as
# shots\<name>.png. Nothing outside the game is captured, and it works when the window is covered.
function Save-Shot($Process, [string]$name) {
    $bmp = "$script:Game\out\$name.bmp"
    if (Test-Path $bmp) { [IO.File]::Delete($bmp) }
    Set-Content "$script:Game\out\mp_cmd_$($Process.Id).txt" "shot $name" -Encoding ascii
    for ($i = 0; $i -lt 40 -and -not (Test-Path $bmp); $i++) { Start-Sleep -Milliseconds 250 }
    if (-not (Test-Path $bmp)) { return $null }
    Start-Sleep -Milliseconds 300
    $img = [System.Drawing.Image]::FromFile($bmp)
    $out = Join-Path $script:Shots "$name.png"
    $img.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose()
    [IO.File]::Delete($bmp)
    $out
}

# Same as the player pressing the game's Start button (end deployment / end initial orders).
function Send-Ready($Process) { Set-Content "$script:Game\out\mp_cmd_$($Process.Id).txt" "ready" -Encoding ascii; Start-Sleep -Milliseconds 700 }

# Ends the battle the way the game's own "leave for the operational phase" confirmation does.
function Send-Resign($Process) { Set-Content "$script:Game/out/mp_cmd_$($Process.Id).txt" "resign" -Encoding ascii; Start-Sleep -Milliseconds 700 }

# Test commands for the operational map: the hand-over button, and (host) ending the turn.
function Send-Command($Process, [string]$what) { Set-Content "$script:Game/out/mp_cmd_$($Process.Id).txt" $what -Encoding ascii; Start-Sleep -Milliseconds 700 }

function Get-MpLog($Process, [string]$program) { "$script:Game\out\basic_network_${program}_$($Process.Id).log" }

# Waits until a line matching $pattern appears in a log file; returns the line or $null on timeout.
function Wait-Log([string]$path, [string]$pattern, [int]$timeout = 60) {
    $deadline = (Get-Date).AddSeconds($timeout)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $path) {
            $m = Select-String -Path $path -Pattern $pattern | Select-Object -Last 1
            if ($m) { return $m.Line }
        }
        Start-Sleep -Milliseconds 500
    }
    $null
}

function Stop-Game($Process) {
    if (-not $Process) { return }
    $Process.Refresh()
    if (-not $Process.HasExited) { [void]$Process.CloseMainWindow(); Start-Sleep 4; $Process.Refresh(); if (-not $Process.HasExited) { $Process.Kill() } }
}

function Assert-NoGameRunning {
    if (Get-Process starter_x64 -ErrorAction SilentlyContinue) { "A game instance is already running; test skipped."; exit 2 }
}

$script:Results = @()
function Check([string]$what, [bool]$ok, [string]$detail = "") {
    $script:Results += [pscustomobject]@{ Result = $(if ($ok) { "PASS" } else { "FAIL" }); Check = $what; Detail = $detail }
    "{0}  {1}  {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $what, $detail
}
function Show-Results { ""; "{0} passed, {1} failed" -f @($script:Results | Where-Object Result -eq PASS).Count, @($script:Results | Where-Object Result -eq FAIL).Count }
