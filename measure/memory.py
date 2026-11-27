"""Repeats docs/exe-map/MAP.md sections 1.1 and 1.2 (their verified parts): the program's memory start-up - the
two 24 GiB views, the call to D3D12ConfigureVirtualMemoryX, the nine fixed pools - and the streaming page pool.

    python measure/memory.py "<your game folder>\\game_scarlett_master.exe"

Reads the executable read-only; nothing is written. Only the shape of the code is checked (which functions are
called, with which fixed numbers, where results are kept); what the values mean stays as MAP.md marks it.
"""
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from pe_image import Checks, open_from_arguments

exe = open_from_arguments(__doc__)
c = Checks("memory: the start-up views, the fixed pools and the streaming page pool (MAP.md 1.1, 1.2)")

START_UP = 0x1E1CF60
PAGE_POOL = 0x1E6C110
DEVICE = 0x8759D00


def thunk_import(address):
    """The import a one-jump thunk at `address` leads to."""
    ins = exe.instructions(address, address + 8, 1)[0]
    target = exe.rip_target(ins)
    return exe.import_at(target) if ins.mnemonic == "jmp" and target is not None else None


def calls(function):
    """(address, called address or None, {argument: number}) for each call in a function. The arguments are the
    fixed numbers moved into r8d, r9d, ecx, edx and the first stack argument since the call before."""
    begin, end = exe.function_of(function)
    body = exe.instructions(begin, end)
    found = []
    since = 0
    for i, ins in enumerate(body):
        if ins.mnemonic != "call":
            continue
        args = {}
        for before in body[since:i]:
            if before.mnemonic == "mov" and len(before.operands) == 2 and before.operands[1].type == X86_OP_IMM:
                place = before.op_str.split(",")[0].strip()
                if place in ("ecx", "edx", "r8d", "r9d") or place == "dword ptr [rsp + 0x20]":
                    args[place] = exe.number(before.address)
        target = ins.operands[0].imm if ins.operands[0].type == X86_OP_IMM else None
        found.append((ins.address, target, args, ins))
        since = i + 1
    return found


def calls_to_import(function, name):
    return [(a, args) for a, t, args, _ins in calls(function) if t is not None and thunk_import(t)
            and thunk_import(t)[1] == name]


def stored_after(call_address, function):
    """The global the call's result (rax) is stored into right after the call, if any."""
    begin, end = exe.function_of(function)
    for ins in exe.instructions(call_address, end, 6)[1:]:
        if ins.mnemonic == "mov" and ins.operands and ins.operands[0].type == X86_OP_MEM and ins.op_str.endswith("rax"):
            return exe.rip_target(ins)
    return None


# 1.1 - the start-up function.
begin, end = exe.function_of(START_UP)
c.same("size of the memory start-up function (bytes)", end - begin, 1677)
c.same("who calls or jumps to it", exe.callers_and_jumps(START_UP), [0x5C356E0])
body = exe.instructions(begin, end)
guard = [ins.mnemonic for ins in body if exe.rip_target(ins) == 0x71BF9F8]
c.same("the run-once guard byte 0x71bf9f8: tested, then set", guard, ["cmp", "mov"])
first_call = [t for _a, t, _args, _i in calls(START_UP)][0]
c.same("the first call asks fn 0x55deb88", first_call, 0x55DEB88)
test_code = [(ins.mnemonic, [o.imm for o in ins.operands if o.type == X86_OP_IMM][:1] if ins.mnemonic != "ja"
               and not ins.mnemonic.startswith("jae") else [])
             for ins in exe.instructions(0x1E1CF89, 0x1E1CFA0)]
c.same("the value it returns is tested against 8 and the mask 0x1c2 (only 1, 6, 7, 8)",
       test_code, [("cmp", [8]), ("ja", []), ("mov", [0x1C2]), ("bt", []), ("jae", [])])
size_code = [(ins.mnemonic, [o.imm for o in ins.operands if o.type == X86_OP_IMM])
             for ins in exe.instructions(0x1E1CFB0, 0x1E1CFC2)]
c.same("view size = ((fn 0x75140() ^ 1) | 2) << 33",
       size_code, [("call", [0x75140]), ("xor", [1]), ("movzx", []), ("or", [2]), ("shl", [33])])
field_code = [(ins.mnemonic, [o.imm for o in ins.operands if o.type == X86_OP_IMM])
              for ins in exe.instructions(0x1E1CFC5, 0x1E1CFDA)]
c.same("the block's field = fn 0x4da2350(ceil(size / 16 GiB)) + 2",
       field_code, [("shr", [12]), ("add", [0x3FFFFF]), ("shr", [22]), ("call", [0x4DA2350]), ("add", [2])])
configure = calls_to_import(START_UP, "D3D12ConfigureVirtualMemoryX")
c.same("D3D12ConfigureVirtualMemoryX is called once, a failure stops with int3",
       [len(configure), exe.at(configure[0][0] + 9).mnemonic if configure else None], [1, "int3"])
views = calls_to_import(START_UP, "XMemVirtualAlloc")
c.same("the two views: XMemVirtualAlloc (type, attributes, protect) -> where the address is kept",
       [((args.get("r8d"), args.get("r9d"), args.get("dword ptr [rsp + 0x20]")), stored_after(a, START_UP))
        for a, args in views[:2]],
       [((0x20402000, 0x21, 0x4), 0x71BF970), ((0x20402000, 0x3, 0x801), 0x71BF978)])
c.same("the view size is kept in 0x71bf980",
       any(ins.mnemonic == "mov" and exe.rip_target(ins) == 0x71BF980 and ins.operands[0].type == X86_OP_MEM
           for ins in body), True)
pools = views[2:]
c.same("the fixed pools: XMemVirtualAlloc (type, attributes, protect) kinds",
       sorted({(args.get("r8d"), args.get("r9d"), args.get("dword ptr [rsp + 0x20]")) for _a, args in pools}),
       [(0x20003000, 0x183, 0x22004), (0x20003000, 0x283, 0x2404)])
loop_end = [ins for ins in body if ins.mnemonic == "cmp" and ins.operands[1].type == X86_OP_IMM
            and ins.operands[1].imm == 0x48]
c.same("the pool loop walks 9 sizes of 8 bytes from the table at 0x71bf9b0",
       [len(loop_end) == 1, any(ins.mnemonic == "lea" and exe.rip_target(ins) == 0x71BF9B0 for ins in body)],
       [True, True])
slots = sorted({exe.rip_target(ins) for ins in body if exe.rip_target(ins) is not None
                and 0x71C13A8 <= exe.rip_target(ins) <= 0x71C1438 and (exe.rip_target(ins) - 0x71C13A8) % 16 == 0})
c.same("the ten 16-byte pool slots 0x71c13a8..0x71c1438", slots, [0x71C13A8 + 16 * i for i in range(10)])
allocators = sorted({t for _a, t, _args, _i in calls(START_UP)} & {0x1D14170, 0x1CFF250, 0x221A4F0, 0x5B1A7D0})
c.same("the pool allocators it hands pools to", allocators, [0x1CFF250, 0x1D14170, 0x221A4F0, 0x5B1A7D0])

# 1.2 - the streaming page pool.
begin, end = exe.function_of(PAGE_POOL)
c.same("size of the streaming page pool function (bytes)", end - begin, 951)
c.same("who calls or jumps to it", exe.callers_and_jumps(PAGE_POOL), [0x5C4CEB0])
view_refs = [f for g in (0x71BF970, 0x71BF978, 0x71BF980) for _a, f, _i in exe.rip_references(g)]
c.same("references to the view globals 0x71bf970/978/980, and the functions they are in",
       [len(view_refs), sorted(set(view_refs))], [7, [START_UP, PAGE_POOL]])
body = exe.instructions(begin, end)
c.same("it returns at once when 0x9e1dac0 is already set",
       [(ins.mnemonic, exe.rip_target(ins)) for ins in body[8:10]], [("cmp", 0x9E1DAC0), ("je", None)])
c.same("the pool size is turned into 64 KiB pages (shift by 16)",
       any(ins.mnemonic == "shr" and ins.operands[1].type == X86_OP_IMM and ins.operands[1].imm == 16 for ins in body),
       True)
own_views = calls_to_import(PAGE_POOL, "XMemVirtualAlloc")
c.same("its XMemVirtualAlloc calls (type, attributes, protect)",
       [(args.get("r8d"), args.get("r9d"), args.get("dword ptr [rsp + 0x20]")) for _a, args in own_views],
       [(0x20402000, 0x21, 0x4), (0x20402000, 0x3, 0x801), (0x20402000, 0x23, 0x2004), (0x20402000, 0x23, 0x2004)])
c.same("the pool address (attributes 0x23) is kept in 0x71bfaa8", stored_after(own_views[2][0], PAGE_POOL),
       0x71BFAA8)
physical = calls_to_import(PAGE_POOL, "XMemAllocatePhysicalPages")
c.same("XMemAllocatePhysicalPages is asked with 0x380", [args.get("ecx") for _a, args in physical], [0x380])
c.same("XMemMapPhysicalPages is called once", len(calls_to_import(PAGE_POOL, "XMemMapPhysicalPages")), 1)
methods = []
for a, t, _args, ins in calls(PAGE_POOL):
    if t is None and ins.operands[0].type == X86_OP_MEM:
        before = exe.instructions(a - 0x30, a)
        uses_device = any(exe.rip_target(b) == DEVICE for b in before)
        kept = [exe.rip_target(b) for b in before if b.mnemonic == "lea" and exe.rip_target(b) in (0x71BFCC8,
                                                                                                    0x71BFCE8)]
        methods.append((ins.operands[0].mem.disp, uses_device, kept))
c.same("device methods called (offset, on the device 0x8759d00, global handed over)",
       methods, [(0x250, True, [0x71BFCC8]), (0x40, True, [0x71BFCE8])])
c.same("the free list ends in 0x1fffff",
       any(ins.mnemonic == "mov" and len(ins.operands) == 2 and ins.operands[1].type == X86_OP_IMM
           and ins.operands[1].imm == 0x1FFFFF for ins in body), True)


def switch_targets(table, count):
    """A switch table of 32-bit offsets counted from the table itself."""
    targets = []
    for i in range(count):
        entry = exe.dword(table + 4 * i)
        targets.append(table + (entry - (1 << 32) if entry & 0x80000000 else entry))
    return targets


# 1.3 - the page mapper.
MAPPER = 0xF2C470
leaf = [ins.mnemonic for ins in exe.instructions(0xF2C460, MAPPER)]
c.same("0xf2c460 is a 4-instruction leaf that returns or falls into 0xf2c470",
       leaf[:4], ["mov", "cmp", "jne", "ret"])
c.same("who calls the leaf (by function)", exe.callers_and_jumps(0xF2C460), [0xE88F0])
c.same("the .rdata place 0x62fc790 holds the address of fn 0xe88f0",
       int.from_bytes(exe.read(0x62FC790, 8), "little") - 0x140000000, 0xE88F0)
c.same("the six operation kinds jump to (table 0x674c2bc)", switch_targets(0x674C2BC, 6),
       [0xF2C642, 0xF2C9A5, 0xF2C7CF, 0xF2C893, 0xF2C712, 0xF2CA63])
begin, end = exe.function_of(MAPPER)
mapper = calls(MAPPER)
c.same("the lock 0x71bfad8 is taken and released in the mapper",
       sum(1 for ins in exe.instructions(begin, end) if ins.mnemonic == "lea" and exe.rip_target(ins) == 0x71BFAD8), 2)
c.same("map: fn 0x55aa6a0 records a page, unmap: fn 0x221ba60 returns it (3 kinds each)",
       [sum(1 for _a, t, _x, _i in mapper if t == 0x55AA6A0), sum(1 for _a, t, _x, _i in mapper if t == 0x221BA60)],
       [3, 3])
c.same("the scatter entries are sorted by fn 0x221bbc0, then ONE XMemMapPhysicalPagesScatter",
       [any(t == 0x221BBC0 for _a, t, _x, _i in mapper), len(calls_to_import(MAPPER, "XMemMapPhysicalPagesScatter"))],
       [True, 1])
queue_calls = [ins.operands[0].mem.disp for _a, t, _x, ins in mapper if t is None and ins.operands[0].type == X86_OP_MEM]
c.same("the GPU batches go to the pool queue 0x71bfce8, method +0xf8 (slot 31), with the pool address 0x71bfaa8",
       [queue_calls, sorted({exe.rip_target(ins) for ins in exe.instructions(begin, end)} & {0x71BFCE8, 0x71BFAA8})],
       [[0xF8], [0x71BFAA8, 0x71BFCE8]])

# 1.6 - who else touches the tables (references from code that has a .pdata entry).
counts = {}
for g in (0x71BFB08, 0x9E1DAD0, 0x9E1DAC0, 0x71BFAA8, 0x71BFCE8):
    refs = exe.rip_references(g)
    counts[g] = (len(refs), len({f for _a, f, _i in refs}))
c.same("references (count, functions): view page table 0x71bfb08, GPU base 0x9e1dad0, CPU base 0x9e1dac0, "
       "pool address 0x71bfaa8, pool queue 0x71bfce8",
       [counts[g] for g in (0x71BFB08, 0x9E1DAD0, 0x9E1DAC0, 0x71BFAA8, 0x71BFCE8)],
       [(53, 14), (47, 34), (59, 41), (5, 3), (2, 2)])
begin, end = exe.function_of(0x5238590)
c.same("the buddy allocate fn 0x5238590: size, and its one caller", [end - begin, exe.callers_and_jumps(0x5238590)],
       [495, [0x52384C0]])
begin, end = exe.function_of(0x52384C0)
c.same("its byte-size wrapper fn 0x52384c0: size, and its one caller", [end - begin, exe.callers_and_jumps(0x52384C0)],
       [199, [0x14C640]])

# 1.7 - the pool switch.
inside = [begin_ <= t < end_ for begin_, end_ in [exe.function_of(START_UP)] for t in switch_targets(0x674C270, 6)]
c.same("the pool switch table 0x674c270 leads into the start-up function", inside, [True] * 6)

c.done()
