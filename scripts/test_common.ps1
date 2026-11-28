# What the three parts of the game test share (scripts\measure_test.ps1, scripts\run_program.ps1,
# scripts\game_test.ps1): where the records go, and the fingerprints they compare. Dot-sourced, not run.

function Get-TestOut([string]$Out) {
    if (-not $Out) { $Out = Join-Path $env:LOCALAPPDATA 'whiwhi\game-tests' }
    [IO.Path]::GetFullPath($Out)
}

function Resolve-GameFolder([string]$Game) {
    if (-not $Game -or -not (Test-Path -LiteralPath $Game)) { throw 'give -Game <your game folder> (or set WHIWHI_GAME)' }
    $Game = (Resolve-Path -LiteralPath $Game).Path
    $exe = Get-ChildItem -LiteralPath $Game -Filter '*.exe' | Where-Object { $_.Name -like 'game_*' } | Select-Object -First 1
    if (-not $exe) { throw "no game_*.exe in $Game" }
    @{ folder = $Game; exe = $exe }
}

# One hash over every .exe and .dll of a build folder, by name and content.
function Get-BuildHash($folder) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $rows = Get-ChildItem -LiteralPath $folder -File | Where-Object { $_.Extension -in '.exe', '.dll' } | Sort-Object Name |
        ForEach-Object { $_.Name + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($rows -join "`n")))).Replace('-', '')
}

# One hash over a folder's listing (names, sizes, times): read only, the files themselves are not opened.
function Get-Listing($root) {
    $rows = Get-ChildItem -LiteralPath $root -Recurse -Force | Sort-Object FullName |
        ForEach-Object { '{0}|{1}|{2}' -f $_.FullName, $(if ($_.PSIsContainer) { 'dir' } else { $_.Length }), $_.LastWriteTimeUtc.Ticks }
    $sha = [Security.Cryptography.SHA256]::Create()
    [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($rows -join "`n")))).Replace('-', '')
}

# What the measurements read: the game folder's listing, the exe, every script in measure/ and the build's PC
# graphics library (the C++ measurement runs beside it). The same key means the same results.
function Get-MeasureKey([string]$repo, [string]$Game, $exe) {
    $build = Join-Path $repo 'weave\build'
    $rows = @("game $(Get-Listing $Game)", "exe $((Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash)") +
        @(Get-ChildItem -LiteralPath (Join-Path $repo 'measure') -File | Sort-Object Name | ForEach-Object { "$($_.Name) $((Get-FileHash -LiteralPath $_.FullName).Hash)" }) +
        @('d3d12_vk.dll', 'd3d12core.dll' | ForEach-Object { "$_ $((Get-FileHash -LiteralPath (Join-Path $build $_)).Hash)" })
    $rows -join "`n"
}

# key=value lines (run.txt, summary.txt, last.txt) into a table.
function Read-Pairs([string]$file) {
    $t = @{}
    if (Test-Path -LiteralPath $file) { Get-Content -LiteralPath $file | ForEach-Object { $k, $v = $_ -split '=', 2; if ($k) { $t[$k] = $v } } }
    $t
}
