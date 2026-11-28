# Installs the weave on this PC as a personal port of your own copy of the game, and makes a way to start it.
#
# Nothing of the game is copied: the port starts the original executable from your own installation, read-only, and
# the weave answers it while it runs. What this script builds and puts together, all from sources you got yourself:
#   1. the weave (weave\build.cmd: build and the sample tests);
#   2. the vkd3d-proton fork (D3D12 on Vulkan, docs/BUILD-AND-RUN.md) -> d3d12_vk.dll, d3d12core.dll;
#   3. FFmpeg's XMA2 decoder (scripts\setup_ffmpeg.ps1) and libopus (scripts\setup_opus.ps1);
#   4. the port folder: the weave's programs and libraries, without the ones your installation ships itself, and a
#      start script (start.ps1, start.cmd) that runs the original executable through the weave and keeps its log.
#
#   powershell -ExecutionPolicy Bypass -File scripts\install_port.ps1 -Game <your game folder> -Vkd3d <fork source>
#       -FFmpeg <ffmpeg source> -Opus <opus source> [-Target <port folder>] [-ToolPath <folders>] [-Shortcut]
#
# -ToolPath: folders put in front of PATH while vkd3d-proton builds (widl, ninja, glslangValidator).
# Your game folder is only read. The console's temporary drive T: must exist (docs/BUILD-AND-RUN.md).
# The installed port is yours, for playing and research on your own PC: do not share it. The script refuses a port
# folder inside the repository or inside the game folder.

param(
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$Vkd3d,
    [Parameter(Mandatory = $true)][string]$FFmpeg,
    [Parameter(Mandatory = $true)][string]$Opus,
    [string]$Target = (Join-Path $env:LOCALAPPDATA 'whiwhi\port'),
    [string[]]$ToolPath = @(),
    [string]$Vcvars = '',
    [switch]$Shortcut
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$weaveBuild = Join-Path $repo 'weave\build'

function Find-Vcvars {
    if ($Vcvars) { return $Vcvars }
    $roots = @("${env:ProgramFiles(x86)}\Microsoft Visual Studio", "$env:ProgramFiles\Microsoft Visual Studio")
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $found = Get-ChildItem -Path $root -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    throw 'vcvars64.bat not found: install the Visual Studio Build Tools or pass -Vcvars'
}

function Step([string]$what) { Write-Host ''; Write-Host "== $what" }

$Game = (Resolve-Path -LiteralPath $Game).Path
$exe = Join-Path $Game 'game_scarlett_master.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "no game_scarlett_master.exe in $Game" }
if (-not (Test-Path 'T:\')) { throw 'the console temporary drive T: does not exist (docs/BUILD-AND-RUN.md)' }
# The installed port is for your own PC and for research on it; it is never shared and never part of the repository.
$targetFull = [IO.Path]::GetFullPath($Target).TrimEnd('\') + '\'
foreach ($inside in @($repo, $Game)) {
    if ($targetFull.StartsWith(([IO.Path]::GetFullPath($inside).TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase)) {
        throw "the port folder must lie outside the repository and outside the game folder: $Target"
    }
}
$vcvars = Find-Vcvars

Step '1. the weave'
& cmd /c "`"$(Join-Path $repo 'weave\build.cmd')`""
if ($LASTEXITCODE -ne 0) { throw 'the weave did not build or a sample test failed' }

Step '2. the vkd3d-proton fork'
$vkBuild = Join-Path $Vkd3d 'build'
$path = (@($ToolPath) + @($env:PATH)) -join ';'
$setup = if (Test-Path -LiteralPath (Join-Path $vkBuild 'build.ninja')) { '' } else {
    "python -m mesonbuild.mesonmain setup `"$vkBuild`" -Dbuildtype=release -Denable_tests=false && " }
$old = @{ PATH = $env:PATH; PYTHONUTF8 = $env:PYTHONUTF8; LC_ALL = $env:LC_ALL }
$env:PATH = $path; $env:PYTHONUTF8 = '0'; $env:LC_ALL = 'C'
Push-Location -LiteralPath $Vkd3d
try {
    & cmd /c "call `"$vcvars`" >nul && $setup ninja -C `"$vkBuild`""
    if ($LASTEXITCODE -ne 0) { throw 'vkd3d-proton did not build' }
} finally {
    Pop-Location
    $env:PATH = $old.PATH; $env:PYTHONUTF8 = $old.PYTHONUTF8; $env:LC_ALL = $old.LC_ALL
}
Copy-Item -LiteralPath (Join-Path $vkBuild 'libs\d3d12\d3d12.dll') -Destination (Join-Path $weaveBuild 'd3d12_vk.dll') -Force
Copy-Item -LiteralPath (Join-Path $vkBuild 'libs\d3d12core\d3d12core.dll') -Destination (Join-Path $weaveBuild 'd3d12core.dll') -Force

Step '3. the sound decoders'
& (Join-Path $PSScriptRoot 'setup_ffmpeg.ps1') -Source $FFmpeg -Vcvars $vcvars
& (Join-Path $PSScriptRoot 'setup_opus.ps1') -Source $Opus -Vcvars $vcvars

Step '4. the port folder'
New-Item -ItemType Directory -Force -Path $Target, (Join-Path $Target 'logs') | Out-Null
Get-ChildItem -LiteralPath $Target -File | Where-Object { $_.Extension -in '.exe', '.dll' } | Remove-Item -Force
foreach ($file in Get-ChildItem -LiteralPath $weaveBuild -File | Where-Object { $_.Extension -in '.exe', '.dll' }) {
    if (Test-Path -LiteralPath (Join-Path $Game $file.Name)) {
        Write-Host "the installation ships $($file.Name) itself - the weave's file is left out"
        continue
    }
    Copy-Item -LiteralPath $file.FullName -Destination $Target -Force
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fullscreen_window.ps1') -Destination $Target -Force

$start = @"
# Starts the original executable from your installation through the weave. Written by scripts\install_port.ps1.
`$ErrorActionPreference = 'Stop'
`$here = Split-Path -Parent `$MyInvocation.MyCommand.Path
`$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
`$env:DISABLE_RTSS_LAYER = '1'
`$env:DISABLE_VULKAN_OBS_CAPTURE = '1'
`$log = Join-Path `$here "logs\`$stamp.err"
# Nothing else open when the program starts: every window is minimised first, through Windows' shell (as Win+D).
(New-Object -ComObject Shell.Application).MinimizeAll()
Start-Sleep -Milliseconds 500
`$p = Start-Process -FilePath (Join-Path `$here 'boot_image.exe') -WorkingDirectory `$here -PassThru ``
    -ArgumentList '"$exe"', '"$Game"', '--seconds', '86400' ``
    -RedirectStandardError `$log -RedirectStandardOutput (Join-Path `$here "logs\`$stamp.out")
Write-Host "running; its log: `$log"
# On the console the program's window is the screen: Windows is asked to show it frameless over the whole monitor.
Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', ``
    '-File', "``"`$(Join-Path `$here 'fullscreen_window.ps1')``"", '-ProcessId', "`$(`$p.Id)", ``
    '-Log', "``"`$(Join-Path `$here "logs\`$stamp.fullscreen.txt")``"", '-ProgramLog', "``"`$log``""
`$p.WaitForExit()
Write-Host "ended (exit code `$(`$p.ExitCode))"
"@
[IO.File]::WriteAllText((Join-Path $Target 'start.ps1'), $start.Replace("`r`n", "`n"), (New-Object Text.UTF8Encoding $false))
$cmd = "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0start.ps1`"`r`npause`r`n"
[IO.File]::WriteAllText((Join-Path $Target 'start.cmd'), $cmd, (New-Object Text.ASCIIEncoding))

if ($Shortcut) {
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Weave port.lnk'))
    $link.TargetPath = Join-Path $Target 'start.cmd'
    $link.WorkingDirectory = $Target
    $link.Save()
}

Step 'done'
Write-Host "port folder: $Target"
Write-Host "start it with: $(Join-Path $Target 'start.cmd')"
