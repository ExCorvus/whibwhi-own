# Writing a Direct3D shader container

The writer's types are in the namespace `whiwhi::shell::dxbc` (it has its own `Operand`, `Opcode` and
`SignatureElement`, which are not the instruction reader's or the container reader's types of those names).

This document describes `shell/shader_dxbc.hpp` / `shell/shader_dxbc.cpp`, the writer that produces a
Direct3D shader container (`DXBC`) holding an input signature (`ISGN`), an output signature (`OSGN`) and a
shader-model-5.1 tokenized program (`SHEX`). The PC graphics library the shell hands shaders to accepts only
Direct3D shader containers, so a translator has to write one.

It is an instruction-level builder, not a compiler: the caller supplies declarations and instructions and the
writer lays out the bytes. The writer only produces shader model 5.1; the shader-model-5.0 instruction form is
not written.

Evidence in this document is marked:

- **[tested here]** - measured by building the bytes and running them through a tool in this repository, or by
  reading the output of the system `D3DCompiler_47.dll` while writing the code.
- **[public: ...]** - read from a named public source. No code was copied; only published numbers, layouts and
  behaviour are used.
- **[not established]** - the work did not establish it; the writer does not rely on it.

The two primary public sources are:

- `d3d11TokenizedProgramFormat.hpp`, the Windows SDK header that documents the D3D11 tokenized program format.
  It gives the opcode numbers, operand token fields, declaration token fields and signature element numbers
  used here. [public: Windows SDK `d3d11TokenizedProgramFormat.hpp`]
- The public description of the DXBC container in RenderDoc's `dxbc_container.cpp` (MIT-licensed), which
  documents the container header and the modified-MD5 checksum, together with the DXBC format page on
  reverseengineering.wiki. [public: RenderDoc `dxbc_container.cpp`; reverseengineering.wiki `Format:DXBC`]

---

## 1. Container layout

A container is a 32-byte header, an array of part offsets, and the parts. All integers are little-endian.
**[tested here]** The layout was confirmed by building containers and having `D3DDisassemble` accept them, and
by reading containers that `D3DCompiler_47.dll` produced.

| Offset | Size | Meaning |
|---|---|---|
| `0x00` | 4 | `'DXBC'` |
| `0x04` | 16 | checksum (see section 2) |
| `0x14` | 4 | container version, written as `1` (version 1.0) |
| `0x18` | 4 | total container size in bytes |
| `0x1C` | 4 | number of parts |
| `0x20` | `4 * parts` | offset of each part, from the start of the container |

Each part is `{ four-CC (4 bytes), size (4 bytes), payload (size bytes) }`. Parts and offsets are four-byte
aligned. The writer emits three parts in this order:

| Four-CC | Payload |
|---|---|
| `ISGN` | input signature (section 3) |
| `OSGN` | output signature (section 3) |
| `SHEX` | shader-model-5.1 program (section 4) |

The parts `RDEF` (resource definitions), `STAT`, `PRIV` and the debug chunks are not written (section 8).
`D3DDisassemble` accepts a container without them **[tested here]**: the three-part container is enough to
decode the program and print the declarations and instructions.

---

## 2. The checksum

The 16 bytes at offset `0x04` are a documented variant of MD5 over the bytes from offset `0x14` to the end of
the container (that is, from the version word through the last part). **[public: RenderDoc
`dxbc_container.cpp` describes this finalisation; the MD5 compression is RFC 1321]**

Write `length = total_size - 0x14`, `hash_bits = length * 8`, and `second = (hash_bits >> 2) | 1`. Then:

1. Feed every whole 64-byte block of the region through the MD5 compression function.
2. Let `leftover = length % 64`.
3. If `leftover >= 56`: feed the `leftover` bytes followed by a `0x80` byte, zero-padded to 64 bytes; then
   feed one 64-byte block that holds `hash_bits` in its first four bytes and `second` in its last four bytes.
4. Otherwise feed one 64-byte block holding, in order, `hash_bits` (4 bytes), the `leftover` bytes, a `0x80`
   byte, zeroes, and `second` in the last four bytes.

The result is the MD5 state `a, b, c, d` written little-endian, in that order. Step 4 places the length at the
*start* of the final block rather than the end; that is the non-standard part of the DXBC variant.

**[tested here]** The implementation was checked two ways:

- Given a container produced by the system `D3DCompiler_47.dll`, `compute_dxbc_checksum` reproduces the 16
  bytes Microsoft stored. The test does this with a small shader it writes itself (`D3DCompile`, target
  `vs_5_1`), so no game data is involved.
- `D3DDisassemble` **rejects** a container whose checksum byte has been altered (it returns failure), which
  shows the checksum is load-bearing rather than decorative. The test asserts this.

---

## 3. Signature chunks (`ISGN` / `OSGN`)

A signature chunk is:

| Offset | Size | Meaning |
|---|---|---|
| `0x00` | 4 | number of elements |
| `0x04` | 4 | a word observed as `8` in `fxc` output **[not established: its meaning]** |
| `0x08` | `24 * count` | elements |
| after elements | ... | null-terminated semantic names |
| end | pad | zero or more `0xAB` bytes to a four-byte boundary **[tested here: fxc pads with 0xAB]** |

Each element is 24 bytes:

| Offset | Size | Meaning |
|---|---|---|
| `+0x00` | 4 | name offset, from the start of the chunk payload |
| `+0x04` | 4 | semantic index |
| `+0x08` | 4 | system-value number (0 = none; `POSITION` = 1, `IS_FRONT_FACE` = 9, ...) |
| `+0x0C` | 4 | component type (1 = uint32, 2 = sint32, 3 = float32) |
| `+0x10` | 4 | register number |
| `+0x14` | 1 | component mask |
| `+0x15` | 1 | read/write mask |
| `+0x16` | 2 | unused, zero |

**[tested here]** The field order and the input/output mask convention were read from `fxc` output: an input
element records the same bits in the read/write mask as in the mask, while an output element records zero.
The writer follows that convention.

The signature chunks are not where the program's `dcl_input` / `dcl_output` lines come from; those are tokens
in `SHEX`. The signature chunks give the runtime and the reflection API the semantic names and formats, and
`D3DDisassemble` prints them in its comment block. **[tested here]** A container with correct `ISGN`/`OSGN`
and `SHEX` disassembles with both the comment block and the `dcl_` lines.

---

## 4. The program chunk (`SHEX`)

`SHEX` payload:

| Offset | Size | Meaning |
|---|---|---|
| `+0x00` | 4 | version token |
| `+0x04` | 4 | length in DWORDs, **including these two words** |
| `+0x08` | ... | instruction and declaration tokens |

The version token is `(program_type << 16) | (5 << 4) | 1`. Program types: pixel 0, vertex 1, geometry 2,
hull 3, domain 4, compute 5. So a pixel program is `0x00000051`, a vertex program `0x00010051`, a compute
program `0x00050051`. **[tested here: D3DCompiler_47 emits these for `*_5_1`; `D3DDisassemble` accepts what
the writer emits]**

The part's declared size is `length * 4`, i.e. `(2 + instruction_words) * 4`. **[tested here]**

### 4.1 Instruction token

A standard instruction token has:

- bits `0..10`: opcode number
- bits `24..30`: instruction length in DWORDs, including the opcode token and every operand token
- bit 13: saturate
- bit 18: the conditional test (`0` = test zero, `1` = test non-zero)
- bits 19..22: per-component precise values
- bit 31: an extended opcode token follows

**[public: `d3d11TokenizedProgramFormat.hpp`]** The writer never emits extended opcode tokens: in shader model
5.1 the resource dimension and return type are declared once, so `sample` / `ld` do not repeat them (section
6.2). **[tested here]** Shader model 5.0 does repeat them; the writer does not produce 5.0.

### 4.2 Operand token

The common fields of an operand token are:

- bits `0..1`: number of components (`0` = none, `1` = one, `2` = four)
- bits `2..3`: selection mode for four-component operands (`0` = mask, `1` = swizzle, `2` = select one)
- bits `4..11`: the mask or swizzle or the single selected component
- bits `12..19`: operand type (register file)
- bits `20..21`: index dimension (number of index DWORDs)
- bits `22..30`: how each index dimension is represented (`0` = immediate 32-bit)
- bit 31: an extended operand token follows

**[public: `d3d11TokenizedProgramFormat.hpp`]**

The writer's operand forms:

| Operand | Type | Index dim | Selection | Extra DWORDs |
|---|---|---|---|---|
| temp / input / output source | 0 / 1 / 2 | 1 | swizzle or select-one | register |
| temp / input / output destination | 0 / 1 / 2 | 1 | mask | register |
| immediate 32-bit | 4 | 0 | value count | 1 or 4 values |
| constant buffer source | 8 | 3 | identity swizzle | logical id, register, element |
| resource source | 7 | 2 | identity swizzle | logical id, index in range |
| sampler source | 6 | 2 | none (0 components) | logical id, index in range |
| unordered-access destination | 30 | 2 | mask | logical id, index in range |
| thread id / group id / group-local id | 32 / 33 / 34 | 0 | swizzle or select-one | none |
| null | 13 | 0 | none | none |

**[tested here]** These forms were checked against tokens `D3DCompiler_47` produced for `*_5_1`. A few exact
values seen:

- `temp_dest(0)` -> `0x001000F2` then `0`
- `temp_source(0)` (identity swizzle) -> `0x00100E46` then `0`
- `temp_scalar(1, 0)` (`r1.x`) -> `0x0010000A` then `1`
- `constant_buffer_source(0, 2, 0)` -> `0x00308E46`, `0`, `2`, `0` (the three-index 5.1 form)
- `resource_source(0, 2)` -> `0x00207E46`, `0`, `2`
- `sampler_source(0, 0)` -> `0x00206000`, `0`, `0`
- `uav_dest(0)` -> `0x0021E0F2`, `0`, `0`
- `immediate4f(...)` -> `0x00004002` then four words; `immediate1f(...)` -> `0x00004001` then one word

### 4.3 Declarations

All declaration tokens use the same instruction token with an opcode from the table below. The column
"length" is the instruction-token length field in DWORDs.

| Declaration | Opcode | Length | Extra opcode-token bits | Following DWORDs |
|---|---|---|---|---|
| `dcl_globalFlags` | 106 (`0x6A`) | 1 | global flag bits 11..18 | none |
| `dcl_temps N` | 104 (`0x68`) | 2 | none | N |
| `dcl_input` | 95 (`0x5F`) | 3 | none | operand (input, mask, index dim 1) |
| `dcl_input` (thread id) | 95 (`0x5F`) | 2 | none | operand (type 32/33/34, mask, index dim 0) |
| `dcl_input_sgv` | 96 (`0x60`) | 4 | none | operand (input), system-value number |
| `dcl_input_siv` | 97 (`0x61`) | 4 | none | operand (input), system-value number |
| `dcl_input_ps` | 98 (`0x62`) | 3 | interpolation bits 11..14 | operand (input, mask) |
| `dcl_input_ps_siv` | 100 (`0x64`) | 4 | interpolation bits 11..14 | operand (input, mask), system-value number |
| `dcl_output` | 101 (`0x65`) | 3 | none | operand (output, mask) |
| `dcl_output_siv` | 103 (`0x67`) | 4 | none | operand (output, mask), system-value number |
| `dcl_constantbuffer` | 89 (`0x59`) | 7 | bit 11: dynamic indexing | declaration operand (3 indices), size in 16-byte vectors, register space |
| `dcl_sampler` | 90 (`0x5A`) | 6 | sampler mode bits 11..14 | declaration operand (3 indices), register space |
| `dcl_resource_texture*` | 88 (`0x58`) | 7 | resource dimension bits 11..15 | declaration operand (3 indices), return-type word, register space |
| `dcl_resource_raw` | 161 (`0xA1`) | 6 | none | declaration operand (3 indices), register space |
| `dcl_resource_structured` | 162 (`0xA2`) | 7 | none | declaration operand (3 indices), byte stride, register space |
| `dcl_uav_typed_texture*` | 156 (`0x9C`) | 7 | dimension bits 11..15; bit 16: globally coherent | declaration operand (3 indices), return-type word, register space |
| `dcl_uav_raw` | 157 (`0x9D`) | 6 | bit 16: globally coherent | declaration operand (3 indices), register space |
| `dcl_uav_structured` | 158 (`0x9E`) | 7 | bit 16: globally coherent | declaration operand (3 indices), byte stride, register space |
| `dcl_thread_group` | 155 (`0x9B`) | 4 | none | x, y, z |

**[public: `d3d11TokenizedProgramFormat.hpp` for the opcode numbers and bit fields; tested here for the
shader-model-5.1 shape]**

The *declaration operand* for a resource, sampler, unordered-access view or constant buffer has index
dimension 3 and a four-component identity swizzle. Its three indices are:

1. the logical id (the 0-based order of declaration; the disassembler prints it as `T0`, `S0`, `CB0`, `U0`),
2. the lower bound of the register range,
3. the upper bound of the register range.

A trailing DWORD gives the register space. A constant buffer declaration also carries the buffer size in
16-byte vectors before the space. **[tested here]** For example, a texture declared at register 1 in space 2
with a range of one register is the operand `0x00307E46` with indices `0, 1, 1`, then the return-type word,
then `2`; `D3DDisassemble` prints `dcl_resource_texture2d (float,float,float,float) T0[1:1], space=2`.

The return-type word packs four 4-bit component types (`1` = UNORM, `2` = SNORM, `3` = SINT, `4` = UINT,
`5` = FLOAT). The writer uses one type for all four components; a mixed return type is not written (section
8).

The global flag bits are: 11 refactoring allowed, 12 double precision, 13 force early depth/stencil,
14 raw and structured buffers, 15 skip optimization, 16 minimum precision, 17 double extensions, 18 shader
extensions. **[public: `d3d11TokenizedProgramFormat.hpp`]**

---

## 5. Instructions

The writer exposes these opcodes as an enumeration (never a raw number): mov, movc, add, mul, mad, min, max,
dp2, dp3, dp4, rsq, sqrt, div, exp, log, frc, round_ne, round_ni, round_pi, round_z, ftoi, ftou, itof, utof,
iadd, imad, imul, imin, imax, ineg, and, or, xor, not, ishl, ishr, ushr, lt, ge, eq, ne, ieq, ine, ilt, ige,
ult, uge, udiv, umul, umad, umin, umax, if, else, endif, loop, endloop, break, breakc, continue, ret, discard,
sample, sample_l, sample_b, ld, ld_raw, ld_structured, store_uav_typed, store_raw, store_structured,
ld_uav_typed. It also declares the enumerations for the declaration opcodes above. An instruction that is not
in the enumeration cannot be written.

The operand order for each instruction is the order the format expects. The writer does not check that an
opcode accepts a particular operand list; that is the caller's responsibility (section 8).

Representative forms, as `D3DDisassemble` prints them **[tested here]**:

| Instruction | Operands written |
|---|---|
| `mov d, a` | destination, source |
| `movc d, c, a, b` | destination, condition, true value, false value |
| `add d, a, b` | destination, source, source |
| `dp4 d, a, b` | destination, source, source |
| `mad d, a, b, c` | destination, three sources |
| `imul hi, lo, a, b` | high destination (often `null`), low destination, two sources |
| `if_nz a` | one source; test bit set to non-zero |
| `else`, `endif`, `loop`, `endloop`, `break`, `continue`, `ret` | no operands |
| `breakc_nz a` | one source; test bit set to non-zero |
| `discard_nz a` | one source; test bit set to non-zero |
| `sample d, addr, t, s` | destination, address, resource, sampler |
| `sample_l d, addr, t, s, lod` | destination, address, resource, sampler, level |
| `sample_b d, addr, t, s, bias` | destination, address, resource, sampler, bias |
| `ld d, addr, t` | destination, address, resource |
| `ld_raw d, addr, t` | destination, address, resource |
| `ld_structured d, addr, offset, t` | destination, address, immediate byte offset, resource |
| `store_uav_typed u, addr, value` | unordered-access destination, address, value |
| `store_raw u, addr, value` | unordered-access destination, address, value |
| `store_structured u, addr, offset, value` | unordered-access destination, address, immediate byte offset, value |

**[tested here]** `imul` (and `umul`) take two destinations, the high and low 32 bits of the product.
`D3DCompiler_47` writes `imul null, lo, a, b` for a plain 32-bit integer multiply; the high destination is
the `null` operand (type 13, encoded as `0x0000D000`). The test writes it that way.

**[tested here]** The shader-model-5.1 `sample` / `ld` do not carry the two extended opcode tokens (resource
dimension, resource return type) that shader model 5.0 carries; `D3DDisassemble` prints `sample` in 5.1 and
`sample_indexable(...)` in 5.0. Since this writer produces 5.1, the disassembly is `sample`, not
`sample_indexable`.

---

## 6. What the test proves

`shell/tests/test_shader_dxbc.cpp` builds six containers and hands each to the system `D3DDisassemble`
(loaded by full path from `C:\Windows\System32\D3DCompiler_47.dll` with `LoadLibraryW` and `GetProcAddress`).
It requires the call to succeed and requires every expected declaration and instruction line to appear whole
in the disassembly (leading indentation inside `if`/`loop` blocks is ignored). If the library or the function
is missing the test prints that and fails; it does not pass silently.

The six shaders are:

1. **Pixel, texture sample.** Declares a constant buffer, a sampler, a `Texture2D`, a linear-interpolated
   input and an output; samples the texture and multiplies by `CB0[0][1]`.
   Checks `dcl_temps 2`, `sample r0.xyzw, v0.xyxx, T0[0].xyzw, S0[0]`,
   `mul r0.xyzw, r0.xyzw, CB0[0][1].xyzw`.
2. **Vertex, position transform.** Declares a four-vector constant buffer, an input and a `SV_Position`
   output; transforms with four `dp4`s. Checks the `dcl_output_siv o0.xyzw, position` line and the `dp4` lines.
3. **Compute, texture in / unordered-access out.** Reads with `ld`, writes with `store_uav_typed`, declares a
   thread group of 8x8x1. Checks `dcl_thread_group 8, 8, 1`, `ld ...`, `store_uav_typed ...`.
4. **Control flow.** `if`/`else`/`endif` and a `loop` containing an `iadd`, an `ige` and a `breakc`. Checks
   `if_nz`, `else`, `endif`, `loop`, `breakc_nz`, `endloop`.
5. **Register spaces other than zero.** A resource in space 2, a sampler in space 3 and a constant buffer in
   space 4, each with a register range. Checks `... space=2`, `... space=3`, `... space=4`, `T0[1]`, `S0[1]`
   and `CB0[2][0]`.
6. **Raw and structured buffers.** `ld_raw` / `store_raw` and `ld_structured` / `store_structured` on raw and
   structured resources with register spaces. Checks the four instruction lines and the declarations.

It also checks:

- **The checksum is load-bearing.** A container with one altered checksum byte is rejected by
  `D3DDisassemble` (`hr != 0`).
- **The checksum matches Microsoft's.** When `D3DCompile` is present in the same library, the test compiles a
  shader it writes itself and checks that `compute_dxbc_checksum` reproduces the 16 bytes Microsoft stored.

A failing line prints the whole disassembly, so a wrong expectation is visible.

---

## 7. How the writer is built

`DxbcBuilder` collects instruction words and signature records. `build()`:

1. Builds the `ISGN` and `OSGN` payloads.
2. Builds the `SHEX` payload (version, length, instruction words).
3. Lays out the 32-byte header, the three part offsets and the three parts, four-byte aligned.
4. Writes the total size and the part count.
5. Computes the checksum over offset `0x14` to the end and writes it at `0x04`.

Nothing is written to disk, and no shader state is kept between calls.

---

## 8. What is not covered

- **No `RDEF`, `STAT`, `PRIV` or debug chunks.** The container disassembles, but it is not a complete
  reflection blob: `D3DReflect` cannot recover resource names, types or bindings from it, and the runtime may
  require `RDEF` for some operations. The task did not require `RDEF`, and none is written.
- **Shader model 5.1 only.** The shader-model-5.0 instruction form (in particular the extended resource
  dimension and return type tokens before `sample` / `ld`) is not written. Shader model 4 is not written.
- **One return type for all four components.** A mixed resource return type is not written.
- **No extended opcode tokens.** Sample controls (explicit sample offsets), resource sample counts for
  multisampled resources, and the other extended forms are not written.
- **No comparison-sampler instructions** (`sample_c`, `sample_c_lz`), no gather, no `resinfo`, no `bufinfo`
  (the `bufinfo` opcode is present in the enumeration but no dedicated test covers it).
- **No input/output register arrays** (`dcl_index_range`), no indexable temps, no geometry/hull/domain
  program declarations, no interfaces or dynamic linking, no thread-group shared memory.
- **No unordered-access counter flag** and no explicit globally-coherent flag is exercised (the API can set
  it; the tests do not).
- **No operand/opcode validation.** The builder lays out the operands it is given; it does not verify that a
  particular opcode accepts them. A wrong operand list produces a container the disassembler may reject or
  decode differently; it is not caught in the builder.
- **Proof is disassembly, not execution.** `D3DDisassemble` accepting a container proves the bytes parse as
  the intended program. It does not prove the PC driver accepts the container for pipeline creation, because
  that needs a device and a GPU run, which this work did not do.

## 9. Added for the pass that writes a PC shader for a console program

- **An instruction from a stored list of operands** (`emit(opcode, flags, operands, count)`), beside the form
  that takes a braced list.
- **More opcodes in the enumeration:** nop, rcp, f32tof16, f16tof32, ubfe, ibfe, bfi, bfrev, countbits,
  firstbit_hi, firstbit_lo, firstbit_shi, uaddc, usubb, swapc, sincos, deriv_rtx, deriv_rty, resinfo, lod,
  gather4, ld_ms, sample_c, sample_c_lz, sample_d. The numbers were read in the opcode list of the PC graphics
  library's own shader reader (dxbc-spirv, `dxbc_types.h`). Checked by the system disassembler in the test:
  nop, rcp, f32tof16, f16tof32, ubfe and `discard_z`. The others are numbers only so far: no test writes them.
- **One component of a constant buffer element** as a source (`constant_buffer_scalar`): the operand's
  selection mode is "one component" instead of the swizzle. Disassembles as `CB0[500][0].y`.
- **A sampler or a resource taken from its declared range at run time** (`sampler_source_indexed`,
  `resource_source_indexed`): the second index is given as "a number plus the value of a register component" -
  the three bits at 22 + 3n of the operand token say how index n is given, 0 for a number and 3 for a number
  plus a relative operand, which follows the number (read in the same reader, `dxbc_parser.h`). With a sampler
  declared over a range without an upper end this disassembles as `dcl_sampler S0[0:*], mode_default,
  space=22344` and `S0[r1.x + 0]`. Checked for the sampler; the resource form is written the same way and not
  checked.
- Still true of all of it: the proof is disassembly, not a graphics device.
