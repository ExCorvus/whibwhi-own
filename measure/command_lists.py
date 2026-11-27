"""Repeats what docs/exe-map/COMMAND-LIST-ORDER.md sections 1 and 2 say the executable does with a command list:

- the fields of the list object the program reads and writes itself (write position +0x18, end +0x30, the bytes
  +0x140 and +0x141, the two tables +0x144 and +0x344, the two 16-byte blocks +0x544 and +0x554, the flag byte
  +0x548), in its four flush functions;
- the record words it writes when there is room (0xc000b600, 0xc000b800, 0x4001b700), and that 0xc000b600 is
  written in one place only;
- the method slots it calls, at each place the map names: the public-shaped ones (Close, CopyBufferRegion,
  ResourceBarrier, ExecuteIndirect), the "emit this record" ones, and the four console-only ones.

What the slots mean on the PC is not checked here: most of it is marked unknown in the map.

    python measure/command_lists.py "<your game folder>\\game_scarlett_master.exe"

Reads the executable read-only; nothing is written.
"""
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from pe_image import Checks, open_from_arguments

exe = open_from_arguments(__doc__)
c = Checks("command lists: the list object's fields and the method slots, read in the executable")

FLUSHES = {0x32520: 6037, 0x32130: 1005, 0x248C2A0: 1027, 0x248C6B0: 935}


def memory(ins):
    return [op for op in ins.operands if op.type == X86_OP_MEM]


def method_offset(address):
    """The method table offset an indirect call at address goes through, or None."""
    ins = exe.at(address)
    if ins.mnemonic != "call" or exe.rip_target(ins) is not None or not memory(ins):
        return None
    return memory(ins)[0].mem.disp


def field_of(address):
    """The displacement of the memory operand at address, when it has no index register."""
    ops = [op for op in memory(exe.at(address)) if op.mem.index == 0]
    return ops[0].mem.disp if ops else None


def indexed_field(address):
    """(displacement, scale) of an indexed memory operand at address, or None."""
    ops = [op for op in memory(exe.at(address)) if op.mem.index != 0]
    return (ops[0].mem.disp, ops[0].mem.scale) if ops else None


def byte_stores(function, displacement, value=None):
    """Addresses in a function that store a byte (optionally a fixed one) to [reg + displacement]."""
    found = []
    for ins in exe.instructions(*exe.function_of(function)):
        ops = ins.operands
        if ins.mnemonic == "mov" and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[0].size == 1 and \
                ops[0].mem.disp == displacement and ops[0].mem.index == 0:
            if value is None or (ops[1].type == X86_OP_IMM and ops[1].imm == value):
                found.append(ins.address)
    return found


def sixteen_byte_stores(function, displacement):
    return [ins.address for ins in exe.instructions(*exe.function_of(function))
            if ins.mnemonic in ("vmovups", "vmovdqu") and ins.operands[0].type == X86_OP_MEM
            and ins.operands[0].mem.disp == displacement]


# --- the four flush functions -------------------------------------------------------------------------------
c.same("sizes of the four flush functions (map section 2.2)",
       {f: exe.function_of(f)[1] - exe.function_of(f)[0] for f in FLUSHES}, FLUSHES)

# --- section 1: the fields ----------------------------------------------------------------------------------
# +0x18 / +0x30: the program reads the write position and compares it with the end right after.
pairs = {}
for site in (0x3258A, 0x32876, 0x32978, 0x248C327, 0x248C4ED):
    following = exe.instructions(site, site + 48, limit=4)[1:]
    nxt = [ins for ins in following if ins.mnemonic == "cmp"][0]
    pairs[site] = (field_of(site), nxt.mnemonic, field_of(nxt.address))
c.same("at the map's places the program reads +0x18 and compares it with +0x30 within three instructions",
       pairs, {s: (0x18, "cmp", 0x30) for s in pairs})
c.same("the map's place 0x32a48 (listed among the +0x18 places) reads a field of the renderer instead",
       indexed_field(0x32A48), (0x12640, 8))

# +0x140: set to 1 when the record word is below 0x40000000.
sets = {f: byte_stores(f, 0x140, 1) for f in FLUSHES}
c.same("places that set byte +0x140 to 1, per flush function", sets,
       {0x32520: [0x33C01, 0x33C55],
        0x32130: [0x32251, 0x322A6, 0x32348, 0x323D1, 0x32493],
        0x248C2A0: [0x248C445, 0x248C4AB, 0x248C50B, 0x248C589, 0x248C641, 0x248C66A],
        0x248C6B0: [0x248C805, 0x248C86B, 0x248C8C8, 0x248C943, 0x248C9F5, 0x248CA1E]})
tested, untested = {}, {}
for f in FLUSHES:
    code = exe.instructions(*exe.function_of(f))
    where = {ins.address: n for n, ins in enumerate(code)}
    tested[f], untested[f] = [], []
    for s in sets[f]:
        before = code[max(0, where[s] - 3):where[s]]
        test = any(ins.mnemonic == "cmp" and any(op.type == X86_OP_IMM and op.imm in (0x40000000, 0x3FFFFFFF)
                                                 for op in ins.operands) for ins in before)
        (tested if test else untested)[f].append(s)
c.same("those sets that follow a test of the record word against 0x40000000 (cmp 0x40000000, or "
       "cmp 0x3fffffff + ja)", {f: len(v) for f, v in tested.items()},
       {0x32520: 0, 0x32130: 5, 0x248C2A0: 5, 0x248C6B0: 5})
c.same("the sets with no test: each after a record that also copies bit 20 into +0x141, and in fn 0x32520 "
       "after a run of records (map said: set when the word is below 0x40000000)",
       untested, {0x32520: [0x33C01, 0x33C55], 0x32130: [], 0x248C2A0: [0x248C641], 0x248C6B0: [0x248C9F5]})

# +0x141: copied from bit 20.
bit20 = {}
for site in (0x248C650, 0x33C11, 0x248CA04):
    code = exe.instructions(*exe.function_of(site))
    k = [n for n, ins in enumerate(code) if ins.address == site][0]
    before = code[k - 2:k]
    bit20[site] = (field_of(site), [(i.mnemonic, [o.imm for o in i.operands if o.type == X86_OP_IMM]) for i in before])
c.same("byte +0x141 is bit 20 of a block word (shr 20, and 1), at the map's place and two more",
       bit20, {s: (0x141, [("shr", [0x14]), ("and", [1])]) for s in bit20})

c.same("the table at +0x144 is read with the root-parameter type as index (dwords)",
       [indexed_field(s) for s in (0x32242, 0x32297)], [(0x144, 4)] * 2)
c.same("the table at +0x344 is read with the render-state byte as index (dwords)",
       [indexed_field(s) for s in (0x248C436, 0x248C49C, 0x248C4FC, 0x248C57A, 0x248C65B)], [(0x344, 4)] * 5)
c.same("16-byte stores to +0x544, per flush function",
       {f: sixteen_byte_stores(f, 0x544) for f in FLUSHES},
       {0x32520: [0x3278B], 0x32130: [], 0x248C2A0: [0x248C312], 0x248C6B0: [0x248C705]})
c.same("16-byte stores to +0x554, per flush function",
       {f: sixteen_byte_stores(f, 0x554) for f in FLUSHES},
       {0x32520: [0x32799], 0x32130: [], 0x248C2A0: [0x248C31F], 0x248C6B0: [0x248C712]})
c.same("the flag byte +0x548 is tested with 3 at the map's places",
       [(field_of(s), exe.at(s).mnemonic, exe.number(s)) for s in (0x32276, 0x32373, 0x324DD)],
       [(0x548, "test", 3)] * 3)

# --- section 1: the record words ----------------------------------------------------------------------------
c.same("record words written at 0x32596 and 0x32982, and or-ed in at 0x32880",
       [(exe.at(a).mnemonic, exe.number(a)) for a in (0x32596, 0x32982, 0x32880)],
       [("mov", 0xC000B600), ("mov", 0xC000B800), ("or", 0x4001B700)])
pattern = (0xC000B600).to_bytes(4, "little")
hits, i = [], exe.data.find(pattern)
while i >= 0:
    hits.append(i)
    i = exe.data.find(pattern, i + 1)
text = exe.sections[0]
c.same("the bytes of 0xc000b600 occur once in the whole file, inside the writer at 0x32596 (rva 0x32598)",
       [h - text[3] + text[1] for h in hits], [0x32598])
near = [exe.rip_target(ins) for ins in exe.instructions(0x32570, 0x325BA) if exe.rip_target(ins) is not None]
c.same("the second word of that record comes from the table at 0x61ecf30", 0x61ECF30 in near, True)

# --- section 1: the builder library is handed the list ------------------------------------------------------
lib, name = exe.import_at(exe.rip_target(exe.at(0x24902AC)))
c.same("0x24902ac calls the builder's batched build (its decoration names ID3D12GraphicsCommandList6)",
       (lib, "BuildRaytracingAccelerationStructureBatched2X" in name, "ID3D12GraphicsCommandList6" in name),
       ("XboxBvhBuilder.dll", True, True))
c.same("then slot +0xd0 on the same list at 0x24902d2", method_offset(0x24902D2), 0xD0)

# --- section 2.1: public-shaped calls -----------------------------------------------------------------------
closes = (0x17F6D48, 0x2478BCD, 0x2478CC2)
c.same("Close (+0x48) at the map's three places", [method_offset(s) for s in closes], [0x48] * 3)
checked = {}
for s in closes:
    after = exe.instructions(s, s + 16, limit=4)[1:]
    checked[s] = [i.mnemonic for i in after]
c.same("the result of each of the three is tested (map said: only at 0x2478bd5); the two frame lists stop "
       "the program with int3 when it is negative",
       checked, {0x17F6D48: ["test", "js", "add"], 0x2478BCD: ["test", "jns", "int3"],
                 0x2478CC2: ["test", "jns", "int3"]})
c.same("CopyBufferRegion (+0x78) at 0x249073f", method_offset(0x249073F), 0x78)
barriers = (0x24901A5, 0x24902D2, 0x24906C0, 0x24907A1, 0x249081F, 0x24909F7, 0x2490C81)
c.same("ResourceBarrier (+0xd0) at the seven places the map names", [method_offset(s) for s in barriers],
       [0xD0] * 7)
indirect = {0x248CBBA: 0x248CA90, 0x248CD09: 0x248CBE0, 0x248CDEF: 0x248CD20, 0x248D198: 0x248D000,
            0x248D33A: 0x248D1B0, 0x248D4E8: 0x248D350, 0x248D68A: 0x248D500, 0x4E33F91: 0x4E33E10,
            0x555E976: 0x555E800, 0x94445: 0x94360}
c.same("ExecuteIndirect (+0x1e8) at the map's ten places, in the functions it names",
       {s: (method_offset(s), exe.function_of(s)[0]) for s in indirect},
       {s: (0x1E8, f) for s, f in indirect.items()})

# --- section 2.2: the emit methods --------------------------------------------------------------------------
emit = {
    0x325BA: (0xA0, 0x32520), 0x33431: (0xA8, 0x32520), 0x33403: (0xB0, 0x32520), 0x32A06: (0xB8, 0x32520),
    0x3299B: (0xC0, 0x32520), 0x248C33B: (0xC8, 0x248C2A0), 0x248C72E: (0xC8, 0x248C6B0), 0x327B6: (0xC8, 0x32520),
    0x2482B99: (0xE8, 0x2482AE0), 0x24843B0: (0xF0, 0x2484330), 0x2484434: (0xF0, 0x2484330),
    0x248C373: (0x108, 0x248C2A0), 0x248C74A: (0x108, 0x248C6B0), 0x327C5: (0x110, 0x32520),
    0x248C465: (0x118, 0x248C2A0), 0x248C825: (0x118, 0x248C6B0), 0x321FF: (0x120, 0x32130),
    0x3223A: (0x120, 0x32130), 0x248C600: (0x138, 0x248C2A0), 0x248C9B7: (0x138, 0x248C6B0),
    0x3246C: (0x140, 0x32130), 0x248C528: (0x158, 0x248C2A0), 0x248C8E5: (0x158, 0x248C6B0),
    0x32321: (0x160, 0x32130), 0x248C5A6: (0x168, 0x248C2A0), 0x248C960: (0x168, 0x248C6B0),
    0x323E0: (0x170, 0x32130), 0x328B8: (0x178, 0x32520), 0x32960: (0x180, 0x32520), 0x33311: (0x190, 0x32520),
    0x50A62D8: (0x1A0, 0x50A5EB0), 0x979699: (0x1F0, 0x979650), 0x33390: (0x230, 0x32520),
    0x2490030: (0x238, 0x248FFD0), 0x6C6672: (0x240, 0x6C6630), 0x2482B31: (0x240, 0x2482AE0),
    0x2484B0C: (0x250, 0x2484AD0), 0x3281A: (0x258, 0x32520), 0x32638: (0x3F0, 0x32520),
    0x329E3: (0x470, 0x32520),
}
c.same("each emit method at the map's place, through the slot and in the function it names (40 places)",
       {s: (method_offset(s), exe.function_of(s)[0]) for s in emit}, emit)

# --- section 2.2: the four console-only slots ---------------------------------------------------------------
c.same("+0x458 at 0x2474cce in fn 0x2474cc0, a negative result stops the program at 0x2474ce1",
       (method_offset(0x2474CCE), exe.function_of(0x2474CCE)[0], exe.at(0x2474CE1).mnemonic),
       (0x458, 0x2474CC0, "int3"))
c.same("+0x490 at 0x3210d in fn 0x320e0", (method_offset(0x3210D), exe.function_of(0x3210D)[0]),
       (0x490, 0x320E0))
requests = {0x2478BBF: 0x418, 0x2478CB4: 0x418, 0x2478B9E: 0x420, 0x2478C93: 0x420}
c.same("+0x418 and +0x420, twice each, in the graphics set-up fn 0x2478440",
       {s: (method_offset(s), exe.function_of(s)[0]) for s in requests},
       {s: (o, 0x2478440) for s, o in requests.items()})

c.done()
