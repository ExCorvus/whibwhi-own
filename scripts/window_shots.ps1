# Takes pictures of the running program's window (class sgaWindowClass, owned by boot_image.exe) during a visible
# game test: one every 3 seconds once the window is visible, up to -Max. Start it before the game test, in its own
# (hidden) window:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\window_shots.ps1 [-Out <folder>] [-Max 12]
#
# The pictures show what the program drew, so they stay on this PC (default %LOCALAPPDATA%\whiwhi\window-shots, one
# folder per start, with log.txt), never in the repository (AGENTS.md rule 2).
param(
    [string]$Out = (Join-Path $env:LOCALAPPDATA 'whiwhi\window-shots'),
    [int]$Max = 12
)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public static IntPtr Find(uint pid, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner != pid) return true;
            var sb = new StringBuilder(128); GetClassName(h, sb, 128);
            if (sb.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
$dir = Join-Path $Out (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'log.txt'
$deadline = (Get-Date).AddSeconds(1200)
$taken = 0; $seen = $false; $t0 = Get-Date
while ((Get-Date) -lt $deadline -and $taken -lt $Max) {
    $p = Get-Process boot_image -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { if ($seen) { break }; Start-Sleep -Milliseconds 500; continue }
    $seen = $true
    $h = [Win]::Find([uint32]$p.Id, 'sgaWindowClass')
    if ($h -eq [IntPtr]::Zero -or -not [Win]::IsWindowVisible($h)) { Start-Sleep -Milliseconds 500; continue }
    $r = New-Object Win+RECT; [void][Win]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -gt 0 -and $hh -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap $w, $hh
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
        $sec = [int]((Get-Date) - $t0).TotalSeconds
        $file = Join-Path $dir ("window_{0:D3}s.png" -f $sec)
        $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
        "$sec s: $file ($w x $hh)" | Add-Content $log
        $taken++
    }
    Start-Sleep -Seconds 3
}
"done: $taken picture(s)" | Add-Content $log
