# Puts the program's window over one whole monitor, without a frame, the way the console shows it (ExCorvus,
# 2026-11-27). Measured in the executable: the program creates one ordinary window (style 0xcf0000, Windows'
# default position, its own size) and its window function is only a jump to DefWindowProcA, so it never asks for
# fullscreen and reacts to no key; on the console its window is the screen by default. This script does it from
# outside, through Windows, for a visible game test: the weave and the program are not changed.
#
#   scripts\fullscreen_window.ps1 -ProcessId <id> -Log <file> [-Monitor \\.\DISPLAY1] [-WaitSeconds 120]
#
# It waits until the process shows its window of class sgaWindowClass, writes to -Log which monitor Windows had put
# the window on and where, then sets the window's style to a frameless popup and its place to the whole of -Monitor
# (the output the weave names to the program, IDXGIAdapter::EnumOutputs(0)), and writes the result.
param(
    [Parameter(Mandatory)][int]$ProcessId,
    [Parameter(Mandatory)][string]$Log,
    [string]$Monitor = '\\.\DISPLAY1',
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
