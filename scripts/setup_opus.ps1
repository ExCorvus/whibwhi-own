# Builds the Opus decoder library (libopus) as a DLL for the weave's stream channel.
#
# The program sends its streamed sound through the console's stream channel (xapu.dll) in packets of 960 samples
# (docs/console-libs/acp-xapu.md, FINDINGS.md). The weave does not decode them itself: it hands each packet to
# libopus (BSD licence). libopus is not part of this repository (AGENTS.md rule 8): you get its source yourself
# (https://github.com/xiph/opus), this script builds the library alone (no programs, no tests) and copies opus.dll
# beside the weave.
#
# Needs: Visual Studio Build Tools (vcvars64.bat, which also brings CMake and Ninja) and an Opus source checkout.
#
#   powershell -ExecutionPolicy Bypass -File scripts\setup_opus.ps1 -Source <opus source> [-Work <build folder>]
#
# The source folder is not changed: Opus is configured and built in the work folder.

param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Work = (Join-Path $env:LOCALAPPDATA 'whiwhi\opus-build'),
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

if (-not (Test-Path (Join-Path $Source 'CMakeLists.txt'))) { throw "no Opus source at $Source" }
$vcvars = Find-Vcvars
$build = Join-Path $Work 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$configure = "cmake -S `"$Source`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON " +
             "-DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DBUILD_TESTING=OFF"
$ErrorActionPreference = 'Continue'
cmd /c "`"$vcvars`" >nul && $configure && cmake --build `"$build`""
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code -ne 0) { throw "the Opus build failed (exit code $code)" }

$dll = Get-ChildItem $build -Recurse -Filter 'opus.dll' | Select-Object -First 1
if (-not $dll) { throw "the build left no opus.dll in $build" }
if (-not (Test-Path $weaveBuild)) { New-Item -ItemType Directory -Force -Path $weaveBuild | Out-Null }
Copy-Item $dll.FullName -Destination $weaveBuild -Force
Write-Output "opus.dll copied to $weaveBuild"
