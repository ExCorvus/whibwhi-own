# The game test (AGENTS.md rule 11): runs the real game from your own copy inside the weave, once, and records
# what the game did - every call it made through its import table and how the weave answered it, which calls the
# weave said "not provided" to, where the run ended, frames and pipelines - then compares that with the last run
# you accepted, and with the map the project already has of the executable (docs/exe-map, docs/console-libs):
# every call of the run is looked up there, and one the map does not name is listed as "not in the map", to be
# read from the executable before anything is built for it (against-the-map.txt). Nothing of the game is written anywhere: the record holds names, counts and addresses only, and it
# stays on your PC (under -Out), never in the repository.
#
#   scripts\game_test.ps1 -Game <your game folder> [-Seconds 60] [-NoBuild] [-Accept] [-Show] [-Out <folder>]
#
#   -Game     the folder of your game copy (read-only; checked unchanged after the run). Or set WHIWHI_GAME.
#   -Seconds  how long the game runs (default 60).
#   -NoBuild  use the build in weave\build as it is (default: build and run the sample tests first).
#   -Accept   if the verdict is PASS, this run becomes the baseline later runs are compared with.
#   -Show     start the game where you can see it, and open a second window that shows its log as it is written
#             (default: the game starts hidden).
#   -Out      where the records go (default %LOCALAPPDATA%\whiwhi\game-tests).
#
# The game test is the real scenario: the whole installation runs as it is started, and the verdict judges the
# whole, not one error. Two rules are hard: FAIL when the game folder changed, or when a script in measure/ (run on
# the copy before the game starts) says a result of FINDINGS.md no longer holds. Beyond them the run is judged
# against the baseline's whole: every meeting point between the program and the weave (the wants ledger, whole.txt:
# attached, partly or missing) must be the same or better, every import provided in the baseline still provided.
# FAIL when anything got worse. Where the run ended, frames and pipelines are listed as notes: a single stop is not
# the verdict (measured 2026-11-27: whether a run outlives 60 s after the program's breakpoint is Windows' timing).
# The last verdict and a hash of the build it ran on are kept in <Out>\last.txt, so a commit can check that the
# build being committed is the one that passed.
param(
    [string]$Game = $env:WHIWHI_GAME,
    [int]$Seconds = 60,
    [switch]$NoBuild,
    [switch]$Accept,
    [switch]$Show,
    [string]$Out = (Join-Path $env:LOCALAPPDATA 'whiwhi\game-tests')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'weave\build'
if (-not $Game -or -not (Test-Path -LiteralPath $Game)) { throw 'give -Game <your game folder> (or set WHIWHI_GAME)' }
$Game = (Resolve-Path -LiteralPath $Game).Path
$exe = Get-ChildItem -LiteralPath $Game -Filter '*.exe' | Where-Object { $_.Name -like 'game_*' } | Select-Object -First 1
if (-not $exe) { throw "no game_*.exe in $Game" }
$outFull = [IO.Path]::GetFullPath($Out)
if ($outFull.StartsWith($Game, [StringComparison]::OrdinalIgnoreCase)) { throw 'the records must not go inside the game folder' }
if (Get-Process boot_image -ErrorAction SilentlyContinue) { throw 'a run is already going (one at a time)' }

if (-not $NoBuild) {
    $buildLog = & (Join-Path $repo 'weave\build.cmd') 2>&1
    if (-not ($buildLog -match '100% tests passed')) { $buildLog | Select-Object -Last 15; throw 'the build or its sample tests failed: the game was not started' }
}

function Get-BuildHash($folder) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $rows = Get-ChildItem -LiteralPath $folder -File | Where-Object { $_.Extension -in '.exe', '.dll' } | Sort-Object Name |
        ForEach-Object { $_.Name + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($rows -join "`n")))).Replace('-', '')
}
function Get-Listing($root) {
    $rows = Get-ChildItem -LiteralPath $root -Recurse -Force | Sort-Object FullName |
        ForEach-Object { '{0}|{1}|{2}' -f $_.FullName, $(if ($_.PSIsContainer) { 'dir' } else { $_.Length }), $_.LastWriteTimeUtc.Ticks }
    $sha = [Security.Cryptography.SHA256]::Create()
    [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($rows -join "`n")))).Replace('-', '')
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$run = Join-Path $outFull "run-$stamp"
$work = Join-Path $outFull 'work'
New-Item -ItemType Directory -Force -Path $run, $work | Out-Null
Get-ChildItem -LiteralPath $work -File | Remove-Item -Force
Copy-Item (Join-Path $build '*.exe'), (Join-Path $build '*.dll') $work -Force
# A library the installation ships itself is loaded from the installation, not from the weave's build: the weave's
# file of the same name would win (host_libraries.cpp loads from beside boot_image.exe first).
Get-ChildItem -LiteralPath $work -File | Where-Object { Test-Path -LiteralPath (Join-Path $Game $_.Name) } |
    ForEach-Object { Write-Host "the installation ships $($_.Name) itself - the weave's file is left out"; Remove-Item -LiteralPath $_.FullName -Force }
# The build is recorded as built (the commit gate checks the build folder), not as the work folder holds it.
$buildHash = Get-BuildHash $build
$commit = (git -C $repo rev-parse --short HEAD).Trim()
$dirty = @(git -C $repo status --porcelain -- weave scripts).Count

$before = Get-Listing $Game
$exeHashBefore = (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash

# The measurements (measure/README.md) run on the copy first, one after another, before the game starts: each
# script checks results of FINDINGS.md on the file its usage line names, read-only. One that says DIFFERS makes
# the verdict FAIL; the game still runs, so the run's record is complete. Their output goes to measure.txt.
$measureLog = Join-Path $run 'measure.txt'
$measureFail = New-Object System.Collections.Generic.List[string]
$measureDir = Join-Path $repo 'measure'
$measureTools = Join-Path $outFull 'measure-tools'
$ErrorActionPreference = 'Continue'  # a line a tool writes to its error output is part of its record, not a stop
foreach ($script in Get-ChildItem -LiteralPath $measureDir -File | Where-Object { $_.Extension -in '.py', '.cpp' } | Sort-Object Name) {
    $text = Get-Content -LiteralPath $script.FullName -Raw
    $usage = [regex]::Match($text, 'game folder>\\+([A-Za-z0-9_.]+)')
    if (-not $usage.Success) {
        # The shared readers name no file: they are not measurements. A script that checks results and names no
        # file would be skipped without a word, so that is a failure, not a skip.
        if ($text -match '(?m)(\.same\(|^\s*same\(")') { $measureFail.Add("measure/$($script.Name) names no file of the game folder in its usage line") }
        continue
    }
    $measureInput = Join-Path $Game $usage.Groups[1].Value
    Add-Content -LiteralPath $measureLog -Value "---- $($script.Name) on $($usage.Groups[1].Value)" -Encoding utf8
    if ($script.Extension -eq '.py') {
        $output = & python $script.FullName $measureInput 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } else {
        # A C++ measurement is compiled with the same compiler as the weave and run beside the PC graphics
        # library of the weave's build.
        New-Item -ItemType Directory -Force -Path $measureTools | Out-Null
        Copy-Item (Join-Path $build 'd3d12_vk.dll'), (Join-Path $build 'd3d12core.dll') $measureTools -Force
        $vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
        $compiled = & cmd /c "`"$vcvars`" >nul && cd /d `"$measureTools`" && cl /nologo /std:c++17 /EHsc /O2 `"$($script.FullName)`"" 2>&1 | ForEach-Object { "$_" }
        $tool = Join-Path $measureTools ($script.BaseName + '.exe')
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $tool)) {
            $output = @('not compiled:') + $compiled
            $code = 1
        } else {
            $output = & $tool $measureInput 2>&1 | ForEach-Object { "$_" }
            $code = $LASTEXITCODE
        }
    }
    Add-Content -LiteralPath $measureLog -Value $output -Encoding utf8
    $last = @($output | Where-Object { $_ -match 'the same as FINDINGS' }) | Select-Object -Last 1
    Write-Host "measure/$($script.Name): $(if ($last) { $last } else { "exit $code" })"
    if ($code -ne 0) { $measureFail.Add("measure/$($script.Name): $(if ($last) { $last } else { "exit $code" }) (measure.txt)") }
}
$ErrorActionPreference = 'Stop'
$log = Join-Path $run 'run.err'
$env:WHIWHI_CALL_LEDGER = Join-Path $run 'calls.tsv'
$env:WHIWHI_WANTS_LEDGER = Join-Path $run 'wants.txt'
$env:DISABLE_RTSS_LAYER = '1'
$env:DISABLE_VULKAN_OBS_CAPTURE = '1'
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $work 'boot_image.exe') -WorkingDirectory $work -PassThru `
    -WindowStyle $(if ($Show) { 'Normal' } else { 'Hidden' }) `
    -ArgumentList "`"$($exe.FullName)`"", "`"$Game`"", '--seconds', "$Seconds" `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $run 'run.out')
if ($Show) {
    Start-Process -FilePath 'powershell.exe' -ArgumentList '-NoProfile', '-Command',
        "`$host.UI.RawUI.WindowTitle = 'game test log'; Get-Content -LiteralPath '$log' -Wait -Tail 50"
}
try { $p.PriorityClass = 'Normal' } catch {}
if (-not $p.WaitForExit(($Seconds + 60) * 1000)) { try { $p.Kill() } catch {}; $p.WaitForExit(15000) | Out-Null }
$elapsed = [int]$sw.Elapsed.TotalSeconds
Remove-Item Env:\WHIWHI_CALL_LEDGER
Remove-Item Env:\WHIWHI_WANTS_LEDGER
$unchanged = ($before -eq (Get-Listing $Game)) -and ($exeHashBefore -eq (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash)

# What the run did, from its own log and the call ledger.
$text = if (Test-Path $log) { Get-Content -LiteralPath $log } else { @() }
$alive = [bool]($text -match 'still running after \d+s')
$endLine = ($text | Where-Object { $_ -notmatch "the watchdog's progress stamp" } |
    Select-String -Pattern 'still running after|exception 0x|entry thread finished|watchdog|process exiting' |
    Select-Object -First 1).Line
# How long the image's own progress report went unchanged (read only by the boot tool; the watchdog stops the
# image after about 30 s without a new one).
$progressReports = ''; $progressLongest = ''
$p = $text | Select-String -Pattern "progress stamp \(read only\): (\d+) new reports, longest unchanged ([\d.]+) s" | Select-Object -First 1
if ($p) { $progressReports = $p.Matches[0].Groups[1].Value; $progressLongest = $p.Matches[0].Groups[2].Value }
$frame = 0
foreach ($m in ($text | Select-String -Pattern 'frame token (\d+)')) { $frame = [Math]::Max($frame, [int]$m.Matches[0].Groups[1].Value) }
$made = 0
foreach ($m in ($text | Select-String -Pattern 'device slot 86 #\d+:.*?; (\d+) made|pipelines built by the weave.*?: (\d+) built')) {
    $g = $m.Matches[0].Groups; $v = if ($g[1].Success) { $g[1].Value } else { $g[2].Value }
    $made = [Math]::Max($made, [int]$v)
}
# One line per distinct answer: what was asked and where from (the return address stays: the image sits at its
# preferred base), without the argument values, which differ from run to run.
$normalise = { param($l) ($l -replace ';? ?arguments [^)]*', '' -replace 'called with \([^)]*\)', 'called with (...)' -replace '0x[0-9A-Fa-f]{9,}', '#').Trim() }
$notProvided = $text | Where-Object { $_ -match 'not provided|answered not implemented' -and $_ -notmatch '^\[boot\] call ledger' } |
    ForEach-Object { & $normalise $_ } | Sort-Object -Unique
$notProvided | Set-Content -LiteralPath (Join-Path $run 'not-provided.txt') -Encoding utf8
$calls = @{}
$ledger = Join-Path $run 'calls.tsv'
if (Test-Path $ledger) {
    foreach ($row in Get-Content -LiteralPath $ledger) {
        $f = $row -split "`t"
        if ($f.Count -ge 4) { $calls[$f[1]] = @{ answer = $f[2]; count = [uint64]$f[3] } }
    }
}
$called = @($calls.Keys | Where-Object { $calls[$_].count -gt 0 })
$calledUnprovided = @($called | Where-Object { $calls[$_].answer -eq 'not-provided' })

# The whole, from the wants ledger the weave wrote (weave/d3d12_xs.cpp): one line per (step of the program's flow,
# what the program asked for, what the weave gave). Each meeting point - step and what was asked - is attached when
# everything the weave gave there was carried out, missing when nothing was, partly when both happened.
$gapPattern = 'not made|not provided|not implemented|held|not passed on|not one|refused'
function Read-Whole([string]$file) {
    $points = @{}
    if (-not (Test-Path -LiteralPath $file)) { return $points }
    foreach ($row in Get-Content -LiteralPath $file) {
        if ($row.StartsWith('#')) { continue }
        $f = $row -split "`t"
        if ($f.Count -lt 4) { continue }
        $parts = $f[3] -split ' \| ', 3
        if ($parts.Count -lt 3) { continue }
        $point = "$($parts[0]) | $($parts[1])"
        if (-not $points.ContainsKey($point)) { $points[$point] = @{ given = [uint64]0; gap = [uint64]0 } }
        if ($parts[0] -match $gapPattern -or $parts[2] -match $gapPattern) { $points[$point].gap += [uint64]$f[0] } else { $points[$point].given += [uint64]$f[0] }
    }
    $points
}
function Get-State($p) { if ($p.gap -eq 0) { 'attached' } elseif ($p.given -eq 0) { 'missing' } else { 'partly' } }
$whole = Read-Whole (Join-Path $run 'wants.txt')
$wholeRows = foreach ($k in ($whole.Keys | Sort-Object)) { "{0}`t{1}`t{2}`t{3}" -f (Get-State $whole[$k]), $whole[$k].given, $whole[$k].gap, $k }
$wholeRows | Set-Content -LiteralPath (Join-Path $run 'whole.txt') -Encoding utf8
$wholeCounts = @{ attached = 0; partly = 0; missing = 0 }
foreach ($k in $whole.Keys) { $wholeCounts[(Get-State $whole[$k])]++ }

# Against the map the project already has (docs/exe-map and docs/console-libs, measured from the executable):
# where each call this run made is described there. A call the map does not name is listed as not in the map,
# so it is read from the executable and written into the map before anything is built for it.
$mapFiles = @(Get-ChildItem -LiteralPath (Join-Path $repo 'docs\exe-map'), (Join-Path $repo 'docs\console-libs') -Filter '*.md')
$mapText = @{}
foreach ($m in $mapFiles) { $mapText[$m.Name] = Get-Content -LiteralPath $m.FullName -Raw }
function Find-InMap([string]$pattern, [string[]]$only = @()) {
    @($mapText.Keys | Where-Object { (-not $only.Count -or $only -contains $_) -and $mapText[$_] -match $pattern } | Sort-Object)
}
$mapRows = New-Object System.Collections.Generic.List[string]
$unmappedImports = 0
foreach ($k in ($called | Sort-Object)) {
    $name = ($k -split '!', 2)[1]
    $where = Find-InMap ('\b' + [regex]::Escape($name) + '\b')
    if (-not $where.Count) { $unmappedImports++ }
    $mapRows.Add($(if ($where.Count) { "import`t$k`tin the map: $($where -join ', ')" } else { "import`t$k`tNOT IN THE MAP" }))
}
$unmappedAnswers = 0
foreach ($l in $notProvided) {
    $where = @()
    if ($l -match '\] device: method slot (\d+)') { $where = Find-InMap ("slot $($Matches[1])\b|\| *$($Matches[1]) *\|") @('GRAPHICS-CALLS.md') }
    elseif ($l -match '\] command list: method slot (\d+)') { $where = Find-InMap ("slot $($Matches[1])\b|\| *$($Matches[1]) *\|") @('COMMAND-LIST-ORDER.md', 'COMMAND-LIST.md') }
    elseif ($l -match 'provider \{([0-9a-f]{8})') { $where = Find-InMap $Matches[1] }
    elseif ($l -match '^\[xcrdapi\] queue method (\+0x[0-9a-f]+)') { $where = Find-InMap ([regex]::Escape($Matches[1])) @('xcrdapi-dstorage.md') }
    elseif ($l -match '^\[\w+\] (\w+)::') { $where = Find-InMap ('\b' + $Matches[1] + '\b') }
    if (-not $where.Count) { $unmappedAnswers++ }
    $mapRows.Add($(if ($where.Count) { "answer`t$l`tin the map: $($where -join ', ')" } else { "answer`t$l`tNOT IN THE MAP" }))
}
$mapRows | Set-Content -LiteralPath (Join-Path $run 'against-the-map.txt') -Encoding utf8
$summary = [ordered]@{
    time = $stamp; commit = $commit; uncommitted_weave_files = $dirty; build = $buildHash; seconds = $Seconds
    elapsed = $elapsed; game_folder_unchanged = $unchanged; alive_at_end = $alive; end = $endLine
    imports_called = $called.Count; imports_called_not_provided = ($calledUnprovided -join ' ')
    last_frame_token = $frame; pipelines_made = $made; not_provided_answers = @($notProvided).Count
    imports_called_not_in_map = $unmappedImports; answers_not_in_map = $unmappedAnswers
    progress_reports = $progressReports; progress_longest_unchanged_s = $progressLongest
    measurements_differing = $measureFail.Count
    whole_attached = $wholeCounts.attached; whole_partly = $wholeCounts.partly; whole_missing = $wholeCounts.missing
}
$summary.GetEnumerator() | ForEach-Object { '{0}={1}' -f $_.Key, $_.Value } | Set-Content -LiteralPath (Join-Path $run 'summary.txt') -Encoding utf8

# Compare with the baseline.
$fail = New-Object System.Collections.Generic.List[string]
$note = New-Object System.Collections.Generic.List[string]
if (-not $unchanged) { $fail.Add('THE GAME FOLDER CHANGED') }
foreach ($m in $measureFail) { $fail.Add("a FINDINGS result no longer holds on this copy: $m") }
if (-not (Test-Path $ledger)) { $fail.Add('no call ledger was written (the run ended before its report)') }
$baseline = Join-Path $outFull 'baseline'
if (Test-Path (Join-Path $baseline 'summary.txt')) {
    $b = @{}; Get-Content (Join-Path $baseline 'summary.txt') | ForEach-Object { $k, $v = $_ -split '=', 2; $b[$k] = $v }
    if ($b['end'] -ne $endLine) { $note.Add("the run ended elsewhere than the baseline's: $endLine") }
    # The whole against the baseline's whole: a meeting point that was attached and is now partly or missing, or
    # was partly and is now missing, or that the program reached in the baseline and not now, is worse.
    $bwhole = Read-Whole (Join-Path $baseline 'wants.txt')
    if ($bwhole.Count) {
        $rank = @{ attached = 2; partly = 1; missing = 0 }
        foreach ($k in $bwhole.Keys) {
            $was = Get-State $bwhole[$k]
            if (-not $whole.ContainsKey($k)) { $fail.Add("worse in the whole: $k was reached in the baseline ($was), not now"); continue }
            $now = Get-State $whole[$k]
            if ($rank[$now] -lt $rank[$was]) { $fail.Add("worse in the whole: $k was $was, now $now") }
            elseif ($rank[$now] -gt $rank[$was]) { $note.Add("better in the whole: $k was $was, now $now") }
        }
        foreach ($k in $whole.Keys) { if (-not $bwhole.ContainsKey($k)) { $note.Add("newly reached: $k ($(Get-State $whole[$k]))") } }
    } else {
        $note.Add('the baseline has no wants ledger yet: the whole is recorded (whole.txt), not compared')
    }
    $bcalls = @{}
    if (Test-Path (Join-Path $baseline 'calls.tsv')) {
        foreach ($row in Get-Content (Join-Path $baseline 'calls.tsv')) {
            $f = $row -split "`t"; if ($f.Count -ge 4) { $bcalls[$f[1]] = @{ answer = $f[2]; count = [uint64]$f[3] } }
        }
    }
    foreach ($k in $bcalls.Keys) {
        if ($bcalls[$k].count -gt 0 -and $calls.ContainsKey($k) -and $bcalls[$k].answer -eq 'provided' -and $calls[$k].answer -ne 'provided') {
            $fail.Add("worse in the whole: $k was provided and called in the baseline, now not provided")
        }
        if ($bcalls[$k].count -gt 0 -and (-not $calls.ContainsKey($k) -or $calls[$k].count -eq 0)) { $note.Add("no longer called: $k") }
    }
    foreach ($k in $called) { if (-not $bcalls.ContainsKey($k) -or $bcalls[$k].count -eq 0) { $note.Add("newly called: $k ($($calls[$k].answer))") } }
    $bnp = if (Test-Path (Join-Path $baseline 'not-provided.txt')) { Get-Content (Join-Path $baseline 'not-provided.txt') } else { @() }
    foreach ($l in $notProvided) { if ($bnp -notcontains $l) { $note.Add("new 'not provided': $l") } }
    foreach ($l in $bnp) { if ($notProvided -notcontains $l) { $note.Add("no longer 'not provided' (answered now, or not reached): $l") } }
    if ([int]$b['last_frame_token'] -gt $frame) { $note.Add("last frame token $frame, baseline $($b['last_frame_token'])") }
    if ([int]$b['pipelines_made'] -gt $made) { $note.Add("pipelines made $made, baseline $($b['pipelines_made'])") }
} else {
    $note.Add('no baseline yet: run again with -Accept to make one')
}
foreach ($r in $mapRows) { if ($r -match "^answer`t(.*)`tNOT IN THE MAP$") { $note.Add("not in the map yet: $($Matches[1])") } }
$verdict = if ($fail.Count) { 'FAIL' } else { 'PASS' }
$report = @("GAME TEST $verdict  ($stamp, commit $commit, $dirty uncommitted file(s) under weave/ and scripts/)") +
    ($summary.GetEnumerator() | ForEach-Object { '  {0}: {1}' -f $_.Key, $_.Value }) +
    ($fail | ForEach-Object { "  FAIL: $_" }) + ($note | Select-Object -First 60 | ForEach-Object { "  note: $_" })
$report | Set-Content -LiteralPath (Join-Path $run 'verdict.txt') -Encoding utf8
@("verdict=$verdict", "build=$buildHash", "run=$run", "commit=$commit") | Set-Content -LiteralPath (Join-Path $outFull 'last.txt') -Encoding utf8
if ($Accept -and $verdict -eq 'PASS') {
    if (Test-Path $baseline) { Remove-Item -LiteralPath $baseline -Recurse -Force }
    Copy-Item -LiteralPath $run -Destination $baseline -Recurse
    $report += '  this run is now the baseline'
}
$report
if ($verdict -eq 'FAIL') { exit 1 }
