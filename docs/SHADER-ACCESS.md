# What the console shader instructions do to memory, images, attributes and outputs

This is the second half of the shader reader. [`shell/shader_isa.hpp`](../shell/shader_isa.hpp) reads the fields of
an instruction; [SHADER-MEANINGS.md](SHADER-MEANINGS.md) says what the arithmetic ones compute on one lane; this
document and [`shell/shader_access.*`](../shell/shader_access.hpp) answer the questions a translator needs for the
instructions that touch **memory, images, interpolation and outputs**: which register holds the base address, how
the offset is formed, how many words land where, in which order an image instruction's address components are read,
which registers hold a descriptor, and what an export writes.

Every statement below is marked with where it comes from:

* **from source** - a named public file says it, and the file is quoted or pointed at;
* **tested** - `shell/tests/test_shader_access.cpp` builds instruction words by hand from the field layouts and
  asserts the answer;
* **proposal** - what a translator should do with the fact; no source says this, it is a design suggestion;
* **not established** - no public source read for this work answers it. Such an instruction is **refused** by
  `access_of` with a reason, never approximated.

The prose is the contract; `docs/exe-map/` describes the game, this file describes the hardware it runs on. Nothing
here is game data: every number is a public encoding value or a register index in a made-up example.

## 1. The public sources

| short name | file | what was read from it |
|---|---|---|
| **L-SMEM** | LLVM, `llvm/lib/Target/AMDGPU/SMInstructions.td` | the SMEM encoding (`Inst{5-0} = sbase{6-1}`, `Inst{12-6} = sdst`, `Inst{52-32} = offset{20-0}`, `Inst{63-57} = soffset`), the register classes of each load form (base `SReg_64` for `s_load_*`, `SReg_128` for `s_buffer_load_*`; destinations `SReg_32/64/128/256/512`) |
| **L-BUF** | LLVM, `llvm/lib/Target/AMDGPU/BUFInstructions.td` | the MUBUF/MTBUF fields (`offset` [11:0], `offen` [12], `idxen` [13], and the second word's `vaddr`, `vdata`, `srsrc`, `soffset`), and that the offset field is a general 32-bit source: `SOffset = (ins SCSrc_b32:$soffset)` |
| **L-MIMG** | LLVM, `llvm/lib/Target/AMDGPU/MIMGInstructions.td`, `SIInstrFormats.td` | the image encoding (`MIMGe_gfx10`: `nsa` [2:1], `dim` [5:3], `dmask` [11:8], `unorm` [12], `r128` [15], `tfe` [16], `lwe` [17], `a16` [62], `d16` [63], `vaddr` [39:32], `vdata` [47:40], `srsrc` [52:48], `ssamp` [57:53]), the descriptor widths (`SReg_256_XNULL:$srsrc`, `SReg_128_XNULL:$ssamp`), and the non-sequential-address forms |
| **L-INTR** | LLVM, `llvm/include/llvm/IR/IntrinsicsAMDGPU.td` | the address-argument order of every dimension-aware image intrinsic (`AMDGPUDimProfile::AddrArgs`, `AMDGPUSampleVariant`) and of the interpolation intrinsics (`__builtin_amdgcn_interp_p1/p2/mov`) and export intrinsics |
| **L-DEF** | LLVM, `llvm/lib/Target/AMDGPU/SIDefines.h`, `Utils/AMDGPUBaseInfo.{h,cpp}`, `MCTargetDesc/AMDGPUInstPrinter.cpp` | the export target numbers, the `s_waitcnt` counter fields, the interpolation parameter names `p10`/`p20`/`p0` |
| **L-TEST** | LLVM's assembler tests `llvm/test/MC/AMDGPU/gfx10_asm_smem.s`, `gfx10_asm_mubuf.s`, `exp-gfx10.s` | the bytes the assembler really emits, used to check two readings of the encodings |
| **L-USAGE** | LLVM, `llvm/docs/AMDGPUUsage.md` | the hardware's out-of-bounds behaviour for buffer accesses |
| **A-OPC** | Mesa ACO, `src/amd/compiler/aco_opcodes.py` | every opcode number quoted below, gfx10 column |
| **A-ASM** | Mesa ACO, `src/amd/compiler/aco_assembler.cpp` | how each instruction is encoded: the SMEM base register `>> 1`, the "no offset" encodings, the image address words and their packing |
| **A-ISEL** | Mesa ACO, `src/amd/compiler/instruction_selection/aco_isel_helpers.cpp`, `aco_select_nir.cpp`, `aco_select_nir_intrinsics.cpp`, `aco_select_ps_prolog.cpp`, `aco_select_ps_epilog.cpp` | the order in which the compiler places an instruction's operands (`/* gather MIMG address components */`), the interpolation steps, the export masks |
| **A-EXEC** | Mesa ACO, `aco_insert_exec_mask.cpp`, `aco_lower_to_hw_instr.cpp` | how the compiler builds and clears `EXEC` and what a discard compiles to |
| **A-IR** | Mesa ACO, `aco_ir.h` | what each image flag means (`dmask` "Data VGPR enable mask", `unrm` "Force address to be un-normalized", `tfe` "texture fail enable", `lwe` "LOD warning enable", `r128` "NAVI: Texture resource size", `a16` "Address components are 16-bits", `d16` "Convert 32-bit data to 16-bit data") |
| **M-DESC** | Mesa, `src/amd/common/ac_descriptors.c`, `ac_descriptors.h` | the four words of a buffer descriptor and the out-of-bounds checks |
| **M-SI** | Mesa, `src/gallium/drivers/radeonsi/si_descriptors.c` | the 14-bit stride field, the record count scaled by the stride |

None of this code is copied. The project's own files only read the published field positions and tables.

## 2. Scalar memory loads (`SMEM`)

`s_load_dword`, `x2`, `x4`, `x8`, `x16` and `s_buffer_load_dword`, `x2`, `x4`, `x8`, `x16` are the ten load forms
of this format. Their opcode numbers, from A-OPC's gfx10 column: **0, 1, 2, 3, 4** and **8, 9, 10, 11, 12**. ACO
lists no `s_load_dwordx3` for gfx10 (it is a gfx12 opcode), and nothing at 5, 6, 7. The stores (`s_store_dword*`)
are at 0x10-0x12 and `s_buffer_store_dword*` at 0x18-0x1a; this file does not describe them (refused).

| answer | value for this generation | how it is established |
|---|---|---|
| the base | for `s_load_*`: the **64-bit** value in the register pair at `sbase * 2`; for `s_buffer_load_*`: the four-word **buffer descriptor** at `sbase * 2` | L-SMEM: the field is `sbase{6-1}` (so the encoded value is the register number divided by 2), and the base register class is `SReg_64` for `s_load_*` and `SReg_128` for `s_buffer_load_*`. **tested** (a field of 1 gives register 2; a field of 8 gives register 16) |
| the offset | a **21-bit unsigned byte offset** carried in the instruction (`Inst{52-32}`); it is not scaled | L-SMEM (the field); A-ASM (a constant operand is written straight into this field). **tested** (`0x1234` comes back as `0x1234`) |
| the offset register | a 7-bit scalar register code (`Inst{63-57}`). **125 (SGPR_NULL) means "no register offset"** | A-ASM: on GFX10 the register offset is disabled by naming SGPR_NULL; and the real encoding of `s_load_dwordx4 s[20:23], s[2:3], 0x0` in L-TEST ends in `0xfa`, whose top seven bits are 125. **tested** (125 answers `offset_register_is_null`) |
| how many words, and where | 1, 2, 4, 8 or 16 consecutive scalar registers starting at `sdst` | L-SMEM: the destination register classes are `SReg_32`, `SReg_64`, `SReg_128`, `SReg_256`, `SReg_512`. **tested** |
| is it bounds-checked | `s_load_*`: **no** - only a 64-bit value and an offset take part, no descriptor and no record count; `s_buffer_load_*`: **yes**, against the descriptor's record count (section 3) | L-SMEM (the operand classes show there is no descriptor for `s_load_*`); M-DESC and L-USAGE for the buffer forms. **tested** (the two forms answer differently) |

An `s_load_dwordx4 s[20:23], s[2:3], 0x20` therefore reads 16 bytes from the byte address
`s[2:3] + 0x20` into `s20..s23`. With an SGPR in the offset field, that register's 32-bit value is added.

**What the game does with this**: the module's first table says a descriptor table's address arrives in one scalar
register, and the programs load descriptors as `s_load_dwordx4`/`x8`/`x16` at offsets that are multiples of 32
bytes (SHADER-TRANSLATOR.md section 15). This document's facts are what makes those loads describable: the base is
the pair at `sbase * 2`, the offset is the immediate, and the result is the descriptor's words.

## 3. Buffer loads and stores (`MUBUF`, `MTBUF`) and the buffer descriptor

The untyped buffer forms (`MUBUF`) and the typed ones (`MTBUF`) share an encoding shape and differ in whether the
descriptor's format converts the data. Opcodes, A-OPC gfx10 column:

| opcode | instruction | words/components |
|---|---|---|
| 0x00-0x03 | `buffer_load_format_x`, `_xy`, `_xyz`, `_xyzw` (typed: the descriptor's format converts) | 1, 2, 3, 4 |
| 0x04-0x07 | `buffer_store_format_x` .. `_xyzw` | 1, 2, 3, 4 |
| 0x08 / 0x09 / 0x0a / 0x0b | `buffer_load_ubyte` / `sbyte` / `ushort` / `sshort` | 1 |
| 0x0c / 0x0d / 0x0f / 0x0e | `buffer_load_dword` / `x2` / `x3` / `x4` | 1, 2, 3, 4 |
| 0x18 / 0x1a / 0x1c / 0x1d / 0x1f / 0x1e | `buffer_store_byte` / `short` / `dword` / `x2` / `x3` / `x4` | 1, 1, 1, 2, 3, 4 |
| 0x19 / 0x1b | `buffer_store_byte_d16_hi` / `short_d16_hi` | 1 |
| 0x80-0x83 / 0x84-0x87 | `buffer_load_format_d16_*` / `buffer_store_format_d16_*` | 1, 2, 3, 4 (16-bit) |
| `MTBUF` 0x00-0x07 | `tbuffer_load_format_*` / `tbuffer_store_format_*` | 1, 2, 3, 4 |
| `MTBUF` 0x08-0x0f | `tbuffer_*_format_d16_*` | 1, 2, 3, 4 (16-bit) |

(The d16 forms have different numbers in the two formats - 0x80-0x87 for `MUBUF`, 0x08-0x0f for `MTBUF` - because
the typed format's opcode has only four bits.)

### 3.1 The address

| answer | value | how it is established |
|---|---|---|
| the descriptor | the **four scalar registers** at `srsrc * 4` (the field is `srsrc >> 2`) | L-BUF (`srsrc` is `SReg_128` in the instruction definitions); A-ASM writes `reg >> 2` into the field. **tested** |
| the immediate offset | 12 bits, bytes (`offset` [11:0]) | L-BUF; **tested** |
| the index register | `idxen` set: the vector register in `vaddr` holds the **element index**, which the hardware scales by the descriptor's **stride** | L-BUF (the flag); A-ISEL packs the index and the byte offset as a two-register vector in that order when both flags are set (`if (offen && idxen) vaddr = {index, offset}`); the scale follows from the descriptor's stride field and the out-of-bounds check (3.2) |
| the offset register | `offen` set: the vector register in `vaddr` holds a **byte offset**; with both flags set it is `vaddr + 1` | as above. **tested** (both flags give `index = vaddr`, `offset = vaddr + 1`) |
| the scalar offset | the 8-bit `soffset` field is a **general 32-bit source**: 0..127 is a scalar register, 128..192 the integers 0..64, 193..208 -1..-16, 240..248 the float constants. The usual "no offset" is the **inline constant 0 (code 128)**, and SGPR_NULL (125) also occurs | L-BUF (`SCSrc_b32`); L-TEST: `buffer_load_dword v5, off, s[8:11], 0` ends with `0x80` = 128. **tested** (128 answers `soffset_is_zero`, 3 answers `soffset_register`) |
| the data | `components` **consecutive vector registers** starting at `vdata` (loads write them, stores read them) | A-OPC for the count per opcode; the register field for the first. **tested** |
| out of range | the hardware checks the access against the descriptor and returns **zero** for an out-of-range word (stores are dropped), word by word | L-USAGE: "Instructions that load from or store to buffer resources ... generally implement handling for out of bounds (OOB) memory accesses", "the `voffset` used for the access will be range-checked for each 32-bit word independently", and the example `<0, v0, v1, v2>` for a partly out-of-range load. **tested** (`bounds_checked` is set) |

The full picture is therefore:

```
byte address = descriptor base
             + immediate offset (12 bits)
             + (idxen ? index * stride : 0)
             + (offen  ? offset_register : 0)
             + soffset
```

The scaling of the index by the stride is a reading of the descriptor's fields and of the out-of-bounds check
quoted in 3.2 ("`index >= NUM_RECORDS`"), which only makes sense if `index` is an element number and the byte
position comes from the stride; the sources read state the stride field and the flags, not the multiplication in
words. **Marked: from source for the fields, a reading for the arithmetic.**

### 3.2 The four words of a buffer descriptor

M-DESC, `ac_build_buffer_descriptor` (the driver function that builds such a descriptor), with M-SI for the stride
width:

| word | contents |
|---|---|
| 0 | the low 32 bits of the base address (`desc[0] = state->va`) |
| 1 | the address's **high** bits in the low half (`S_008F04_BASE_ADDRESS_HI(state->va >> 32)`) and the **stride** above them (`S_008F04_STRIDE(state->stride)`); the stride field is 14 bits (M-SI: "The stride field in the resource descriptor has 14 bits", asserted `< (1 << 14)`) |
| 2 | the **record count** (`rsrc_word2 = state->size`); for a strided ring buffer the driver multiplies it by the stride first (M-SI: `if (gfx_level >= GFX8 && stride) num_records *= stride`), so there it counts bytes |
| 3 | the **flags**: destination swizzles, index stride, add-tid-enable, the data format, the out-of-bounds select and the resource-level flag (`ac_set_buf_desc_word3`) |

The out-of-bounds select chooses the check; the driver's own comment for this generation reads (M-DESC,
`ac_set_buf_desc_word3`):

```
GFX10:
 - 0: (index >= NUM_RECORDS) || (offset >= STRIDE)
 - 1: index >= NUM_RECORDS
 - 2: NUM_RECORDS == 0
 - 3: if SWIZZLE_ENABLE: swizzle_address >= NUM_RECORDS
      else:                offset >= NUM_RECORDS
```

`ac_build_raw_buffer_descriptor` picks `V_008F0C_OOB_SELECT_RAW` for a plain raw buffer; radeonsi's ring buffers
use `V_008F0C_OOB_SELECT_DISABLED` (M-SI). Which value the console's descriptors carry is **not established** by
the sources read - and a translator does not need it, because a PC translation keeps the game's own bounds
information only if it also keeps the game's descriptor bytes (see section 9).

## 4. Image instructions (`MIMG`)

### 4.1 The encoding

| field | position | meaning |
|---|---|---|
| `vaddr` | word 1 bits 7:0 | the first address vector register |
| `vdata` | word 1 bits 15:8 | the first data vector register |
| `srsrc` | word 1 bits 20:16 (`srsrc >> 2`) | the **eight** scalar registers at `srsrc * 4` hold the image descriptor |
| `ssamp` | word 1 bits 25:21 (`ssamp >> 2`) | the **four** scalar registers at `ssamp * 4` hold the sampler descriptor |
| `dim` | word 0 bits 5:3 | 0 1D, 1 2D, 2 3D, 3 cube, 4 1D array, 5 2D array, 6 2D multisampled, 7 2D multisampled array |
| `dmask` | word 0 bits 11:8 | which components the instruction reads or writes |
| `nsa` | word 0 bits 2:1 | the number of **extra address words** the instruction carries |
| `a16` / `d16` | word 1 bits 30 / 31 | 16-bit address components / 16-bit data components |
| `unorm` | word 0 bit 12 | the coordinates are unnormalised texel coordinates |
| `r128` | word 0 bit 15 | the `r128` flag (see 4.5) |
| `tfe` / `lwe` | word 0 bits 16 / 17 | texture-fail status word / lod-warning flag |

The descriptor widths are L-MIMG: the instruction's operand list gives `srsrc` the register class `SReg_256_XNULL`
(eight 32-bit registers) and `ssamp` `SReg_128_XNULL` (four). The dim numbers are the encodings of LLVM's
`AMDGPUDimProps` in L-INTR (`0x0` 1d, `0x1` 2d, ... `0x7` 2darraymsaa), which is what the `dim` field holds
(A-IR: `dim : 3; /* NAVI: dimensionality */`). The flag meanings are A-IR's comments. **tested**: the descriptor
registers, the dim name, `dmask`, and each flag.

### 4.2 Which opcodes this file describes

The numbers are A-OPC's gfx10 column. `access_of` describes:

| opcode(s) | instruction(s) |
|---|---|
| 0x00 / 0x01 | `image_load` (no lod) / `image_load_mip` (an explicit mip) |
| 0x08 / 0x09 | `image_store` / `image_store_mip` |
| 0x0e | `image_get_resinfo` - the image's size for one mip |
| 0x60 | `image_get_lod` |
| 0x20-0x3f | the sample family: `image_sample`, `_cl`, `_d`, `_d_cl`, `_l`, `_b`, `_b_cl`, `_lz`, the `_c` compare forms (0x28-0x2f), the `_o` offset forms (0x30-0x37) and both together (0x38-0x3f) |
| 0x40-0x5f | the gather family `image_gather4*` (the numbers 0x42, 0x43, 0x4a, 0x4b, 0x52, 0x53, 0x5a, 0x5b name nothing on this generation) |
| 0x68-0x6f | the combined-derivative sample forms `image_sample_cd`, `_cd_cl`, `_c_cd`, `_c_cd_cl` and their `_o` forms |

Everything else - atomics, the packed (`_pck`) forms, the 16-bit-coordinate `_g16` sampling forms, `image_msaa_load`,
the ray-tracing forms - is **refused with a reason**, not guessed.

### 4.3 The address components - the critical order

Every dimension-aware image intrinsic is built by L-INTR as

```
AddrArgs = {offset} {bias} {z-compare} (gradients) (coordinates + slice) (lod/clamp)
```

(the comment in L-INTR above `AMDGPUSampleVariant::ExtraAddrArgs` reads `// {offset} {bias} {z-compare}`, and
`AMDGPUDimProfile::AddrArgs` concatenates exactly in that order). Mesa's compiler places the same components in
the same order as the instruction's address operands (A-ISEL, `aco_select_nir.cpp`, the block beginning
`/* gather MIMG address components */`: `offset`, `bias`, `compare`, then the derivatives, then the coordinates;
`get_image_coords` appends the sample index and the lod/clamp after the coordinates). Two independent compilers
agreeing on the order, one of them (ACO) writing the final machine code, is the strongest check available here.

So, in the **order the hardware reads the address registers**:

| # | component | present when | words |
|---|---|---|---|
| 1 | texel **offset**: 6 signed bits per coordinate, packed (first coordinate in bits 7:0, second in 15:8, third in 23:16) | the opcode ends in `_o` | 1 |
| 2 | lod **bias** | `_b` | 1 |
| 3 | **compare** value | `_c` | 1 |
| 4 | **derivatives**: `dx` for coordinate 0, then 1, then 2; then `dy` for 0, 1, 2 | `_d` | 2 x (number of coordinates) |
| 5 | **coordinates**: `s`[, `t`[, `r`]]; for a cube `s`, `t`, **face** | always, except `image_get_resinfo` | 1, 2 or 3 |
| 6 | **array slice** | an arrayed dim | 1 |
| 7 | **sample index** | a multisampled dim | 1 |
| 8 | **lod** (`_l`) or **lod clamp** (`_cl`) | `_l` / `_cl` | 1 |

The number of coordinates is the dim's: 1D and 1D array 1, 2D / 2D array / 2D multisampled / 2D multisampled array
2, 3D 3, cube 2 (plus the face). The derivatives use only the coordinates, not the slice, the face or the sample
index - L-INTR's `GradientArgs` are built from `coord_names` alone.

Worked example, 2D `image_sample_c_d` (`dmask` 0x3, `vaddr` v2):

| word | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| role | compare | dx of s | dx of t | dy of s | dy of t |
| register | v2 | v3 | v4 | v5 | v6 |
| then | **coordinates** s in v7, t in v8 | | | | |

The second half of the table's last line is part of the same instruction: the full list for that opcode is
`compare, dx, dx, dy, dy, coord, coord` at v2..v8 - **tested** exactly, and the same shape is tested for
`image_sample` (coords only), `image_sample_c_l` (compare, coords, lod), `image_sample_cl` (coords, clamp),
`image_sample_o` (offset first), `image_load` on a 2D array (coords, slice), `image_load_mip` (coords, lod),
`image_get_resinfo` (the mip level alone) and `image_gather4_c` (compare, coords).

**Why the order matters.** A translator that swaps the coordinates and the derivatives produces a shader that
still "works": it fetches from wrong places. The order is the one fact in this document that cannot be recovered
from the shader by testing, so it is the one to check first against a second source if a texture ever looks wrong.

### 4.4 Consecutive registers, and the non-sequential form (`nsa`)

With `nsa` = 0 the address components are **consecutive registers** starting at `vaddr`: component *i* is in
`vaddr + i`.

With `nsa` > 0 the first component is still `vaddr`, and the others are named in the extra words: each extra word
holds **four 8-bit register numbers, one per byte**, and `nsa` is the number of extra words. A-ASM is explicit:

```c
nsa[k / 4] |= reg(ctx, instr->operands[i], 8) << (k % 4 * 8);
```

and `get_mimg_nsa_dwords` computes how many such words are needed: one word covers up to five address dwords
(`DIV_ROUND_UP(addr_dwords - 1, 4)`). **Tested**: a 2D `image_sample_d` with `nsa` = 2 and the extra words
`0x0D0C0B0A, 0x11100F0E` puts its six components in v2, v10, v11, v12, v13, v14; and an instruction that needs
more components than its extra words can name is refused.

SHADER-TRANSLATOR.md section 14 said this reader "does not establish from its sources which bits of those words hold
a register number". It does now: bits 7:0, 15:8, 23:16, 31:24 of each extra word, in that order.

With `a16` the address components are 16-bit and **two share one register**: component 0 is the low half of the
first address register, component 1 its high half, component 2 the low half of the next, and so on (A-ISEL packs
them as `v2b` pairs in order; A-IR: "Address components are 16-bits"). **Tested**: a 2D `image_sample_l` with
`a16` puts `s` in the low half of v2, `t` in its high half and the lod in the low half of v3.

### 4.5 The data side of an image instruction

| answer | value | how it is established |
|---|---|---|
| `dmask` | bit *i* selects component *i* (x, y, z, w) of the value | A-IR: "Data VGPR enable mask". **tested** |
| where the components land | the selected components go to **consecutive registers starting at `vdata`**, in increasing component order; `result_words` is their number | A-ISEL computes the data size from the mask (`num_bytes = util_bitcount(dmask) * ...`) and expands the result vector from consecutive registers. **tested** |
| a gather | returns **four** values whatever the mask says: "gather4 selects the component by dmask and always returns vec4" (A-ISEL) | **tested** |
| `d16` | two 16-bit components per register, so half as many registers | A-IR; A-ISEL's `unpacked_d16` handling. **tested** |
| `tfe` | one **extra** data word after the components: the texture-fail status | A-IR ("texture fail enable"); A-ISEL adds four bytes to the data size when it is set. **tested** |
| `lwe` | the lod-warning flag; what it changes is **not established** here (A-IR only names it) | refused only if it matters to a translation, which it does not: the data layout is unaffected |
| `unrm` | the coordinates are unnormalised texel coordinates instead of [0,1] | A-IR. **tested** (the bit is reported) |
| `r128` | A-IR says only "NAVI: Texture resource size". In A-ISEL it is set for the ray-tracing image forms, where the single address word is an index. What it means for an ordinary fetch is **not established** | reported as a flag; a translator must refuse an `r128` image it meets, because the address interpretation is not established |
| a store | `dmask` selects which components of the data at `vdata` are written; no result | A-ISEL's store path. **tested** |
| `dmask` = 0 | refused: nothing would be read or written, and the sources read do not describe such an instruction | **tested** |

### 4.6 The image and sampler descriptors

The **image descriptor is the eight scalar registers at `srsrc * 4`**, the **sampler descriptor the four at
`ssamp * 4`** (L-MIMG's operand classes; A-ASM encodes `reg >> 2` into both fields). Their internal field layout is
the hardware's `T#`/`S#` record; this document does not restate it, and a translator that keeps the game's own
descriptor bytes does not need it (section 9). What matters here is *which* registers, and that a static sampler
is written by moves just before the sample rather than loaded (exe-map/SHADER-PATH.md section 12).

## 5. Interpolation (`VINTRP`)

Three one-word instructions, numbers 0, 1, 2 (A-OPC; L's `VOP3Instructions.td` gives the same three at 0x200-0x202
as their three-operand encodings). Fields: `vsrc` [7:0], `attrchan` [9:8], `attr` [15:10], `vdst` [25:18].

| instruction | what the sources establish |
|---|---|
| `v_interp_p1_f32` (0) | L-INTR: `__builtin_amdgcn_interp_p1 <i>, <attr_chan>, <attr>, <m0>` - the operand is the **first barycentric coordinate**, and M0 is part of the input. A-ISEL (`aco_isel_helpers.cpp`, `emit_interp_instr`) emits it as `v_interp_p1_f32 dst, coord1, m0, attr, chan` |
| `v_interp_p2_f32` (1) | L-INTR: `__builtin_amdgcn_interp_p2 <p1>, <j>, <attr_chan>, <attr>, <m0>` - it takes the **first step's result** and the **second coordinate**. A-ISEL: `v_interp_p2_f32 dst, coord2, m0, interp_p1, attr, chan` |
| `v_interp_mov_f32` (2) | L-INTR: "param values: 0 = P10, 1 = P20, 2 = P0"; L's disassembler prints the same three names for the same numbers (`AMDGPUInstPrinter::printInterpSlot`); A-ISEL passes `(vertex_id + 2) % 3` and marks 2 as `/* P0 */`. The source field holds that number itself, 0, 1 or 2. It is not a register and not an inline-constant code: A-ASM writes `0x3 & constantValue()` into the field for this instruction (`emit_vintrp_instruction`), and L's printer reads the field as a plain number. **tested**; counted on the game's programs in boot b115 (docs/SHADER-TRANSLATOR.md section 25). An earlier form of this row said the field was an inline-constant code; that was wrong and the code refused every real move |

**The formula.** The three values the instructions address are named `P0`, `P10` and `P20`; the first step takes
the first coordinate `i`, the second takes `j` and the first step's result. That is the shape of

```
attribute = P0 + i * P10 + j * P20
v_interp_p1_f32:  dst = P0 + i * P10          (the part that uses i)
v_interp_p2_f32:  dst = dst + j * P20         (the part that uses j)
```

The **names and the operand roles are from source**; the polynomial itself is the reading of those names (P10 and
P20 are the two edge deltas of a linearly interpolated attribute) - no file read for this work spells the
arithmetic out, so it is marked **from source for the operands, a reading for the arithmetic**.

**M0** holds the primitive's mask: A-ISEL passes the shader argument `prim_mask` into M0 before each step
(`bld.m0(prim_mask)`). What the hardware does with it (which bits select the primitive's vertices) is **not
established** here; a translator supplies the same 64-bit value the shell provides for the pixel shader's M0, or
refuses the program.

**`attr` and `chan`**: the attribute number and the component inside it. The pair selects which parameter the
interpolation unit reads; the game's programs use them together with the pixel module's seven small export
sequences (exe-map/SHADER-PATH.md section 10).

## 6. Export (`EXP`)

Fields (L-MIMG's `EXPe`): `en` [3:0], `tgt` [9:4], `compr` [10], `done` [11], `vm` [12], and four source registers
in word 1, one per byte.

| target | class | source |
|---|---|---|
| 0..7 | colour, MRT0..MRT7 | L-DEF (`ET_MRT0 = 0`, `ET_MRT7 = 7`) |
| 8 | depth / stencil / sample mask (MRTZ) | L-DEF (`ET_MRTZ = 8`) |
| 9 | a null export (writes nothing; pre-GFX11) | L-DEF (`ET_NULL = 9`) |
| 12..15, 16 | position, POS0..POS3, and POS4 on this generation | L-DEF (`ET_POS0 = 12`, `ET_POS3 = 15`, `ET_POS4 = 16` with the comment `// GFX10+`) |
| 20 | the primitive's setup data (PRIM) | L-DEF (`ET_PRIM = 20`, `// GFX10+`) |
| 32..63 | vertex parameter PARAM0..PARAM31, interpolated later by a pixel shader | L-DEF (`ET_PARAM0 = 32`, `ET_PARAM31 = 63`) |

Any other target is **refused**: the public table for this generation names no other value (21 and 22 are the
dual-source-blend targets and are marked GFX11+).

| answer | value | how it is established |
|---|---|---|
| `en` | one bit per component, in the order x, y, z, w of the four sources | A-ISEL (`enabled_channels`); **tested** |
| `compr` | **two mask bits per register**: a register is written if either of its two bits is set, and each register holds two 16-bit components. The compiler packs them with `v_cvt_pkrtz_f16_f32` (two floats round-to-zero into one register) before the export | A-ISEL: `channel_mask = compressed ? ((write_mask & 0x3) ? 1 : 0) | ((write_mask & 0xc) ? 2 : 0) : write_mask` and the comment "Compressed export uses two bits for a channel"; L-INTR's `int_amdgcn_exp_compr` takes `src0`/`src1` as `v2f16`/`v2i16`. **tested** (the flag and the raw mask) |
| `done` | the last export of the program | A-ISEL: the null export of a pixel shader sets done; **tested** |
| `vm` (valid mask) | the flag used for the "row" exports; L-INTR calls the intrinsic argument `vm (ignored on GFX11+)` | A-ISEL sets it on the null export. **tested** (reported) |

## 7. The whole-wave instructions a one-lane translation must understand

These are not memory operations; a translation has to *understand* them because they change what "a lane" means or
when memory has arrived, and then either drop them or restructure around them.

| instruction | number | what it is | how to establish it |
|---|---|---|---|
| `s_wqm_b64` | SOP1 10 (`s_wqm_b32` at 9) | turns a lane mask into **whole-quad** masks: every group of four lanes is enabled if any of its lanes is. It is what a pixel shader does before derivatives and sampling, so that helper lanes take part | L's `SOPInstructions.td` gives the number and the intrinsic `int_amdgcn_s_wqm`; A-EXEC and A-ISEL emit it before a sample that uses implicit derivatives (`bld.sop1(Builder::s_wqm, Definition(exec), ..., Operand(exec))`). **tested** |
| `s_and_saveexec_b64` (`_or_`, `_xor_`, `_nand_`, `_nor_`) | SOP1 0x24, 0x25, 0x26, 0x29, 0x2a | saves the old EXEC in the destination and replaces EXEC with the old EXEC combined with the source mask (AND/OR/XOR/NAND/NOR). Restoring is a move back into EXEC (`s_mov_b64 exec, saved`) | L `SOPInstructions.td` (numbers and names); A-EXEC uses the AND form for if/else regions. **tested** |
| `s_waitcnt` | SOPP 12 | waits until the memory counters named by its 16-bit field have drained to their thresholds. The fields on this generation: **VM_CNT** bits 3:0 and 15:14 (a six-bit counter), **EXP_CNT** bits 6:4, **LGKM_CNT** bits 13:8 | L-DEF, `Utils/AMDGPUBaseInfo.cpp`, the shifts and widths for major version 10. **tested** (5 / 3 / 12 and the two high vmcnt bits) |
| `s_clause` | SOPP 33 (0x21) | declares how many of the following scalar memory instructions may be issued as one group | L `SOPInstructions.td` (`SOPP_Real_32_gfx10_gfx13<0x021>`); docs/SHADER-OPCODES.md measured it in the game's programs. **tested** |
| `s_inst_prefetch` | SOPP 32 (0x20) | prefetches instruction bytes into the instruction cache | L `SOPInstructions.td` (`SOPP_Real_32_gfx10<0x020>`). **tested** |
| `s_setprio` | SOPP 15 | a scheduler priority hint for the wave | L `SOPInstructions.td`; docs/SHADER-OPCODES.md. **tested** |

Only the four scalar instructions above are answered; `s_branch`, `s_cbranch_*`, `s_endpgm` and the ordinary
scalar moves are **refused** by this file (they are control flow, not access).

### 7.1 What a discard looks like

A discard is not one instruction. Mesa's compiler builds it as (A-EXEC, `aco_insert_exec_mask.cpp`,
`p_discard_if`):

```c
/* discard from current exec */
s_andn2_b64 exec, current_exec, cond      // clear the bit of every lane that discards
```

and then, in `aco_lower_to_hw_instr.cpp`, a branch to a block that sends the ordered "ps done" message if needed,
writes a **null export** (`exp` with target 9, `en` 0, `done` and `vm` set) and ends the program (`s_endpgm`); the
branch is `s_cbranch_execz` on the new EXEC (or `s_cbranch_scc0` on SCC when the condition came in SCC). So a
translated pixel shader sees a lane's bit in EXEC being cleared and a branch to the export-and-end block.

In a lane translation the effect is: the lane's own bit of the (saved) mask decides whether its stores, exports and
the rest of the program happen. That is what the existing `shader_meaning.*` lane model already does for vector
writes; a translator has to carry it for exports and stores too. **Proposal**, since no source states it: treat
`s_andn2_b64 exec, ...` as "the lanes whose bit was cleared stop writing outputs", and the branch on `execz` as
"if no lane of this wave is left, the wave ends".

## 8. What a translator does with each

**This whole section is a proposal.** It says how the facts above can be turned into another GPU's shader
language; nothing in it is established by a source.

| console instruction | the equivalent high-level operation |
|---|---|
| `s_load_dword*` | read 1..16 consecutive 32-bit words of the shell's flat buffer at `(s[base] + offset)`. The result becomes 1..16 scalar variables. Since the address comes from the game's own registers, this is a plain load of the game's memory, exactly as section 4 of SHADER-TRANSLATOR.md describes |
| `s_buffer_load_dword*` | the same read, but through the descriptor: the address is word 0 and the low half of word 1, the length is word 2. In a PC shader this is a **read of a constant buffer** whose binding the shell derives from the module's tables (SHADER-TRANSLATOR.md section 20). The bounds check must be reproduced only if the game's data can be out of range; the shell's flat buffer already contains the game's memory, so a raw load is faithful as long as the descriptor's base is what the game wrote |
| `buffer_load_*` / `tbuffer_load_*` | a load of `components` words at the computed address; a typed buffer's `data_format` conversion becomes a format conversion in the PC shader (or the descriptor is replaced by a typed PC buffer). Out-of-range must give zero, so the translation should keep the descriptor's record count as a bound |
| `buffer_store_*` / `tbuffer_store_*` | the same, in reverse |
| `image_load` / `image_load_mip` | a **texel fetch** at the coordinates (plus the slice or the sample index), with an explicit mip for `_mip`. On a PC this is an `OpImageFetch` |
| `image_store` | an **image write** of the components `dmask` names |
| `image_sample*` | a **texture sample** with exactly the arguments the address order gives: coordinates, derivatives or lod or bias, the compare value for a shadow sample, the offset for an offset sample. The sampling *itself* is where the console's texture layout matters (SHADER-TRANSLATOR.md section 5): a translated fetch that keeps the game's texture memory has to apply the console's tiling before a PC sampler sees it, or the PC binding has to carry the tiled memory as a buffer and the shader does the tiling arithmetic |
| `image_gather4*` | a **gather** of the component `dmask` selects into four registers |
| `image_get_resinfo` | the image's **size** in texels for the given mip (and the number of levels / samples by `dmask`) - a value the shader computes or reads from the descriptor |
| `image_get_lod` | the **derivative-based lod** of a coordinate pair (two results by `dmask`) |
| `v_interp_p1_f32` / `p2_f32` | an **input attribute**: the value of attribute `attr`, component `chan`, interpolated with the two barycentric coordinates the hardware provided; on a PC it is a fragment-shader input (`OpLoad` of the input variable), and the barycentrics/`P0/P10/P20` are the console's way of producing the same number |
| `v_interp_mov_f32` | the same attribute **without interpolation** (one of `P0`, `P10`, `P20`, i.e. a provoking vertex's value): a flat-shaded input |
| `exp` colour (0..7) | a write to **render target n** of the components `en` names; with `compr`, two 16-bit values per register, so the target is 16-bit |
| `exp` depth (8) | a write of depth / stencil / sample mask |
| `exp` position (12..16) | a **clip-space position** (a vertex shader's output): the four components go to POS0..POS3 (and POS4 on this generation), one export each |
| `exp` parameter (32..63) | a **vertex output parameter** that a pixel shader reads back with `v_interp_*` |
| `s_wqm_b64` | no PC instruction: the whole-quad mask belongs to the console's helper-lane model. A translation that emits per-fragment code drops it; one that models helper lanes has to reproduce "enable the quad" |
| `s_and_saveexec_b64` / the move back into EXEC | the console's **branch masks**. In structured SPIR-V these become `OpSelectionMerge`/`OpBranchConditional` on the mask, or per-lane predicates. The existing `shader_flow.*` already structures the branches; the exec masks decide *which lanes* take them |
| `s_waitcnt`, `s_clause`, `s_inst_prefetch`, `s_setprio` | **nothing**: they order and schedule memory on the console. A translator drops them once the loads and stores they guard are ordered by the PC shader's own dependencies (SPIR-V memory semantics replace the counters) |
| a discard (`s_andn2_b64 exec` + branch to an export-and-end block) | a **discard**: `OpDemoteToHelperInvocation` for a pixel shader, or the same lane-mask handling as above. The null export in the exit block is not a colour write and must not become one |

## 9. What is not established, and is therefore refused

| question | state |
|---|---|
| the internal fields of an image descriptor (`T#`) and a sampler descriptor (`S#`) | not restated here. The translation does not need them if it keeps the game's descriptor bytes; if it converts them, the hardware's field layout has to be established from the ISA documents first |
| what `r128` changes for an ordinary fetch | not established (A-IR names it only as "Texture resource size"); an `r128` image is refused by this file's data model - it is reported, and a translator must not treat it as a normal fetch |
| what `lwe` changes | not established; it does not change the data layout, so it is reported and ignored |
| the arithmetic of interpolation (`P0 + i*P10 + j*P20`) | the operand roles are from source; the polynomial is a reading of the names (section 5) |
| what the hardware does with M0 during interpolation | not established; M0 carries the primitive mask from the API |
| which `OOB_SELECT` value the console's descriptors carry | not established (the driver picks different values for different buffer kinds) |
| the scaling of the index by the stride | a reading of the descriptor semantics (section 3.1) |
| the opcodes outside section 4.2 (atomics, packed, `_g16`, `image_msaa_load`, ray tracing) | refused with the opcode number |
| the SMEM store forms | refused (`s_store_dword*`, `s_buffer_store_dword*`) |
| every export target the public table does not name on this generation (for example 10, 11, 21, 22) | refused with the target number |

## 10. Where the code is

* [`shell/shader_access.hpp`](../shell/shader_access.hpp) - `AccessKind`, `ImageAddressRole`, `AccessInfo`,
  `access_of`;
* [`shell/shader_access.cpp`](../shell/shader_access.cpp) - the tables of sections 2-7, each with the source of the
  numbers next to it;
* [`shell/tests/test_shader_access.cpp`](../shell/tests/test_shader_access.cpp) - hand-built instruction words for
  every group, two or more per group, the field-by-field answers, and the refusals.

The test is registered as `shader_access` in `shell/CMakeLists.txt` and runs with the rest under
`shell\build.cmd`. No game bytes are used anywhere in it.