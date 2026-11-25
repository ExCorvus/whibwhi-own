# shell

The runtime core.

- `image.hpp` / `image.cpp` — PE image mapper. Parses a PE64 image and maps it flat (headers at 0, each section
  at its RVA). PE structures defined from the public PE spec.
- `hosting.hpp` / `hosting.cpp` — flat hosting model. One contiguous GPU backing over the game's flat VA space;
  `device_address = heap_base + (game_va - flat_base)`; real pages bound on demand.
- `tests/` — unit tests (no game data).

## Build + test

```
build.cmd
```

MSVC + the bundled CMake/Ninja. Builds `whiwhi_shell` and runs `test_hosting` + `test_image` via CTest. No GPU
needed.
