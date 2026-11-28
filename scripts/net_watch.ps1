# Watches the network during a game test (AGENTS.md rule 3: nothing online): every 2 seconds, while boot_image.exe
# runs, it writes how many TCP connections and UDP endpoints the process holds and where they lead. Read only; it
# changes nothing. Start it before the game test, in its own (hidden) window:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\net_watch.ps1 [-Out <file>] [-Minutes 25]
#
# The record stays on this PC (default %LOCALAPPDATA%\whiwhi\net_watch.txt), never in the repository.
param(
    [string]$Out = (Join-Path $env:LOCALAPPDATA 'whiwhi\net_watch.txt'),
    [int]$Minutes = 25
)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null
Set-Content -LiteralPath $Out -Value "# network watch of boot_image.exe started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') (every 2 s: TCP, UDP, remote ends)" -Encoding utf8
$end = (Get-Date).AddMinutes($Minutes)
while ((Get-Date) -lt $end) {
    $p = Get-Process boot_image -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) {
        $t = @(Get-NetTCPConnection -OwningProcess $p.Id -ErrorAction SilentlyContinue)
        $u = @(Get-NetUDPEndpoint -OwningProcess $p.Id -ErrorAction SilentlyContinue)
        $ends = ($t | ForEach-Object { "$($_.RemoteAddress):$($_.RemotePort)/$($_.State)" }) -join ' '
        Add-Content -LiteralPath $Out -Value ("{0} pid {1} tcp {2} udp {3} {4}" -f (Get-Date -Format 'HH:mm:ss'), $p.Id, $t.Count, $u.Count, $ends) -Encoding utf8
    }
    Start-Sleep -Seconds 2
}
Add-Content -LiteralPath $Out -Value "# ended $(Get-Date -Format 'HH:mm:ss')" -Encoding utf8
