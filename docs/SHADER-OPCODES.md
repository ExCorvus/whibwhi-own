# Names of the shader instructions the game's programs use most

The graphics library walks each distinct precompiled program of the game's pipeline descriptions and counts
`(format, opcode)` pairs (`shell/shader_walk.*`, [SHADER-TRANSLATOR.md](SHADER-TRANSLATOR.md) section 13). This
document puts a public name on the pairs that occur most in the first 1,024 programs of a run (106,163
instructions), and says for each one which public table it was checked against. It holds no shader bytes: it is a
list of numbers and names.

The names are not written from memory. Every name below is read out of an open-source table for this GPU
generation, and every pair was checked against at least one, usually two, independent public files; where the two
disagree, both readings are written down and the pair is marked **not settled**.

## How to read the marks

| mark | meaning |
|---|---|
| **named** | both public sources below give this instruction at this number in the generation's column |
| **named (one source)** | only one of the two sources covers this number; the other has no entry there, and nothing contradicts it |
| **not settled** | the sources disagree, or agree on the number but not on the name; both readings are in the row |
| **unknown** | no public table for this generation gives any instruction at this number; no name is guessed |

The "checked against" column uses these short codes:

- **A** — Mesa's ACO compiler, `src/amd/compiler/aco_opcodes.py`, the `gfx10` column of the `Opcode` namedtuple
  (MIT licence). Also used: `src/amd/compiler/aco_assembler.cpp`, where the same project states how an instruction
  is re-encoded from its short form into the three-operand form.
- **L** — LLVM's AMDGPU assembler tests, `llvm/test/MC/AMDGPU/gfx10_asm_*.s` (Apache-2.0 with LLVM exceptions).
  These files contain the bytes the assembler really produces for every mnemonic, so they check a name against a
  number independently of ACO.
- **T** — LLVM's AMDGPU instruction definitions, `llvm/lib/Target/AMDGPU/*.td`, where a name is bound to its
  encoding number for `gfx10` (the same licence).

"gfx10" is the name both projects use for the GPU generation this console's chip belongs to; Mesa's `gfx10` column
covers 10.1 through 10.3, which is the generation of an Xbox Series GPU.

## The generation: which column, and what proves it

The column used for every name below is **gfx10**.

1. **The end of the program.** The walk measured that every program ends with the word `0xBF810000`, which in our
   layout is program control (SOPP), opcode 1. Both sources put `s_endpgm` at SOPP 1 in the gfx10 column, and only
   there is the whole program-control table of this generation complete (SOPP 32 and 33, below, exist only in
   gfx10). This is the check the brief named; on its own it does not separate gfx9 from gfx10, because gfx9 also
   has `s_endpgm` at 1. The next four do separate them.

2. **SOPP 32 and 33.** The game uses SOPP 33 2,087 times and SOPP 32 1,103 times. In the gfx10 column these are
   `s_clause` and `s_inst_prefetch`; in the gfx9 and gfx11 columns the table has **no entry at all** for either
   number (gfx11 puts `s_branch` at 32 and `s_cbranch_scc0` at 33, which would make the game branch 1,103 times
   from places where the walk shows the next instruction following). Two scalar-control instructions that exist
   only in one generation's table, used thousands of times, is not a coincidence.

3. **The scalar one-source format (SOP1) is renumbered between generations.** The game uses SOP1 3 3,814 times
   and SOP1 4 5,525 times. In the gfx9 column those numbers are `s_cmov_b64` and `s_not_b32`; in the gfx10 column
   they are `s_mov_b32` and `s_mov_b64`. A shader set that moves scalar registers 9,339 times and never once uses
   `s_mov_b32` would be absurd, so the numbers were read with the gfx10 numbering.

4. **The three-source vector format (VOP3) agrees with the short forms under the gfx10 rule only.** The public
   assembler rule is: a two-source vector instruction is re-encoded in the three-operand form as `opcode + 0x100`,
   a one-source one as `opcode + 0x180` (gfx10; gfx8 and gfx9 used `+0x140`), a comparison as `opcode + 0`. Our
   measured VOP3 numbers satisfy exactly that pattern with the gfx10 short-form numbers: VOP3 257, 259, 260, 264 are
   VOP2 1, 3, 4, 8 (`v_cndmask_b32`, `v_add_f32`, `v_sub_f32`, `v_mul_f32`) plus 0x100; VOP3 384 is VOP1 0 plus
   0x180; VOP3 6 is comparison 6. Under the gfx9 rule the same words would mean `+0x140` for the one-source family
   and would not line up at all.

5. **Numbers that the gfx9 column reads as 16-bit arithmetic.** Under the gfx9 numbering VOP2 8 would be
   `v_mul_u32_u24`, VOP2 43/44/45 would be `v_lshrrev_b16`, `v_ashrrev_i16` and `v_max_f16`, and VOP2 1 would be
   `v_add_f32`. That would make a 16-bit shift the third most used instruction of the game's shaders (5,666 times)
   while `v_mul_f32` appears never, and integer multiply-by-24-bit useful 13,371 times. Under the gfx10 numbering
   the same numbers are `v_mul_f32`, `v_fmac_f32`, `v_fmamk_f32`, `v_fmaak_f32` and `v_cndmask_b32` — the mix a
   shader compiler actually produces.

6. **The image format agrees only with gfx10 for the numbered pairs.** MIMG 32 is `image_sample` and MIMG 8 is
   `image_store` in gfx10; in gfx11 those numbers are `image_sample_c` and `image_store_pck`, and in gfx9 the
   table has `image_sample`/`image_store` at the same numbers too, so this one is consistent but not separating.
   It is listed because the same column has to explain it.

The gfx10 column was chosen on checks 2-5; check 1 is the brief's required check and holds.

## The pairs

Counts are the coordinator's measurement over the 106,163 instructions of the 997 programs that walk exactly to
their end word. "VOP3 n" names the three-operand encoding of an instruction; the name is the instruction's own
name, without encoding suffixes.

| pair | name | what it does (one sentence) | checked against | mark |
|---|---|---|---|---|
| VOP2 8 | `v_mul_f32` | multiplies two 32-bit floats. | A; L `gfx10_asm_vop2.s` | named |
| SOPP 12 | `s_waitcnt` | waits until the memory/load counters selected by its 16-bit field have drained to their thresholds. | A; L `gfx10_asm_sop.s` | named |
| VOP2 43 | `v_fmac_f32` | fused multiply-add that accumulates into the destination: dst = src0 × src1 + dst. | A; L `gfx10_asm_vop2.s` | named |
| SOP1 4 | `s_mov_b64` | copies a 64-bit scalar value (a pair of SGPRs). | A; L `gfx10_asm_sop.s` | named |
| VOP3 331 | `v_fma_f32` | fused multiply-add of three floats: dst = src0 × src1 + src2. | A; L `gfx10_asm_vop3.s` | named |
| VINTRP 1 | `v_interp_p2_f32` | second interpolation step of a pixel-shader attribute: adds the per-pixel term to the value the first step produced. | A; T `VOP3Instructions.td` (0x201) | named |
| VINTRP 0 | `v_interp_p1_f32` | first interpolation step of a pixel-shader attribute: the attribute's value at this pixel from the vertex terms. | A; T `VOP3Instructions.td` (0x200) | named |
| VOP2 3 | `v_add_f32` | adds two 32-bit floats. | A; L `gfx10_asm_vop2.s` | named |
| SOP1 3 | `s_mov_b32` | copies a 32-bit scalar value. | A; L `gfx10_asm_sop.s` | named |
| VOP3 384 | `v_nop` | does nothing (a no-operation instruction). | L `gfx10_asm_vop1.s` (`v_nop_e64` at 0x180); A's re-encoding rule (VOP1 + 0x180) | named |
| VOP2 4 | `v_sub_f32` | subtracts: dst = src0 − src1. | A; L `gfx10_asm_vop2.s` | named |
| VOP2 1 | `v_cndmask_b32` | selects, bit by bit, one of two 32-bit values according to a mask. | A; L `gfx10_asm_vop2.s` | named |
| SOPP 33 | `s_clause` | declares how many of the following scalar memory instructions may be issued as one clause (a grouping hint). | A; L `gfx10_asm_sop.s` | named |
| VOP1 1 | `v_mov_b32` | copies a 32-bit value. | A; L `gfx10_asm_vop1.s` | named |
| VOP1 42 | `v_rcp_f32` | reciprocal of a float (an approximation the hardware defines). | A; L `gfx10_asm_vop1.s` | named |
| SMEM 2 | `s_load_dwordx4` | loads 16 bytes from scalar memory (address in SGPRs) into four SGPRs. | A; L `gfx10_asm_smem.s` | named |
| EXP 0 | `exp` | writes up to four values to one of the shader's outputs (position, parameter or colour target) — the export instruction. | A; L `exp-gfx10.s` | named |
| MIMG 32 | `image_sample` | samples an image: a filtered texture fetch at the given coordinates. | A; L `gfx10_asm_mimg.s` | named |
| VINTRP 2 | `v_interp_mov_f32` | moves an attribute into a register without interpolating it (flat shading). | A; T `VOP3Instructions.td` (0x202) | named |
| VOP2 16 | `v_max_f32` | larger of two floats. | A; L `gfx10_asm_vop2.s` | named |
| VOP3 264 | `v_mul_f32` | multiplies two 32-bit floats (three-operand encoding). | L `gfx10_asm_vop2.s` (e64 at 0x108); A's re-encoding rule | named |
| SOPP 32 | `s_inst_prefetch` | prefetches instruction bytes into the instruction cache. | A; L `gfx10_asm_sop.s` | named |
| SOPP 1 | `s_endpgm` | ends the program. | A; L `gfx10_asm_sop.s` | named |
| SOP2 30 | `s_lshl_b32` | shifts a 32-bit scalar value left. | A; L `gfx10_asm_sop.s` | named |
| SOPP 4 | `s_cbranch_scc0` | branches when the SCC flag is 0. | A; L `gfx10_asm_sop.s` | named |
| SMEM 3 | `s_load_dwordx8` | loads 32 bytes from scalar memory into eight SGPRs. | A; L `gfx10_asm_smem.s` | named |
| VOP2 44 | `v_fmamk_f32` | multiply-add with the multiplier carried as a constant word: dst = src0 × constant + src1. | A; L `gfx10_asm_vop2.s` | named |
| VOP2 26 | `v_lshlrev_b32` | shifts left with the operand order reversed: dst = src1 << src0. | A; L `gfx10_asm_vop2.s` | named |
| VOP1 8 | `v_cvt_i32_f32` | converts a float to a signed 32-bit integer. | A; L `gfx10_asm_vop1.s` | named |
| MUBUF 14 | `buffer_load_dwordx4` | loads 16 bytes through a buffer descriptor into four VGPRs. | A; L `gfx10_asm_mubuf.s` | named |
| VOP3P 33 | `v_fma_mixlo_f16` | fused multiply-add of mixed half/single-precision inputs, writing the low 16-bit half of a packed result. | A; T `VOP3PInstructions.td` (0x21) | named |
| VOP3P 34 | `v_fma_mixhi_f16` | the same, writing the high 16-bit half. | A; T `VOP3PInstructions.td` (0x22) | named |
| SOP1 10 | `s_wqm_b64` | expands the EXEC mask to whole quads (whole-quad mode, used before derivatives and sampling). | A; L `gfx10_asm_sop.s` | named |
| SMEM 9 | `s_buffer_load_dwordx2` | loads 8 bytes through a scalar buffer descriptor into two SGPRs. | A; L `gfx10_asm_smem.s` | named |
| VOP3 257 | `v_cndmask_b32` | bit-by-bit select of two 32-bit values (three-operand encoding). | L `gfx10_asm_vop2.s` (e64 at 0x101); A's re-encoding rule | named |
| VOP1 6 | `v_cvt_f32_u32` | converts an unsigned 32-bit integer to a float. | A; L `gfx10_asm_vop1.s` | named |
| SMEM 10 | `s_buffer_load_dwordx4` | loads 16 bytes through a scalar buffer descriptor into four SGPRs. | A; L `gfx10_asm_smem.s` | named |
| VOP2 45 | `v_fmaak_f32` | multiply-add with the addend carried as a constant word: dst = src0 × src1 + constant. | A; L `gfx10_asm_vop2.s` | named |
| SMEM 8 | `s_buffer_load_dword` | loads 4 bytes through a scalar buffer descriptor into one SGPR. | A; L `gfx10_asm_smem.s` | named |
| SOP2 15 | `s_and_b64` | bitwise AND of two 64-bit scalar values. | A; L `gfx10_asm_sop.s` | named |
| SOPP 8 | `s_cbranch_execz` | branches when the EXEC mask is 0. | A; L `gfx10_asm_sop.s` | named |
| SMEM 4 | `s_load_dwordx16` | loads 64 bytes from scalar memory into sixteen SGPRs. | A; L `gfx10_asm_smem.s` | named |
| SOPC 1 | `s_cmp_lg_i32` | compares two 32-bit integers and sets SCC when they differ (`lg` = less or greater). | A; L `gfx10_asm_sop.s` | named |
| VOP2 47 | `v_cvt_pkrtz_f16_f32` | converts two floats to two half-floats with round-to-zero and packs them into one 32-bit value. | A; L `gfx10_asm_vop2.s` | named |
| SOP2 39 | `s_bfe_u32` | extracts a bit field from a 32-bit value (unsigned). | A; L `gfx10_asm_sop.s` | named |
| SOP2 21 | `s_andn2_b64` | bitwise AND of a 64-bit value with the inverted second operand. | A; L `gfx10_asm_sop.s` | named |
| VOP2 37 | `v_add_nc_u32` | adds two 32-bit unsigned integers without carry-out. | L `gfx10_asm_vop2.s` (`v_add_nc_u32`); A calls the same number `v_add_u32` | not settled (spelling) |
| VOP2 0 | — | — | A: no entry; L: no entry (gfx6/gfx7 use this number for `v_cndmask_b32`, gfx10 does not) | unknown |
| SOP2 11 | `s_cselect_b64` | selects one of two 64-bit values according to SCC. | A; L `gfx10_asm_sop.s` | named |
| SOPC 0 | `s_cmp_eq_i32` | compares two 32-bit integers and sets SCC when they are equal. | A; L `gfx10_asm_sop.s` | named |
| VOP1 37 | `v_exp_f32` | 2 raised to the power of the float operand. | A; L `gfx10_asm_vop1.s`; T `IntrinsicsAMDGPU.td` (`v_exp_{f16\|f32} ... Performs exp2`) | named |
| SOP2 14 | `s_and_b32` | bitwise AND of two 32-bit scalar values. | A; L `gfx10_asm_sop.s` | named |
| SMEM 11 | `s_buffer_load_dwordx8` | loads 32 bytes through a scalar buffer descriptor into eight SGPRs. | A; L `gfx10_asm_smem.s` | named |
| SOPP 15 | `s_setprio` | sets the wave's scheduler priority (a performance hint). | A; L `gfx10_asm_sop.s` | named |
| SMEM 12 | `s_buffer_load_dwordx16` | loads 64 bytes through a scalar buffer descriptor into sixteen SGPRs. | A; L `gfx10_asm_smem.s` | named |
| VOP1 7 | `v_cvt_u32_f32` | converts a float to an unsigned 32-bit integer. | A; L `gfx10_asm_vop1.s` | named |
| MUBUF 13 | `buffer_load_dwordx2` | loads 8 bytes through a buffer descriptor into two VGPRs. | A; L `gfx10_asm_mubuf.s` | named |
| SOPP 2 | `s_branch` | unconditional branch to a 16-bit signed offset relative to the next instruction. | A; L `gfx10_asm_sop.s` | named |
| VOP3 260 | `v_sub_f32` | subtracts (three-operand encoding). | L `gfx10_asm_vop2.s` (e64 at 0x104); A's re-encoding rule | named |
| VOP3 259 | `v_add_f32` | adds (three-operand encoding). | L `gfx10_asm_vop2.s` (e64 at 0x103); A's re-encoding rule | named |
| MIMG 39 | `image_sample_lz` | samples an image at level of detail zero (no mip selection). | A; L `gfx10_asm_mimg.s` | named |
| MIMG 0 | `image_load` | loads texels from an image without sampling or filtering. | A; L `gfx10_asm_mimg.s` | named |
| VOP1 39 | `v_log_f32` | base-2 logarithm of the float operand. | A; L `gfx10_asm_vop1.s`; T `IntrinsicsAMDGPU.td` (`v_log_{f16\|f32}, performs log2`) | named |
| VOP3 6 | `v_cmp_ge_f32` | compares two floats and writes a mask that is true where src0 ≥ src1 (three-operand encoding). | L `gfx10_asm_vopc_e64.s` (`v_cmp_ge_f32_e64` at 6); A (`v_cmp_ge_f32` at VOPC 6) | named |
| MIMG 8 | `image_store` | stores texels into an image. | A; L `gfx10_asm_mimg.s` | named |
| VOP2 22 | `v_lshrrev_b32` | logical right shift with the operand order reversed: dst = src1 >> src0. | A; L `gfx10_asm_vop2.s` | named |
| VOP3 887 | `v_permlane16_b32` | permutes 16-bit halves of a 32-bit value between lanes of the wavefront, using two lane indices and a 64-bit mask. | A; L `gfx10_asm_vop3.s` | named |

VOP2 22 and VOP3 887 have no count in the top-64 list; they are included because the coordinator met them next to
the words of unknown format (below).

### Notes on the two rows that are not plain names

- **VOP2 37.** The number is settled: both sources put the "add two unsigned 32-bit integers, no carry-out"
  instruction there. The *spelling* is not: LLVM's assembly mnemonic for gfx10 is `v_add_nc_u32`, ACO's table
  calls the same encoding `v_add_u32`. The code returns `v_add_nc_u32` and this document records both.
- **VOP3 384 = `v_nop`.** The number is settled by LLVM (`v_nop_e64` is 0x180 in `gfx10_asm_vop1.s`) and by ACO's
  stated rule (one-source instructions are re-encoded as `opcode + 0x180` on gfx10). But the count is odd: 2,990
  no-ops is about 2.8% of all instructions, roughly three per program. Nothing in the public tables suggests
  another instruction at 0x180, so the name stands; whether the game's compiler really emitted that many no-ops
  (for scheduling or alignment inside a program) is **unknown**. A translator may implement it as a no-op without
  risk; if the count later turns out to be a symptom of a mis-sized instruction before it, this row is where to
  look.

### VOP2 0 is unknown

Neither source has any gfx10 instruction at VOP2 opcode 0, and the game has 429 words that read that way. The
number is used on gfx6/gfx7 (`v_cndmask_b32`) but the gfx10 column moved that instruction to 1, and every other
piece of evidence in this document says the programs are gfx10. Possibilities that were not measured: a word of
data inside the program, an encoding of this console's GPU that the public tables do not list, or padding. No name
is given. A translator must refuse it by number, as it already does for every pair not in this table.

## The words with top six bits `0x3F`

Twenty-seven of the 1,024 programs stop on a word whose top six bits are `111111` (`0xFC000000`–`0xFDFFFFFF`).
Every instruction format the public tables give this generation has a different six-bit prefix: `0x32`–`0x3E`,
plus `0x34` for the base three-operand class that gfx10 overrides to `0x35`. Checking every `Inst{31-26}` value in
LLVM's gfx10 instruction definitions and every format in ACO's table, **no gfx10 instruction uses `0x3F`**, and
neither project documents such an encoding for this generation. gfx11's dual-issue format uses `0x32` and its
three-word form `0xCF`, so that is not it either.

**The meaning of those words is unknown.** They are not named here, and the translator must keep refusing a
program that contains one. The pattern the coordinator measured (most often a memory instruction, then
`v_add_f32`, then the unknown word) is consistent with a word of data in the middle of the program or with an
instruction whose length our walk gets wrong, but neither was measured, so neither is claimed.

## The groups a translator needs first

Grouping of the pairs above by what the instruction does, with the share of the 106,163 instructions the listed
pairs cover. The eleven listed groups together cover 100,132 of 106,163 instructions = **94.3%**; the remaining
5.7% are pairs outside the top-64 list.

| group | pairs | instructions | share |
|---|---|---|---|
| arithmetic | VOP2 8, 43, 3, 4, 16, 44, 26, 45, 37, 47, 22; VOP3 331, 264, 260, 259; VOP3P 33, 34; VOP1 42, 8, 6, 37, 7, 39; SOP2 30, 39, 21, 15, 14; SOP1 10 | 46,176 | 43.5% |
| move | VOP2 1; VOP3 257; VOP1 1; SOP1 3, 4; SOP2 11; VOP3 887 | 15,428 | 14.5% |
| memory | SMEM 2, 3, 4, 8, 9, 10, 11, 12; MUBUF 13, 14; SOPP 12, 33 | 15,199 | 14.3% |
| interpolation | VINTRP 0, 1, 2 | 10,708 | 10.1% |
| branches and program control | SOPP 1, 2, 4, 8, 15, 32 | 4,168 | 3.9% |
| no-op | VOP3 384 | 2,990 | 2.8% |
| image | MIMG 0, 8, 32, 39 | 2,062 | 1.9% |
| export | EXP 0 | 1,838 | 1.7% |
| compare | SOPC 0, 1; VOP3 6 | 1,134 | 1.1% |
| unknown | VOP2 0 | 429 | 0.4% |

What that says about the order of work: vector arithmetic alone is 43.5% and, with moves, over half. A translator
that handles the arithmetic, move and memory rows covers 72% of all instructions before it touches interpolation,
images or exports. Interpolation plus export (10,708 + 1,838) is the whole of a simple pixel shader's non-arithmetic
work; image sampling is small by count but is where the console's tiling has to be handled (SHADER-TRANSLATOR.md
section 5), so its 1.9% costs more than its share.

## What this document does not say

- Nothing about operand encoding beyond the numbers; `shell/shader_walk.*` reads lengths and fields, and a
  translator needs each instruction's operand layout separately.
- Nothing about the VOP3P, VOP3-with-modifiers or sub-dword (`sdwa`/`dpp`) forms: the same instruction appears in
  those forms under a different encoding, and only the forms the walk actually met are listed.
- Nothing about gfx10.3-only instructions outside the measured list; `v_permlane16_b32` is in the list because the
  walk met it, not because the whole gfx10.3 set was surveyed.

## Sources

- Mesa / ACO, `src/amd/compiler/aco_opcodes.py` and `src/amd/compiler/aco_assembler.cpp`, MIT licence. Read on
  2026-11-26 from the public GitHub mirror `chaotic-cx/mesa-mirror` (branch `main`), because the upstream GitLab
  host refused automated reads that day. Column used: `gfx10` of the `Opcode` namedtuple; rule used:
  `emit_vop3_instruction` (two-source `+0x100`, one-source `+0x140` on gfx8/gfx9 and `+0x180` otherwise,
  comparison `+0`).
- LLVM, `llvm/lib/Target/AMDGPU/*.td`, `llvm/include/llvm/IR/IntrinsicsAMDGPU.td` and
  `llvm/test/MC/AMDGPU/gfx10_asm_*.s`, Apache-2.0 with LLVM exceptions. Read on 2026-11-26 from
  `llvm/llvm-project` (branch `main`). The files named in the rows above are the ones the number and name were read
  from; the encoding bytes in the `gfx10_asm_*.s` tests are what makes them a second, independent check on ACO.

Both are moving targets: a later change in either project can renumber or rename an instruction between
generations. The numbers here are the ones the game's programs use; a future check should re-read the same files
rather than trust this table.