# Part 1 of the game test: the measurements (measure/README.md) on your game copy, read-only, on their own - the
# program is not started here. Each script checks results of FINDINGS.md on the file its usage line names and says
# `same` or `DIFFERS`.
#
#   scripts\measure_test.ps1 -Game <your game folder> [-Again] [-Out <folder>]
#
#   -Again  measure even when nothing they read has changed.
#
# They run only when something they read has changed (ExCorvus, 2026-11-28: "only measure when it has changed"):
# the game folder (names, sizes, times), the exe, a script in measure/, or the weave's PC graphics library. The
# results stay in <Out>\measure-cache (measure.txt, fail.txt, key.txt, from.txt); scripts\game_test.ps1 judges a
# run only against results whose key is the one of that moment. Nothing of the game is written anywhere.
param(
    [string]$Game = $env:WHIWHI_GAME,
    [switch]$Again,
    [string]$Out
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test_common.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'weave\build'
$g = Resolve-GameFolder $Game
$Game = $g.folder
$outFull = Get-TestOut $Out
if ($outFull.StartsWith($Game, [StringComparison]::OrdinalIgnoreCase)) { throw 'the records must not go inside the game folder' }
$cache = Join-Path $outFull 'measure-cache'
New-Item -ItemType Directory -Force -Path $cache | Out-Null

$key = Get-MeasureKey $repo $Game $g.exe
$keyFile = Join-Path $cache 'key.txt'
if (-not $Again -and (Test-Path -LiteralPath $keyFile) -and ((Get-Content -LiteralPath $keyFile -Raw).TrimEnd() -eq $key.TrimEnd())) {
    $from = (Get-Content -LiteralPath (Join-Path $cache 'from.txt') -Raw).Trim()
    $failing = @(Get-Content -LiteralPath (Join-Path $cache 'fail.txt') -ErrorAction SilentlyContinue | Where-Object { $_ })
    Write-Output "measurements: nothing they read has changed since $from - not measured again ($($failing.Count) failing)"
    $failing | ForEach-Object { Write-Output "  DIFFERS: $_" }
    if ($failing.Count) { exit 1 }
    exit 0
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $cache 'measure.txt'
Set-Content -LiteralPath $log -Value "---- measured $stamp" -Encoding utf8
$fail = New-Object System.Collections.Generic.List[string]
$tools = Join-Path $outFull 'measure-tools'
$ErrorActionPreference = 'Continue'  # a line a tool writes to its error output is part of its record, not a stop
foreach ($script in Get-ChildItem -LiteralPath (Join-Path $repo 'measure') -File | Where-Object { $_.Extension -in '.py', '.cpp' } | Sort-Object Name) {
    $text = Get-Content -LiteralPath $script.FullName -Raw
    $usage = [regex]::Match($text, 'game folder>\\+([A-Za-z0-9_.]+)')
    if (-not $usage.Success) {
        # The shared readers name no file: they are not measurements. A script that checks results and names no
        # file would be skipped without a word, so that is a failure, not a skip.
        if ($text -match '(?m)(\.same\(|^\s*same\(")') { $fail.Add("measure/$($script.Name) names no file of the game folder in its usage line") }
        continue
    }
    $measureInput = Join-Path $Game $usage.Groups[1].Value
    Add-Content -LiteralPath $log -Value "---- $($script.Name) on $($usage.Groups[1].Value)" -Encoding utf8
    if ($script.Extension -eq '.py') {
        $output = & python $script.FullName $measureInput 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } else {
        # A C++ measurement is compiled with the same compiler as the weave and run beside the PC graphics
        # library of the weave's build.
        New-Item -ItemType Directory -Force -Path $tools | Out-Null
        Copy-Item (Join-Path $build 'd3d12_vk.dll'), (Join-Path $build 'd3d12core.dll') $tools -Force
        $vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
        $compiled = & cmd /c "`"$vcvars`" >nul && cd /d `"$tools`" && cl /nologo /std:c++17 /EHsc /O2 `"$($script.FullName)`"" 2>&1 | ForEach-Object { "$_" }
        $tool = Join-Path $tools ($script.BaseName + '.exe')
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $tool)) {
            $output = @('not compiled:') + $compiled
            $code = 1
        } else {
            $output = & $tool $measureInput 2>&1 | ForEach-Object { "$_" }
            $code = $LASTEXITCODE
        }
    }
    Add-Content -LiteralPath $log -Value $output -Encoding utf8
    $last = @($output | Where-Object { $_ -match 'the same as FINDINGS' }) | Select-Object -Last 1
    Write-Output "measure/$($script.Name): $(if ($last) { $last } else { "exit $code" })"
    if ($code -ne 0) { $fail.Add("measure/$($script.Name): $(if ($last) { $last } else { "exit $code" }) (measure.txt)") }
}
$ErrorActionPreference = 'Stop'
# The key is taken again after measuring: a game folder that changed while it was read is not this key.
if ((Get-MeasureKey $repo $Game $g.exe) -ne $key) { $fail.Add('the game folder, the exe, measure/ or the PC graphics library changed while they were measured') }
Set-Content -LiteralPath (Join-Path $cache 'fail.txt') -Value @($fail) -Encoding utf8
Set-Content -LiteralPath (Join-Path $cache 'from.txt') -Value "the measurement of $stamp" -Encoding utf8
Set-Content -LiteralPath $keyFile -Value $key -Encoding utf8
Write-Output "measurements: done ($($fail.Count) failing) - $log"
$fail | ForEach-Object { Write-Output "  DIFFERS: $_" }
if ($fail.Count) { exit 1 }
