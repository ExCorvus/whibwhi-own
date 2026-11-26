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
`AcpHal.dll`, `xapu.dll`, `chad.dll`, `XCRDAPI.dll`, `whiwhi_tls.dll`) and the tools.

## Test

```
ctest --test-dir shell\build --output-on-failure
```

Twelve tests, no GPU and no game files needed (all fixtures are synthetic).

## Run the shell against the game

```
shell\build\boot_image.exe "C:\XboxGames\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe" "C:\XboxGames\GTA6.Xbox.v1.0.2.1" --seconds 120
```

What it does, in order: parses the game executable, places it in memory at its own preferred address, applies
relocations if that address was taken, connects its imports (PC libraries by the same name; the console-only
libraries resolve to the DLLs built beside the tool), gives it thread-local storage, runs its thread-storage
callbacks, then calls its entry point on a new thread. Every import the shell does not provide is logged and
answered with failure, so the game's own error handling decides what happens next; exceptions are logged with
their address in the game. The game folder is never written.

Tools:

- `list_imports <image>` — the libraries and functions an image imports.
- `describe_image <image>` — what an image asks of a loader (base, sections, directories, TLS).
- `connect_imports <image> [library folder] [--names]` — loads an image without starting it and reports what connects.

Environment variables:

- `WHIWHI_GAME_DIR` — the folder console-style paths map onto (default the XboxGames install path).
- `WHIWHI_TLS_REDIRECT=0` — leave TLS array entry 0 alone (default redirects it; the console gives the game index 0).
- `WHIWHI_WATCH=<rva>` — arm a 1-byte write watchpoint on `image+<rva>` on the entry thread (diagnostics).
- `WHIWHI_XMEM_TRACE=1` — log every call to the memory library.

## Status and limits

See `FINDINGS.md` for the measured log. Short version: the shell loads the game, connects 578 of its 587 imports,
and runs its own start-up up to the game's file layer. The game currently stops at a stub slot in its own vtable
that is an `int3` in the retail file (the console's loader fills such slots); the shell does not patch game code.
