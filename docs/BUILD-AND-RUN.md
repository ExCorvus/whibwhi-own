# Build and run the shell yourself (no agent needed)

This is a plain C++17 project. Any editor and any MSVC toolchain work; nothing here requires an AI agent.

## What you need

- Windows 10/11, x64.
- MSVC Build Tools (Visual Studio 2022 Build Tools, "Desktop development with C++" workload) or any MSVC
  with CMake and Ninja. `shell\build.cmd` uses the Build Tools' bundled CMake/Ninja; with another toolchain use
  the manual commands below.
- Your own copy of the game, installed at `C:\XboxGames\GTA6.Xbox.v1.0.2.1` (the default). The game folder is
  read-only for this project: the shell only reads it. Point elsewhere with the `WHIWHI_GAME_DIR` environment
  variable.

## Build

```
shell\build.cmd
```

or manually:

```
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S shell -B shell\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build shell\build
ctest --test-dir shell\build --output-on-failure
```

Everything lands in `shell\build`: the `whiwhi_shell` library, the console libraries (`xmem.dll`, `d3d12_xs.dll`,
`XCRDAPI.dll`, `xgameruntime.dll`, `AcpHal.dll`, `xapu.dll`, `chad.dll`, `xg_xs.dll`, `XboxBvhBuilder.dll`) and
the tools.

## Test

```
ctest --test-dir shell\build --output-on-failure
```

13 tests, no GPU and no game files needed (all inputs are made up).

## Run the shell against the game

The game looks for `xgameruntime.dll` in the folder of the running program and reads its own files from there,
so the shell is run from a folder that holds both our programs and the game's files. Build that folder without
touching the game folder:

1. Make an empty work folder and copy `shell\build\*.exe` and `shell\build\*.dll` into it.
2. For every file in the game folder, make a hard link of the same name in the work folder
   (`mklink /H <work>\<name> <game folder>\<name>`); for every subfolder, a junction (`mklink /J ...`). Skip a
   name that already exists in the work folder.
3. Run from the work folder:

```
boot_image.exe "C:\XboxGames\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe" "C:\XboxGames\GTA6.Xbox.v1.0.2.1" --seconds 120
```

The game also reads the registry value `ForceUseLocalServices` (a DWORD) under
`HKLM\Software\Microsoft\GamingServices`; with it set to 1 the game searches the program's folder for
`xgameruntime.dll`. Setting it needs administrator rights and is your decision.

What `boot_image` does, in order: reads the game executable, places it in memory at its own preferred address
(applying relocations if that address was taken), connects its imports (the shell's own console libraries
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

- `WHIWHI_GAME_DIR` — the folder console-style file paths are mapped onto (default the XboxGames install path).
- `WHIWHI_PEB_GAME=1` — make the process report the game executable as its main program file (not needed when
  running from a work folder as above).
- `WHIWHI_WATCH=<address inside the image, hex>` — stop when one byte at that address is written (diagnostics).
- `WHIWHI_XMEM_TRACE=1` — log every call to the memory library.

## Using vkd3d (D3D12 on Vulkan) underneath

The shell prefers a vkd3d-built `d3d12.dll` (renamed `d3d12_vk.dll`) placed beside `boot_image.exe`, with its
`d3d12core.dll`; otherwise it uses the system's native `d3d12.dll`. Build vkd3d-proton yourself (LGPL-2.1, see
`NOTICE.md`):

```
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/HansKristian-Work/vkd3d-proton.git vkd3d-proton
# install meson, ninja, and widl (e.g. the MSYS2 mingw-w64-x86_64-tools package) first
meson setup build --buildtype release -Denable_tests=false   # inside an MSVC vcvars64 shell, LC_ALL=C
meson compile -C build
copy build\libs\d3d12\d3d12.dll  shell\build\d3d12_vk.dll
copy build\libs\d3d12core\d3d12core.dll  shell\build\d3d12core.dll
```

With those two files beside the tools, `boot_image` logs "using the vkd3d-built d3d12.dll" and the PC device the
shell creates runs on Vulkan. The plan is to use that to show the game's own memory to the GPU without copying it;
that part is not built yet.

## Status and limits

The current state is the block at the top of `FINDINGS.md`. Short version: the shell loads the game, connects 586
of its 587 imports and starts it; the game gets through part of its own start-up and stops. Nothing is drawn on
screen yet.
