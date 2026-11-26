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

27 tests, no GPU and no game files needed (all inputs are made up).

## Run the shell against the game

Run `boot_image.exe` from the build folder (`shell\build`), where the shell's own libraries sit beside it. Give
it the game's executable and the game folder:

```
cd shell\build
boot_image.exe "C:\XboxGames\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe" "C:\XboxGames\GTA6.Xbox.v1.0.2.1" --seconds 120
```

Nothing has to be copied or linked. The game looks for `xgameruntime.dll` in the folder of the running program
(ours is there), and it asks for its package files on drive `G:\`, where the console mounts them; the shell
answers `G:\` from the game folder you named. That drive is read-only inside the shell: a call that would
create, change or delete something on it is refused and never reaches Windows.

Do not put the game's own copies of the Microsoft C++ runtime (`msvcp140*.dll`, `vcruntime140*.dll`,
`concrt140.dll`, `vccorlib140.dll`, `vcomp140.dll`) beside `boot_image.exe`. They are older than the compiler
the shell is built with, and the shell's libraries fault with them; the PC's installed runtime serves both.

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

- `WHIWHI_FILE_TRACE=1` — log every file call of the game that names a path, with what Windows answered.
- `WHIWHI_GAME_DIR` — the folder the storage library maps `G:\` onto. `boot_image` sets it to the game folder
  it was given; only set it yourself when using the storage library without `boot_image`.
- `WHIWHI_PEB_GAME=1` — make the process report the game executable as its main program file (diagnostics; not
  needed for a normal run).
- `VKD3D_SHADER_CACHE_PATH` — vkd3d's on-disk shader cache. The shell sets it to `0` (off) unless you set it.
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
shell creates runs on Vulkan. Buffers the game places in its own memory are opened on that device where they
lie, without a copy, and the game's frames are presented through that library's swap chain.

## libopus (sound decoding)

The game's sound packets are Opus; `xapu.dll` decodes them with libopus (BSD-3-Clause, see `NOTICE.md`), loaded
at run time as `opus.dll` from beside `boot_image.exe`. Without it the decoding channel answers "not provided"
and the run stops at the audio start-up. Build it with the same compiler and CMake as the shell:

```
git clone --depth 1 --branch v1.5.2 https://github.com/xiph/opus.git opus
# inside an MSVC vcvars64 shell:
cmake -S opus -B opus\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
cmake --build opus\build
copy opus\build\opus.dll shell\build\opus.dll
```

## What you will see

A window titled "Americas" opens (the game creates it; the shell shows it) and stays black: the game clears its
display surface and presents it about thirty times a second, but none of its picture is drawn yet. With
`opus.dll` in place the game gets past its audio start-up and keeps running (boot b148: the whole 40 seconds,
at least 256 frames); without it the run ends at the audio start-up after about 13 seconds. `boot_image` prints
where it stopped.

## Status and limits

The current state is the block at the top of `FINDINGS.md`. Short version: the shell loads the game, connects 586
of its 587 imports and starts it; the game gets through its graphics start-up, reads its shaders, records and
presents frames, and stops when it starts its audio. None of the game's picture is drawn yet.
