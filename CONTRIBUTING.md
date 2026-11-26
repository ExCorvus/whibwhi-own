# Contributing

The rules for working on this project are in [AGENTS.md](AGENTS.md); they apply to people and agents alike. This
file adds the conventions for code and credit.

## Safety

- **No game data, ever**: no executable, archives, shaders in any form, textures, models, audio, video, or
  disassembly listings, and no logs that quote them. `.gitignore` blocks the common file types; check `git status`
  before you commit and run `python -I scripts/check_no_game_data.py <files>` (it must print `clean`).
- **No keys, decryption or protection workarounds**, and no links to where to get them.
- **The game folder is read-only.**
- **No third-party code in the repository.**

## Credit

The approach and findings originated with **ExCorvus**; see [NOTICE.md](NOTICE.md) for the credit that is
required when the work is reused.

## Code

- The runtime is in `weave/` (C++17, MSVC). Build and test with `weave\build.cmd`.
- A new module comes with a test in `weave/tests/`. Tests use made-up inputs built by hand, never game files.
- `scripts/check_no_game_data.py` uses only the Python standard library.

## Commits

- One file per commit. The message says what the file is, in plain words.
- There is no pull-request workflow; commits go straight to the GitHub repository.
- Record what you measured in [FINDINGS.md](FINDINGS.md).
