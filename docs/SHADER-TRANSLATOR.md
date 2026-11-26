# The shader translator — plan and first code

This is the plan for turning the game's **own** shaders into something a PC graphics driver runs, and a record
of the first code that is in `shell/` today. It is written in plain words on purpose: the design decisions
matter more than the vocabulary.

## 1. The problem in one paragraph

On the console the game does not ship a shader language file that the driver compiles; it ships the GPU's own
machine code for its shaders (RDNA2-class instructions). The console's GPU runs those instructions as they are.
A PC GPU does not: it wants SPIR-V (for Vulkan) from us. So one piece of the shell has to read the game's own
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
(`device_address = heap_base + (game_va - flat_base)`, `shell/hosting.hpp`).

The translator therefore never invents addresses:

- an instruction that reads memory computes the address from its own registers, exactly as the console does;
- in SPIR-V the address becomes an index into the one flat storage buffer:
  `index = (address - flat_base) / 4`;
- `flat_base` is a push constant the shell sets per dispatch/draw; it is the same value the hosting model uses
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

`shell/`:

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

## 12. Correction after the first runs that reach pipeline creation (boots b81-b85)

Section 2 says the description "carries a pointer to the shader blob" and that the translator reads that blob.
Measured: the blob named by the public descriptions (device slots 62 and 86) is a container with the public
parts, but its code part is empty (the letters "NOBC" where portable code would be). The hardware instructions
arrive through the precompiled descriptions instead: device slot 67 for compute, slot 57 for graphics, in blocks
that each end their program with the end-of-program word. Details are in `docs/exe-map/SHADER-PATH.md`
section 9.

What this changes for the plan:

- The translator's input is a block of a slot 57 or slot 67 description, not a part of the container. The
  container still matters: its ISG1, OSG1 and PSV0 parts describe inputs, outputs and resource use in the public
  format, which the translator needs and does not have to guess.
- Before any translation, the shell needs a complete reader of instruction lengths for the hardware instruction
  set, so that it can walk a whole program from its first word to the end-of-program word. Landing exactly on
  that word is the check that the walk is right. The next step is that reader and, with it, a count of which
  instructions the game's programs use (numbers only, in memory), so the translator covers the common ones first.
- Pipeline requests come from the game's worker threads in very large numbers. Translating on the thread that
  asks is what the game does on the console; rule 7 bans building pipelines on another thread because that
  crashed the driver in the earlier attempt. Which of the two the shell does is ExCorvus's decision; until then only
  the device's own thread is served.

## 13. What the game's programs are made of (boots b86-b87; measured, numbers only)

`shell/shader_walk.*` reads the length of every instruction from the public description of the instruction
formats. The graphics library walks each distinct program block of the precompiled pipeline descriptions once,
in memory, and counts (format, opcode) pairs. Nothing of a program is written anywhere; these are counts.

**The first 1,024 distinct programs of a run** (840 KiB of blocks):

- 997 walk from the block's first word exactly to the end-of-program word. So the length rules are right for
  these, and a program starts at its block's first byte.
- 27 do not: each meets a word whose top six bits are 111111, which is no format of the public description. In
  every one of the 12 looked at, the instruction before it is the same (two-source vector format, opcode 3), the
  stop is 53 to 71 words into the program, and a walk can only be picked up again 3 to 60 words later. What
  those words are is not known (an instruction format of this console's GPU that is not public, or data in the
  middle of the program). Such a program has to be refused by the translator until this is understood.
- The 997 hold 106,163 instructions; the longest has 2,802. They use 222 different (format, opcode) pairs.

**The most used, in order** (format, opcode number inside the format, occurrences). The numbers are as read from
the instruction words; the names have to be taken from the public opcode tables of this GPU generation before
anything is implemented, and are deliberately not written here from memory.

| format opcode: count | | | |
|---|---|---|---|
| VOP2 8: 13,371 | SOPP 12: 6,038 | VOP2 43: 5,666 | SOP1 4: 5,525 |
| VOP3 331: 5,376 | VINTRP 1: 4,707 | VINTRP 0: 4,707 | VOP2 3: 4,343 |
| SOP1 3: 3,814 | VOP3 384: 2,990 | VOP2 4: 2,765 | VOP2 1: 2,190 |
| SOPP 33: 2,087 | VOP1 1: 2,075 | VOP1 42: 2,066 | SMEM 2: 2,047 |
| EXP 0: 1,838 | MIMG 32: 1,554 | VINTRP 2: 1,294 | VOP2 16: 1,144 |
| VOP3 264: 1,122 | SOPP 32: 1,103 | SOPP 1: 997 | SOP2 30: 969 |
| SOPP 4: 936 | SMEM 3: 931 | VOP2 44: 929 | VOP2 26: 752 |
| VOP1 8: 752 | MUBUF 14: 728 | VOP3P 33: 722 | VOP3P 34: 720 |
| SOP1 10: 718 | SMEM 9: 708 | VOP3 257: 695 | VOP1 6: 653 |
| SMEM 10: 621 | VOP2 45: 619 | SMEM 8: 574 | SOP2 15: 541 |
| SOPP 8: 526 | SMEM 4: 512 | SOPC 1: 489 | VOP2 47: 461 |
| SOP2 39: 460 | SOP2 21: 450 | VOP2 37: 431 | VOP2 0: 429 |
| SOP2 11: 411 | SOPC 0: 409 | VOP1 37: 387 | SOP2 14: 372 |
| SMEM 11: 352 | SOPP 15: 317 | SMEM 12: 311 | VOP1 7: 307 |
| MUBUF 13: 290 | SOPP 2: 289 | VOP3 260: 283 | VOP3 259: 279 |
| MIMG 39: 257 | MIMG 0: 251 | VOP1 39: 236 | VOP3 6: 236 |

The first 16 pairs are about two thirds of all instructions; the first 64 are about 94%. Two-source and
three-source vector arithmetic dominate; then attribute interpolation (pixel shaders), scalar moves and program
control, scalar memory loads, exports, image sampling, and buffer loads.

**What this says about the order of work:**

1. Redo the instruction reader (`shader_isa.*`) on top of the walk, with the fields of every format.
2. Straight-line arithmetic first (the vector formats), then scalar moves and memory loads, then exports and
   attribute interpolation - that is a whole simple vertex or pixel shader.
3. Program control (branches) needs the control-flow rebuild of section 6.
4. Image sampling and buffer loads need the memory model of sections 4 and 5.
5. The 27 programs with the unknown words are refused until the words are understood.

## 14. The instruction reader, rebuilt on the program walk

`shell/shader_isa.*` no longer has its own idea of what an instruction is. It calls `shader_walk.*` for the
format, the opcode number and the length of every instruction, and then reads that instruction's fields. One
reader of the stream means the two can never disagree about where an instruction starts or ends, and the
"the walk landed exactly on the end word" check of section 13 applies to every decode.

**What it extracts.** For every format of section 13:

- The destination, as a typed operand: a scalar register (codes 0..101 on this generation), a special scalar
  register (102..127, kept as its number and named where a public table names it: flat scratch, XNACK, VCC,
  TBA/TMA, M0, SGPR_NULL, EXEC), or a vector register. Formats whose result is implicit (scalar compare to SCC,
  vector compare to VCC) report no destination, because none is encoded.
- Up to three sources, each typed: scalar register, vector register, inline constant (with its 32-bit value when
  this build names the code: integers 0..64, -1..-16 and the nine float constants), or "the constant word that
  follows" with that word's value. Values 256..511 of a 9-bit field are vector registers; 255 is the constant
  word; 128..254 are inline codes.
- The 16-bit immediate of the constant formats (SOPK, SOPP), which is part of the instruction word and not a
  constant word.
- The fields of the second word, per format: the three 9-bit sources and the modifiers of VOP3 and VOP3P (clamp,
  absolute/negate-hi, op_sel, output modifier/op_sel-hi, negate); the offsets, flags, address/data registers and
  resource fields of DS, FLAT, MUBUF, MTBUF and SMEM; the export mask, target and four sources of EXP; the
  address, data, resource, sampler, dimension, mask and cache fields of MIMG. MIMG's extra address words (the
  count is in bits 2..1) are kept raw: this reader does not establish from its sources which bits of those words
  hold a register number, so it decodes nothing from them.
- One constant word after an instruction is identified by the walk's length, not by re-testing the 255 fields.
  That covers the source-is-255 case, the six two-source vector opcodes that always take a constant, and the
  scalar constant-format opcode that does the same.

**What is still raw.** The trap temporaries (112..123) are kept as numbers; an inline code outside the ranges
above keeps its code and is marked as having no known value; MIMG's extra address words are raw; the packed
format's op_sel bits are reported as one group (they mean different things per opcode); the format and number
of an MIMG/MTBUF resource field are the encoded fields (`srsrc >> 2`, the combined 7-bit buffer format), not
expanded into register indices or format enumerations. A program whose walk meets a word of no format, or that
does not end on its last word, is refused with the position and the reason.

**Opcode numbers corrected.** The previous reader named `s_mov_b32` as scalar-one-source opcode 0 and
`v_add_f32` as two-source vector opcode 1. In the public tables for this generation `s_mov_b32` is opcode 3 and
`v_add_f32` is opcode 3; the old numbers named other instructions. The reader and the translator now use the
public numbers, and the translator's list of accepted instructions is the same set as before (`s_mov_b32`,
`s_movk_i32`, `s_add_u32`, `s_sub_u32`, `v_mov_b32`, `v_add_f32`, `v_nop`, `s_nop`, `s_endpgm`), now with the
right numbers.

**Public sources.** The field positions come from the GFX10 encoding classes in LLVM's AMDGPU backend
(`VOP1Instructions.td`, `VOP2Instructions.td`, `VOPCInstructions.td`, `VOPInstructions.td`, `SOPInstructions.td`,
`DSInstructions.td`, `FLATInstructions.td`, `BUFInstructions.td`, `SMInstructions.td`, `MIMGInstructions.td`,
`SIInstrFormats.td`), and the operand numbering and inline-constant values from `AMDGPUDisassembler.cpp` and
`AMDGPUBaseInfo.cpp` in the same project. Only the published bit positions and tables were used; no LLVM code
was copied.

**Where the walk and the public tables disagreed, and the correction.** When this reader was written the walk
read three opcode fields too narrowly: the packed three-source format has eight opcode bits at 23..16 (the walk
read seven, 22..16), the untyped buffer format has eight at 25..18 (the walk read seven), and the typed buffer
format has its opcode at 18..16 plus one bit in the second word (the walk read 18..15). That changed only the
opcode number those formats were reported under, never a length. The three fields were checked against the
public definitions (LLVM's `VOP3Pe_gfx10`, `MUBUF_Real_gfx10`, `MTBUF_Real_gfx10`) and the walk now reads them
as defined there; its test has a word for each. The counts of section 13 are not changed by this: the packed
format appears there with opcodes 33 and 34 and the untyped buffer format with 13 and 14, all below 128, and no
typed buffer instruction appears in that list at all.

## 15. Where the translator gets its program, and what the programs expect (boots b89-b96; measured)

**The input is the container, not the precompiled description.** Section 12 said the code is only in the
descriptions of device slots 57 and 67. That was wrong: every container holds the console's program,
zlib-packed, inside a module (`docs/exe-map/SHADER-PATH.md` section 10). `shell/shader_program.*` unpacks
the module in memory and finds the program; the instruction reader decodes it. In a run this worked for
all 581 distinct containers. Because the game falls back to the public pipeline descriptions with these
containers when slots 57 and 67 are refused, a translator can work from the container alone:

| from | what |
|---|---|
| the module's code | the program |
| the container's ISG1 and OSG1 parts (public layout) | the shader's inputs and outputs by name |
| the container's PSV0 part (public layout) | the resources the shader binds: kind, register, space |
| the pipeline description (public layout) | the root signature, the other fixed state |

**What the programs expect and use** (`shell/shader_shape.*`; counts in FINDINGS.md, entry for boots
b89-b96). The points a translator has to be built around:

- A descriptor (of an image, a sampler, a buffer) is always loaded in one step from a table whose address
  arrives in a register, at an offset that is a multiple of 32 bytes. Inference: the register holds a
  root descriptor table's place in the game's descriptor memory. If that holds, "load 8 words from
  [table + 32 * n], then sample" translates to "resource n of that table", and the translator never has to
  read the game's descriptor records at run time.
- Which register carries which table is not the same in every program. It has to come from data: the
  module's tables, the PSV0 part and the root signature. Not established yet; it is the next thing to
  work out.
- 608 of 1,302 precompiled programs have branches, 108 have loops. Control flow has to be rebuilt
  (section 6) before most programs can be translated.

**The back end is an open decision.** The PC device takes shaders only as containers of its own two kinds;
it has no in-memory way to be handed SPIR-V (its one SPIR-V input reads files from a folder, which the shell
does not use). So either this translator writes one of those container kinds instead of SPIR-V, or the
separately built PC library gets a small in-memory entry for translated shaders. Until that is decided, the
work that is the same either way comes first: the register-to-table mapping, control flow, and a form of the
program in between (operations on values, resources by table and position) that either back end can be
written from.

**Order of work from here.**

1. The register-to-table mapping (module tables, PSV0, root signature), measured on the game's containers.
2. Control flow: blocks, branches, loops, as a structure a shader language can express.
3. The in-between form and the most used instructions (section 13's list) on top of it.
4. The back end, once decided.

## 16. Step 1 of the order of work is done: which register carries what (boots b97-b99; measured)

The module that holds a program also says how each resource reaches it (docs/exe-map/SHADER-PATH.md
section 11, `read_module_tables` in shell/shader_program.*). For a translator this means:

- A program does not name resources. It loads a descriptor from memory at [scalar register + offset] and uses
  the loaded words in an image, buffer or sampler instruction. The first table turns (scalar register, offset)
  into the register number the shader's source used; the container's public 'PSV0' part turns that number
  into a resource type and a register space. Together they give the declaration a PC shader needs.
- Constants and buffer addresses that arrive directly in scalar registers are named by the first table too.
- The scalar registers of the root parameters are known without running anything: they are given out in order,
  from s8 (vertex, pixel) or s4 (compute), each parameter taking as many as its count word says.

Counted over all 581 modules of a run, with no exception to the three statements above. What is left open is
listed in FINDINGS.md: ten modules with an entry kind that is not understood, fixed samplers, and the registers
a program reads that are not root parameters (s12 in most pixel programs, s5-s7 in compute programs, s4 in
vertex programs).

Next in the order of work: control flow, then the in-between form.
