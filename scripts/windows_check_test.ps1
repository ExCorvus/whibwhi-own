# The "Windows accepts" game test: one run of the game test (scripts\game_test.ps1, the whole installation, its
# verdict and record as always) in which the weave also hands a copy of every root signature and pipeline it gives
# vkd3d to Windows' own Direct3D 12 on this PC (weave\tools\windows_check.cpp, loaded from System32 only, in a
# process of its own). Its result says, request by request, whether Windows accepts what the weave makes from the
# program, next to what vkd3d answered. The program's answers are the same as in the plain game test.
#
#   scripts\windows_check_test.ps1 -Game <your game folder> [-Seconds 120] [-NoBuild] [-Show]
#
# The result goes to %LOCALAPPDATA%\whiwhi\windows-check\<time>.txt (numbers and answers only, on your PC).
param(
    [string]$Game = $env:WHIWHI_GAME,
    [int]$Seconds = 120,
    [switch]$NoBuild,
    [switch]$Show
)
$ErrorActionPreference = 'Stop'
$folder = Join-Path $env:LOCALAPPDATA 'whiwhi\windows-check'
New-Item -ItemType Directory -Force -Path $folder | Out-Null
$result = Join-Path $folder ((Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')
$env:WHIWHI_WINDOWS_CHECK = $result
try {
    $arguments = @{ Game = $Game; Seconds = $Seconds; NoBuild = $NoBuild; Show = $Show }
    & (Join-Path $PSScriptRoot 'game_test.ps1') @arguments
} finally {
    Remove-Item Env:\WHIWHI_WINDOWS_CHECK -ErrorAction SilentlyContinue
}
# The helper finishes the copies still in its pipe after the run ends.
$helper = Get-Process windows_check -ErrorAction SilentlyContinue
if ($helper) { $helper | Wait-Process -Timeout 600 -ErrorAction SilentlyContinue }
if (Test-Path -LiteralPath $result) {
    Write-Output "Windows accepts - result: $result"
    Get-Content -LiteralPath $result | Where-Object { $_ -match 'Direct3D 12:|adapter|pipe closed|^windows check:   ' }
} else {
    Write-Output "Windows accepts - no result was written ($result): the helper did not start"
}
