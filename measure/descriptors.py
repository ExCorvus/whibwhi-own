"""Repeats docs/exe-map/DESCRIPTORS.md section C (its measured parts): the descriptor record size the program keeps,
the places where the program copies 32-byte descriptors itself (and the device byte +0x18 that sends those copies to
device slot 24 instead), the canned null descriptors, and the check that the type-0 heap starts where the program
asked.

    python measure/descriptors.py "<your game folder>\\game_scarlett_master.exe"

Reads the executable read-only; nothing is written. Only where the code reads and writes is checked; what is inside
a descriptor is not read here (the program never reads it either).
"""
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from pe_image import Checks, open_from_arguments

exe = open_from_arguments(__doc__)
c = Checks("descriptors: record size, the program's own 32-byte copies, null descriptors, heap address (C.1-C.3)")

SETUP = 0x2478440
DEVICE = 0x8759D00
RECORD_SIZE = 0x75E7000
SLOT_15 = 15 * 8
SLOT_24 = 24 * 8


def body_of(function):
    return exe.instructions(*exe.function_of(function))


def writers(address):
    return sorted(a for a, _f, _i in exe.rip_references(address) if exe.writes_memory(a))


def method_calls(body, offset):
    return [ins.address for ins in body if ins.mnemonic == "call" and ins.operands[0].type == X86_OP_MEM
            and exe.rip_target(ins) is None and ins.operands[0].mem.disp == offset]


def immediates(body, mnemonic):
    return [o.imm for ins in body if ins.mnemonic == mnemonic for o in ins.operands if o.type == X86_OP_IMM]


# C.1 - the record size.
setup = body_of(SETUP)
asks = [ins for ins in setup if 0x2478E42 <= ins.address <= 0x2478E9A]
types = []
for i, ins in enumerate(asks):
    if ins.mnemonic == "call" and ins.operands[0].type == X86_OP_MEM and ins.operands[0].mem.disp == SLOT_15:
        value = None
        for before in reversed(asks[max(0, i - 4):i]):
            if before.op_str.startswith("edx,"):
                value = 0 if before.mnemonic == "xor" else exe.number(before.address)
                break
        if value is None:  # the first ask: edx was zeroed before the window
            value = 0 if any(b.mnemonic == "xor" and b.op_str == "edx, edx" for b in setup
                             if 0x2478E30 <= b.address < ins.address) else None
        types.append(value)
c.same("device slot 15 asked for heap types (0x2478e42..0x2478e9a)", types, [0, 1, 2, 3, 4])
c.same("the only store of the record size 0x75e7000", writers(RECORD_SIZE), [0x2478E4C])
multipliers = sorted(a for a, _f, ins in exe.rip_references(RECORD_SIZE) if ins.mnemonic == "imul")
c.same("where the record size turns an index into a byte offset", multipliers,
       [0x4E31CDE, 0x4FD2B6C, 0x4FD2C13, 0x4FD2C7C])
c.same("fn 0x4ecc830 reads the record size", any(f == 0x4ECC830 for _a, f, _i in exe.rip_references(RECORD_SIZE)),
       True)
c.same("the only store of the sampler heap offset 0x75e7010", writers(0x75E7010), [0x2478F9D])

# C.2 - the program's own copies, and the device byte that sends them to slot 24.
COPIES = {0x555EB70: 0x75E7540, 0x4FD2B50: 0x75E7540, 0x4FD2BF0: 0x75E7598, 0x4FD2C60: 0x75E7570, 0x4E31BB0: None}
for function, null_table in COPIES.items():
    body = body_of(function)
    device_byte = any(ins.mnemonic == "cmp" and ins.operands[0].type == X86_OP_MEM and ins.operands[0].mem.disp == 0x18
                      and ins.operands[1].type == X86_OP_IMM and ins.operands[1].imm == 0 for ins in body)
    loads_device = any(exe.rip_target(ins) == DEVICE for ins in body)
    moves = [ins for ins in body if ins.mnemonic == "vmovups"]
    stores = sum(1 for ins in moves if ins.operands[0].type == X86_OP_MEM)
    c.same(f"fn {function:#x}: loads the device, tests its byte +0x18, calls slot 24, copies 2 x 16 bytes",
           [loads_device, device_byte, len(method_calls(body, SLOT_24)), stores >= 2], [True, True, 1, True])
    if null_table is not None:
        tables = sorted({exe.rip_target(ins) for ins in body if exe.rip_target(ins) in (0x75E7540, 0x75E7570, 0x75E7598)})
        # DESCRIPTORS.md said until 2026-11-27 that all three record functions use the table at 0x75e7540.
        c.same(f"fn {function:#x}: the null descriptor it falls back to", tables, [null_table])
for function in (0x4FD2B50, 0x4FD2BF0, 0x4FD2C60):
    # DESCRIPTORS.md said "bits 4..11 of *record" until 2026-11-27; bextr control 0xa10 = 10 bits from bit 16.
    c.same(f"fn {function:#x}: the place is bits 16..25 of the record (bextr control 0xa10)",
           0xA10 in immediates(body_of(function), "mov"), True)
for function in (0x4FD2B50, 0x4FD2C60):
    body = body_of(function)
    c.same(f"fn {function:#x}: null index = bits 12..13 of the record (bextr 0x20c), or 4 - five entries",
           [0x20C in immediates(body, "mov"), 4 in immediates(body, "mov")], [True, True])
body = body_of(0x4E31BB0)
c.same("fn 0x4e31bb0: the slot-70 call (+0x230) comes before its copy; the place is word & 0x3ff",
       [method_calls(body, 70 * 8) and method_calls(body, 70 * 8)[0] < 0x4E31CF0, 0x3FF in immediates(body, "and")],
       [True, True])
c.same("callers of fn 0x4ecc830", exe.callers_and_jumps(0x4ECC830),
       [0x164EE0, 0x12B9800, 0x16475F0, 0x4ECC800, 0x555EB70, 0x5BDB950])

# The canned null descriptors built by fn 0x2478440.
nulls = [ins for ins in setup if 0x2479023 <= ins.address <= 0x24790B5]
c.same("the null descriptors' format 0x2a with view dimension 2, and component mapping 0x1688",
       [0x20000002A in immediates(nulls, "movabs"), 0x1688 in immediates(nulls, "mov")], [True, True])
c.same("each made through the public fn 0x164ee0 (calls at 0x247905a..0x24790b5)",
       [ins.address for ins in nulls if ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM
        and ins.operands[0].imm == 0x164EE0], [0x247905A, 0x247906E, 0x2479086, 0x24790A1, 0x24790B5])
convert = [ins for ins in setup if 0x24790BA <= ins.address <= 0x2479140]
c.same("their addresses turned into indices: distance from 0x75e7398 divided by 0x75e73a8, kept from 0x75de4a0",
       [sum(1 for ins in convert if ins.mnemonic == "div") >= 3,
        {0x75E7398, 0x75E73A8, 0x75DE4A0} <= {exe.rip_target(ins) for ins in convert}], [True, True])

# C.3 - the type-0 heap's address.
c.same("fn 0x2478440 calls fn 0x5cabb00 at 0x2478eb5",
       [exe.at(0x2478EB5).mnemonic, exe.at(0x2478EB5).operands[0].imm], ["call", 0x5CABB00])
body = body_of(0x5CABB00)
c.same("fn 0x5cabb00 asks slot 15, then calls fn 0x1cf3280 with the heap object 0x75e7378",
       [len(method_calls(body, SLOT_15)), 0x75E7378 in {exe.rip_target(ins) for ins in body},
        any(ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM and ins.operands[0].imm == 0x1CF3280
            for ins in body)], [1, True, True])
around = [ins.mnemonic for ins in exe.instructions(0x1CF3362, 0x1CF336D)]
c.same("fn 0x1cf3280 stops with int3 at 0x1cf336c unless the start equals the requested address",
       around, ["test", "je", "cmp", "je", "int3"])
c.same("the only store of the block base 0x75df188, and its function",
       [(a, f) for a, f, _i in exe.rip_references(0x75DF188) if exe.writes_memory(a)], [(0x1E864AC, 0x1E86440)])
staging = [ins for ins in setup if 0x2478EE2 <= ins.address <= 0x2478EF7]
c.same("the staging sampler heap is asked for with no address (r9d zeroed before the call at 0x2478ef7)",
       [staging[-1].mnemonic, staging[-1].operands[0].imm, any(i.mnemonic == "xor" and i.op_str == "r9d, r9d"
                                                               for i in staging)], ["call", 0x5A54700, True])

c.done()
