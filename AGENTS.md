# whiwhi — agent guide

This is **our** project. Read this before changing anything. [CONTRIBUTING.md](CONTRIBUTING.md) has the safety and
code conventions. **The only source here is our own files; no upstream code enters this project.**

## What we are building

A **shell based on the original game executable, nothing else** — Grand Theft Auto VI, Xbox Series 1.0.2.1. The
original game files are the source we host; our runtime (`shell/`) is entirely our own code that wires the game into
one singular piece of software. The game's whole flat memory and code are laid out as-is; the shell only wires the
environment: the GPU reads the game's memory directly, the frame cadence, the liveness. **The shell is the entire
game, flat out.** Not an emulator. See [docs/EARLY-HISTORY.md](docs/EARLY-HISTORY.md) for the candid restart note.

## The footing (binding — ExCorvus)

- **Outward, not from within.** Observe what the game *does* and host the exact environment it expects (memory,
  frame cadence, liveness). Never patch the game's allocations, refusals or error paths into success — the heap-fix
  line is **stopped**. **No heap conversion.** The shell leads; the game follows.
- **Going outside the game's scope is allowed when it serves the port** (host/guide via mapped memory). The trap is
  fixing the game's own logic, not touching its memory. The game folder on disk is **read-only**.
- **Do not build an OS around the game.** No mirroring the game's memory to disk, no eagerly materializing the whole
  shader/asset corpus. Host **in memory, zero-copy**; translate **on demand**; keep the disk lean.
- **Flat-out zero-copy.** Preserve the game's flat unified-VA layout whole; expose it to the GPU with one flat
  backing and a uniform `heap_base` shift, binding real pages on demand, so every V# resolves directly to the game's
  own RAM. No piecemeal imports, shadow copies, "waiting for a copy", or disk mirror.
- **Shaders:** translate the game's own shaders faithfully (RDNA2 → SPIR-V), on demand. Fix only our translator's
  fidelity; never reconstruct or "fix" shader logic.
- **The watchdog stays armed** (a liveness relay of the frame-pacing loop). Never disable or outrun it.
- **BANNED** (crash the AMD driver / whole system): sparse `VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT`;
  off-thread async pipeline builds.
- **No subagents** — the coordinator does the work (narrow exception: a maintenance-only repo-description keeper).
  **ExCorvus gates every push and boot.** One GPU run at a time.
- **Source of truth:** `C:\Users\user\AppData\Local\Temp\opencode\leonida-submission\shared\FACTS.md` + the
  `fix-ledger.md` tail. Nothing is pushed until ExCorvus says.

## Our project (only our own files)

- [`shell/`](shell/) — our own runtime: the original-exe image mapper (`image.*`) + the flat-out hosting model
  (`hosting.*`) + our tests. Build with `shell\build.cmd`.
- [docs/exe-map/MAP.md](docs/exe-map/MAP.md) + [CHANNELS.md](docs/exe-map/CHANNELS.md) — the outward map of the
  original exe (memory model, GPU-address channels, the decoded boot-36 fault §5, the flat-out footing §6).
- [FINDINGS.md](FINDINGS.md) — the living findings log.
- [docs/EARLY-HISTORY.md](docs/EARLY-HISTORY.md) — the candid restart note.
- [docs/RENDERING-PIPELINE.md](docs/RENDERING-PIPELINE.md) — rendering-pipeline map (placeholder).
- [ATTRIBUTION.md](ATTRIBUTION.md) / [NOTICE.md](NOTICE.md) / [LICENSE](LICENSE) — the credit.
- Game folder (read-only): `C:\XboxGames\GTA6.Xbox.v1.0.2.1`. The original exe is the source we host.

## Rules

- **No game data ever** in commits or fixtures: archives, the executable, carved files, shaders in any form (DXBC,
  FSSM, disassembly, SPIR-V), textures, audio, or logs/reports quoting them. The exe-map `funcs/*.txt` listings are
  private. Our no-game-data gate must pass before any push.
- **No keys, decryption or protection workarounds. Nothing online** (no Xbox Live / Rockstar services or domains).
- **Fix, don't disable:** a shader the translator rejects gets a rule, not a skip list.
- **The game folder on disk is read-only.** Never write it.
- **No upstream code.** Do not vendor, link or build upstream source here. Our runtime is our own.

## Build / verify our work

```
shell\build.cmd
```

Builds the shell core (`whiwhi_shell`) and runs our tests (`test_hosting`, `test_image`) via CTest. No game data, no
GPU needed.
