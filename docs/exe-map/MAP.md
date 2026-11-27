# MAP of game_scarlett_master.exe (retail 1.0.2.1) — our own reading

Every statement is marked: **V** = verified (read in the listing named), **I** = inferred from it,
**U** = unknown. RVAs are image-relative (image base 0x140000000).

The file names in backticks below (`funcs\<rva>.txt`, `index-summary.txt`, `xmem-by-function.txt`, `refs-*.txt`)
and the tool `exe_map.py` are private working files: they contain disassembly of the game, so they are not in this
repository and never will be. They are named only to say where each statement was read.

## 0. The image (V, `index-summary.txt`)
- 120,181,760 bytes on disk. `.text` 0x1000..0x5f40966, `.rdata` 0x5f41000, `.data` 0x67e2000 (virtual 0x43ec398, only
  0x622e00 on disk: the rest is zero-filled at load, so globals there are set at run time), `.pdata` 0xabcf000.
- 299,257 functions in the exception table, 92.35 MB of code, median function 117 bytes, largest 129,632 bytes.
- 587 imported functions, 825,108 direct call references between functions.
- Entry point RVA 0x56a1ec8.
- xmem.dll: 12 imports, 57 call sites in 21 functions (`xmem-by-function.txt`).

## 1. Memory: the two 24 GiB views and the page pool

### 1.1 fn 0x1e1cf60 — memory start-up (1,677 bytes; one caller, fn 0x5c356e0; runs once, guard byte 0x71bf9f8)
- **V** Asks fn 0x55deb88 for a value 0..8 and accepts only 1, 6, 7, 8 (mask 0x1c2); anything else hits `int3`.
  **U** what the value is (a console/device type is the likely reading).
- **V** View size = `((fn 0x75140() ^ 1) | 2) << 33`: 24 GiB when fn 0x75140 returns 0, 16 GiB when it returns 1.
  Our boots log 24,576 MiB, so it returned 0 here. **U** what fn 0x75140 tests.
- **V** Calls `D3D12ConfigureVirtualMemoryX` (d3d12_xs.dll import) with a zeroed 32-byte block whose dword at +4 is
  `fn 0x4da2350(ceil(size / 16 GiB)) + 2`; a failure hits `int3`. **U** the meaning of that field.
- **V** `XMemVirtualAlloc(0, size, 0x20402000, attributes 0x21, protect 0x4)` -> global 0x71bf970. **I** the CPU view.
- **V** `XMemVirtualAlloc(0, size, 0x20402000, attributes 0x3, protect 0x801)` -> global 0x71bf978. **I** the GPU-only
  view (0x801 = no CPU access + graphics no-access until pages are mapped). Size kept in global 0x71bf980.
  Either call returning 0 hits `int3`.
- **V** Then a loop over 9 sizes (table at 0x71bf9b0), each a committed pool:
  `XMemVirtualAlloc(0, size, 0x20003000, attributes 0x183 or 0x283, protect 0x22004 or 0x2404)`, recorded as
  {base, size} in the first free of ten 16-byte slots at 0x71c13a8..0x71c1438, then handed to an allocator
  (fns 0x1d14170, 0x1cff250, 0x221a4f0, 0x5b1a7d0). A failed pool hits `int3` and is recorded with base 0.
  **I** these are the game's fixed heaps (0x20003000 = commit + reserve in the 2 MiB page mode).

### 1.2 fn 0x1e6c110 — the streaming page pool (951 bytes; one caller, fn 0x5c4ceb0)
The ONLY other code that touches the view globals 0x71bf970/978/980 (`refs-gpu-view-globals.txt`: 7 references,
all in fn 0x1e1cf60 and this function). Arguments: rcx = view size to use if the views do not exist yet,
rdx = pool size in bytes.
- **V** Returns 0 at once if global 0x9e1dac0 is already set (runs once).
- **V** If the start-up views exist: 0x9e1dac0 = CPU view base, 0x9e1dad0 = GPU view base, 0x9e1dac8 = view size.
  Otherwise it reserves the same pair itself (attributes 0x21/protect 4 and attributes 3/protect 0x801).
- **V** pool pages = pool bytes >> 16 (64 KiB pages). fn 0x555b0f0(view size >> 16) is called first.
  **I** it sets up the allocator of page numbers inside the views.
- **V** `XMemAllocatePhysicalPages(0x380, &count, array)`; if it fails or returns fewer pages than asked, the pages are
  freed and the function returns 0 (no `int3`: the caller decides).
- **V** If the device object (global 0x8759d00) exists:
  - `XMemVirtualAlloc(0, pool bytes, 0x20402000, attributes 0x23, protect 0x2004)` -> global 0x71bfaa8 (the pool
    address: one linear range, CPU read-write + graphics read-write);
  - `XMemMapPhysicalPages(pool address, pages, array)` — the whole pool mapped in order;
  - device vtable +0x250 (slot 74) called with (pool address, pages, &global 0x71bfcc8);
  - device vtable +0x40 (slot 8, CreateCommandQueue in the D3D12 layout) -> global 0x71bfce8, a queue of its own.
  Each failure hits `int3`.
- **V** If there is no device object: both views are released (VirtualFree) and ONE range is reserved instead
  (attributes 0x23, protect 0x2004) and stored as BOTH the CPU and the GPU base. **I** so the game itself has a
  mode in which CPU address == GPU address for streamed memory.
- **V** Bookkeeping, three 21-bit values packed per 64-bit word:
  - global 0x71bfcc0: per pool page, bits 4..35 of the physical page entry returned by XMemAllocatePhysicalPages;
  - global 0x71bfcd0: a free list threaded through the pool pages (entry i = i+1, last = 0x1fffff);
    head 0x71bfcd8 = 0, tail 0x71bfcdc = pages-1, count 0x71bfce0 = pages;
  - global 0x71bfab0: one zeroed byte per pool page.

### 1.3 fn 0xf2c460 / 0xf2c470 — the page mapper (read in full, `funcs\00f2c470.txt`)
- **V** Entry is 0xf2c460 (a 4-instruction leaf with no exception-table entry: "return if the list is empty", else
  falls into 0xf2c470). ONE call site in the whole image: 0xe8900 in fn 0xe88f0, a virtual method (its address is
  stored once, at .rdata 0x62fc790). fn 0xe88f0 passes `object + 0x38` = the pending-operation list.
- **V** The list object: +0 begin, +8 end of an array of 32-bit operations; +0x18 number of CPU scatter entries,
  +0x1c number of GPU ranges, +0x20 number of GPU batches (pre-counted by whoever queued the operations).
  Work arrays live on the stack up to 256 KiB, else on the heap (fn 0x4f97130 / 0x4d9c130). Lock 0x71bfad8 is
  held while the list is processed.
- **V** One operation = 32 bits: bits 31..29 = kind (0..5, switch table 0x674c2bc), bits 28..8 = VIEW PAGE NUMBER
  (21 bits), bits 7..0 = page count - 1 (1..256 pages).

  | kind | code at | CPU view | GPU view | pool pages |
  |---|---|---|---|---|
  | 0 | 0xf2c642 | map | map | taken from the free list |
  | 1 | 0xf2c9a5 | map | - | taken from the free list |
  | 2 | 0xf2c7cf | - | map | taken from the free list |
  | 3 | 0xf2c893 | unmap | unmap | returned to the free list |
  | 4 | 0xf2c712 | unmap | - | returned to the free list |
  | 5 | 0xf2ca63 | - | unmap | returned to the free list |

- **V** Map: for each page, pop a pool page from the free list (head 0x71bfcd8; an empty list hits `int3`),
  record it with fn 0x55aa6a0(view page, pool page), and
  - CPU: add a scatter entry {pool page, view page};
  - GPU: one batch per operation {destination = GPU base + view page * 64 KiB, range count = pages, ranges} with
    one range per page {kind 1, start = POOL PAGE INDEX, count 1}.
- **V** Unmap: fn 0x221ba60(view page) returns the pool page that was there and marks the view page empty; the pool
  page is appended to the free list tail; CPU: scatter entry {none (-1), view page}; GPU: one batch
  {destination, 1 range {kind 2, start 0, count = pages}}.
- **V** After the list: the lock is released; the scatter entries are sorted (fn 0x221bbc0) and turned into
  (address = CPU base + view page * 64 KiB, physical page = table 0x71bfcc0[pool page], or 0 to unmap); when two
  entries name the same address the later one wins; ONE `XMemMapPhysicalPagesScatter(addresses, count, pages)`;
  failure hits `int3`.
- **V** Then, if there is any GPU batch: queue 0x71bfce8, vtable +0xf8 (slot 31) with (batch count, batches,
  pool address 0x71bfaa8, pool page count, 0). The result is not looked at.
- **V** The two page tables (indexed by VIEW PAGE NUMBER, one numbering shared by both views; count in 0x71bfb18):
  - 0x71bfb08: 64-bit entry per view page: bits 0..14 a use count kept on the first page of a block, bits 15..19
    the block order (leaf 0xe8890 uses it to find the block's first page), bits 20..40 the pool page (0x1fffff =
    none);
  - 0x71bfb10: per view page, the physical page entry of its pool page.

### 1.4 What this means
- **V** There is no address-conversion table in the game. A page number P means the same P in both views:
  CPU address = CPU base + P * 64 KiB, GPU address = GPU base + P * 64 KiB = CPU address + (GPU base - CPU base).
- **V** The GPU view is filled ONLY through queue slot 31, and each range names a POOL PAGE INDEX: "show page Q of
  the pool resource (pool address, pool pages) at this GPU address". **I** The pool itself is one linear range that
  the game also made GPU-visible at set-up (protect 0x2004 read as "graphics read-write"; device slot 74 is given
  the pool address).
- **V** Kind 2 pages get no CPU-view entry in this function. **I** They exist only in the GPU view, and the CPU can
  reach their bytes only through the pool address (no other code was found mapping view pages; see 1.6).
- **I** So the faithful reproduction is: one GPU-visible pool, plus a GPU view whose page P reads pool page Q exactly
  as the slot-31 batches say. Nothing has to be searched or converted per descriptor.

### 1.5 Hosting

The hosting model is the project's own (weave/), written from this map; the running log of what it does and
measures is in `../../FINDINGS.md`. Everything in sections 1.1-1.4 is what the executable itself does.

### 1.6 Who else touches these tables (`refs-page-tables.txt`)
- **V** View page table 0x71bfb08: 53 references in 14 functions (fn 0x555b0f0 set-up 11, fn 0x221b7d0 7,
  fn 0x221b910 5, fn 0x221b780 4, fn 0x5121690, fn 0x5238960, fn 0x5238590, fn 0x5238830, ...).
- **V** GPU view base 0x9e1dad0: 47 references in 34 functions; CPU view base 0x9e1dac0: 59 in 41. **I** these are
  the places where the game turns a pointer into a GPU address or back.
- **V** Pool address 0x71bfaa8: 5 references in 3 functions. Pool queue 0x71bfce8: 2 references in 2 functions.
- **V** View page numbers come from a buddy allocator over the view-page table: fn 0x221b7d0 frees a block of
  2^order pages (order = bits 15..19 of the entry), clears its in-use bits 62/63, merges it with its buddy
  (`page xor size`) while the buddy is free and of the same order, then tail-jumps to fn 0x5238960 to put the block
  on a free list; the count of pages in use is global 0x71bfb20. fn 0x221b780 frees both tables at shutdown.
  **U** the allocating side (fn 0x5238590 / 0x5238830 are the likely pair) and who chooses kind 0, 1 or 2.
- **V** fn 0x5238590 (495 bytes) is the buddy ALLOCATE: order in ecx (0..15), free-list heads per order at
  0x71bfb08 + 0x74 + order*4; it splits a larger block when the asked order's list is empty and returns
  (first page << 4) | order. fn 0x52384c0 (199 bytes, one caller fn 0x14c640) wraps it for a byte size:
  order = (bytes + 0xfff) >> 12, returns CPU view base + block << 12, and on exhaustion grows the pool
  (fn 0x5238780) and maps the new range (fn 0xf7c20). **I** a 4 KiB-granular suballocator over the CPU view,
  distinct from the 64 KiB view-page buddy that the page mapper's operations use. **U** how the two share the
  view-page table, and which subsystem fn 0x14c640 serves.

### 1.7 The nine fixed pools and their allocators (V, funcs 01e1cf60 / 05b1a7d0 / 01d14170 / 0554f680 / 01f67770)
fn 0x1e1cf60's loop (9 iterations, sizes from the table 0x71bf9b0) reserves+commits each pool with
`XMemVirtualAlloc(0, size, 0x20003000, attr, protect)` and hands it to an allocator chosen by a switch on the
pool index (jump table 0x674c270). The pool {base,size} is recorded in the first free of ten 16-byte slots
0x71c13a8..0x71c1438.

| index | attr / protect | init wrapper | real allocator | structure |
|---|---|---|---|---|
| 0 | 0x183 / 0x22004 | 0x221a4f0(0, size) then 0x1cff250(0x71c1560) | (case 0, not yet read) | — |
| 1, 2 | 0x183 / 0x22004 | 0x1d14170(obj 0x74438b0 / 0x7444b30, base, size) | **0x1f67770** | free-list / buddy, inline headers |
| 4 | 0x283 / **0x2404 = PAGE_GRAPHICS_READWRITE (0x2000) | PAGE_WRITECOMBINE (0x400) | READWRITE** | 0x5b1a7d0(0x71c2808, size, base, 0x20000, 0x71c28b0) | **0x554f680** | UNIT allocator, 0x20000 units |

- **0x1f67770 (case 1/2):** base aligned up to 32 = the first header; first free byte = header + 0x10; an
  order-indexed free-list bucket array of 0x1200 bytes at obj+0x30; remaining size at obj+0x1230/0x1240/0x1248;
  inline 16-byte headers {next-free low32, size low32, flags}. The WHOLE pool is committed at set-up; free
  blocks stay committed. **I** a V# spanning a free block here still reads committed memory — NOT a hole source.
- **0x554f680 (case 4, write-combining graphics pool):** a UNIT allocator. The 0x20000 argument
  is the unit COUNT (131072); unit size = pool size / 131072. A 20-byte-per-unit state table lives in .data at
  0x71c28b0 (0x1000 header + 0x20000 × 0x14 entries; [0x71c28b0+0x1024] = pool size); each entry's low 26 bits
  = unit index, high 6 bits = state. Pool base at 0x71c2808+0x30; unit count at sub-alloc+0x18; size at +0x28;
  sub-alloc+0x30 = 0xffffffff (free head sentinel).
- The protect values are named in the public Windows SDK header winnt.h: 0x400 is PAGE_WRITECOMBINE and 0x2000 is
  PAGE_GRAPHICS_READWRITE; the earlier note calling 0x400 "NOCACHE" was wrong (PAGE_NOCACHE is 0x200).
- Our library answers these calls (weave/xmem.*); how it maps the flag combinations onto Windows is written there.

## 2. How the game stops itself

### 2.1 fn 0x2474500 — the fatal stop (286 bytes; reached from fn 0x4e9b660, which 31 functions call)
- **V** Sets byte 0x75e7620, takes a lock (0x75e77d8), and if byte 0xa943999 is 1: saves the unhandled-exception
  filter, reads working-set statistics (kind 0x20000001), and if more than 100 MiB is free allocates 0x80 bytes,
  fills them from 0xa943838..0xa9438b7, calls the device (vtable +0x190, slot 50) and another object (0x75df048,
  vtable +0x420 with argument 3), frees the block. Then `int3` at 0x2474606.
- **V** fn 0x4e9b660 (47 bytes, `funcs\04e9b660.txt`) is a result check: it takes a 32-bit result code, returns it
  unchanged, and calls the fatal stop ONLY when the code is negative and equals one of four values:
  0x887A0005, 0x887A0006, 0x887A0007, 0x887A0020 (index = code - 0x887A0005, bit mask 0x8000007 over 28 bits).
  **I** by the public DXGI names: device removed, device hung, device reset, driver internal error.
- **V** 51 call sites in 31 functions pass it the result of a graphics call (fn 0x2478440 alone 11 times).
- **I** So a breakpoint at the fatal stop means the game reacted to "the graphics device is gone" (or another
  listed error). Which of the 51 sites fired does not matter: every one of them means the same thing.

### 2.2 fn 0x2215a60 — the hang watchdog (937 bytes) — see section 4 below
- The next functions in the image are memory functions: fn 0x2215fd0 (XMemIsToolsMemAvailable,
  XMemGetWorkingSetStatistics x2, XMemTransferMemory), fn 0x2216390 (XMemVirtualAlloc), fn 0x22163d0
  (XMemAllocatePhysicalPages), fn 0x2216940 (XMemMapPhysicalPages). **I** the watchdog sits in the same source
  module as the game's low-level system/memory code. **U** whether they share state.

## 3. Open list (next to read)
- fn 0xf2c470 (2,216 bytes, no direct caller: reached through a pointer) — the page mapper (Scatter + slot 31).
- fn 0x555b0f0 — the page-number allocator of the views.
- fn 0x5c4ceb0 — caller of the pool set-up: what sizes it passes.
- fn 0x1e86440 — 6 reservations + 5 page maps (a second paged structure).
- fn 0x22163d0 / 0x2216940 / 0x2473090 / 0x2474660 — the other physical-page users.
- fn 0x4e9b660 and its 31 callers — every reason the game stops itself.
- device slot 74 (+0x250) and slot 50 (+0x190): what the weave answers there.

## 4. The watchdog as the frame loop's liveness relay (read from the image)

Read to answer ExCorvus's hypothesis ("the watchdog is the relay for the frame-pacer system; rendering, pipeline timing
and the fps cap are ONE system"). The answer the image gives: the watchdog does **not** drive or gate the frame-event
calls; it **watches the loop that does**. It is a liveness relay — the loop reports progress, the watchdog counts the
gap, and OUR layer's long blocks are what it sees. Everything below is from `funcs\02215a60.txt`, `02215a20.txt`,
`01ad0fd0.txt`, `022159f0.txt`, `01c82b20.txt`, `055aa250.txt`, `00b5b6b0.txt`, `00b5b6f0.txt`, plus targeted
`dataref`/`jmpref`/`ptrref` sweeps.

### 4.1 It is a thread of its own (V)
- fn 0x2215a20 starts it: `fn 0x2215a60`, argument 0, stack 0xA000, priority 0xF (time-critical) -> fn 0x1ad0ef0.
- fn 0x1ad0fd0 is the engine's thread factory: a `{function, argument, handle}` context (fn 0x2215e10), then
  `CreateThread` with entry **fn 0x2215ea0** (the trampoline: TEB+0x410 = stack base, `call [context]`, then
  fn 0x22160b0 / fn 0x2216160), `SetThreadPriority`, `SetThreadGroupAffinity`, `ResumeThread`.
- So the watchdog is a parallel execution inside the same process, exactly as ExCorvus described it, not a timer callback.
- fn 0x22161b0 stops it (caller 0x21b5cc0): clears 0x71b6330, sets 0x71b6340 = 1, zeroes 0x71b6334, and joins the
  handle kept at 0x6843320 (fn 0x5b32780; the handle is set to -1 afterwards). **V**

### 4.2 The state cluster (V unless marked)
| RVA | width | meaning | writers in the image | readers |
|---|---|---|---|---|
| 0x71b62f0 | qword | selects a second client-check path | **none** (stays 0) — **U** what would set it | fn 0x2215a60 |
| 0x71b6308 | 24 B | a lock (init fn 0x1b0bc20 registers its destructor; taken by fn 0x5c8f5f0 at 0x2215dce; zeroed by fn 0x220fca0) | init only | fn 0x2215a60 |
| 0x71b6330 | byte | pause the checks | fn 0x22161b0 (0) | fn 0x2215a60 |
| 0x71b6331 | byte | the kick: one pass's checks are skipped | **none** (every 4-byte disp32 and the imm32 scans come back empty; the watchdog only reads it and clears it) | fn 0x2215a60 |
| 0x71b6334 | dword | open watch scopes (atomic) | fn 0xb5b6b0 (+1), fn 0xb5b6f0 (-1), fn 0x22161b0 (0) | fn 0x2215a60 |
| 0x71b6338 | qword | QPC of the last progress report | **fn 0x22159f0 only**, besides the watchdog itself, which stores it 4 times when it re-arms (corrected 2026-11-27, measure/watchdog.py) | fn 0x2215a60 |
| 0x71b6340 | byte | stop: the thread leaves | fn 0x22161b0 (1) | fn 0x2215a60 |
| 0x71b6348 | qword | the crash callback | fn 0x22161f0 (4.5) | fn 0x2215a60 |
| 0x6844a20 | 64 x 72 B | the watch-scope table (4.4) | fn 0x55aa250 / fn 0xb5b6b0 / fn 0xb5b6f0 | fn 0x2215a60 |
- The two float thresholds: 0x6159bd4 = **2.0f** (a Sleep overrun longer than this forgives the pass and re-arms) and
  0x6159e20 = **300.0f** s (an open watch scope this old is a hang). 0x9e4f5c0 is the QPC-ticks-to-seconds factor; it
  is .data beyond the file (zero-filled at load), so **U** its value, but its use is V.
- **I** 0x71b6331 being write-free in the image means the kick is a port-side injection point, not something the game
  ever raises; the measured behaviour of `LEONIDA_TITLE_WATCHDOG=0` (boots survive) matches that.

### 4.3 One pass of fn 0x2215a60 (V)
- `QueryPerformanceCounter`; `Sleep(1000)` (the thunk at 0x92eca0); `QueryPerformanceCounter` again. If the sleep
  took more than 2.0 s, the pass is forgiven (last = now, 30 ticks re-armed, kick cleared) — a suspended or throttled
  process is not a hang.
- Else if the kick byte is set: last = now, 30 ticks re-armed, kick cleared, and this pass's checks are skipped.
- Else if the open-scope count is non-zero: for each of the 64 slots, for each of its up-to-8 timestamps, a stamp
  older than 300.0 s (after a `0x89aee1d6` lookup, fn 0x9d3e70) is a hang. The idle countdown is not evaluated at all.
- Else: read the last progress stamp; a new value re-arms 30 ticks, the same value takes one tick away. At zero:
  `call [0x71b6348]` if installed, then the unconditional `movl $0, 0` at 0x2215c35 and `int3`.
- **I** So the budget is: 30 s without progress while nothing is scoped, or 300 s inside one scope, whichever comes
  first; the grace on an overrun is 2 s.

### 4.4 The watch-scope table, and who opens one (V / I)
- fn 0x55aa250 hands the calling thread its slot: the TEB caches the pointer at +0x408; a slot is popped from a
  Treiber free list at 0x71bfef0 (head in the low 32 bits, pop generation in the high 32); slot +0 is the free-list
  link, +4 the open count, +8.. the 8 per-scope QPC stamps.
- fn 0xb5b6b0 opens a scope: push QPC, ++count, `lock inc` 0x71b6334. fn 0xb5b6f0 closes it: `lock dec`, pop the
  stamp, and at count 0 release the slot (fn 0x55aa2d0). One thread can nest up to 8.
- Callers (11), each bracketing a body: 0xb59e40, 0xcc44b0, 0x1d83480, 0x1ee1200, 0x22185a0, 0x248e070, 0x32b5e50,
  0x374f2d0, 0x50070f0, 0x5a9b5b0, and the fatal stop fn 0x2474500 (a crash handler is itself watched). **I** these
  are long I/O, load, save and streaming operations; the frame loop is **not** among them. **U** each one's exact
  operation — not needed for the relay.

### 4.5 The crash path and its callback (V)
- fn 0x22161f0(rcx) installs a crash callback: when the thread's TEB block has bit 1 of byte +0x12d8 set, it stores
  rcx into 0x71b6348. Fn 0x1c82b20 calls it with 0x258b000 (a 13-byte `mov dword ptr [0], 0x2ef1e94b; ret` — a
  fail-fast marker) and restores the previous value on the way out.
- fn 0x1c82b20 is one of the engine's **run-loop wrappers**: it installs that handler, sets byte 0xab731e0 = 1, and
  loops { fn 0x22159f0 (progress), fn 0x1474e00, fn 0x9d8ec0, fn 0x54f5080, `Sleep(fn 0x258b010())`, fn 0x1f8ae00
  (continue?) }, then clears the flag and restores the handler. 23 functions call it (20 calls and 6 tail jumps,
  measure/watchdog.py) — the game modes that run the
  loop. **I** its `Sleep` is a frame-time sleep, so this is the loop where the cap and the progress report meet.

### 4.6 Who reports progress (V)
- fn 0x22159f0 is the whole reporter: `QueryPerformanceCounter` and one store into 0x71b6338. 35 bytes (this
  line said 18 until 2026-11-27; measured by measure/watchdog.py), no lock, no TLS.
- Its call sites: 0x1c82b67 and 0x1c82bc1 (inside the run loop above), 0x120799 (fn 0x120790, a callback that also
  touches the D3D query/state block reached from TEB+0x12a8) and 0xc9e368 (fn 0xc9e350).
- **I** Those are the only things that keep the watchdog quiet. A 30 s gap in them is a fire — and a long block of
  ours in a shim call is exactly such a gap, because the title cannot reach them while it is inside our code.

### 4.7 What connects it to the frame-event path, and what does not (V / I / U)
- **V (negative):** nothing in fn 0x2215a60, its cluster or its callees names a device, queue, frame event, fence or
  pacer; there is no reference to the device global 0x8759d00, the pool queue 0x71bfce8, or any frame-event object.
- **V:** the frame-event calls are device slots 78 (SetFrameIntervalX), 79 (ScheduleFrameEventX), 80
  (WaitFrameEventX), 81 (GetFrameStatisticsX) and queue slot 30/32 (PresentX). The image reaches them through
  capability probes (fn 0x542c080 asks slots 78, 79 and 80 as booleans into a capability struct) and a spread of
  vtable call sites. The watchdog is not on that path.
- **I (the relay reading our runtime uses):** the relay is **liveness, not cadence**. The watchdog does not drive the
  pacer; it watches the loop that both paces (its Sleep, and our WaitFrameEventX model) and reports progress. One
  block in weave code therefore stalls frames *and* drains the watchdog's 30 s budget at the same time — that is the
  one-system coupling.
- **U:** which of the vtable call sites of slots 78-81 belong to the real frame loop (the capability probes obscure
  it). Not needed for the relay; a dedicated chapter would need the call-graph up from each site.

### 4.8 What the weave does with it

The weave does not touch the watchdog: it stays armed and the game's own loop reports progress. The weave only
avoids holding the title thread inside its own calls for longer than the budget (30 s unscoped, 300 s scoped),
because a long block in weave code is exactly the gap the watchdog measures. The frame-event path (device slots
78-81, queue PresentX) is answered by the weave's graphics library unchanged in cadence.

## 5. The graphics error path

Device loss reaches the game only through fn 0x4e9b660 (section 2.1): a negative result from a graphics call
with one of four codes calls the fatal stop. On a PC, the weave's graphics library answers those calls with the
PC's own results, so the game's own error handling decides what happens.

## 6. The shell is the entire game, flat out (ExCorvus 2026-11-25 — the hosting-model footing)

ExCorvus: "i think the shell is actually just the entire game flat out." The weave is NOT a wrapper hosting the game piece
by piece from outside; it IS the game's whole flat memory/code laid out as one singular program, the weave only wiring
the environment (GPU reads the game's memory directly, frame cadence, liveness). The game uses FLAT unified VAs (its
shaders load V# with no added base); the faithful weave preserves that flat layout WHOLE and exposes it to the GPU
zero-copy, so EVERY V# the game writes resolves directly to the game's own RAM — no piecemeal imports, no shadow
copies, no "waiting for a copy", no holes, no 15 GB of fills.

Design consequence (step 2): ONE flat backing covering the game's whole address space, uniform heap_base shift,
zero-copy host imports of the game's real committed/mapped pages; only real pages bound (the rest of the flat space is
unbound reserve the game never names with a valid V#). Constraints to solve within Vulkan: GPU addresses are
driver-assigned (cannot place the backing at the ~2 TB title VA; the heap_base shift is forced); sparse buffers with
DEVICE_ADDRESS_CAPTURE_REPLAY_BIT are BANNED (crash AMD); binding the whole 24 GiB+ eagerly OOMs. So the flat-out
model must bind real pages on demand (as the game commits/maps them) at a driver-assigned device address with a uniform
shift — NOT piecemeal per-descriptor patches, and NOT a disk mirror.
