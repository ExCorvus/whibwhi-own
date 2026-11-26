# Makes a local alpha folder: an optimized build of the shell plus your own copy of the game, side by side.
#
#   powershell -ExecutionPolicy Bypass -File scripts\make_alpha.ps1 -Game "C:\XboxGames\GTA6.Xbox.v1.0.2.1"
#              [-Target C:\whiwhi-alpha] [-SkipGameCopy]
#
# The folder it makes:
#   <Target>\shell   the shell, built with full optimization (no tests), and the optional libraries you built
#                    yourself (d3d12_vk.dll + d3d12core.dll from vkd3d-proton, opus.dll from libopus) if they are
#                    in shell\build
#   <Target>\game    a full copy of your game folder (the game folder itself is only read)
#   <Target>\logs    one log per run
#   <Target>\run.ps1 starts the game in the shell: powershell -ExecutionPolicy Bypass -File <Target>\run.ps1
#
# Nothing of the game is ever put into this repository; the copy stays on your machine. Put the folder on your
# fastest drive (an NVMe drive if you have one): the game reads its archives from the copy.
param(
    [Parameter(Mandatory)][string]$Game,
    [string]$Target = 'C:\whiwhi-alpha',
    [switch]$SkipGameCopy
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $Game 'game_scarlett_master.exe'))) {
    throw "No game_scarlett_master.exe in $Game"
}
if ([IO.Path]::GetFullPath($Target).TrimEnd('\').StartsWith([IO.Path]::GetFullPath($Game).TrimEnd('\'), 'OrdinalIgnoreCase')) {
    throw 'The alpha folder must not be inside the game folder.'
}
New-Item -ItemType Directory -Force -Path $Target, "$Target\shell", "$Target\logs" | Out-Null

# 1. An optimized build of the shell, in its own build folder.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio C++ build tools found.' }
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$build = Join-Path $Target 'build'
$cmd = "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && " +
       "`"$cmake`" -S `"$repo\shell`" -B `"$build`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=Release && " +
       "`"$cmake`" --build `"$build`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw 'The build failed.' }

# 2. The shell's files: the start tool and the libraries, no tests.
Get-ChildItem $build -File | Where-Object { $_.Extension -eq '.dll' -or $_.Name -eq 'boot_image.exe' } |
    Copy-Item -Destination "$Target\shell" -Force
$commit = (git -C $repo rev-parse HEAD 2>$null)
$dirty = (git -C $repo status --porcelain -- shell 2>$null | Measure-Object).Count
Set-Content -Encoding UTF8 -Path "$Target\shell\built_from.txt" -Value @(
    "commit $commit", "uncommitted changes under shell/: $dirty", "built $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
foreach ($own in 'd3d12_vk.dll', 'd3d12core.dll', 'opus.dll') {
    $from = Join-Path $repo "shell\build\$own"
    if (Test-Path -LiteralPath $from) { Copy-Item -LiteralPath $from -Destination "$Target\shell" -Force }
    else { Write-Host "Not found (optional, see docs/BUILD-AND-RUN.md): $from" }
}

# 3. The game, copied (robocopy only reads the source; files already copied and unchanged are skipped).
if (-not $SkipGameCopy) {
    robocopy $Game "$Target\game" /E /COPY:DAT /DCOPY:T /MT:16 /R:1 /W:1 /NFL /NDL /NP | Out-Host
    if ($LASTEXITCODE -ge 8) { throw "Copying the game failed (robocopy code $LASTEXITCODE)." }
}

# 4. The start script.
@'
# Starts the game from this folder's copy, inside this folder's shell.
#   powershell -ExecutionPolicy Bypass -File run.ps1 [-Seconds 300]
param([int]$Seconds = 300)
$here = $PSScriptRoot
if (Get-Process boot_image -ErrorAction SilentlyContinue) { Write-Host 'A run is already going: not started.'; exit 1 }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $here "logs\run-$stamp.err"
$before = (Get-FileHash -Algorithm SHA256 "$here\game\game_scarlett_master.exe").Hash
$env:DISABLE_RTSS_LAYER = '1'
$env:DISABLE_VULKAN_OBS_CAPTURE = '1'
$p = Start-Process -FilePath "$here\shell\boot_image.exe" -WorkingDirectory "$here\shell" -PassThru -WindowStyle Hidden `
    -ArgumentList "`"$here\game\game_scarlett_master.exe`"", "`"$here\game`"", '--seconds', $Seconds `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $here "logs\run-$stamp.out")
try { $p.PriorityClass = 'Normal' } catch {}
$sw = [Diagnostics.Stopwatch]::StartNew()
Write-Host "Running for up to $Seconds s. Log: $log"
if (-not $p.WaitForExit(($Seconds + 60) * 1000)) { try { $p.Kill() } catch {}; $p.WaitForExit(15000) | Out-Null }
$ran = [int]$sw.Elapsed.TotalSeconds
$after = (Get-FileHash -Algorithm SHA256 "$here\game\game_scarlett_master.exe").Hash
function Last($pattern) { (Select-String -Path $log -Pattern $pattern | Select-Object -Last 1).Line }
$proof = Join-Path $here "logs\proof-$stamp.txt"
$unchanged = $before -eq $after
$built = (Get-Content (Join-Path $here 'shell\built_from.txt')) -join '; '
$exit = $p.ExitCode
$end = Last '^\[boot\] (still running|exception|stopped|the image)'
$pipelines = Last 'pipeline builds on the asking threads'
$frame = Last 'PresentX: frame token'
$missing = Select-String -Path $log -Pattern 'not provided' | ForEach-Object {
    $line = $_.Line -replace '\(returns to.*', '' -replace 'arguments.*', ''
    $line.Trim()
} | Sort-Object -Unique
$missing = $missing -join ' | '
Set-Content -Encoding UTF8 -Path $proof -Value @(
    "whiwhi run proof $stamp",
    "game executable SHA-256 before: $before",
    "game executable SHA-256 after:  $after",
    "executable unchanged: $unchanged",
    "shell: $built",
    "ran for $ran s (limit $Seconds s), exit code $exit",
    "end: $end",
    "pipelines: $pipelines",
    "last presented frame: $frame",
    "calls not provided (distinct): $missing",
    "log: $log")
Write-Host "Ended after $ran s. Proof: $proof"
'@ | Set-Content -Encoding UTF8 -Path "$Target\run.ps1"

Write-Host "Alpha folder ready: $Target (start it with: powershell -ExecutionPolicy Bypass -File $Target\run.ps1)"
