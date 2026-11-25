# whiwhi

A shell that hosts the original GTA VI executable (Xbox Series 1.0.2.1) on a PC GPU. Original work by ExCorvus
([ExCorvus](https://codeberg.org/excorvus)).

The original game files are the source; the runtime here is our own code. You need your own copy of the game — this
repository contains no game data, keys or executable, and nothing here decrypts or bypasses any protection.

## Files

```
shell/                       our runtime (C++17, MSVC)
  image.hpp / image.cpp      PE64 image mapper — parses the original exe and maps it flat
  imports.hpp / imports.cpp  import table reader — the libraries and functions the image imports
  loader.hpp / loader.cpp    image loader — places the image in memory, connects its imports, sets page
                             protections, registers its exception table; does not start it
  host_libraries.hpp / .cpp  connects an import to the same-named library on the PC, when there is one
  hosting.hpp / hosting.cpp  flat hosting model — one GPU backing over the game's flat VA space,
                             uniform heap_base shift, real pages bound on demand
  tools/list_imports.cpp     prints an image's imported libraries and function counts
  tools/describe_image.cpp   prints what an image asks of a loader (sections, relocations, thread-local storage)
  tools/connect_imports.cpp  loads an image without starting it and reports which imports connect
  tests/                     unit tests (hosting, image, imports, loader; no game data)
  CMakeLists.txt             build definition (whiwhi_shell + tools + tests, CTest)
  build.cmd                  build + test helper
docs/
  exe-map/MAP.md             outward map of the original exe: memory model, GPU-address channels
  exe-map/CHANNELS.md        every channel that hands a heap address to the GPU
  EARLY-HISTORY.md           note on the early git and the restart
  RENDERING-PIPELINE.md      rendering-pipeline map (placeholder)
scripts/
  check_no_game_data.py      no-game-data gate (run before every push)
FINDINGS.md                  living findings log
AGENTS.md                    guide for coding agents
CONTRIBUTING.md              safety and code conventions
ATTRIBUTION.md / NOTICE.md   authorship and credit
LICENSE                      GPLv3-or-later (our own code)
```

## Build

```
shell\build.cmd
```

Builds `whiwhi_shell` and runs the tests via CTest. No game data or GPU needed.

## License

GPLv3-or-later for our own code — see [LICENSE](LICENSE). Grand Theft Auto and related names are trademarks of
their owners; this project is not affiliated with or endorsed by them.
