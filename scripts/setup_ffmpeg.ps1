# Builds FFmpeg's XMA2 decoder as DLLs for the weave's sound chip.
#
# The sound chip the program talks to decodes XMA2 audio (docs/console-libs/acp-xapu.md, FINDINGS.md). The weave does
# not decode it itself: it hands the program's XMA2 packets to FFmpeg's decoder (libavcodec, LGPL-2.1). FFmpeg is not
# part of this repository (AGENTS.md rule 8): you get its source yourself, this script builds only what is needed
# (the XMA1/XMA2 decoders, no programs, no network, no assembly) and copies the DLLs beside the weave.
#
# Needs: Visual Studio Build Tools (vcvars64.bat), MSYS2 with the packages make and diffutils
# (`pacman -S make diffutils`), and an FFmpeg source checkout.
#
#   powershell -ExecutionPolicy Bypass -File scripts\setup_ffmpeg.ps1 -Source <ffmpeg source> [-Work <build folder>]
#
# The source folder is not changed: FFmpeg is configured and built in the work folder.

param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Work = (Join-Path $env:LOCALAPPDATA 'whiwhi\ffmpeg-build'),
    [string]$Msys = 'C:\msys64',
    [string]$Vcvars = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$weaveBuild = Join-Path $repo 'weave\build'

function Find-Vcvars {
    if ($Vcvars) { return $Vcvars }
    $roots = @("${env:ProgramFiles(x86)}\Microsoft Visual Studio", "$env:ProgramFiles\Microsoft Visual Studio")
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $found = Get-ChildItem $root -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    throw 'vcvars64.bat not found: install the Visual Studio Build Tools or pass -Vcvars'
}

$bash = Join-Path $Msys 'usr\bin\bash.exe'
if (-not (Test-Path $bash)) { throw "MSYS2 bash not found at $bash (pass -Msys)" }
foreach ($tool in 'make.exe', 'diff.exe') {
    if (-not (Test-Path (Join-Path $Msys "usr\bin\$tool"))) { throw "MSYS2 lacks ${tool}: run pacman -S make diffutils" }
}
if (-not (Test-Path (Join-Path $Source 'configure'))) { throw "no FFmpeg source at $Source" }
$vc = Find-Vcvars
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$prefix = Join-Path $Work 'install'

function To-Msys([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    return '/' + $full.Substring(0, 1).ToLower() + $full.Substring(2).Replace('\', '/')
}

$options = @(
    '--toolchain=msvc', '--arch=x86_64', '--target-os=win64',
    '--enable-shared', '--disable-static',
    '--disable-everything', '--enable-decoder=xma1', '--enable-decoder=xma2',
    '--disable-programs', '--disable-doc', '--disable-network', '--disable-x86asm', '--disable-autodetect',
    '--disable-avdevice', '--disable-avformat', '--disable-avfilter', '--disable-swscale', '--disable-swresample',
    "--prefix=$(To-Msys $prefix)"
) -join ' '

# MSYS2 has its own link.exe; the compiler's folder is put first so that MSVC's linker is the one found.
$steps = @(
    'set -e',
    'export PATH="$(dirname "$(which cl)"):$PATH"',
    "cd '$(To-Msys $Work)'",
    "'$(To-Msys $Source)/configure' $options",
    "make -j$([Environment]::ProcessorCount)",
    'make install'
) -join ' && '

$env:MSYS2_PATH_TYPE = 'inherit'
$env:CHERE_INVOKING = '1'
$log = Join-Path $Work 'build.log'
Write-Host "building FFmpeg's XMA2 decoder in $Work (log: $log)"
# The compiler writes warnings to its error channel; only the exit code decides.
$ErrorActionPreference = 'Continue'
cmd /c "`"$vc`" >nul && `"$bash`" -lc `"$($steps.Replace('"', '\"'))`"" *> $log
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0) { throw "the FFmpeg build failed (exit $LASTEXITCODE), see $log" }

$dlls = Get-ChildItem (Join-Path $prefix 'bin') -Filter '*.dll'
if (-not $dlls) { throw "no DLLs in $prefix\bin" }
New-Item -ItemType Directory -Force -Path $weaveBuild | Out-Null
foreach ($dll in $dlls) {
    Copy-Item $dll.FullName $weaveBuild -Force
    Write-Host "  $($dll.Name) -> weave\build"
}
Write-Host "done: headers in $(Join-Path $prefix 'include'), libraries in $(Join-Path $prefix 'lib')"
