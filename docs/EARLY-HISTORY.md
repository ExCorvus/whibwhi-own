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

A fresh, independent port of the original game executable; the sources are the original game files. The project is
a shell that hosts the original exe: it sits outside the game and provides the environment the game expects
(unified memory, frame cadence, liveness). A shell, not an emulator — we do not recreate console hardware.

The AI trap is looking at the code from within instead of outward: diving into the game's logic and patching its
allocations/refusals/error paths to force a result, versus observing what the game does from outside and hosting
exactly that. Going outside the game's scope is allowed when it serves the port (host/guide via mapped memory); what
is forbidden is fixing the game's own logic. The game folder on disk stays read-only.

The memory/hosting model is derived from the original executable's own map ([`exe-map/`](exe-map/)) and written by
us. The heap-patch line is stopped. The watchdog stays armed (a liveness relay of the frame-pacing loop).

## The scrape

All upstream source was removed from the project; the only source now is our own files (`shell/`). The upstream
tree is referenced prior art outside the project, never vendored or built here. The port is our own shell based on
the original exe — no heap conversion.

— the reconstruction effort (original work: [ExCorvus](https://github.com/ExCorvus))
