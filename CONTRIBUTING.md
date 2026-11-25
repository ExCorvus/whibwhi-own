# Contributing

This is **our** project (whiwhi — the GTA pipeline reconstruction). **The only source here is our own files; no
upstream code enters this project.** A few rules keep it safe to host and keep the credit correct. There is no
upstream coordination issue or pull-request workflow — this is our own project, and **ExCorvus gates every push**.

## Safety rules

- **No game data, ever.** Archives, the executable, carved files, shaders (DXBC, FSSM, disassembly, SPIR-V),
  textures, models, audio, videos, and reports or logs that quote them. `.gitignore` blocks the common types; check
  `git status` before you commit and run our no-game-data gate before every push. The exe-map `funcs/*.txt`
  disassembly listings are private — never push them.
- **No keys, decryption or protection workarounds**, and no links to where to get them. The tools only read files
  that are already plain on disk.
- Short excerpts needed to explain a format (a few header bytes, one instruction sequence) are fine in docs and
  comments.
- **The game folder on disk is read-only.** Never write it.
- **No upstream code.** Do not vendor, link or build upstream source here.

## Credit (required)

The approach and findings originated with **ExCorvus** — see [NOTICE.md](NOTICE.md). Crediting the origin is
required, not optional. Our runtime (`shell/`) is entirely our own code.

## How work lands here

- The coordinator does the work (no subagents, except a maintenance-only repo-description keeper). ExCorvus approves
  every push and every boot; one GPU run at a time.
- Record what you measured and what you tested (and on which GPU/driver) in [FINDINGS.md](FINDINGS.md) and the
  fix-ledger; there is no pull request. Source of truth:
  `C:\Users\user\AppData\Local\Temp\opencode\leonida-submission\shared\FACTS.md` + the `fix-ledger.md` tail.
- Read [AGENTS.md](AGENTS.md) for the binding footing (the shell, outward not from within, the traps, the watchdog).

## Working on our code

- Our runtime lives in `shell/` (C++17). Build and test with `shell\build.cmd` (MSVC + bundled CMake/Ninja; runs
  `test_hosting` + `test_image` via CTest). A new module comes with our own test in `shell/tests/` (no game data —
  build any fixture by hand).
- Python tools run with `python -I` and use only the standard library unless a file says otherwise.
- A text file a tool writes names its line ending — `open(path, "w", encoding="utf-8", newline="\n")` — so the same
  input gives the same bytes on every platform.
- Keep the build working on Windows first: paths through `os.path`, no bash in the build.
