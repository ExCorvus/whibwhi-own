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
  count is in bits 2..1) are kept raw by this reader. The access pass of section 25 decodes them: each word
  holds up to four 8-bit register numbers.
- One constant word after an instruction is identified by the walk's length, not by re-testing the 255 fields.
  That covers the source-is-255 case, the six two-source vector opcodes that always take a constant, and the
  scalar constant-format opcode that does the same.

**What is still raw.** The trap temporaries (112..123) are kept as numbers; an inline code outside the ranges
above keeps its code and is marked as having no known value; MIMG's extra address words are raw in this reader (section 25's pass decodes them); the packed
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

## 17. Control flow: blocks, branches and loops

Step 2 of the order of work is built: `shell/shader_flow.{hpp,cpp}` reads a decoded program and answers what a
compiler needs about its control flow, and `shell/tests/test_shader_flow.cpp` measures every number below on
programs built word by word in the test (no game bytes). This section is written from that code and those tests;
statements taken from a public source are marked, and what is not established is listed at the end.

**Branches.** The program-control words of this generation and their numbers are taken from two public tables,
which agree: LLVM's AMDGPU backend, `SOPInstructions.td` (the gfx10 `SOPP_Real_32` definitions) and Mesa's ACO
compiler, `aco_opcodes.py` (its gfx10 column). `s_endpgm` is 0x001, `s_branch` 0x002, and the six conditional
branches are 0x004 `s_cbranch_scc0`, 0x005 `scc1`, 0x006 `vccz`, 0x007 `vccnz`, 0x008 `execz`, 0x009 `execnz`.
The 16-bit immediate is a signed count of 4-byte words counted from the instruction that follows the branch:
target byte = branch pc + 4 + immediate * 4. LLVM states this in `MCTargetDesc/AMDGPUAsmBackend.cpp`
(`adjustFixupValue`, `fixup_si_sopp_br`: the encoded value is (label - branch - 4) / 4) and Mesa's ACO in
`aco_assembler.cpp` (`fix_branches` writes target offset - branch offset - 1, both in words). Measured by the
test: a program whose branch carries an immediate of 1 starts the block at the third instruction after the
branch, and one carrying -3 branches to the instruction two words before it.

**Blocks.** A block starts at the first instruction, at every branch target, and at the instruction after every
branch (an unconditional branch included, so dead code after one forms its own block and is reported). Each
`FlowBlock` records the first instruction and its count, both byte offsets, how it ends (fallthrough,
unconditional branch, conditional branch with one of the six conditions, or end of program) and its successor
block indices. A branch target that is outside the program, or that is not the first word of an instruction (for
example the second word of a two-word VOP3), makes the whole analysis refuse with the branch's pc and the target's
pc: measured by the test with three such programs.

**Graph facts.** Predecessors, reachability from the entry, and a reverse postorder are reported; unreachable
blocks are listed in `FlowGraph::unreachable` and left in the block list (the test's dead block after an
unconditional branch is still block 1). Immediate dominators are computed by the iterative algorithm of Cooper,
Harvey and Kennedy (the intersection walks two blocks up their dominator chains by postorder number, relaxed in
reverse postorder). Back edges are the edges whose head dominates their tail; a natural loop is its header plus
every block that reaches the back edge's tail without passing the header (the construction in Muchnick and in
Aho/Lam/Sethi/Ullman), and each loop lists its blocks, its back edges, the blocks that leave it, and the blocks
it leaves to. A graph is reducible exactly when every retreating edge of a depth-first walk is a back edge
(Tarjan; Hecht and Ullman); the test's irreducible program is an entry that branches into both blocks of a
two-block cycle - its dominators are computed, no edge qualifies as a back edge, its end-of-program block is
unreachable, and the pass reports "not reducible".

**Structuring.** For a reducible graph, `structure_of` builds a tree of regions a shader language can express
without goto: `sequence`, `if-then` (with which side the body is on), `if-then-else`, `loop` (header, the one
block it leaves to, the body, the blocks that break and the blocks that continue), and plain `block` leaves. It
works the way Cifuentes and Erosa/Hendren describe: postdominators of the reversed graph (with one virtual exit)
give the block where the two paths of a conditional meet again, so a conditional becomes an if-then when one
successor is that meeting block and an if-then-else when neither is; natural loops are built from the inside out,
a loop's body is read from the header's inside successor until the loop's exit or header, and inner loops and
conditionals inside it are handled the same way. The test measures exact trees for: straight line, if-then,
if-then-else, nested ifs (an inner conditional whose merge is inside the outer body), a loop with one exit, a
loop with a break out of the middle (block 2 leaves, the latch at block 3 continues), and two nested loops; it
also measures the case where a branch targets the program's last instruction.

**What is refused, with a reason, never guessed.** An irreducible graph is not structured. A loop that leaves to
more than one block is not structured (the test's loop leaves to blocks 4 and 5; one loop statement can only
leave to one). A loop whose header has two paths into its body is refused. A conditional branch whose two paths
do not meet again at one block inside the region being built is refused - that covers one side ending the
program, and one side leaving the enclosing loop while the other stays. A block that would have to appear twice
(a crossing edge) is refused. The flow pass itself refuses, by name: `s_setpc_b64` and `s_swappc_b64` (jumps and
calls to an address in a register), `s_call_b64` (subroutine call), `s_subvector_loop_begin` and
`s_subvector_loop_end` (hardware loops), the four `s_cbranch_cdbg*` debug-system conditional branches,
`s_trap`, `s_endpgm_saved` and `s_endpgm_ordered_ps_done`. Their existence and numbers are public (LLVM
`SOPInstructions.td` and ACO `aco_opcodes.py`). Which of them the game's programs contain is measured in
section 18.

**Not established.** How the hardware's execution-mask instructions (`s_and_saveexec_b64` and the like) interact
with branches in compiled shaders: public sources describe the mask instructions as ordinary scalar
instructions, and no source read for this work says how a compiler divides a wave's work with them. The pass
treats a conditional branch as "either edge possible" and makes no claim about which lanes take it; a back end
will need that from measurement, not from this file. Also open: both LLVM (the gfx1010 offset-0x3f workaround in
`AMDGPUAsmBackend.cpp`) and ACO (`fix_branches_gfx10`) document a hardware bug for a branch whose encoded offset
is 0x3f and avoid emitting it; whether the console's GPU has that bug is not established here, and this pass
decodes the arithmetic only. Finally, the two text forms (`flow_text`, `structure_text`) are diagnostics for
this project's logs: they print block indices, pcs and numbers, and are never written to a file by the shell.

## 18. What the control-flow pass makes of the game's programs (boot b109; measured)

The pass of section 17 was run, in memory, on every program the instruction reader decoded in one run: 927
programs from the first 1,024 different shader containers (505 pixel, 227 vertex, 195 compute). Numbers and the
pass's own reasons only; no program text is logged.

| | pixel | vertex | compute |
|---|---|---|---|
| programs decoded | 505 | 227 | 195 |
| flow analysed | 505 | 1 | 195 |
| not analysed: a call to an address held in a register (`s_swappc_b64`) | 0 | 226 | 0 |
| analysed and reducible | 505 | 1 | 195 |
| no loop | 401 | 1 | 132 |
| one loop / several loops | 4 / 100 | 0 | 9 / 54 |
| written as nested ifs and loops | 401 | 1 | 136 |
| not written as a tree | 104 | 0 | 59 |

- **Every analysed program is reducible** (701 of 701): no loop is entered at two places.
- **Sizes.** Pixel: 333 are one block, 68 have 2 to 4 blocks, 102 have 5 to 16, 2 have 17 to 64. Compute: 55 are
  one block, 41 have 2 to 4, 21 have 5 to 16, 34 have 17 to 64, 44 have more than 64. 11 compute programs hold
  blocks no path reaches.
- **Why 163 are not written as a tree.** All 104 pixel programs with a loop, and 18 compute programs: the block
  that heads the loop ends in a conditional branch with both ways inside the loop, and the pass only handles a
  header with one way in. 31 compute programs: a loop leaves to more than one block. 10 compute programs: a
  block would have to appear twice, or was not placed. None of the other refusals of section 17 occurred: no
  subroutine call by number, no hardware loop, no debug branch, no trap, no other end-of-program form.
- **The vertex call.** 226 of 227 vertex programs call through the address in s4 within their first
  instructions (section 16 and FINDINGS.md). The pass refuses a program with such a call. Since the call comes
  back to the next instruction, it can be treated as an ordinary instruction once the translator supplies what
  the called routine supplies (the vertex inputs, in vector registers from v4 on). Not built.

What this means for the translator: flow is not a blocker. The tree is the convenient form and already covers
538 programs. For the rest, any flow graph can be written as one loop around a choice of the next block, which
needs no tree; reducibility of all 701 means the tree form can also be extended (a loop header with two ways
in, a loop with several exits) without changing the programs' meaning. Neither extension is built.

## 19. The reader's three-operand instructions, corrected (public table; hand-built words; boot b109)

Checking the instruction meanings against the reader showed two faults in how the reader read the two-word
vector format (VOP3), both corrected in `shell/shader_isa.*`:

- **A compare's result is not a vector register.** Opcodes below 0x100 in this format are the compares; their
  destination field names the scalar register pair, the condition mask or the execution mask that gets the
  result. The reader had typed it as a vector register for every instruction.
- **Not every instruction uses all three source fields.** The reader counted three sources always, so an unused
  field holding 0 looked like a read of scalar register 0. The public table gives the operand count of each
  instruction; with its rule for this format (a compare keeps its number, a two-source instruction is its
  number + 0x100, a one-source instruction its number + 0x180): compares use two fields; re-encoded two-source
  instructions use two, except the select and the three carry-in adds and subtracts, whose third field is a
  lane mask; re-encoded one-source instructions use one, and the no-operation, pipe flush and clear-exception
  use none; the format's own instructions use two or three as listed. For an opcode the table does not list
  all three fields are kept and the reader says the count is not established. Eight instructions have a
  second, scalar result in bits 14 to 8 of the first word (the public encoding file states that field).

Source: Mesa ACO's opcode table (the column for this GPU generation) for the lists and counts, LLVM's AMDGPU
encoding description for the field positions. Numbers only were taken.

Measured effect (boot b109 against b108, the same 505 pixel programs): the report lists the most common sets
of scalar registers read before written, covering 502 of the 505. In b108, 106 of those 502 seemed to read s0
or s1; in b109 none does. The listed programs read only registers the module's first table names and the one
after them (353 read s8-s9 s11-s12, 96 s8-s12, 24 s12, 12 s8 and s12, 10 s8-s9, 5 s8, 2 s9-s14), and 225 of
227 vertex programs read s4 and s8. The program-shape pass also counts a compare's result, and the mask
a select reads, as a pair of scalar registers: the programs move and combine their masks with the 64-bit scalar
instructions (inferred from that; the number of lanes of a wave is not stated by the programs).

## 20. What the binding rule means for the back end (from the measurements of FINDINGS.md, boots b100-b108)

The module's first table says which scalar register carries which root parameter and where in a descriptor
table each resource sits; the game's root signature object agrees with it only in the order and kind of the
parameters (16 different root signatures were named with 926 different programs; one with 98 different first
tables). A PC pipeline therefore cannot take the game's root signature as it is. For each pipeline the shell
has to build a PC root signature from the first tables of the pipeline's programs - the same parameters in
the same order, each table holding the entries the first table lists at their places - and apply the root
arguments the game sets against that one. Register spaces come from the container's public resource list for
declared resources; for an entry no program of the pipeline declares, the class and space are not established.
Not built.

## 21. Any control flow as structured code

The target language has nested control flow only: if / else / endif, loop / endloop, break, and continue. Section
17 builds a tree of regions for that, and section 18 measured that it covers 538 of the 701 analysed programs and
named the rest: a loop header with two paths into its body, a loop that leaves to two blocks, and a block that
would appear twice. Section 18 ended by saying that the dispatch-loop form was not built; this section is that
pass. `shell/shader_emit_flow.{hpp,cpp}` writes any graph `flow_of` accepts as a program in that language, and
`shell/tests/test_shader_emit_flow.cpp` proves by execution that the program runs the same blocks in the same
order as the graph. Everything below is measured by that test on programs built word by word in it, unless it is
marked as a source.

**The form.** A flat list of items in emission order: `BLOCK(n)`, `IF(n)` with which edge is the then side,
`ELSE`, `ENDIF`, `LOOP`, `ENDLOOP`, `BREAK`, `CONTINUE`, and - only when the graph cannot be written nested -
`SET_NEXT(k)` and `IF_NEXT(k)` over a variable `next`, whose value `END` means the program has ended. A `BLOCK`
item means the block's instructions up to, not including, the program-control word it ends with: a branch is
replaced by the items, and an end-of-program word ends the program after that block. So the program's text ends
where the program ends, and no return statement is needed.

**The nested form, for the graphs `structure_of` writes.** The tree is read from the outside in and becomes
items, with three things added that the tree names but does not spell out:

- a loop's header block is named by the tree (the loop region's block) but is not placed in it; it is emitted as
  the first item inside the loop, and its own branch becomes the loop's test (`if (the branch leaves) break`);
- a block whose branch leaves the loop is listed by the tree as a break block (or a continue block); it is
  emitted as `BLOCK`, then `IF (taken) BREAK ELSE <rest of the region> ENDIF`, so the rest runs only on the edge
  that stays inside;
- an inner loop that leaves to the enclosing loop's exit is followed by a `BREAK`; the tree does not list that
  edge anywhere, and without it the outer loop would run again.

The seven tree shapes of section 17 come out as: straight line 1 item; if-then 5; if-then-else 7; nested ifs 9;
a loop 10; a loop with a break 15; two nested loops 19. How many items an if costs is fixed: the branch block's
item, the `IF`, the arm, the `ELSE` when there is one, and the `ENDIF`.

**The dispatch loop, and how much of the program it takes.** The pass first asks `structure_of` for the whole
graph. If that is refused, it looks for a cut: a block `c` other than the entry such that (a) the blocks a run can
reach from `c` do not include the entry, (b) every edge from outside that set enters it at `c`, and (c) every
natural loop lies wholly inside or wholly outside the set. The smallest such set is tried first. Its prefix is
written by building a graph of the prefix blocks plus one extra block, ending the program, that stands for the
dispatch, and asking `structure_of` to write that graph: the prefix is accepted only when the extra block comes
out as the last block, at the top level, so the nested part is complete before the dispatch begins. Then come the
prefix items, `next = c`, and one dispatch loop over the set: `LOOP; if next == b0 { BLOCK(b0); next = ... }
else if next == b1 { ... } ...; if next == END break; ENDLOOP`, where a block ending in a conditional branch sets
`next` with an `IF` on its own condition, and a block ending the program sets `next = END`. If no cut is usable
the whole reachable program is one dispatch loop entered at the entry block.

Measured on the three shapes section 17 refuses and on the irreducible graph:

| shape | blocks | items | what the pass did |
|---|---|---|---|
| loop whose header has two paths into the body | 6 | 39 | block 0 nested, 5 blocks dispatched from block 1 |
| loop that leaves to two blocks | 6 | 39 | block 0 nested, 5 blocks dispatched from block 1 |
| diamond whose arm is shared (a block twice) | 5 | 38 | no prefix possible; the whole program is dispatched |
| irreducible (two blocks entered from outside) | 3 | 24 | no cut is usable; the whole program is dispatched |

The crossing-edge shape is the one that cannot keep anything nested: every block after the first is reached from
two sides, so there is no single-entry suffix to cut at. The two loop shapes keep the code before the loop.

**Proof by execution.** The test holds two small interpreters. One walks the graph block by block; the other runs
the item list, with a stack for the loops and the `next` variable. Both are given the same branch decision for
each (block, visit count), from a hash of the two, so both see the same decision at the same visit. They must
visit the same blocks in the same order and agree on whether the program ended. Measured: the twelve hand-built
shapes with eight decision sequences each, and 2,000 random programs (1 to 40 instructions, random branches, the
end always reachable) with three sequences each, 6,000 comparisons in all - zero mismatches. Of the 2,000, 665
(33 %) were written nested with no helper variable and 1,335 needed a dispatch loop, 421 of those after a nested
prefix. A one-off run of 50,000 programs and 150,000 comparisons was also clean; that larger number is not part
of the committed test.

**Size.** For the nested form the cost is about one item per block plus one `IF`/`ELSE`/`ENDIF` set per branch:
1.22 items per block over the 665 nested random programs. The dispatch form costs, per block, one `IF_NEXT`, one
`ENDIF`, the block item, and its transfer (one item for an unconditional branch, fallthrough or end of program,
five for a conditional branch), plus one `ELSE` between neighbouring arms and five items for the loop and the END
test: about nine items per block when every block ends in a branch. Over the 1,335 dispatched random programs the
average was 77.6 items for 11.4 blocks (6.8 items per block; the random graphs are smaller and less branchy than
the average game program). This is a size cost of the form, not a measurement on the game's programs: no run of
the game was made for this work.

**What is not established, and what is not done.**

- The pass has not been run on the game's 163 refused programs. No run of the game was made for this work, and
  the counts of section 18 are the reasons `structure_of` gave in the earlier run, not this pass's output.
- The emitted form is this project's own. The back end's spelling of the items (its syntax for a loop, and how a
  `next` variable becomes a register or an index) is not written; that is a later pass.
- The question of section 17 stands: a conditional branch is treated as "either edge possible", and how the
  hardware's execution mask divides a wave's work is not established here.
- A structurer of the relooper family (Zakai, "Emscripten: an LLVM-to-JavaScript compiler", 2011) can sometimes
  avoid the helper variable by copying blocks; that family is cited only, and not implemented, because a copied
  block means the block's instructions run at two places in the emitted program, which this project's translator
  has no need of yet.
- While building this, one property of the graph facts was seen and worked with rather than changed: a natural
  loop as `flow_of` reports it can list a block that no path reaches (a dead block that falls into the loop body -
  the hand-built "loop whose header has two paths into the body" has one, and random programs showed the same).
  For a block no run can enter this makes no difference to the program's meaning, but `structure_of` requires every
  loop block it is given to be placed, so the front graph of a cut lists only the reachable loop blocks. This is a
  fact about the loop list, not a wrong answer from `shader_flow.*`, and that file was not changed.

## 22. What section 21's pass makes of the game's programs (boot b114; measured)

Counted in a run (boot b114) over the programs of the first 1,024 different containers. The flow analysis
accepted 700 of them in that run (505 pixel, 194 compute, 1 vertex; which containers are among a run's first
1,024 differs a little from run to run, so section 18 has 701). The pass writes every one of them:

| | pixel | compute | vertex |
|---|---|---|---|
| nested, no helper variable | 401 | 135 | 1 |
| one loop around a choice of the next block, over all blocks | 104 | 32 | - |
| a nested start, then such a loop over a part of the blocks | - | 27 | - |
| not written | 0 | 0 | 0 |

The 226 vertex programs the flow analysis does not accept are the ones that call through a register near their
start (section 18); nothing about them is counted here. The choice loops of the pixel programs run 5 to 16
blocks (two run up to 4); of the compute programs 36 run more than 64 blocks.

## 23. A program's inputs and outputs against what its container declares (boots b111-b113; measured)

A container has two public parts that list the inputs and the outputs the shader declares ('ISG1' and 'OSG1':
for each element a name, an index, a system value number, a register and a component mask).
`shell/shader_program.*` reads them (`read_signature`), and the run counts, for each program of a run (505 pixel and 227 vertex programs),
whether the simple links between those lists and the program's own instructions hold. They do:

**Pixel programs (505).**
- An interpolation instruction names its input by an attribute number and a channel. In all 500 programs
  that interpolate, attribute number n is the n-th register among the registers of the input elements that are
  not system values, taken in rising order, and every channel used lies in the mask of the elements at that
  register. (Two elements may share a register, each with its own components; the number counts the register
  once. An earlier wording here said "the n-th input element", which is not what was counted; section 26.) (In
  489 the attribute number is also that register itself; the other 11 declare a system value before their ordinary inputs,
  which moves the registers but not the attribute numbers.) 5 programs neither interpolate nor declare an
  ordinary input. In 13 programs some declared input is never interpolated.
- 146 programs declare the position input (system value 1). Vector registers read before they are written:
  v0 and v1 in 431 programs (354 without a declared position, 77 with), v0 to v3 in 68 programs that all
  declare the position, v2 alone in 1, none in 5. (The "v2 alone" was a mistake of the shape pass, corrected in
  section 25: that program reads no vector register.) Reading, not established: v0 and v1 carry the two
  interpolation weights and, when the shader reads the position, v2 and v3 carry its x and y.
- Exports: in all 505 the colour export targets are exactly the registers of the render-target output
  elements (system value 64) and every such element is exported; no program exports or declares depth. Every
  export is in the compressed form (two 16-bit values in a register), every export carries the valid-mask
  flag, and the done flag is on the last export only.

**Vertex programs (227).**
- Inputs: the elements' registers are 0, 1, 2, ... with one element each (227 of 227). The vector registers a
  program reads before writing them, from v4 up, are exactly the components the shader reads when element r
  takes the four registers v(4 + 4r) to v(7 + 4r), one for each component (227 of 227; the other reading tried,
  one register for each component in the mask, holds in 167). No register below v4 is read, except v0 by the
  one program whose only input is the vertex number (system value 6) - the one vertex program that does not
  call through s4 either.
- Outputs: a parameter export to target 32 + k is the k-th register among the registers of the output elements
  that are not the position, in rising order (224 of 227). The other 3 declare the position as their only output and still
  export one parameter (target 32), which nothing declares. The position is exported to target 12 in all
  227; no vertex export is compressed, none has the valid-mask flag, and the done flag is not on the last
  export.

**Samplers loaded from memory.** Every sampler descriptor a pixel program loads through a register is at
offset 0 of what that register points at (989 uses); compute programs load at 16-byte places 64 and 82 (11
uses). In the same run the image made its samplers at places 0 to 10 of its sampler heap (device slot 61), so
places 64 and 82 held nothing yet. That a program's place is a place in the sampler heap rests on the root
signature flag (section 20 and FINDINGS.md, boots b100-b108) and is not confirmed by this.

What this gives the translator: the declared inputs and outputs of the PC shader it writes can be taken from
the container's own two lists, and each is tied to the console program by a rule that held for every program
counted - attribute number to input element, vertex input element to four vector registers, parameter and
colour export targets to output elements.

## 24. The PC container writer and what is still between the pieces

`shell/shader_dxbc.*` (namespace `whiwhi::shell::dxbc`; `docs/SHADER-CONTAINER-WRITER.md`) writes the
container the PC graphics library compiles: the two signature parts and a shader-model-5.1 program, with the
container's checksum. Its test builds eight shaders by hand and has the system's own shader disassembler read
each one back. It has not been given to the graphics device.

So the pieces exist separately: the program and its tables out of the container (sections 15, 16), the fields
of every instruction (14, 19), what the common arithmetic instructions compute (docs/SHADER-MEANINGS.md), the
control flow as nested code (17, 21, 22), the inputs and outputs (23), and a writer for the PC's container.
What joins them is not written: a pass that walks a program's items and writes, instruction by instruction,
the PC instructions that compute the same thing for one pixel, vertex or thread, with the resources declared
from the module's tables and a PC root signature to match (section 20). Until that pass exists and its output
has drawn a picture that can be compared with what the game means to show, none of the above has been checked
by eye.

## 25. What the memory, image, interpolation and export instructions touch (a helper's pass, reviewed; boot b115)

`shell/shader_access.*` answers, for one decoded instruction that touches memory, an image, an interpolated
attribute or an output, which registers hold what, in which order, and how many. It describes; it does not
execute. Every statement and its public source is in [SHADER-ACCESS.md](SHADER-ACCESS.md). What a translator
takes from it:

| group | what the pass tells |
|---|---|
| scalar memory loads | the base is a pair of scalar registers; a 21-bit byte offset; an optional register offset, where the number 125 in the field means none; 1, 2, 4, 8 or 16 words into consecutive scalar registers |
| scalar buffer loads | the same through a four-word buffer descriptor, checked against the descriptor's size; a read outside it gives zero |
| vector buffer loads and stores | the descriptor's four scalar registers, the index and offset registers, a 12-bit byte offset, how many components |
| image instructions | the image descriptor's eight scalar registers and the sampler's four; the order of the address words (offset, bias, compare value, derivatives, coordinates, slice or face, sample number, level of detail); the extra address words, each holding up to four register numbers; which result components are written |
| interpolation | the two steps and the move, the attribute number and component, the register each step reads through |
| export | the target number and its class (colour, depth, position, parameter), the four source registers, the packed form (two 16-bit values in a register), the last-export and valid-mask flags |
| whole-wave instructions | the quad-mask instruction, the wait counters, the hints a one-pixel translation can leave out |

Tested on instruction words built by hand only. The pass of section 26 uses it on the game's programs.

**Review before folding.** The coordinator rebuilt the helper's branch, ran the game-data check and compared
five of its statements with the public files it had read (the address-word order comment in LLVM's intrinsic
definitions, the parameter names, the extra address words and the "no register offset" number in Mesa's
assembler). Four agreed. One did not, and it exposed two mistakes of our own:

1. **The parameter move's source field.** The helper took the field of `v_interp_mov_f32` for an
   inline-constant code (128 for 0, 130 for 2). Both public sources say otherwise: Mesa's assembler writes the
   number 0, 1 or 2 straight into the field, and LLVM's printer reads the field as that plain number. With the
   helper's reading every real move would have been refused. Corrected in `shader_access.cpp` and its test.
2. **The reader did not record what the interpolation steps read.** `shader_isa.*` kept the source field of the
   interpolation format as a raw number and never listed it among the instruction's operands. Now the two steps
   (opcodes 0 and 1) have one source operand, a vector register; the move (opcode 2) has none, and its field is
   marked as a selector.
3. **The shape pass counted the move's selector as a register read.** A move with selector 2 looked like "reads
   v2". Corrected in `shader_shape.cpp`, with a test.

**Measured, boot b115** (the pixel programs of the first 1,024 different containers of the run: 505, of which
500 interpolate; game folder unchanged):

| statement | programs |
|---|---|
| uses the two interpolation steps only | 499 |
| in those: the first step reads through v0 and the second through v1, and no other register | 499 of 499 |
| uses the move only, with selector 2 | 1 |
| vector registers read before written: v0 and v1 | 431 (354 without a declared position input, 77 with) |
| the same: v0 to v3 | 68, all declaring the position input |
| the same: none | 6 (the 5 that do not interpolate, and the one that uses the move) |

So the "v2 alone" program of section 23 does not exist: it was the move-only program with its selector read as
a register.

**Not established.** What v2 and v3 hold in the 68 programs that read them (the position's x and y is a
reading). The arithmetic of the two steps: the operand roles are from public source, the polynomial is a
reading of the public names. What the value in M0 means. Whether the game's programs ever use the image forms
the pass refuses.

## 26. The pass that writes a PC shader for a console program (boots b116-b118; measured)

`shell/shader_pc.*` is the piece section 24 said was missing. It takes one decoded pixel or vertex program
together with its module's two tables, the container's input and output lists and its declared resources, and
writes a PC shader container with `shader_dxbc.*`. **Nothing it writes has been given to the graphics device
yet**, and no picture comes of it yet. What it does:

- **One lane.** Each machine register the program uses becomes one PC temporary. The pass takes every lane of
  the wave to be this one pixel or vertex, so a lane mask (VCC, EXEC, a register pair) is all ones or zero.
  EXEC starts as all ones. A vector register takes a new value only while EXEC's low bit is set; a compare
  writes its result ANDed with EXEC into both halves of its mask; whole-quad mode leaves a mask as it is.
- **Control flow** is taken from section 21's items, one PC instruction for each (if, else, loop, break,
  continue, the "next block" variable). A vertex program's call through a register is skipped (taken to come
  back with the inputs in place: not established).
- **Loads of descriptors are not carried out.** A load through a register the module's first table names is
  looked up in the tables and becomes a fact about the registers it fills: this is the texture at shader
  register r, this is constant buffer r. The instruction that later uses those registers is written against
  the PC resource. A fact made in one block is used in another only when the first block always runs before
  the second and the registers are written nowhere else.
- **Samplers by number.** The game's pixel programs choose their sampler at run time: they load four words from
  the sampler heap at a place they compute (a root constant times 16 in the programs read). The pass keeps the
  computed number and samples with element `number` of a sampler range without an upper end, declared in a
  register space of the shell's own (0x5748). The PC root signature will have to put the whole sampler heap
  there.
- **Root constants** are read from a constant buffer at their declared register at the start of the program.
- **Inputs and outputs** by the rules of section 23: a pixel program's second interpolation step becomes a
  read of the input at the attribute's register and channel (the first step only says which); a vertex
  program's input element r is copied into vector registers 4 + 4r onward at the start, the vertex number into
  register 0; a colour export goes to the output at the target's register (the packed form is unpacked with
  the PC's half-to-float conversion); position and parameter exports go to their output registers; an export
  with the valid-mask flag is followed by a discard when the lane is not live.
- **Instructions:** 186 (format, opcode) pairs and the three families of compares. Each number was compared
  with the public opcode table when the list was written. The pass refuses the whole program when it holds one
  instruction outside the list, and says which.

**How it is tested** (`shell/tests/test_shader_pc.cpp`, hand-built programs only). The written program is run
by a small evaluator of PC instructions in the test, the console program by the reference interpreter of
`shader_meaning.*`, and both must leave the same registers, masks and condition behind: 3,275 random
straight-line programs and 1,189 random programs with forward branches and a counted loop agree (as many again
are left out because the interpreter refuses them or they produce a not-a-number or a signed zero whose
handling is not established). More than seventy instructions the interpreter does not cover are checked one at a
time against the value their public definition gives. A textured pixel program of the shape the game's
simplest one has, and a vertex program, are written from made-up tables and lists, run, and their containers
read back by the system's shader disassembler. The test proves that the two models agree, not that a PC
graphics device or the console hardware computes what the models compute.

**Three mistakes found by building it, all corrected:**

1. `shader_meaning.*` left the scalar condition untouched after `s_and_b32`, `s_and_b64`, `s_andn2_b64` and
   `s_lshl_b32`. The public definitions say they write it (docs/SHADER-MEANINGS.md, the correction at the end).
2. Sections 23 here and 13 of docs/exe-map/SHADER-PATH.md said an attribute number is "the n-th input element".
   What was counted in boots b111-b113 is the n-th **register**: two elements may share a register. The pass
   first followed the wording and refused 227 pixel programs for it.
3. The pass first read the fields of the two no-operations a program begins with as if they meant something
   (an operand selection); they hold the program's identification words.

**Measured on the game's programs** (boot b118; the pixel and vertex programs of the run's first 1,024 different
containers; the written containers are thrown away after the count; game folder unchanged):

| | pixel (505) | vertex (227) |
|---|---|---|
| written | 281 | 129 |
| not written | 224 | 98 |

What the written ones declare: 192 pixel programs constants, the sampler heap and a texture; 60 those and a
constant buffer; 24 nothing; 5 a constant buffer. 120 vertex programs a constant buffer; 7 a constant buffer
and a buffer read by offset; 1 such a buffer; 1 nothing.

Why the others are not written (the first reason the pass meets in each program):

| reason | programs |
|---|---|
| vertex: a buffer read with an element index (`buffer_load_dwordx4`) | 96 |
| pixel: a load through an address the module's tables do not name | 88 |
| pixel: reads v2 (64) or v3 (4) before writing it - the registers section 25 reads as the position's x and y, not established | 68 |
| pixel: `v_mac_f32` (multiply-add, not fused), not covered | 32 |
| pixel: `s_buffer_load_dwordx2` through a descriptor the pass knows nothing about | 14 |
| pixel: reads s9 (10) or s11 (1) before writing it | 11 |
| pixel: a scalar move into a special register the pass does not carry | 6 |
| pixel: a word of a descriptor read as a number | 2 |
| pixel: `v_cvt_f32_ubyte3`, not covered | 2 |
| pixel: the interpolation move | 1 |
| vertex: a parameter export the output list does not have (two of the three programs of section 23) | 2 |

Every vertex program holds only covered instructions. Counted over all 505 pixel programs, the instructions
the pass does not cover are: `image_load` (in 68 programs), `v_cmpx_eq_u32` (64; a compare that writes EXEC),
`v_mac_f32` (48), the two-word form of `v_fmac_f32` (18), `v_readfirstlane_b32` (5), `v_cmpx_lt_f32` (4),
`v_cvt_f32_ubyte0` to `ubyte3` (2 each), `v_cos_f32` (2), `v_interp_mov_f32` (1).

**Taken to be so, not established** (also listed at the top of `shader_pc.hpp`): the vertex call comes back
with the inputs in place; an input component an element does not have reads as 0, or 1 for the fourth; float
pixel inputs are interpolated with perspective at the pixel centre; a sampled texture holds float data; the
PC's multiply-add, minimum, maximum and clamp treat rounding, signed zeros and not-a-numbers as the console
does. Read in the PC graphics library's reader: its float-to-half conversion rounds toward zero, as the
console's packing instruction does; the two mixed half-float multiply-adds round to nearest on the console, so
there the PC result can differ by one unit of a half.

**Not built:** compute programs; the PC root signature that matches a written shader (section 20); giving a
written shader to the device; everything in the two tables above. The older SPIR-V translator
(`shader_translate.*`, `spirv.*`) is not on this path: the PC device takes only its own container kinds.

## 27. Down the list of refusals: 415 of 505 pixel and 224 of 227 vertex programs written (boots b119-b130; measured)

Section 26 ended with a count of why programs were not written. This section takes the largest reasons in turn.
Each was first read on the game's programs (in private listings that stay on the machine that made them), then
built, then tested on made-up programs, then counted again.

| run | what was added | pixel written | vertex written |
|---|---|---|---|
| b118 | (section 26) | 281 | 129 |
| b121 | constant buffers read at an element number | 281 | 224 |
| b123 | what the pass knows about a register follows the last write to it | 367 | 224 |
| b129 | five more kinds of instruction | 415 | 224 |
| b130 | the pixel position | 415 | 224 |

**A buffer read at an element number (96 vertex programs).** Measured: in every one of the 96 the read is the
four-word buffer load with its element-number flag set, no offset register, through a four-word descriptor
that the program loaded from its descriptor table; the element number is a float turned into an integer just
before. Each of these programs declares exactly one resource, a constant buffer, and the descriptor is that
constant buffer's. So the read is "row n of the constant buffer", n worked out at run time. The pass writes it
as a constant-buffer read with a run-time element and declares the buffer as indexed that way. Inferred, not
measured: that one element of the console's read is 16 bytes, the PC's row size (nothing in the program or its
tables gives an element size; four words are read for each number).

**What the pass knows about a register (88 pixel programs).** The pass keeps facts such as "this register holds
the address of the descriptor table" or "these eight registers hold the descriptor of texture 3". It used to
drop a fact unless the registers were written only once in the whole program. The game's pixel programs do
otherwise: they load a texture's eight words into the registers s4 to s11, over the table's own address in s8
and over the sampler heap's address in s11 (copied elsewhere first), and loop afterwards. A fact now belongs to
the write that made it: for every place in the program the pass works out which single instruction last wrote
each register (or that several could have, where paths join), and a fact holds exactly where its write is that
last one. The rule follows writes, not values: two different instructions that copy the same address into a
register on two paths still end the fact where the paths join. That loses a program now and then and never
writes a wrong one.

**More instructions** (numbers and operand lists from the public opcode table; each tested alone):

| instruction | what is written | source for the meaning |
|---|---|---|
| multiply-accumulate, plain (`v_mac_f32`) | multiply, then add to the destination: the product is rounded before the sum | the name pair with the fused form in the public table; tested against the fused form on a case where the two differ |
| multiply-accumulate, fused, two-word form (`v_fmac_f32`) | the fused multiply-add of section 26 | as the one-word form |
| byte n of a word as a float (`v_cvt_f32_ubyte0` to `3`) | shift, mask to eight bits, convert; byte 0 is the lowest | LLVM's own simplification rules for these instructions |
| compare into the execution mask (the one-word compares numbered 16 above the ordinary ones) | the ordinary compare, its result ANDed into the execution mask; the condition mask is left alone | the public table's rule for these numbers, and Mesa's assembler: on this generation they write only the execution mask |

Not carried: a modifier on the value a multiply-accumulate adds to; the two-word form of a compare into the
execution mask. The public compiler notes that the plain multiply-accumulate flushes very small numbers to zero
on the hardware; the PC instructions do not.

**The pixel position (68 programs).** Measured: of the 146 pixel programs whose input list has the position, 78
are marked as reading none of it and read no vector register past v1 before writing it; the other 68 are marked
as reading its x and y, and read exactly v2 and v3. The order (x in v2, y in v3) is the order in which the
public driver lists what a pixel program of this hardware starts with: the interpolation weights first, then
the position's components that are used. The pass fills v2 and v3 from the PC's position input when exactly
those conditions hold, and refuses otherwise. Taken to be so: the value is the pixel centre's window
coordinate on both sides.

**What is still refused (boot b130; first reason met).**

| programs | reason |
|---|---|
| 64 pixel | a texel read at integer coordinates (`image_load`): these are the programs that use the position. The pass does not know the number type of the texture |
| 11 pixel | a register read before any write that nothing known provides (s9 in 10, s11 in 1) |
| 6 pixel | a scalar move into a special register the pass does not carry |
| 4 pixel | `v_readfirstlane_b32` |
| 2 pixel | a resource taken from the descriptor heap by number |
| 2 pixel | a word of a descriptor used as a number |
| 1 pixel | the interpolation move |
| 3 vertex | an export of a parameter the output list does not have (they declare only the position) |

Everything of section 26 that was "taken to be so" still is, and still nothing the pass writes has been given
to the graphics device. Compute programs are not handled. A PC root signature for a pipeline, built from what
the written shaders declare, is the next piece.
