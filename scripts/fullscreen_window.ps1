# Puts the program's window over one whole monitor, without a frame, the way the console shows it (ExCorvus,
# 2026-11-27). Measured in the executable: the program creates one ordinary window (style 0xcf0000, Windows'
# default position, its own size) and its window function is only a jump to DefWindowProcA, so it never asks for
# fullscreen and reacts to no key; on the console its window is the screen by default. This script does it from
# outside, through Windows, for a visible game test: the weave and the program are not changed.
#
#   scripts\fullscreen_window.ps1 -ProcessId <id> -Log <file> [-ProgramLog <run.err>] [-Monitor \\.\DISPLAYn]
#                                 [-Away <window title>] [-WaitSeconds 120]
#
# It waits until the process shows its window of class sgaWindowClass, writes to -Log which monitor Windows had put
# the window on and where, then sets the window's style to a frameless popup and its place to the whole of the
# program's screen, and writes the result. The program's screen is the output the weave names to it
# (IDXGIAdapter::EnumOutputs(0), the first screen of the graphics card it draws on), read from -ProgramLog; without
# one, -Monitor; without that, the monitor Windows put the window on. (Measured 2026-11-28: after the screens were
# plugged anew, the card's screen was \\.\DISPLAY6, no longer \\.\DISPLAY1, which had been fixed here.)
# On the console nothing else is on the program's screen, and nothing else has the focus when the program starts
# (ExCorvus, 2026-11-28): a window titled -Away (the live log of the run, started minimised so that it never takes
# the focus) is shown on another screen without being made active, and then the program's window is brought to
# the front.
param(
    [Parameter(Mandatory)][int]$ProcessId,
    [Parameter(Mandatory)][string]$Log,
    [string]$ProgramLog,
    [string]$Monitor,
    [string]$Away,
    [int]$WaitSeconds = 120
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class FullscreenWindow {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] public struct MONITORINFOEX {
        public int Size; public RECT Monitor; public RECT Work; public int Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Device;
    }
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    public delegate bool MonitorProc(IntPtr m, IntPtr dc, ref RECT r, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr m, ref MONITORINFOEX i);
    [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonitorProc f, IntPtr p);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int index);
    [DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtr(IntPtr h, int index, IntPtr value);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);

    public static IntPtr Titled(string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var text = new StringBuilder(256); GetWindowText(h, text, 256);
            if (IsWindowVisible(h) && text.ToString().Contains(title)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static string Other(string device) {
        string other = null;
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr m, IntPtr dc, ref RECT r, IntPtr p) => {
            string d = Info(m).Device;
            if (!string.Equals(d, device, StringComparison.OrdinalIgnoreCase)) { other = d; return false; }
            return true;
        }, IntPtr.Zero);
        return other;
    }

    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            var name = new StringBuilder(64); GetClassName(h, name, 64);
            if (owner == pid && name.ToString() == "sgaWindowClass" && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static MONITORINFOEX Info(IntPtr m) {
        var i = new MONITORINFOEX(); i.Size = Marshal.SizeOf(typeof(MONITORINFOEX)); GetMonitorInfo(m, ref i); return i;
    }
    public static IntPtr Named(string device) {
        IntPtr found = IntPtr.Zero;
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr m, IntPtr dc, ref RECT r, IntPtr p) => {
            if (string.Equals(Info(m).Device, device, StringComparison.OrdinalIgnoreCase)) { found = m; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
# Real pixels on every monitor (per-monitor aware, version 2), so that a scaled second monitor does not shift the
# numbers this script reads and sets.
[FullscreenWindow]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
function Note([string]$text) { Add-Content -LiteralPath $Log -Value ("[fullscreen] " + $text) }
function Place($r) { "{0},{1} {2} x {3}" -f $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T) }

$window = [IntPtr]::Zero
$until = (Get-Date).AddSeconds($WaitSeconds)
while ($window -eq [IntPtr]::Zero -and (Get-Date) -lt $until) {
    if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) { Note 'the program ended before it showed its window'; exit 0 }
    Start-Sleep -Milliseconds 250
    $window = [FullscreenWindow]::Find([uint32]$ProcessId)
}
if ($window -eq [IntPtr]::Zero) { Note "no visible window of the program within $WaitSeconds s - nothing done"; exit 0 }

$r = New-Object FullscreenWindow+RECT
[FullscreenWindow]::GetWindowRect($window, [ref]$r) | Out-Null
$on = [FullscreenWindow]::Info([FullscreenWindow]::MonitorFromWindow($window, 2))
$style = [FullscreenWindow]::GetWindowLongPtr($window, -16).ToInt64()
Note ("the program's window as Windows placed it: {0}, style 0x{1:x}, on {2} ({3})" -f (Place $r), $style, $on.Device, (Place $on.Monitor))

$named = $null
$asked = Get-Date
while (-not $Monitor -and $ProgramLog -and -not $named -and ((Get-Date) - $asked).TotalSeconds -lt 5) {
    if (Test-Path -LiteralPath $ProgramLog) {
        $named = Select-String -LiteralPath $ProgramLog -Pattern 'EnumOutputs\(0\) - Windows answered 0x00000000, (\\\\\.\\DISPLAY\d+)' |
            Select-Object -First 1
    }
    if (-not $named) { Start-Sleep -Milliseconds 250 }
}
if (-not $Monitor -and $named) {
    if ($named) {
        $Monitor = $named.Matches[0].Groups[1].Value
        Note "the program's screen, as the weave named it to the program: $Monitor"
    }
}
if (-not $Monitor) {
    $Monitor = $on.Device
    Note "the program's screen: not named in its log - the monitor Windows put the window on, $Monitor"
}
$target = [FullscreenWindow]::Named($Monitor)
if ($target -eq [IntPtr]::Zero) { Note "no monitor named $Monitor - nothing done"; exit 0 }
$m = ([FullscreenWindow]::Info($target)).Monitor
$WS_POPUP = 0x80000000L; $WS_VISIBLE = 0x10000000L; $WS_OVERLAPPEDWINDOW = 0xcf0000L
$newStyle = ($style -band (-bnot $WS_OVERLAPPEDWINDOW)) -bor $WS_POPUP -bor $WS_VISIBLE
[FullscreenWindow]::SetWindowLongPtr($window, -16, [IntPtr]$newStyle) | Out-Null
# SWP_FRAMECHANGED 0x20 | SWP_NOOWNERZORDER 0x200 | SWP_SHOWWINDOW 0x40
$ok = [FullscreenWindow]::SetWindowPos($window, [IntPtr]::Zero, $m.L, $m.T, $m.R - $m.L, $m.B - $m.T, 0x260)
[FullscreenWindow]::GetWindowRect($window, [ref]$r) | Out-Null
$on = [FullscreenWindow]::Info([FullscreenWindow]::MonitorFromWindow($window, 2))
Note ("set frameless over {0}: {1}; now {2}, style 0x{3:x}, on {4}" -f $Monitor, $(if ($ok) { 'done' } else { 'Windows refused' }),
    (Place $r), [FullscreenWindow]::GetWindowLongPtr($window, -16).ToInt64(), $on.Device)
# On the console the program's window is the screen, in front. Windows lets a process bring a window to the front
# only in some cases; the answer is written down as it comes.
if ($Away) {
    $other = [FullscreenWindow]::Other($Monitor)
    $awayWindow = [FullscreenWindow]::Titled($Away)
    if ($awayWindow -ne [IntPtr]::Zero -and $other) {
        $o = ([FullscreenWindow]::Info([FullscreenWindow]::Named($other))).Work
        [FullscreenWindow]::ShowWindow($awayWindow, 4) | Out-Null  # SW_SHOWNOACTIVATE: shown, not made active
        # SWP_NOZORDER 0x4 | SWP_NOACTIVATE 0x10
        $moved = [FullscreenWindow]::SetWindowPos($awayWindow, [IntPtr]::Zero, $o.L, $o.T, [Math]::Min(1600, $o.R - $o.L), [Math]::Min(900, $o.B - $o.T), 0x14)
        Note ("the window '{0}' shown on {1}: {2}" -f $Away, $other, $(if ($moved) { 'done' } else { 'Windows refused' }))
    }
}
$front = [FullscreenWindow]::SetForegroundWindow($window)
Note ("brought to the front: {0}" -f $(if ($front) { 'yes' } else { 'Windows refused' }))

# Then, as long as the program runs: every change of the window's place, style, front state or responsiveness is
# written down with its time (seconds since this script found the window), and a place other than the monitor is
# set back to the monitor once each time it changes (also written down).
$found = Get-Date
$last = ''
while (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) {
    Start-Sleep -Milliseconds 500
    if (-not [FullscreenWindow]::GetWindowRect($window, [ref]$r)) { break }
    $style = [FullscreenWindow]::GetWindowLongPtr($window, -16).ToInt64()
    $isFront = [FullscreenWindow]::GetForegroundWindow() -eq $window
    $hung = [FullscreenWindow]::IsHungAppWindow($window)
    $now = "{0}, style 0x{1:x}, in front: {2}, not responding: {3}" -f (Place $r), $style, $isFront, $hung
    if ($now -ne $last) {
        $t = ((Get-Date) - $found).TotalSeconds
        Note ("{0:0.0} s: {1}" -f $t, $now)
        $last = $now
        if ($r.L -ne $m.L -or $r.T -ne $m.T -or $r.R -ne $m.R -or $r.B -ne $m.B) {
            $ok = [FullscreenWindow]::SetWindowPos($window, [IntPtr]::Zero, $m.L, $m.T, $m.R - $m.L, $m.B - $m.T, 0x260)
            Note ("{0:0.0} s: set back over {1}: {2}" -f $t, $Monitor, $(if ($ok) { 'done' } else { 'Windows refused' }))
        }
    }
}
Note 'the program ended'
