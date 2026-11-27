# NOTICE — origin and required credit

Copyright (C) 2026 ExCorvus.

**This project and its approach originated with [ExCorvus](https://github.com/ExCorvus).**

The reverse-engineering findings, the memory/rendering model, and the porting approach documented here
(`FINDINGS.md`, `docs/exe-map/`, `ATTRIBUTION.md`) are her original work, developed with an AI coding agent
(see `ATTRIBUTION.md` for the split of ideas vs implementation).

## Third-party library used at run time: vkd3d-proton (LGPL-2.1)

The weave can answer the game's D3D12 calls through **vkd3d-proton** (D3D12 implemented on Vulkan), which lets the
weave import the game's own memory into the GPU zero-copy. vkd3d-proton is copyright its authors (Valve /
Hans-Kristian Arntzen / contributors) and licensed under the **GNU Lesser General Public License v2.1**; its source
and license are at <https://github.com/HansKristian-Work/vkd3d-proton> (see its `LICENSE` and `COPYING.LESSER`).
This project uses a fork of it, <https://github.com/ExCorvus/vkd3d-proton> (branch `whibwhi-own`), under the same
licence, whose shader translator dxil-spirv (MIT, copyright its authors) comes from the fork
<https://github.com/ExCorvus/dxil-spirv> (branch `whibwhi-own`). It is not vendored into this repository: build it separately (see `docs/BUILD-AND-RUN.md`) and place the built
`d3d12.dll` (as `d3d12_vk.dll`) and `d3d12core.dll` beside the weave executable. ExCorvus approved reusing it with
these notices on 2026-11-24. The LGPL requires that users can relink/replace it — that holds here, since it is
loaded by file name at run time and the system d3d12.dll is the fallback.

## Required credit (additional term under GNU GPL v3 §7(b)–(c))

This program is free software under the GNU General Public License v3 (or later). In addition, as permitted
by §7(b) and §7(c) of the GPL, the following attribution requirements apply to anyone who uses, builds on,
forks, or redistributes this code, its findings, or its approach:

1. **Keep this notice.** You must retain this `NOTICE.md` and the copyright/author attributions in any copy,
   fork, or substantial portion of the work, including modified versions.
2. **Credit the origin.** Any public use, fork, derivative, video, write-up, or redistribution that builds on
   this approach must give prominent credit to the original author:

   > Based on the GTA VI Xbox→PC porting approach originally created by **ExCorvus**.

3. **Mark changes.** If you modify the work, carry a prominent notice stating that you changed it, so the
   origin is not misrepresented.

These requirements are intended to be compatible with the GPL: they preserve author attribution and legal
notices and do not restrict the freedoms the GPL grants. If any part is held incompatible with the GPL, the
GPL terms prevail to the extent of the conflict, but the request to credit the origin remains.
