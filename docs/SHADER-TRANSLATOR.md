# The shader translator — plan and first code

This is the plan for turning the game's **own** shaders into something a PC graphics driver runs, and a record
of the first code that is in `weave/` today. It is written in plain words on purpose: the design decisions
matter more than the vocabulary.

## 1. The problem in one paragraph

On the console the game does not ship a shader language file that the driver compiles; it ships the GPU's own
machine code for its shaders (RDNA2-class instructions). The console's GPU runs those instructions as they are.
A PC GPU does not: it wants SPIR-V (for Vulkan) from us. So one piece of the weave has to read the game's own
shader code and re-express it as SPIR-V, exactly — not "something similar that looks right". The rule from
ExCorvus stands: **translate faithfully; when our translator is wrong, fix the translator, never rewrite the
shader.** A shader we cannot translate yet is refused with a precise reason, not approximated.

## 2. Where it plugs in

The image never asks for a shader compiler directly. It builds a pipeline state and hands it to the device
(see `docs/exe-map/GRAPHICS-CALLS.md`, device slots 62 and 67). That description carries a pointer to the
shader blob the game loaded from its own data. So the translator's home is behind those device slots:

```
game builds a pipeline state  ->  our d3d12_xs device wrapper (slot 62 / 67)
                                     -> find the shader blob in the description
                                     -> ShaderTranslator::translate(bytes in game memory)
                                     -> SPIR-V module (in memory)
                                     -> our GPU backend builds a pipeline from it
```

The same translator will later serve any place a shader appears (compute first, then graphics). Nothing in it
knows about GTA VI: it reads a console shader and writes SPIR-V. The game-specific facts (where the blob is,
which stage, the workgroup size) stay in the device wrapper.

## 3. What one translation pass does

1. **Locate the code.** The caller gives us a pointer and a length into the game's memory. Nothing is copied to
   disk. The shader *header* (stage, entry offset, user data/registers, resource declarations) will be parsed
   here; today the caller states the stage and workgroup size, because the blob container has not been measured
   yet.
2. **Decode the instruction stream.** Each instruction's format (SOP1, VOP3, SMEM, …), opcode and register
   fields are read from its words. A word that matches no known format stops the pass.
3. **Check it.** Every instruction must be one the translator actually implements. Anything else stops the
   pass. This check happens before a single byte of SPIR-V is produced, so there is never a half-right module.
4. **Describe the shader in memory.** Registers become variables, the shader's constant inputs become push
   constants, and its memory reads become reads of the game's own memory (below). The intermediate form is the
   decoded instruction list plus, as control flow is added, a basic-block graph.
5. **Write SPIR-V.** Our own small writer emits the module: capability, memory model, entry point, types,
   constants, register variables, the function body. The writer is our code; no third-party SPIR-V library or
   generated header is used.
6. **Cache by content.** The module is stored under a hash of the exact shader bytes plus the options that
   change the module (stage, workgroup size, entry). The same shader asked for twice is translated once. The
   cache is in memory only; the process ends, it is gone.

## 4. The memory model (why this fits the flat-out footing)

The game's shaders address memory with the game's own flat addresses. There is no conversion table (MAP.md
§1.4): a graphics address is a raw title address, and the console hardware reads it directly. Our hosting
model already solves this with one flat backing and a uniform shift
(`device_address = heap_base + (game_va - flat_base)`, `weave/hosting.hpp`).

The translator therefore never invents addresses:

- an instruction that reads memory computes the address from its own registers, exactly as the console does;
- in SPIR-V the address becomes an index into the one flat storage buffer:
  `index = (address - flat_base) / 4`;
- `flat_base` is a push constant the weave sets per dispatch/draw; it is the same value the hosting model uses
  for every other GPU reference.

Consequence: a shader that reads a pointer out of a buffer (the game does this constantly — indirect draws,
descriptor tables) reads the game's real bytes, and the address it computes is a real title address that the
flat backing already covers. No shadow copies, no rebasing pass, nothing to keep in sync.

Registers are untyped 32-bit patterns on this hardware. The translator keeps them as 32-bit integers and only
reinterprets them as floats at the instruction that says so. That is not a shortcut; it is what the hardware
does, and it keeps future reinterpretations correct.

## 5. The texture question (the hard part, stated honestly)

Textures are where "faithful" costs real work. On the console, a texture's memory is laid out in the console's
own tiling/swizzle, and the sampler decodes that layout in hardware. On a PC, the driver chooses its own
layout; a normal `OpImageSample` would read the PC layout, not the console's.

Two ways out, and the choice:

- **Not chosen:** converting every texture to a PC layout up front. That changes the bytes the game's own
  shaders and compute passes read, breaks the game's size/mip arithmetic, and is exactly the "materialize the
  asset corpus" trap.
- **Chosen:** keep the game's texture memory as it is, and make the *shader* read it the way the console's
  sampler would. That means the translated fetch must apply the console's tiling arithmetic. This is real work
  and it is staged: the texture description and the 0x6c8-byte layout block the game itself computes
  (GRAPHICS-CALLS.md §3) are the sources for the arithmetic. A tiling mode we have not implemented is a
  refusal for that shader, not a silently wrong fetch.

Until that stage is done, translated **compute** shaders that do not sample textures are useful and testable
(the game's GPU-driven world depends heavily on compute). Graphics stages and texture fetches come after.

## 6. Control flow

Console shaders branch with scalar branches, `s_cbranch_*`, and indirect jumps. SPIR-V wants structured
control flow (blocks, `OpSelectionMerge`, `OpLoopMerge`). The plan:

- build basic blocks from branch targets and translate structured if/else and simple loops first;
- an irreducible graph or an indirect jump is **refused** with the PC and the reason;
- no "flatten every branch into a state machine" trick at first: it can be faithful, but it hides translation
  bugs and makes the output huge. If a real shader needs it, it becomes a deliberate, tested mode.

Today the translator refuses all branches. That is visible in the tests, so it cannot silently regress.

## 7. Threading, caching, and the two banned things

- Translation runs on the calling thread, on demand, synchronously. That is also why it does not run into the
  banned "off-thread async pipeline builds": there is no background compiler thread at all.
- The in-memory cache is keyed by content plus options, guarded by one mutex, and versioned by a tag inside the
  hash so a translator change never serves an old module.
- Nothing is written anywhere: no `.spv` files, no temp files, no logs quoting shader bytes. Diagnostics name
  PCs, formats, opcodes and reasons only.

## 8. What is in the code today

`weave/`:

- `spirv.hpp` / `spirv.cpp` — our SPIR-V writer. Module header and instruction framing from the public
  specification, with the opcodes, capabilities, storage classes and decorations the translator uses.
- `shader_isa.hpp` / `shader_isa.cpp` — the instruction reader. Classifies the formats this project needs
  (SOP1, SOP2, SOPK, SOPC, SOPP, VOP1, VOP2, VOPC, VOP3, SMEM, MUBUF, DS, FLAT, IMAGE), decodes the scalar and
  vector register fields, the inline constants the translator implements (integers 0..64, the eight common
  float constants), and the two-word forms. Anything else is named by format and opcode number, never guessed.
- `shader_translate.hpp` / `shader_translate.cpp` — the translator and the cache. It translates, today, a
  straight-line compute shader made of: `s_mov_b32`, `s_movk_i32`, `s_add_u32`, `s_sub_u32`, `v_mov_b32`,
  `v_add_f32`, `s_nop`, `s_endpgm`. Everything else is refused with a diagnostic: other opcodes, branches,
  stages other than compute, unknown inline constants, bodies without `s_endpgm`, bodies with code after it,
  a write to (or read of) a special scalar register such as EXEC/VCC/M0, and an entry offset other than 0
  (refused rather than silently ignored until the shader header is parsed).
- `tests/test_shader.cpp` — hand-built instruction words (field layouts written out in the test), checking the
  decode, a successful module, the refusals, the cache and its invalidation by workgroup size. No game data.

The emitted module has been checked with Khronos' `spirv-val` (the Vulkan SDK's validator, run locally on a
module written to a temp file — not committed and not a game shader): it validates as SPIR-V 1.0.

The module for the test shader looks like this in outline (IDs are numbers):

```
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 8 4 1
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
... one Private %uint variable per register ...
%main = OpFunction %void None
  ... stores, loads, OpBitcast, OpFAdd ...
  OpReturn
OpFunctionEnd
```

## 9. What is deliberately not done yet

- The **shader blob container** (header: stage, entry, user registers, resource declarations) — not measured
  yet; the caller states what it knows.
- **Memory instructions** (SMEM, FLAT, MUBUF, DS, IMAGE) — their operand layouts are in the header enum but not
  decoded yet; the flat storage buffer and push constants will appear with the first of them.
- **Control flow**, **graphics stages**, **interpolation/export/import**, **wave intrinsics**, **texture
  tiling** — each gets its own rules and tests, in that order of need.
- **Pipeline integration** behind device slots 62/67 — the wrapper still answers those as not provided.

## 10. Next steps, in order

1. Read the shader blob container from the image's own pipeline path (the pointer at +0x118 in fn 0x247fd30's
   object) and write down its header fields. This is the first thing to measure.
2. Decode SMEM (`s_load_dword*`) and FLAT (raw loads/stores) and emit the flat storage-buffer reads. That makes
   the first *real* compute shader translatable, not just a synthetic one.
3. Add branches and the basic-block graph, then simple loops.
4. Add the shader header parse, so stage/workgroup/user registers come from the blob instead of the caller.
5. Implement slots 62/67 in the device wrapper: hand the blob to the translator, cache the module, and (only
   when ExCorvus opens a GPU run) build the pipeline.
6. Texture description/layout block (xg_xs) and the tiled fetch path, starting with the tiling modes the game
   uses for its compute work.

## 11. Open questions

- Does the blob hold extra metadata the shader itself never sees (resource binding tables, register counts)?
  The answer decides whether the translator needs a second input besides the code.
- Are there shaders with multiple entry points (libraries)? Not measured yet; it needs reading the game's
  pipeline creation calls.
- Which tiling modes actually appear in the game's textures — this bounds the texture work we have to do first.