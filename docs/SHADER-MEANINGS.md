# What the covered console shader instructions compute

This is the meaning half of the shader reader: [SHADER-OPCODES.md](SHADER-OPCODES.md) puts a public name on an
instruction, `shell/shader_isa.*` reads its fields, and `shell/shader_meaning.*` says what it does with them, as a
reference interpreter for one lane that runs with no GPU (`shell/tests/test_shader_meaning.cpp`).

Two things about this document:

* **Every row says where the meaning comes from**, and the last column marks it:
  **tested** = the interpreter's behaviour for it is asserted in the test against a value worked out by hand;
  **from source** = a named public source states it; **not established** = the sources read do not answer it, and
  the interpreter refuses that input instead of choosing a value (all such places are listed at the end).
* Nothing here is written from memory. Where the sources are silent the interpreter refuses, and the refusal
  names the instruction.

## The lane model (which register a per-lane result lives in)

| fact | how this build establishes it |
|---|---|
| A vector instruction writes a lane only when that lane's bit is set in EXEC; a disabled lane keeps its old value. | The lane-enable mask is what makes a wave execute a subset of its lanes; EXEC at scalar codes 126/127 is named by `AMDGPUDisassembler.cpp` (`decodeSpecialReg32`), which `shader_isa.cpp` already uses for the register numbers. **from source (encoding/lane model)**; **tested** (a write with the lane disabled changes nothing). |
| The one-word compare form (VOPC) has no destination field: its result mask is **VCC**. | The VOPC encoding has no `vdst` (LLVM `VOPCInstructions.td`, `VOPCe`), which is why `shader_isa.cpp` decodes no destination for it. **from source**. |
| A compare writes one bit per lane: the lane's own bit of the mask. `v_cndmask_b32` reads that bit for this lane. | A per-lane compare result in a wave is a bit-per-lane mask; the lane's condition is bit `lane`. **from source (per-lane mask model)**; **tested** (lane 5 writes bit 5 and leaves bit 0 alone; a cndmask on lane 0 ignores bit 1). |
| A lane EXEC does not enable gets its compare bit **cleared** (the mask is the result AND the executing lanes). | Same mask model as above. **tested**. |
| The three-operand compare names its mask in the destination field: 0..101 an SGPR pair, 106 VCC, 126 EXEC. | The field is decoded by `AMDGPUDisassembler.cpp` as an SGPR code; LLVM's `VOP3a_gfx10` gives it 8 bits, and the compare uses `VCC` with `Defs`/`Uses` in `VOPCInstructions.td`. **tested** (s0:s1 and VCC destinations). |
| `s_cselect_b64` reads the scalar condition **SCC**. | LLVM `SOPInstructions.td`: the instruction is inside `let Uses = [SCC]`. **from source**. |

**A reading detail that matters for anyone reusing this:** `shader_isa.*` decodes the three-operand (VOP3)
destination field as a *vector register* number for every instruction of that format — correct for the arithmetic
instructions, whose `vdst` is a VGPR. For a **compare** the same field holds a *scalar pair* code. The reader
cannot tell the two apart without knowing the opcode, so `shader_meaning.cpp` reads `Instruction::vop3.vdst`
itself for compares and says so in a comment. A later step may want that fixed in the reader.

## The instructions

Counts are from one run of the game, over 106,163 instructions (docs/SHADER-OPCODES.md). "A" = Mesa's ACO compiler,
`src/amd/compiler/aco_opcodes.py` (the `gfx10` column of each opcode row) — MIT licence. "A-lower" = Mesa's
`src/amd/compiler/aco_lower_to_hw_instr.cpp`, where the same project emits the instruction and so fixes operand
order. "L-td" = LLVM's AMDGPU instruction definitions (`VOP1Instructions.td`, `VOP2Instructions.td`,
`VOP3PInstructions.td`, `VOPCInstructions.td`, `SOPInstructions.td`, `AMDGPUInstrInfo.td`) and
`AMDGPUModifierSyntax` (the modifier page) — Apache-2.0 with LLVM exceptions. No code from any of them is copied.

### VOP2 (one word, two sources; the second source field is a VGPR number only)

| format | opcode | name | what it computes | established by | mark |
|---|---|---|---|---|---|
| VOP2 | 8 | `v_mul_f32` | dst = src0 × src1, rounded once. | A; L-td | tested |
| VOP2 | 43 | `v_fmac_f32` | dst = fma(src0, src1, **the destination's old value**): one fused multiply-add, rounded once. | A (three sources, destination as accumulator); the test shows a value that a separate multiply and add cannot produce. | tested |
| VOP2 | 3 | `v_add_f32` | dst = src0 + src1. | A; L-td | tested |
| VOP2 | 4 | `v_sub_f32` | dst = src0 − src1. | A; L-td | tested |
| VOP2 | 1 | `v_cndmask_b32` | dst = (this lane's **VCC** bit) ? src1 : src0 — the mask picks the **second** source. | A-lower (64-bit `umin` is `(x > y) ? y : x` and is emitted as `v_cndmask_b32 dst, x, y, vcc`); A (`src(..., VCC)`). | tested |
| VOP2 | 16 | `v_max_f32` | the larger of src0 and src1; the NaN-ignoring ("maxNum") form — a NaN operand gives the other operand. | A calls the same number `v_max_num_f32` in the generation where it was renamed; IEEE 754-2008 `maxNum`. | tested (one NaN); both NaN refused |
| VOP2 | 44 | `v_fmamk_f32` | dst = fma(src0, **the constant word**, src1): the constant is the multiplier. | A/L-td (`src(F32, F32, IMM)`) and the name (`…mk` = multiply by the constant); see SHADER-OPCODES.md. | tested |
| VOP2 | 26 | `v_lshlrev_b32` | dst = src1 << src0 — the operand order is reversed, so the first source is the shift count. | A-lower (`v_lshrrev_b32 dst, shift, value`); the `rev` naming. | tested (counts 0, 31) |
| VOP2 | 45 | `v_fmaak_f32` | dst = fma(src0, src1, **the constant word**): the constant is the addend. | as `v_fmamk_f32`, with the name (`…ak`). | tested |
| VOP2 | 47 | `v_cvt_pkrtz_f16_f32` | two floats to two halves with **round toward zero**; src0 becomes the low half, src1 the high half. | L-td on the conversion node ("two packed f16 with round to zero"); A (`#v_cvt_pk_rtz_f16_f32 in GFX11`). | tested (RTZ and packing) |
| VOP2 | 37 | `v_add_nc_u32` | dst = src0 + src1 as 32-bit unsigned, wrapping (no carry out). | L-td mnemonic; A calls the same number `v_add_u32`. | tested |
| VOP2 | 22 | `v_lshrrev_b32` | dst = src1 >> src0 (logical), reversed order as above. | A-lower; the `rev` naming. | tested (counts 0, 31) |

### VOP1 (one word, one source)

| format | opcode | name | what it computes | established by | mark |
|---|---|---|---|---|---|
| VOP1 | 1 | `v_mov_b32` | copies the 32-bit pattern unchanged (no float meaning). | A; L-td | tested |
| VOP1 | 42 | `v_rcp_f32` | dst = 1 / src0, the hardware's approximation. | A (transcendental class); L-td mnemonic. | tested within 1e-6 relative |
| VOP1 | 8 | `v_cvt_i32_f32` | src0 to signed 32-bit, **truncating toward zero**. | A; L-td; the C++ conversion has the same meaning as LLVM's `fptosi`. | tested (in range) |
| VOP1 | 6 | `v_cvt_f32_u32` | src0 (unsigned 32-bit) to float. | A; L-td | tested (exact values) |
| VOP1 | 37 | `v_exp_f32` | dst = 2^src0. | L-td intrinsic table: the matching intrinsic "performs exp2" (quoted in SHADER-OPCODES.md). | tested (exact and 1e-6) |
| VOP1 | 7 | `v_cvt_u32_f32` | src0 to unsigned 32-bit, **truncating toward zero**. | A; L-td | tested (in range) |
| VOP1 | 39 | `v_log_f32` | dst = log2(src0). | L-td intrinsic table: "performs log2" (quoted in SHADER-OPCODES.md). | tested (exact) |

### VOPC (one word, compare; result to VCC) and the three-operand compares below 0x100

The number of a compare in the three-operand format is the one-word number (docs/SHADER-OPCODES.md, and
`v_cmp_ge_f32` is VOPC 6 and VOP3 6 in A's table).

| format | opcodes | names | what it computes | established by | mark |
|---|---|---|---|---|---|
| VOPC | 0x00–0x0F | `v_cmp_f_f32`, `v_cmp_lt_f32`, `v_cmp_eq_f32`, `v_cmp_le_f32`, `v_cmp_gt_f32`, `v_cmp_lg_f32`, `v_cmp_ge_f32`, `v_cmp_o_f32`, `v_cmp_u_f32`, `v_cmp_nge_f32`, `v_cmp_nlg_f32`, `v_cmp_ngt_f32`, `v_cmp_nle_f32`, `v_cmp_neq_f32`, `v_cmp_nlt_f32`, `v_cmp_tru_f32` | the named IEEE predicate on two floats; `lg` is "ordered and not equal", `o` is ordered, `u` is unordered, the `n…` forms are the complements (so they are true on a NaN) | L-td (`VOPC_Real_gfx6_gfx7_gfx10`, opcodes 0x000–0x00F) for the numbers and names; the predicates are the standard IEEE ones and the `n` forms the negations | tested (ge, o, u, nge, NaN) |
| VOPC | 0x80–0x87 | `v_cmp_f_i32`, `lt`, `eq`, `le`, `gt`, `ne`, `ge`, `tru` | the named comparison of two **signed** 32-bit integers | L-td (same table, opcodes 0x080–0x087) | tested (lt, eq) |
| VOPC | 0xC0–0xC7 | `v_cmp_f_u32`, `lt`, `eq`, `le`, `gt`, `ne`, `ge`, `tru` | the named comparison of two **unsigned** 32-bit integers | L-td (opcodes 0x0C0–0x0C7) | tested (lt) |
| VOP3 | 0x00–0x0F, 0x80–0x87, 0xC0–0xC7 | the same names | the same comparisons, with absolute-value/negate modifiers allowed on the float ones, and the mask written to the destination pair | as above; the modifier fields are `Vop3Fields` | tested (ge to s0:s1; eq with an abs modifier refused) |

### VOP3 (two words, three sources) and its modifiers

The modifier fields as `Vop3Fields` exposes them, and what they mean:

* **abs** (bit per source) takes the absolute value, **neg** (bit per source) negates — bit operations on the sign.
  **from source (AMDGPUModifierSyntax)**, **tested**.
* **omod** (2 bits) is the "standard output modifier (result ×2, ×4, ÷2)" — codes 1, 2, 3, applied to the result
  before the clamp. **from source** (the comment in LLVM's `VOPInstructions.td`, ordered by the code values; the
  modifier page says it is applied before clamp). **tested** (×4 and ÷2).
* **clamp** for a floating-point result clamps to **[0.0, 1.0]**, after omod. **from source
  (AMDGPUModifierSyntax)**; **tested**.
* **op_sel** selects 16-bit halves and has no meaning on a 32-bit instruction: the interpreter refuses a non-zero
  op_sel there. **not established (refused)**.

| format | opcode | name | what it computes | established by | mark |
|---|---|---|---|---|---|
| VOP3 | 331 (0x14B) | `v_fma_f32` | dst = fma(src0, src1, src2), one rounding, with the three sources' modifiers | A; L-td | tested (fused) |
| VOP3 | 264 (0x108) | `v_mul_f32` | dst = src0 × src1 | L `gfx10_asm_vop2.s` (e64 at 0x108); A's re-encoding rule | tested |
| VOP3 | 257 (0x101) | `v_cndmask_b32` | dst = (bit `lane` of the **third** source, a 64-bit pair) ? src1 : src0 | as VOP2 1; A lists the mask as the third operand | tested |
| VOP3 | 260 (0x104) | `v_sub_f32` | dst = src0 − src1 | as VOP2 4 | tested |
| VOP3 | 259 (0x103) | `v_add_f32` | dst = src0 + src1 | as VOP2 3 | tested |
| VOP3 | 6 | `v_cmp_ge_f32` | src0 ≥ src1 (with modifiers), written as a mask bit | as VOPC 6 | tested |

### VOP3P (packed): the half-precision multiply-adds

| format | opcode | name | what it computes | established by | mark |
|---|---|---|---|---|---|
| VOP3P | 33 (0x21) | `v_fma_mixlo_f16` | a fused multiply-add whose three sources are each read **either** as a whole 32-bit float (`op_sel_hi` bit set) **or** as a half from the half `op_sel` selects; the arithmetic is in f32 and the result is rounded to a half with round-to-nearest-even and written into the **low** 16 bits of the destination | L-td `VOP3PInstructions.td`: "Size of src arguments (16/32) is controlled by op_sel_hi. For 16-bit src arguments their location (hi/lo) are controlled by op_sel"; "the mix instructions round the f32 result again when they write the f16 destination"; "Clamp modifier is applied after conversion to f16"; A (opcode and the separate `..._rtz` pseudo pair, which exists as the round-to-zero alternative) | tested |
| VOP3P | 34 (0x22) | `v_fma_mixhi_f16` | the same, written into the **high** 16 bits | as above | tested |

Which 16 bits the *other* half keeps is a **choice** of this interpreter (it leaves them unchanged); see the
not-established list.

### SOP1, SOP2, SOPK (scalar)

| format | opcode | name | what it computes | established by | mark |
|---|---|---|---|---|---|
| SOP1 | 4 | `s_mov_b64` | copies a 64-bit scalar value (a register pair, or the VCC/EXEC pair) | A; L-td | tested (pair across the word boundary; VCC and EXEC) |
| SOP1 | 3 | `s_mov_b32` | copies a 32-bit scalar value | A; L-td | tested |
| SOP2 | 30 | `s_lshl_b32` | dst = src0 << src1 (the plain order, not reversed) | L-td pattern `(cshl_32 src0 src1)`; A | tested (counts 0, 31) |
| SOP2 | 15 | `s_and_b64` | dst = src0 & src1 over 64 bits | L-td pattern | tested |
| SOP2 | 39 | `s_bfe_u32` | the field described by src1: offset in bits [4:0], **width** in bits [22:16]; bits of the field past bit 31 are zero; width 0 is refused | Mesa's compiler writes the operand as `(width << 16) \| offset` wherever it emits the instruction (`aco_lower.cpp`; `aco_select_nir_intrinsics.cpp` has `28 \| (4 << 16)` commented "extract last 4 bits"). **Corrected:** this row first said "width - 1 in bits [10:5]", which no source supported; found when the statement was checked against the compiler. | tested (offset/width, the compiler's own example, width 1, width 32, field past bit 31, width 0 refused) |
| SOP2 | 21 | `s_andn2_b64` | dst = src0 & ~src1 | L-td pattern `(and src0, (not src1))` | tested |
| SOP2 | 11 | `s_cselect_b64` | dst = SCC ? **src0** : src1 — the **first** source is the true one (the opposite order to `v_cndmask_b32`) | L-td pattern: `(UniformSelect src0, src1)` selects into `S_CSELECT_B32` | tested (both conditions) |
| SOP2 | 14 | `s_and_b32` | dst = src0 & src1 | L-td pattern | tested |
| SOPK | 0 | `s_movk_i32` | dst = the 16-bit immediate, sign-extended | L-td (`S_MOVK_I32`) and the name | tested (0xFFFE, 0x7FFF) |

### SOPC (scalar compare; result to SCC)

| format | opcodes | names | what it computes | established by | mark |
|---|---|---|---|---|---|
| SOPC | 0–5 | `s_cmp_eq_i32`, `lg_i32`, `gt_i32`, `ge_i32`, `lt_i32`, `le_i32` | the named **signed** comparison of two 32-bit values, into SCC | A (same numbers); L-td (`SOPC_Real`) | tested (eq, lg, lt, gt, ge) |
| SOPC | 6–11 | `s_cmp_eq_u32`, `lg_u32`, `gt_u32`, `ge_u32`, `lt_u32`, `le_u32` | the named **unsigned** comparison | A; L-td | tested (lt_u32) |

### SOPP (program control)

| format | opcode | name | what it computes | established by | mark |
|---|---|---|---|---|---|
| SOPP | 1 | `s_endpgm` | ends the program: `step` reports `done` | A; L-td | tested |
| SOPP | 0 | `s_nop` | does nothing | A; L-td | tested |

## Outside this step (refused by name)

* Every memory instruction: **SMEM**, **MUBUF**, **MTBUF**, **DS**, **FLAT**.
* **MIMG** (image and sampling) and **VINTRP** (attribute interpolation).
* **EXP** (export).
* Every **branch** and program-control instruction other than `s_nop` and `s_endpgm`: `s_branch`,
  `s_cbranch_*`, `s_setprio`, and the wait/clause/prefetch instructions `s_waitcnt`, `s_clause`,
  `s_inst_prefetch`. Control flow is `shader_flow.*`'s step.
* Instruction numbers this step does not implement, even when they have a public name: `v_nop` (VOP1 0 and
  VOP3 0x180), `s_add_u32`/`s_sub_u32`, the remaining SOP1/SOP2/SOPK/SOPC numbers, `v_min_f32`,
  `v_mul_legacy_f32`, `s_wqm_b64`, `v_permlane16_b32`, and every opcode not in the tables above.
* The compares not listed: the f64, i64 and u64 families (VOPC 0x20–0x2F, 0xA0–0xA7, 0xE0–0xE7), the 16-bit
  families (0x88–0x8F, 0xA9–0xAE, 0xC8–0xEF) and `v_cmp_class_f32` (0x88).

## Not established (the interpreter refuses that input, and says why)

1. **A shift count of 32 or more** for `v_lshlrev_b32`, `v_lshrrev_b32` and `s_lshl_b32`. The public sources read
   do not state the result (LLVM models the hardware with its own "`cshl`" node rather than a plain shift, which
   suggests the hardware does not simply wrap — but what it does do is not written down in the files read).
   The interpreter computes counts 0–31 and refuses 32 and above.
2. **Float-to-integer conversion on NaN, negative (unsigned), or out-of-range input** for `v_cvt_i32_f32` and
   `v_cvt_u32_f32`: whether the hardware saturates, wraps or produces the "large negative" value is not stated in
   the sources read. In-range inputs truncate toward zero.
3. **`v_cvt_f32_u32` for a value that is not exactly representable**: the rounding is not stated in the sources
   read; the interpreter uses the C++ conversion (round to nearest, ties to even). The test only asserts values
   that are exact.
4. **`v_cvt_pkrtz_f16_f32` with a NaN input, with an input above 65504, or with a result that would be a half
   denormal**: refused. Whether the hardware keeps or flushes a half denormal is not stated anywhere this build
   could read.
5. **`v_max_f32` when both operands are NaN** (and, as a documented choice, the −0/+0 pair returns +0, the IEEE
   `maxNum` result).
6. **`v_rcp_f32` on zero, infinity or NaN**, and the precision of `v_rcp_f32`, `v_exp_f32`, `v_log_f32`
   generally: a CPU cannot reproduce the hardware's approximation. The interpreter returns the C library's value
   and the test allows **1e-6 relative** error; edge-case values are refused.
7. **The clamp modifier on a NaN result** (refused): the modifier page gives the range but not what a NaN
   becomes.
8. **`s_bfe_u32`'s SCC value**: the instruction is defined to write SCC, but the value is not stated in the
   sources read. The interpreter writes "the field is not zero" and the test asserts that choice.
9. **The half a `v_fma_mixlo_f16`/`v_fma_mixhi_f16` does not write**: the interpreter leaves it unchanged. The
   sources read state which half is written and that a pair is built from the two instructions, but not what the
   other half becomes. A NaN result, a result above 65504 and a result that would be a half denormal are refused;
   a high-half negate (`neg_hi`) is refused because the sources read do not say which half it applies to here.
10. **A 64-bit move from a 32-bit inline constant** is sign-extended (`s_mov_b64 s[4:5], -1` gives all ones).
    That is the common idiom and the reading the interpreter uses; the sources read do not state it in words.
11. **`op_sel` on a 32-bit three-operand instruction** is refused rather than ignored.