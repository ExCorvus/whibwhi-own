# shell

The runtime core.

- `image.hpp` / `image.cpp` — PE image mapper. Parses a PE64 image and maps it flat (headers at 0, each section
  at its RVA). PE structures defined from the public PE spec.
- `imports.hpp` / `imports.cpp` — import table reader. Lists every library the mapped image imports and, per
  function, its name or ordinal and the address-table slot the loader fills.
- `hosting.hpp` / `hosting.cpp` — flat hosting model. One contiguous GPU backing over the game's flat VA space;
  `device_address = heap_base + (game_va - flat_base)`; real pages bound on demand.
- `tools/list_imports.cpp` — command-line tool: `list_imports <image file>` prints the imported libraries and
  function counts.
- `tests/` — unit tests (no game data).

## Build + test

```
build.cmd
```

MSVC + the bundled CMake/Ninja. Builds `whiwhi_shell` and `list_imports`, and runs `test_hosting`, `test_image`
and `test_imports` via CTest. No GPU needed.
