# The shader path: how the game's shader bytes reach the device

This file records what the original executable's own code does between "the renderer wants a pipeline" and "the
device is called with shader bytes". It was read from the image's code with the private map (function bodies, call
sites, data references); nothing here is a disassembly listing. RVAs are image-relative (image base 0x140000000).

Marks: **V** = read in the listing, **I** = inferred from what was read, **U** = not established.

The purpose is to say what our `d3d12_xs` wrapper will actually be handed, and what header fields the game itself
has already validated by then, so the shader translator reads the same container the game reads.

---

## 1. The pipeline-creation calls on the device

Every call below was found by scanning all functions in the image for `call qword ptr [reg + imm]` and keeping
only the functions that also load the device pointer from one of its globals (0x8759d00, 0x75df048, and the
copies at 0x75deff8..0x75df048 that the device-creation function fills with the same pointer). **Exactly one call
site exists in the whole image for each of the five methods below** (V, by that method).

| slot | call site | shape as the game calls it | description that is passed |
|---|---|---|---|
| 55 | 0x247a5c2 in fn 0x2479f40 | (device, desc, riid, out) | a large stack block (0xab8 bytes of frame); the fields were not read (U) |
| 57 | 0x2477578 in fn 0x24774c0 | (device, desc, riid, out) | desc = request object + 0x40; the first qword (a size, I) is written immediately before the call |
| 62 | 0x247fe66 in fn 0x247fd30 | (device, desc, **0, 0**, riid, out) | a 0x40-byte block: +0x00 root signature, +0x08 shader pointer, +0x10 shader length, +0x18 = 1, rest zero |
| 67 | 0x24800d5 in fn 0x2480050 | (device, desc, riid, out) | desc = cache node + 0x18; the root signature pointer is stored into its first qword just before the call |
| 101 | 0x24813ef in fn 0x24801b0 | (device, desc, riid, out) | `{UINT kind = 3; UINT count; const Entry*}` with `Entry = {UINT type; UINT unused; const void* payload}`, 16 bytes each |

Notes that matter for the wrapper:

- **Slot 62 has two extra arguments in the middle** (V): the call is
  `(device, desc, 0, 0, riid, out)` — the identifier and the result are the 5th and 6th arguments, on the stack,
  not the 3rd and 4th as on the PC's `CreateComputePipelineState`. A handler that assumes the public order reads
  the wrong registers.
- **Slot 62's description has the shape of `D3D12_COMPUTE_PIPELINE_STATE_DESC`** (V for the offsets, I for the
  identification): +0x00 `pRootSignature`, +0x08/+0x10 `CS = {pShaderBytecode, Length}`, +0x18 `NodeMask = 1`,
  +0x20 `CachedPSO` = null, +0x30 `Flags` = 0. The block is zeroed before the shader pair is written into it.
- Slot 67 is tried **before** slot 62 and its result is used instead of slot 62 when it is non-zero (V). It is a
  cache lookup: the key is the pipeline request's own +0x18 field, the table lives behind global 0x75ddb90, and
  the stored value is a reference-counted object (a hit is AddRef'ed through vtable slot 1, V).
- Slot 101's object is the **same device**: the device-creation function 0x2478440 writes the device pointer to
  global 0x8759d00 and to the copies at 0x75deff8..0x75df048 including 0x75df018 (V). The console device therefore
  has a *third* pipeline entry point above the public range.
- Slots 55 and 57 are reached from fn 0x2479760 (the function that also builds a graphics state stream), so they
  belong to the graphics side (I). Their descriptions were not read field by field (U).

### 1.1 Slot 101: the pointer-array pipeline description (V for the layout)

fn 0x24801b0 builds the description on the stack:

```
desc:  +0x00 UINT kind = 3
       +0x04 UINT entry count
       +0x08 const Entry* entries
Entry: +0x00 UINT type
       +0x04 (never written)
       +0x08 const void* payload
```

Entry type numbers written, in the order the function fills them: **5** (the shader entry), then **0xB** repeated
for each of N items, then **2, 7, 2, 7, 9, 7, 0xA, 1**. From 2 onwards these agree with the public
`D3D12_PIPELINE_STATE_SUBOBJECT_TYPE` numbering (1 = vertex shader, 2 = pixel shader, 7 = stream output,
9 = rasterizer, 10 = depth-stencil, 11 = input layout) (V for the numbers, I for the agreement). Type **5** is
where the shader bytes sit, and its payload is not a `D3D12_SHADER_BYTECODE` but a four-field block:

```
+0x00 const void* code            <- the shader container (see section 2)
+0x08 size_t      code_size
+0x10 UINT        name_count
+0x18 const wchar_t* names[]      (each entry is 0x18 bytes, only the name pointer is filled)
```

I read this as a **shader library**: one code blob plus a list of exported entry names (I). After the pipeline is
created, the game queries the returned object for a second identifier (0x61e06e8) and resolves each name through
that object (a call at vtable slot 3 per name) into a table it keeps (V). That matches the open question in
`docs/SHADER-TRANSLATOR.md` §11 about multiple entry points: at least this path carries a name list.

---

## 2. Where the shader bytes come from: one function

Every shader the game hands to the device comes out of **fn 0x86f8e0**, and that function has five callers in the
whole image (V): 0xf9f650 and 0x247fd30 (compute, slot 62), 0x24801b0 (graphics, slot 101), 0x2476e20 (root
signature only) and 0x86f090 (a second graphics stream builder that asks for five shader pairs in a row). Its
contract (V):

- arguments: `out_pair` (a `{const void* ptr; size_t size;}` to fill), a *shader record*, a second output pointer,
  and an **index byte**;
- the record holds a pointer table at +0x08 indexed by that byte (`ptr = record[index]`), the byte length for
  index 0 at +0x20 and the byte length for the other indexes at +0x2c;
- **that index is 0 in every call site in the image** (V). A stage is selected by passing a *different record*:
  fn 0x86f090 calls the function five times, each with its own record, and writes the five results into a table of
  `{ptr, size}` pairs 0x18 bytes apart (V). So the shell cannot read the shader's stage out of this call — it comes
  from which record was passed, which is the renderer's business (U which record is which stage);
- if that index's pointer is already set, it is returned as it is;
- otherwise the bytes are **copied out of GPU-visible memory**: the record's +0x38 object gets two vtable calls
  (slot 16, +0x80) to prepare, then slot 10 (+0x50) with the length, into a buffer the function allocates. That
  call must return exactly the length; if it does not, the game executes `int3` (0x86f9c5, V). So a wrong answer
  from our layer here is an immediate stop, not a silent short read;
- then the **outer container check** (V): the word at +0 of the copied bytes must be 0xDCE7. If it is, the real
  size is the dword at +0x08, a second buffer of that size is allocated and fn 0x51189f0 unpacks the blob into
  it; the unpacked pointer and the dword at +0x08 become the `{ptr, size}` the caller receives. If the word is
  **not** 0xDCE7 the copied bytes are used as they are (V) — that branch is the "already unpacked" case.

The pointer that reaches the device is therefore the **unpacked** blob, and its first dword is what the root
signature step checks: `DXBC` (section 4).

---

## 3. The outer container (the game's own packing)

Header read by fn 0x51189f0 and fn 0x5118a30 (V):

| offset | size | meaning |
|---|---|---|
| +0x00 | 2 | magic word **0xDCE7** (little-endian, so the bytes are E7 DC) |
| +0x02 | 1 | method: **3** = stored, **0x0D** = compressed; nothing else is handled |
| +0x03 | 1 | not read by these functions (U) |
| +0x04 | 4 | container size in bytes, header included; the payload length is this value minus 0x0C |
| +0x08 | 4 | unpacked size: the size the game allocates and later hands on as the shader length |
| +0x0C | … | payload: the stored bytes (method 3) or the compressed stream (method 0x0D) |

- method 3: `memcpy(destination, blob + 0x0C, size_at_+0x08)` (V);
- method 0x0D: a decompressor at 0x25d6a0, given the payload and a 0x2D30-byte scratch buffer that fn 0x5118a20
  returns for exactly this method (V);
- any other method value: neither branch runs, and the caller still uses the fresh buffer (I). That case is a
  hazard, not a path we should reproduce.

Nothing in the game reads a checksum, a per-chunk table or a version number in this header. It is a two-word
header and a payload.

## 4. The inner container: `DXBC` (the game checks it)

The unpacked blob is checked by **fn 0xfa09f0**, whose only job is to obtain the pipeline's root signature (V):

- dword at +0x00 must be **`DXBC`** (0x43425844), otherwise the function returns `E_NOINTERFACE` (0x80004002) and
  the caller treats the pipeline as failed;
- dword at **+0x1C** = chunk count, must be non-zero;
- the chunk offsets are the dwords at **+0x20 + 4·i** for i = 0 … count−1;
- at each offset: `{char fourcc[4]; UINT size; payload[size]}`;
- it looks for the fourcc **`PRIV`**; its payload must begin with the two bytes **0xDC, 0xE2**;
- the dword at PRIV payload **+3** is used as the key of an in-process map of root signatures (V);
- the root signature description starts at PRIV payload **+2** and runs to the end of the chunk; it is converted
  to a D3D12 versioned description by fn 0x55ac630 and serialised through the imported
  `D3D12SerializeVersionedRootSignature`, then created through device slot 16 (V). The description's own leading
  bytes (read by 0x55ac630, V) are: byte at +0 = a count of 0x20-byte entries, word at +1 = a count, byte at +5 =
  a count of 0x34-byte entries, entries from +6.

So the container format as the game sees it is the public DXBC chunk layout, and the game reads exactly three
things in it: the magic, the chunk count and the chunk offsets, and then one chunk (`PRIV`). The **total size at
+0x18, the 16 bytes at +0x04 and every other chunk are never read by the game**: a scan of every instruction in
the image that compares an immediate with `DXBC` or `PRIV` finds only fn 0xfa09f0 (V for the scan and for that
function; the 0x18/0x04 fields are named after the public container layout, I).

Consequence (U, and it must stay open): **the fourcc of the chunk that holds the console instruction stream is not
established by the game's code.** The image never looks at it — the console driver does. Any fourcc we wrote down
for it would be a guess. The instruction bytes are in *some* chunk of that container, but which one is for the
shell's translator to determine from a logged container header, not to assume now.

What the game passes on as the shader length is the dword at the outer container's +0x08 (V), i.e. the length of
this whole DXBC container, not the length of any chunk.

---

## 5. How the shader pair reaches each description

- **Compute (slot 62)**: fn 0x247fd30 calls fn 0x86f8e0 for stage 0 into the two halves of the zeroed 0x40-byte
  block (+0x08 pointer, +0x10 length), puts the root signature at +0 and 1 at +0x18, and calls (V). The pipeline
  object is stored at request+0x120; on failure the root signature at request+0x118 is released (V).
- **Graphics (slot 101)**: fn 0x24801b0 calls fn 0x86f8e0 for stage 0, copies the pair into the four-field shader
  block (section 1.1) and puts that block in entry type 5 (V).
- **Slots 55/57/67**: the shader pair is not visible in their own call sites; it is in the description the caller
  built (U). Slot 67's first desc qword is the root signature; the rest of that node is written elsewhere (U).

## 6. How many, and which comes first

- One compute pipeline call site (slot 62), one graphics pipeline call site (slot 101), plus slot 55/57/67 whose
  descriptions I could not read far enough to classify (U). All of them are single call sites (V, method above);
  the counts are of *sites*, and a site runs many times.
- **Both pipeline creations are jobs** (V): fn 0xf9f650 and fn 0x2480100 allocate a job object (fn 0xf9fbd0) whose
  entry is 0x247fd00 → fn 0x247fd30 (compute) or 0x2480180 → fn 0x24801b0 (graphics), and submit it (fn 0xf9fbc0).
  Job objects are how this engine runs work on its own worker threads (I).
- During device start-up (fn 0x19f4df0, the chain that also calls `D3D12XboxCreateDevice`) the only pipeline work
  reached is fn 0x1d65750 → fn 0x2476e20, which creates **root signatures** and fills the pipeline cache; it does
  not call a pipeline method itself (V).
- So the first pipeline state the shell is likely to see is the **compute** one from fn 0x247fd30 / slot 62, with
  a `D3D12_COMPUTE_PIPELINE_STATE_DESC`-shaped description whose CS pair is a real shader container (I). The first
  thing the translator would have to read is therefore a compute container, not a graphics one.

## 7. What a run has to log to settle the rest (in memory only)

These are the questions this reading could not close, each with the smallest measurement that closes it. All of it
is logging of numbers and header fields in memory during a run — nothing is written to a file, per rule 2 and 5.

1. **Which of the five methods fires first, and with which arguments.** Log, for each of slots 55/57/62/67/101, the
   argument values and the first 64 bytes of the description as words. Settles: the desc layouts of 55/57/67, and
   whether 62 really is tried after 67.
2. **Which thread makes the first pipeline call.** Log `GetCurrentThreadId()` at the first pipeline call. Settles
   whether the game builds pipelines off-thread (rule 7 is about what *we* do, but if the caller is already a
   worker thread the shell must not move the work again, and ExCorvus must decide the answer).
3. **What the pointer handed over really points at.** Log the first 16 bytes at the shader pointer and the length,
   for the first pipeline. Expectation from this file: `DXBC` first (the unpacked form), and 0xDCE7 only if the
   outer branch was skipped. Settles: whether our wrapper sees the packed or the unpacked container.
4. **The chunk table of that container.** Log the dword at +0x1C and the chunk offsets at +0x20 (+ fourcc and size
   of each chunk). Settles: which fourcc holds the instruction stream, and therefore what the translator's
   "instruction stream at offset X, length Y" answer can be. That is the one fact this document cannot supply.
5. **Which record belongs to which stage** for graphics pipelines. The index passed to fn 0x86f8e0 is 0 in every
   call site, so the stage is not in that argument (section 2). Logging the *caller* function and the record
   pointer for each of the five calls fn 0x86f090 makes settles the order; until then the shell must not infer a
   stage from the call itself.

## 8. What is in `shell/shader_container.*` because of this file

`shader_container.hpp/.cpp` is a reader for the two headers above and nothing more. It reports:

- which container it recognised (`DXBC`, the game's 0xDCE7 packing, or neither);
- the declared sizes each header carries, and (for `DXBC`) the chunk table with each chunk's fourcc, offset and
  length;
- whether the `PRIV` chunk exists and its payload begins with 0xDC 0xE2 (the two bytes the game requires);
- and explicitly **no instruction-stream offset**: this file does not establish one, so the reader never names one
  and says why.

It refuses, with a reason, on: a header shorter than its own declared fields, a declared container size larger
than the bytes it was given, a chunk count that does not fit, a chunk offset inside the header, a chunk offset
outside the buffer, and a chunk whose declared size runs past the end. Tests build these headers by hand; no game
bytes are used anywhere.

## 9. Measured in runs (boots b81-b85): what the requests really carry

The graphics library now reads every pipeline request in memory, counts it, and logs numbers about the first few
(sizes, part names, thread, return address; no shader bytes are written anywhere). This closes section 7 and
corrects two expectations above.

- **Which methods fire, and from where.** Slots 57, 62, 67 and 86 are all called; the measured call sites are
  slot 62 at 0x247fe66 (fn 0x247fd30), slot 67 at 0x24800d5 (fn 0x2480050), slot 57 at 0x2477578 (fn 0x24774c0,
  run as a job) and slot 86 at 0x2479a87 (fn 0x2479760).
- **Slot 62** has the public compute pipeline description: root signature, shader pointer, shader length, and
  the number 1; two more arguments that are zero; then the identifier and the place for the result. It is tried
  after slot 67 answered failure for the same shader.
- **Slot 67** is a lookup of a precompiled compute pipeline: the game finds a node by hash in its own table,
  stores the root signature in it and passes the node. Its words are: root signature, then four (pointer, size)
  pairs with sizes 0x1c, 0x14, a multiple of 0x40 (384 bytes up to about 129,000 bytes seen), and one more.
- **Slot 57** is the same for graphics: root signature, then (pointer, size) pairs of about 0x184 to 0x18c bytes,
  0x78 to 0xe0 bytes, and two blocks whose sizes are multiples of 0x40.
- **Slot 86** is the public pipeline state stream ({size, pointer}; each element a type number and its data): the
  three requests made on the thread that created the device carry element 0 (root signature), 1 (vertex shader)
  and 2 (pixel shader), with shaders stored inside the executable itself.
- **Slot 101** is the public state-object description of a ray-tracing pipeline (kind 3; entry types 5, 0xB, 2, 7,
  9, 0xA, 1 = library, hit group, local root signature, association, shader configuration, pipeline
  configuration, global root signature). It is read and refused: ray tracing is not provided.
- **The container.** Every container named by a request is the `DXBC` container with the same seven parts:
  SFI0, ISG1, OSG1, PSV0, HASH, DXIL, PRIV. The PRIV part begins as the game requires. The DXIL part's program
  header names the stage (vertex, pixel, compute) and shader model 6.6.
- **The DXIL part holds no portable code.** Its header gives a code size of four bytes, and those four bytes are
  the letters "NOBC". Between the header and those letters lie a few hundred bytes (about 450 to 800 in the
  shaders looked at) that were not understood at this point: they are a directory and a zlib-packed block
  that holds the console's program (section 10). The PC device cannot build a pipeline from these containers,
  and the shell does not hand them over.
- **Where the hardware code is.** In the slot 67 description the block of the third pair, and in the slot 57
  description two blocks, each contain exactly one end-of-program word of the hardware instruction set
  (0xBF810000), some way before the block's end. Those blocks are the precompiled programs. The other blocks of
  a description have no such word; what they hold (per-stage data) is not understood.
- **Established in boots b86-b87** with a complete reader of instruction lengths (`shell/shader_walk.*`): a
  program starts at its block's first byte. Of the first 1,024 distinct program blocks of a run, a walk from
  the first word lands exactly on the end-of-program word in 997. After the end word a block holds filler
  words (the instruction set's "code end" word, about 50 per program), some zero words and, in some
  programs, other words (about 1,400 in all; probably constants the program reads, not checked).
- **Not established:** which block of a slot 57 description belongs to which stage; what the 27 programs
  hold that the walk cannot read (see `docs/SHADER-TRANSLATOR.md` section 13).
- **Threads.** Three requests come from the thread that created the device. Every other request comes from the
  game's worker threads, tens of thousands within the first twenty seconds. The shell serves pipeline requests on
  the device's own thread only (rule 7); what to do with requests from the game's own worker threads is ExCorvus's
  decision.

Section 6's expectation that the first pipeline's compute container "is a real shader container" the translator
could read was first judged wrong here ("the container is real, but the instructions are not in it"). That
judgement was itself wrong: the instructions are in the container, packed. See section 10.

## 10. Correction (boots b89-b96; measured): the container carries the console's program, packed

**The directory.** Inside the 'DXIL' part, right after the public 24-byte program header:

| at | what |
|---|---|
| +0x18 | the bytes 'XDAT' |
| +0x1c | a word, 3 in every container seen (meaning not established) |
| +0x20 | size of the entries in bytes (36 in every container seen) |
| +0x24 | entries of 12 bytes: kind, offset from the start of the part, size |

Every container of a run has the same three kinds, in this order: 0x12 (16 bytes), 0x0D (the packed block),
0x07 (28 bytes, the last four being the letters "NOBC" that the public header names as its code).

**The packed block (kind 0x0D).** The bytes 'XBCH', a word (3 in every container seen), the packed size, 0,
the unpacked size, 0, then one zlib stream. `shell/shader_program.*` (`read_packed_program`) reads the
directory and unpacks the block in memory; a field that does not hold up is refused with a reason.

**The unpacked block is a module.** Measured over all 581 distinct containers of boot b96:

| at | what |
|---|---|
| +0x00 | the bytes 'FSSM' |
| +0x04 | 0x101 in all 581 |
| +0x08 | the block's size minus 16 in all 581 |
| +0x10 | 6 in all 581 |
| +0x18 .. +0x70, eight bytes apart | a padding value, except one place that holds a number: +0x38 in vertex shader modules (6 of 6), +0x40 in pixel shader modules (567 of 567), +0x70 in compute shader modules (8 of 8) |

The number plus 0x10 is the place of a record. The record's first word plus 0x10 is where the code starts;
its third word is the code's size in bytes. The code is the program up to its end-of-program word, then
filler words (0xBF9F0000) to the code's end. In all 581: a walk from the code's first word lands exactly on
the block's one end-of-program word, only filler follows inside the code, and the instruction reader decodes
the program. `find_module_program` does this and refuses any block laid out differently.

**Not established:** the record's other words; the tables before the code (in pixel shader modules also seven
slots of 0x194 bytes with short instruction sequences that end in export instructions); what follows the
code (a table of numbers, then the shader's name as text); entry kinds 0x12 and 0x07; the two words that are
3.

**The module's program and the precompiled program are not the same words** (0 of 581 matched a program
handed over through device slots 57 or 67 in the same run). Why is not established.

**What this changes.** When the shell refuses slots 57 and 67, the game asks again through slots 62 and 86
with public pipeline descriptions and these containers. So the shell does not need the precompiled
descriptions: the container has the program, and its public parts (ISG1, OSG1, PSV0) say what the shader
takes in, puts out and binds.

**A warning for the next reader.** A walk that lands on the end word does not prove where a program starts:
zero words, and the module's first words, read as one-word instructions, so a walk started there lands too.
Two such starts were taken for real before the header was read.

## 11. The two tables after a module's code (boots b97-b99; measured over 581 modules)

The record that names the code (section 10) also names two tables and a name. Offsets are counted from +0x10
of the block, as in the rest of the module.

| in the record | what |
|---|---|
| +0x28 | offset of the first table |
| +0x30 | offset of the second table; the padding value 0xDEADBEEF when the shader declares no resources |
| +0x38 | number of entries in the first table |
| +0x3c | number of entries in the second table |
| +0x60 | offset of the name record |
| +0x6c | size of the name record: 20 bytes plus the name's length |

Entries of both tables are five 32-bit words. The first table ends where the second starts; the second ends
where the name record starts. The name record is the words 0, 0, 0xFFFFFFFF, 0, (length << 16), then the name.

**First table: how the pipeline's root parameters reach the program.** Words: kind, offset, count, register
number, last.

| kind | what | the words |
|---|---|---|
| 3 | a resource descriptor in a table | `last` = the scalar register that holds the table's address; `offset` in 32-bit words from it (8 words to a 32-byte descriptor) |
| 6 | a sampler descriptor in a table | the same |
| 1 | a buffer whose address arrives in scalar registers | `last` = the first of them |
| 0 | constants that arrive in scalar registers | low byte of `last` = the first of them; bit 0x100 is set (not understood) |
| 4 | a sampler fixed by the pipeline | second byte of `last` = its index |
| 5 | not understood | 10 pixel modules; `last` = a scalar register through which those programs load descriptors |

The count word is the number of scalar registers the parameter takes. Registers are given out in order, from
s8 for vertex and pixel programs and from s4 for compute programs.

**Second table: the resources this shader declares.**

| word | what |
|---|---|
| 0 | low byte: class (0 read-only resource, 1 sampler, 2 constant buffer, 3 writable resource); second byte: how it arrives (the first table's kind) |
| 1 | offset in words inside the table |
| 2 | the register number the shader's source gave it |
| 3 | 4 for textures, 1 for a raw buffer, 0 otherwise (not understood) |
| 4 | low byte: the descriptor's size in words (8 for an image, 4 for a buffer or sampler); second byte: the scalar register; third byte: the fixed sampler's index |

The register space is in neither table; the container's 'PSV0' part has it.

**How this was checked.** For every module of a run the shell compares the tables with each other, with the
resources the container's 'PSV0' part declares, and with what the program does (which register it loads each
descriptor through, and at which offset). The numbers are in FINDINGS.md under the same date. No byte of a
table is logged.
