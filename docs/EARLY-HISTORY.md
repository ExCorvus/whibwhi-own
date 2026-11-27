# A note on the early history

The repository's early commits were made on a wrong footing. The commit history was later rebuilt as one commit
per file; this note is the record of what the early work was and why the project restarted.

## What went wrong at the start

The work began as an attempt to fix the upstream Leonida project, and the early commits treated our runtime as
something to patch onto that project. They tried to "fix" the runtime's heap handling: importing the title's
committed system RAM as host memory, anchoring committed extents at a page's own region, and covering hole-crossing
spans with contiguous chunks.

Those heap "fixes" are a trap. They force the game's memory to resolve — patching an allocation or refusal into
success — instead of hosting the environment the original executable expects. The pattern gave it away: the patches
cleared the world-start write fault and made the frames render, but the boot never reached the world. Advancing is
not the same as being correct.

## The footing we restarted on

A fresh, independent start from the original game executable; the sources are the original game files. The project is
a shell that hosts the original exe: it sits outside the game and provides the environment the game expects
(unified memory, frame cadence, liveness). A shell, not an emulator — we do not recreate console hardware.

The AI trap is looking at the code from within instead of outward: diving into the game's logic and patching its
allocations/refusals/error paths to force a result, versus observing what the game does from outside and hosting
exactly that. Going outside the game's scope is allowed when it serves the hosting (host/guide via mapped memory); what
is forbidden is fixing the game's own logic. The game folder on disk stays read-only.

The memory/hosting model is derived from the original executable's own map ([`exe-map/`](exe-map/)) and written by
us. The heap-patch line is stopped. The watchdog stays armed (a liveness relay of the frame-pacing loop).

(Corrected later: "outside the game" was not the answer either. The shell is not wrapped around the program but
woven through it; see "Since the restart" below and [ATTRIBUTION.md](../ATTRIBUTION.md) item 14.)

## The scrape

All upstream source was removed from the project; the only source now is our own files (`shell/`). The upstream
tree is referenced prior art outside the project, never vendored or built here. The project is our own shell based on
the original exe — no heap conversion. (That folder is called `weave/` now.)

## Since the restart (2026-11-26 and 2026-11-27)

We are still in the early history. What happened since the restart, in order; the measurements behind each step are
in [FINDINGS.md](../FINDINGS.md) under their dates.

- **One program, not two (2026-11-26).** Work done after the first boots had treated the executable and the shell as
  two programs talking to each other: our own shader layout, our own pipelines, our own copies. The repository went
  back to the state before that, and the rule became that the executable and the shell are one software. Words
  count too: a summary that says "the game asks, the shell answers" already pulls the two apart again.
- **One system.** The heaps, the watchdog, the timings and the frame pacing are not separate parts. They act as a
  single thing, and only the shell reaches out of it, to Windows, the GPU and sound. Every place where the shell
  decided something on its own was listed as a cut through that system.
- **The heaps trap, a second time.** Errors that look like heap errors kept pulling the work into the program's
  descriptor tables. They were symptoms of missing pieces elsewhere. The rule since: write such an error down,
  never chase it into the heaps.
- **A weave, not a wrapper (2026-11-26, ExCorvus).** The shell is not wrapped around the program but woven through
  it, like crochet: pull the wrong thread and the fabric comes apart. The shell was renamed the weave, everywhere.
- **The installation's own libraries.** Tests had left out the libraries the installation ships. With them in, the
  builder library led to the console shader compiler's own functions. They were taught to a fork of the shader
  translator (dxil-spirv) and of vkd3d-proton. A driver crash on the way turned out to be one value the translator
  left unwritten. One of these changes was accepted upstream into dxil-spirv on 2026-11-27.
- **The program's own layout.** The shell had used one layout of its own for every shader. Shaders now find their
  resources where the program's own root signatures and tables put them.
- **Repeatable measurements.** Every result read from the executable got a script in [`measure/`](../measure/) that
  anyone with their own copy can run. Re-checking the maps with a second tool found several claims that were wrong,
  and they were corrected.
- **Sound, drives and the whole.** The console's sound chip is answered through a decoder (FFmpeg), not recreated.
  The console's temporary drive T: is a real partition. The game test stopped judging single errors and judges
  the whole: every point where the program meets the weave.
- **Compile first, then weave (2026-11-27, ExCorvus).** Installing the port means compiling the weave and what it
  needs first; the weaving itself happens only when the original executable runs. Nothing of the game is copied.
- **Windows answers where Windows has it.** Where the weave reaches outward, it is reaching for Windows. The
  runtime's start and the interfaces measured with it are now Windows' own answers, passed through unchanged;
  nothing goes online.

— the reconstruction effort (original work: [ExCorvus](https://github.com/ExCorvus))
