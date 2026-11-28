# Part 2 of the game test: starts the program from your game copy inside the weave, and nothing else - no
# measurement runs beside it (ExCorvus, 2026-11-28: the measurements and the program start separately). You can
# start it yourself: Windows then treats the program as one you started, and lets its window come to the front.
#
#   scripts\run_program.ps1 -Game <your game folder> [-Seconds 120] [-Build] [-Hidden] [-Out <folder>]
#
#   -Seconds  how long the program runs before the boot tool ends it (default 120).
#   -Build    build the weave and run its sample tests first (default: the build in weave\build as it is).
#   -Hidden   start it hidden (default: where you can see it, with a second window that shows its log).
#
# The run's record goes to <Out>\run-<time> (default %LOCALAPPDATA%\whiwhi\game-tests): its log, the call and
# wants ledgers, what scripts\fullscreen_window.ps1 did, and run.txt (build, commit, times, whether the game folder
# stayed unchanged). scripts\game_test.ps1 -Run <that folder> judges it. Nothing of the game is written anywhere.
param(
    [string]$Game = $env:WHIWHI_GAME,
    [int]$Seconds = 120,
    [switch]$Build,
    [switch]$Hidden,
    [string]$Out,
    [string]$StartedBy = 'by hand'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test_common.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo 'weave\build'
$g = Resolve-GameFolder $Game
$Game = $g.folder
$exe = $g.exe
$outFull = Get-TestOut $Out
if ($outFull.StartsWith($Game, [StringComparison]::OrdinalIgnoreCase)) { throw 'the records must not go inside the game folder' }
if (Get-Process boot_image -ErrorAction SilentlyContinue) { throw 'a run is already going (one at a time)' }

if ($Build) {
    $buildLog = & (Join-Path $repo 'weave\build.cmd') 2>&1
    if (-not ($buildLog -match '100% tests passed')) { $buildLog | Select-Object -Last 15; throw 'the build or its sample tests failed: the program was not started' }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$run = Join-Path $outFull "run-$stamp"
$work = Join-Path $outFull 'work'
New-Item -ItemType Directory -Force -Path $run, $work | Out-Null
Get-ChildItem -LiteralPath $work -File | Remove-Item -Force -ErrorAction Stop
Copy-Item (Join-Path $buildDir '*.exe'), (Join-Path $buildDir '*.dll') $work -Force -ErrorAction Stop
# The run must use exactly the build it is recorded for: every file in the work folder is compared with the build's
# (measured 2026-11-27 19:37: one library in the work folder stayed an older copy and the run used it).
$stale = @(Get-ChildItem -LiteralPath $work -File | Where-Object {
        (Get-FileHash -LiteralPath $_.FullName).Hash -ne (Get-FileHash -LiteralPath (Join-Path $buildDir $_.Name)).Hash })
if ($stale.Count) {
    throw "the work folder does not hold the build as built: $(($stale | ForEach-Object Name) -join ', ') - nothing was run"
}
# A library the installation ships itself is loaded from the installation, not from the weave's build: the weave's
# file of the same name would win (host_libraries.cpp loads from beside boot_image.exe first).
Get-ChildItem -LiteralPath $work -File | Where-Object { Test-Path -LiteralPath (Join-Path $Game $_.Name) } |
    ForEach-Object { Write-Host "the installation ships $($_.Name) itself - the weave's file is left out"; Remove-Item -LiteralPath $_.FullName -Force }
# The build is recorded as built (the commit gate checks the build folder), not as the work folder holds it.
$buildHash = Get-BuildHash $buildDir
$commit = (git -C $repo rev-parse --short HEAD).Trim()
$dirty = @(git -C $repo status --porcelain -- weave scripts).Count
$sha = [Security.Cryptography.SHA256]::Create()
$measureKey = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes((Get-MeasureKey $repo $Game $exe).TrimEnd()))).Replace('-', '')
$before = Get-Listing $Game
$exeHashBefore = (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash

$log = Join-Path $run 'run.err'
$env:WHIWHI_CALL_LEDGER = Join-Path $run 'calls.tsv'
$env:WHIWHI_WANTS_LEDGER = Join-Path $run 'wants.txt'
$env:DISABLE_RTSS_LAYER = '1'
$env:DISABLE_VULKAN_OBS_CAPTURE = '1'
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $work 'boot_image.exe') -WorkingDirectory $work -PassThru `
    -WindowStyle $(if ($Hidden) { 'Hidden' } else { 'Normal' }) `
    -ArgumentList "`"$($exe.FullName)`"", "`"$Game`"", '--seconds', "$Seconds" `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $run 'run.out')
Remove-Item Env:\WHIWHI_CALL_LEDGER, Env:\WHIWHI_WANTS_LEDGER
if (-not $Hidden) {
    $logWindow = Start-Process -FilePath 'powershell.exe' -PassThru -WindowStyle Minimized -ArgumentList '-NoProfile', '-Command',
        "`$host.UI.RawUI.WindowTitle = 'program log'; Get-Content -LiteralPath '$log' -Wait -Tail 50"
    # The console shows the program's window as the whole screen; Windows is asked for that from outside
    # (scripts\fullscreen_window.ps1). What it did is written to fullscreen.txt in the run's folder.
    Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', "`"$(Join-Path $PSScriptRoot 'fullscreen_window.ps1')`"", '-ProcessId', "$($p.Id)",
        '-Log', "`"$(Join-Path $run 'fullscreen.txt')`"", '-ProgramLog', "`"$log`"", '-Away', "`"program log`""
}
try { $p.PriorityClass = 'Normal' } catch {}
if (-not $p.WaitForExit(($Seconds + 60) * 1000)) { try { $p.Kill() } catch {}; $p.WaitForExit(15000) | Out-Null }
$elapsed = [int]$sw.Elapsed.TotalSeconds
# The log window follows the log only while the program runs (the whole log stays in run.err).
if ($logWindow) { Start-Sleep -Seconds 5; try { $logWindow.Kill() } catch {} }
$unchanged = ($before -eq (Get-Listing $Game)) -and ($exeHashBefore -eq (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash)

@("time=$stamp", "commit=$commit", "uncommitted_weave_files=$dirty", "build=$buildHash", "seconds=$Seconds",
    "elapsed=$elapsed", "game_folder_unchanged=$unchanged", "measure_key=$measureKey", "started=$StartedBy",
    "game=$Game") | Set-Content -LiteralPath (Join-Path $run 'run.txt') -Encoding utf8
Write-Output "the program ran $elapsed s; game folder unchanged: $unchanged; record: $run"
if (-not $unchanged) { Write-Output 'THE GAME FOLDER CHANGED' }
