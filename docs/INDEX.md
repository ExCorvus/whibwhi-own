# What is already known, by topic

Not all of the project fits into one agent's memory (AGENTS.md, "Look it up first"). This page lists each topic
with its state in a sentence or two and where it is written down. [FINDINGS.md](../FINDINGS.md) entries are named by
their date and the start of their heading. Keep this page current with every FINDINGS entry that changes a
topic's state. Where this page and a dated entry disagree, the newer entry wins, and this page is corrected.

## Where a run ends

- **The fault in the user interface** at image+0x161b994, while the program creates a `flash.display.Sprite`
  with an empty script value. It happens in every run since 2026-11-27, at about 57 to 106 s. It is a cause of its
  own after loading, not a follow-on of refused pipelines.
  FINDINGS 2026-11-27 "the fault at about 70 s"; 2026-11-28 "second run with the sampler heap" (the correction).
- **The second stop** at image+0x344196 is the program's own stop on a "no size" answer for a texture.
  FINDINGS 2026-11-27 "the second stop at image+0x344196".
- **The same ending hides progress:** compare counts (frames, pipelines made), not where the run ends.
  [MAPPING-PITFALLS.md](MAPPING-PITFALLS.md); FINDINGS 2026-11-28 "a steady 30-frame rhythm".

## The program's own clocks

- **Watchdog:** the program's own thread. Its state is read over the whole run (scopes, crash marker, the
  progress stamp, the last 10 s before a fault). It runs normally up to the fault. A second check path (field
  0x71b62f0) is never written. [exe-map/MAP.md](exe-map/MAP.md) section 4; FINDINGS 2026-11-28 "the watchdog's
  whole state", "the watchdog's and the frame pacer's last 10 s".
- **Frame pacer and frame clock:** the program's own pacer sets 33,333 us. The weave keeps only the console frame
  clock (device slots 78-81) on Windows' timer. 30 frames a second, each frame empty while draws are held.
  FINDINGS 2026-11-27 "the frame rhythm and the frame pacer"; 2026-11-28 "a steady 30-frame rhythm". Display
  time is answered for only some frames (FINDINGS 2026-11-28 "last 10 s").

## Shaders and pipelines: the ways the program hands them over

Not all shaders come the same way. Do not treat them all the same.
- **Slot 57** (graphics base pipelines, asked first): bare code pieces plus descriptions, no container. Never
  answered so far. FINDINGS 2026-11-28 "what device slots 57, 61 and 67 are handed", "the code handed to device
  slots 57 and 67 is not the code inside the containers", "slot 57 makes base pipelines".
- **Where the 57/67 pieces lie:** the code is in one region of the program's own memory, and the pieces around it
  are in another. The program names the place; it hands over no container. FINDINGS 2026-11-28 "where the pieces
  handed to slots 57 and 67 lie". The parcels of 62 and 86 lie elsewhere: on the stack of the thread that asks. For
  compute, the parcel is made in a scratch area on that stack before the first way is asked, and it is handed
  over only when the first way gives nothing ("correction: the compute parcel"). The builder library's lie in its own file. FINDINGS 2026-11-28
  "where the parcels lie", "the parcels are put together on the stack".
- **Slot 55:** a pipeline derived from a base that slot 57 made. Never reached, because slot 57 is never answered.
  [exe-map/GRAPHICS-CALLS.md](exe-map/GRAPHICS-CALLS.md).
- **Slot 86** (graphics, the fallback when no base pipeline exists): a stream with one container per stage. The
  weave translates these into its own PC form today. FINDINGS 2026-11-28 "the sampler heap reached".
- **Slot 67** (compute, asked first): bare pieces, no container. Never answered. The installation's FidelityFX
  library names it in its own text: DeserializeComputePipelineStateX, a pipeline from code already compiled for the
  GPU. FINDINGS 2026-11-28 "the installation's FidelityFX library names the first compute way".
- **FidelityFX:** the installation holds FSR 4 (the AI upscaler) alone. The denoiser is the program's own.
  Requests come from job workers, so the pass that asks is not on the stack (same entry).
- **Slot 62** (compute, the fallback): a container. The installation's builder library's 52 shaders are PC DXIL
  and are made by vkd3d unchanged (53 pipelines). None of them has run yet. The program's own (2,041) are answered "not implemented"; vkd3d refuses
  them unchanged with "invalid shader bytecode" (step 0, 2026-11-28 19:04/19:07).
  FINDINGS 2026-11-26 "the builder's shaders call the console's own shader functions"; 2026-11-28 "the ray test
  is in the program's own compute shaders".
- **Who hands them over:** the program's passes load through one effect loader (fn 0x1adcc20 -> fn 0x247b6c0).
  Graphics programs go at load time to 57, compute programs to 67 then 62. Slot 86 comes at draw time when no base
  pipeline exists. The builder library calls 62 itself. FINDINGS 2026-11-28 "how the program's passes hand over
  their shaders".
- **Order on each thread:** 57 then 86, 67 then 62. FINDINGS 2026-11-28 "the order of the program's pipeline
  requests".
- **Plan:** the program's own shaders go unchanged to vkd3d-xbox, and dxil-spirv-xbox gets a door for them (not
  built). FINDINGS 2026-11-28 "plan (not a finding)".

## Ray tracing

- **XboxBvhBuilder.dll:** the installation's own library. Its compute shaders are PC DXIL carrying the console
  compiler's own functions (copysign and others), handed over as they should be. Its root signature has flags
  0x3000000, which Windows refuses and vkd3d takes. Source: the weave's comments in `weave/d3d12_xs.cpp`
  (D3D12SerializeVersionedRootSignature) and `weave/xbox_bvh_builder.cpp`; FINDINGS 2026-11-26 "the builder's
  shaders", 2026-11-27 "the builder's 52 shaders".
- **Ray tests:** none in the graphics pipelines. They are in the program's own compute shaders (14 at first
  count, then 55 once more of them could be read). FINDINGS 2026-11-27 "the refused pipelines hold no ray test";
  2026-11-28 "the ray test is in the program's own compute shaders", "the code reader knows the extension word".
- **The program's lighting passes:** set up in one function (fn 0xcba630; FINDINGS 2026-11-28 "the program's own
  compute shaders handed unchanged to vkd3d"), one pass after another by name: bvh_skinning, indirect_light_probes, indirect_diffuse_lighting,
  sr_tracing/sr_denoising and others.

## Shader translation (the weave's own translator)

- It writes DXBC and models a wave as one lane, so it cannot express cross-lane steps or ray tests.
  [SHADER-TRANSLATOR.md](SHADER-TRANSLATOR.md); FINDINGS 2026-11-28 "the code reader knows the extension word".
- **Samplers:** taken by number, through Windows' sampler heap; device slot 61 answered. FINDINGS 2026-11-28
  "samplers taken by number", "the second part of the sampler", "device slot 61 answered".
- **Shader store and vkd3d cache:** on the PC only. AGENTS.md rule 5; FINDINGS 2026-11-28 "the shader store",
  "vkd3d's pipeline cache".
- **The rectangle list** (topology 0x11): drawn through a geometry shader that adds the fourth corner.
  FINDINGS 2026-11-28 "the console's rectangle list answered".
- **Windows' own D3D12 as a check:** FINDINGS 2026-11-28 "Windows' own Direct3D 12 accepts", "which root
  signatures Windows refuses". vkd3d stays the path.

## Heaps

- Look at how heaps act, not at what is in them. [exe-map/DESCRIPTORS.md](exe-map/DESCRIPTORS.md).
- **One memory for the heaps that shaders see:** vkd3d-xbox takes the program's memory. Heaps the shaders do not
  see are held twice, and the shell copies the library's bytes back into the program's memory.
  FINDINGS 2026-11-28 "every heap the shaders see is held twice", "one memory per heap, first run", "one memory
  for the heaps shaders see", "the shell listens to the heaps", "the shell speaks".
- **The GPU on this PC** writes its own instruction words; the driver takes memory someone else chose as a heap.
  FINDINGS 2026-11-28 "the GPU on this PC answers", "the driver takes memory someone else chose".

## Windows answers

- **The runtime and its interfaces** go to Windows' own runtime. The user is answered as Windows answers on an
  account with no Xbox user. Networking is held. FINDINGS 2026-11-27 "every runtime interface the program asks
  for"; 2026-11-28 "the user".
- **FidelityFX** goes to Windows' own AMD library (amd_fidelityfx_dx12.dll from the driver). FINDINGS
  2026-11-28 "the FidelityFX import answered by Windows' own AMD library".
- **GameInput** goes to Windows' current GameInput (4.0). FINDINGS 2026-11-27 "the import GameInput.dll".
- **IDXGIDevice** is answered from Windows' adapter and monitor. FINDINGS 2026-11-27 "the device's IDXGIDevice".
- **Window and screens:** the program handles no window messages. A script makes the window fullscreen from
  outside; the process manifest makes it aware of screen scaling. FINDINGS 2026-11-27 "console formats 118-120"
  (fullscreen); 2026-11-28 "the window in front", "the screens plugged anew", "the shell's process tells Windows".

## Sound, storage, files

- **Sound:** the AcpHal chip answers, the stream channel answers, and the program passes the sound start.
  FINDINGS 2026-11-27 entries from "the program's audio setup" to "the silent answer".
- **Files:** every archive and the texture database open fine. FINDINGS 2026-11-27 "which files the program asks
  Windows for", and the entry "the fault at about 70 s" (file trace).
- **The T: drive:** FINDINGS 2026-11-27 "the T: drive".

## The executable as a whole

- The exe contains four builds, a runtime loader, crash reporting, an AS3 machine for the user interface and
  three bytecode machines. A survey from 2026-11-28 is kept on the PC (not in the repository). The maps are in
  [exe-map/](exe-map/), and the libraries in [console-libs/](console-libs/).
- **The version gate:** device slot 49 is switched off by the program's own byte, not by the Windows version.
  FINDINGS 2026-11-28 "the watchdog's whole state, and the version gate's byte".

## Sources and rules

- No leaked development kits. AGENTS.md rule 12; FINDINGS 2026-11-28 "sources".
- Pitfalls of mapping a program: [MAPPING-PITFALLS.md](MAPPING-PITFALLS.md). The idea behind the project:
  [CODEWEB-THEORY.md](CODEWEB-THEORY.md).
