# Build and run the weave yourself (no agent needed)

This is a plain C++17 project. Any editor and any MSVC toolchain work; nothing here requires an AI agent.

## What you need

- Windows 10/11, x64.
- MSVC Build Tools (Visual Studio 2022 Build Tools, "Desktop development with C++" workload) or any MSVC
  with CMake and Ninja. `weave\build.cmd` uses the Build Tools' bundled CMake/Ninja; with another toolchain use
  the manual commands below.
- Your own copy of the game, installed at `C:\XboxGames\GTA6.Xbox.v1.0.2.1` (the default). The game folder is
  read-only for this project: the weave only reads it. Point elsewhere with the `WHIWHI_GAME_DIR` environment
  variable.
- A drive **T:** of 2 GB (see the next section).

## The console's temporary drive T:

The program keeps files of its own on drive T: (measured: its telemetry cache, crash reports and a studio cache;
FINDINGS.md, 2026-11-27). On the console T: is the title's temporary local storage: 2 GB of its own on the
console's SSD, written while the program runs (Microsoft's public page "Local storage on Xbox console"). The weave
does not redirect T: to a folder: the program's T: calls go to Windows as they are, so the PC needs a real drive
T:. Make it once, as a 2 GB partition on an SSD that is not your Windows drive (an NVMe SSD comes closest to the
console's). In a PowerShell opened as administrator, with your data drive's letter instead of `D`:

```powershell
$d = Get-Partition -DriveLetter D
$d | Resize-Partition -Size ($d.Size - 2GB)
New-Partition -DiskNumber $d.DiskNumber -Size 2GB -DriveLetter T |
    Format-Volume -FileSystem NTFS -NewFileSystemLabel "T" -Confirm:$false
```

The first command makes that partition 2 GB smaller at its end (the files on it stay); the second makes a new
2 GB partition with the letter T: in the freed space. Shrinking a partition is an ordinary Windows operation, but
it changes your disk: back up what you cannot lose first. If Windows cannot shrink the partition (files that cannot
be moved at its end), choose another data drive. On the console the system empties T: after the program ends; on
a PC nothing does that yet (open).

## Build

```
weave\build.cmd
```

or manually:

```
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S weave -B weave\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build weave\build
ctest --test-dir weave\build --output-on-failure
```

Everything lands in `weave\build`: the `whiwhi_weave` library, the console libraries (`xmem.dll`, `d3d12_xs.dll`,
`XCRDAPI.dll`, `xgameruntime.dll`, `AcpHal.dll`, `xapu.dll`, `chad.dll`, `xg_xs.dll`, `XboxBvhBuilder.dll`) and
the tools.

## Test

```
ctest --test-dir weave\build --output-on-failure
```

27 tests, no GPU and no game files needed (all inputs are made up). They check pieces and are a build check
only; they are not proof that the weave works with the game (AGENTS.md rule 11). That is the game test:

## The game test (the real game, from your copy)

```
powershell -ExecutionPolicy Bypass -File scripts\game_test.ps1 -Game <your game folder> [-Seconds 60] [-Accept]
```

It has three parts, each a script of its own (ExCorvus, 2026-11-28: the measurements and the program start
separately):

```
powershell -ExecutionPolicy Bypass -File scripts\measure_test.ps1 -Game <your game folder>
powershell -ExecutionPolicy Bypass -File scripts\run_program.ps1 -Game <your game folder> [-Seconds 120]
powershell -ExecutionPolicy Bypass -File scripts\game_test.ps1 -Game <your game folder> -Run last [-Accept]
```

`measure_test.ps1` measures your copy and starts nothing. `run_program.ps1` starts the program and nothing else,
where you can see it; started by you, Windows treats the program as one you started and lets its window come to
the front (measured 2026-11-28: started from a console in the background, Windows refused that). `game_test.ps1
-Run` starts nothing: it judges the run folder (`last` = the newest). Without `-Run`, `game_test.ps1` does all
three: it builds the weave, measures, starts `run_program.ps1` as a process of its own, and judges that run.

In more detail: it builds the weave, then runs every measurement in [measure/](../measure/README.md) on your copy (read-only; each
checks results of FINDINGS.md and says `same` or `DIFFERS`; the output goes to `measure.txt` of the run; the C++
one is compiled with the weave's compiler and run beside the weave's PC graphics library, so it needs Python with
numpy and capstone and the fork's `d3d12_vk.dll` and `d3d12core.dll` in `weave\build`). The measurements run only
when something they read has changed: the game folder (names, sizes, times), the exe, a script in `measure/`, or the
weave's PC graphics library. Otherwise the results of the run that last measured are taken over, and `measure.txt`
names that run (kept in `measure-cache` beside the runs). Then it runs the real game
from your copy inside the weave once (read-only; the folder is checked unchanged afterwards), and records what the
game did:

- every function the game imports, how often the game called it during the run, and whether the weave provides
  it (the loader puts a small counting stub in each import slot; `WHIWHI_CALL_LEDGER` names the file);
- every distinct "not provided" answer the weave gave, with the address in the game it was called from;
- whether the game was still alive at the end, the last frame number and how many pipelines were made.
- where each call of the run is named in the map the project already has of the executable (docs/exe-map,
  docs/console-libs), or that it is not in the map yet (against-the-map.txt). Being named there is not proof the
  entry is right: parts of the map were written in stretches that later proved wrong.

It compares that with the last run you accepted (`-Accept` makes a passing run the baseline) and says PASS or
FAIL: FAIL when the game folder changed, when a measurement says a FINDINGS result no longer holds on your copy
(or a measurement names no file to read), when the baseline was alive at its end and this run is not, or when an
import the game called in the baseline is not provided any more. Every other difference is listed as a note.
The records stay on your PC (`%LOCALAPPDATA%\whiwhi\game-tests`, or `-Out`); they hold names, counts and
addresses only, nothing of the game itself.

### Does Windows accept what the weave makes

```
powershell -ExecutionPolicy Bypass -File scripts\windows_check_test.ps1 -Game <your game folder> [-Seconds 120] [-Show]
```

The same game test, with one addition: every root signature and pipeline the weave hands to vkd3d is also handed,
as a copy, to Windows' own Direct3D 12 on your PC (`weave\build\windows_check.exe`, which loads `d3d12.dll` from
System32 only, in a process of its own). The result (`%LOCALAPPDATA%\whiwhi\windows-check\<time>.txt`) lists, side
by side, what vkd3d and what Windows answered to each. With Windows' optional "Graphics Tools" feature installed,
its debug layer would also say why something is refused; without it only the answer numbers are known.

## Run the weave against the game

Run `boot_image.exe` from the build folder (`weave\build`), where the weave's own libraries sit beside it. Give
it the game's executable and the game folder:

```
cd weave\build
boot_image.exe "C:\XboxGames\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe" "C:\XboxGames\GTA6.Xbox.v1.0.2.1" --seconds 120
```

Nothing has to be copied or linked. The game looks for `xgameruntime.dll` in the folder of the running program
(ours is there), and it asks for its package files on drive `G:\`, where the console mounts them; the weave
answers `G:\` from the game folder you named. That drive is read-only inside the weave: a call that would
create, change or delete something on it is refused and never reaches Windows.

Do not put the game's own copies of the Microsoft C++ runtime (`msvcp140*.dll`, `vcruntime140*.dll`,
`concrt140.dll`, `vccorlib140.dll`, `vcomp140.dll`) beside `boot_image.exe`. They are older than the compiler
the weave is built with, and the weave's libraries fault with them; the PC's installed runtime serves both.

The game also reads the registry value `ForceUseLocalServices` (a DWORD) under
`HKLM\Software\Microsoft\GamingServices`; with it set to 1 the game searches the program's folder for
`xgameruntime.dll`. Setting it needs administrator rights and is your decision.

What `boot_image` does, in order: reads the game executable, places it in memory at its own preferred address
(applying relocations if that address was taken), connects its imports (the weave's own console libraries
beside the tool; otherwise the PC's library of the same name), gives it thread-local storage, runs its
thread-storage callbacks, then calls its entry point on a new thread. A call to an import nothing provides is
logged and answered with failure, so the game's own error handling decides what happens next. Faults are logged
with a backtrace. The game folder is never written.

The other tools:

- `list_imports <image>` — the libraries and functions an image imports.
- `describe_image <image>` — what an image asks of a loader (base, sections, directories, thread-local storage).
- `connect_imports <image> [library folder] [--names]` — loads an image without starting it and reports what
  connects.

Environment variables read by the tools and libraries:

- `WHIWHI_FILE_TRACE=1` — log every file call of the game that names a path, with what Windows answered.
- `WHIWHI_GAME_DIR` — the folder the storage library maps `G:\` onto. `boot_image` sets it to the game folder
  it was given; only set it yourself when using the storage library without `boot_image`.
- `WHIWHI_PEB_GAME=1` — make the process report the game executable as its main program file (diagnostics; not
  needed for a normal run).
- `VKD3D_SHADER_CACHE_PATH` — vkd3d's on-disk shader cache. The weave sets it to `0` (off) unless you set it.
- `WHIWHI_WATCH=<address inside the image, hex>` — stop when one byte at that address is written (diagnostics).
- `WHIWHI_XMEM_TRACE=1` — log every call to the memory library.

## Using vkd3d (D3D12 on Vulkan) underneath

The weave prefers a vkd3d-built `d3d12.dll` (renamed `d3d12_vk.dll`) placed beside `boot_image.exe`, with its
`d3d12core.dll`; otherwise it uses the system's native `d3d12.dll`. Build vkd3d-proton yourself (LGPL-2.1, see
`NOTICE.md`), from the fork this project uses: <https://github.com/ExCorvus/vkd3d-xbox>, branch `whibwhi-own`.
Its shader translator comes from <https://github.com/ExCorvus/dxil-spirv-xbox> (branch `whibwhi-own`, a submodule of
the fork). The fork adds what the installation's own libraries need: their shaders call the console shader
compiler's own functions (`xbox.*`) and use instructions the original translator did not read (see FINDINGS.md,
2026-11-26 and 2026-11-27). With the original vkd3d-proton the installation's XboxBvhBuilder.dll cannot make its
pipelines and the run stops about 5 seconds in.

```
git clone --recurse-submodules -b whibwhi-own https://github.com/ExCorvus/vkd3d-xbox.git vkd3d-proton
# install meson, ninja, and widl (e.g. the MSYS2 mingw-w64-x86_64-tools package) first
meson setup build --buildtype release -Denable_tests=false   # inside an MSVC vcvars64 command prompt, LC_ALL=C
meson compile -C build
copy build\libs\d3d12\d3d12.dll  weave\build\d3d12_vk.dll
copy build\libs\d3d12core\d3d12core.dll  weave\build\d3d12core.dll
```

With those two files beside the tools, `boot_image` logs "using the vkd3d-built d3d12.dll" and the PC device the
weave creates runs on Vulkan. Buffers the game places in its own memory are opened on that device where they
lie, without a copy, and the game's frames are presented through that library's swap chain.

## The sound chip's decoder: FFmpeg

The program's sound chip decodes XMA2 audio. The weave's chip hands that work to FFmpeg's XMA2 decoder (LGPL-2.1,
see `NOTICE.md`), built separately. You need MSYS2 with `make` and `diffutils` (`pacman -S make diffutils`), the
Visual Studio Build Tools, and an FFmpeg source checkout (<https://ffmpeg.org/download.html>):

```
git clone https://git.ffmpeg.org/ffmpeg.git ffmpeg
powershell -ExecutionPolicy Bypass -File scripts\setup_ffmpeg.ps1 -Source ffmpeg
```

The script builds only the XMA1/XMA2 decoders (no programs, no network, no assembly) in
`%LOCALAPPDATA%\whiwhi\ffmpeg-build` and copies `avcodec-*.dll` and `avutil-*.dll` into `weave\build`. Run it before
`weave\build.cmd`: the build takes FFmpeg's headers from there. Without FFmpeg the weave still builds, and its sound
chip answers "not implemented".

## What you will see

A window titled "Americas" opens (the game creates it; the weave shows it) and stays black: the game clears its
display surface and presents it about thirty times a second, but none of its picture is drawn yet. After about
13 seconds the run ends at the game's audio start-up. `boot_image` prints where it stopped.

## Status and limits

The current state is the block at the top of `FINDINGS.md`. Short version: the weave loads the game, connects 586
of its 587 imports and starts it; the game gets through its graphics start-up, reads its shaders, records and
presents frames, and stops when it starts its audio. None of the game's picture is drawn yet.
