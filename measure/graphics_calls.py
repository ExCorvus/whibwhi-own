"""Repeats what docs/exe-map/GRAPHICS-CALLS.md section 1.2 (D3D12XboxCreateDevice) and section 4 (the
XboxBvhBuilder library) say the executable does:

- the device is asked for once, by fn 0x2478440 through a jump to the D3D12XboxCreateDevice import, after the
  program has registered and shown its own window; the 64-byte creation block and the call's arguments;
- what it asks of the device right after (custom heap properties, the pool queue, the objects of slot 48, the
  command signatures, the version-dependent slot 49);
- the builder library's seven methods and where each is called; the one builder object of 0x18 bytes kept in
  global 0x75df0a8.

    python measure/graphics_calls.py "<your game folder>\\game_scarlett_master.exe"

Reads the executable read-only; nothing is written.
"""
import struct

from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from pe_image import Checks, open_from_arguments

exe = open_from_arguments(__doc__)
c = Checks("graphics calls: the device's creation and the ray-tracing builder, read in the executable")

SETUP = 0x2478440        # the graphics set-up function
CALL = 0x24785B7         # its call that asks for the device
DEVICE = 0x75DF048       # where the device arrives
KEPT = 0x8759D00         # where the program keeps it
BUILDER = 0x75DF0A8      # the one builder object

setup = exe.instructions(*exe.function_of(SETUP))
at = {ins.address: i for i, ins in enumerate(setup)}


def direct_target(ins):
    if ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM:
        return ins.operands[0].imm
    return None


def imported(ins):
    """The import an instruction calls or jumps through, or None."""
    target = exe.rip_target(ins)
    return exe.import_at(target) if ins.mnemonic in ("call", "jmp") and target is not None else None


def method_calls(code, holders, window=30):
    """[(address, slot)] of calls through a method table where a holder global was read shortly before."""
    found = []
    for i, ins in enumerate(code):
        if ins.mnemonic != "call" or ins.operands[0].type != X86_OP_MEM or exe.rip_target(ins) is not None:
            continue
        if any(exe.rip_target(earlier) in holders for earlier in code[max(0, i - window):i]):
            found.append((ins.address, ins.operands[0].mem.disp // 8))
    return found


def argument_before(address, register):
    """The fixed number or the address last placed in a register before a call (back to the previous call)."""
    i = at[address]
    for ins in reversed(setup[max(0, i - 40):i]):
        if ins.mnemonic == "call":
            break
        if ins.operands and ins.operands[0].type == X86_OP_REG and ins.reg_name(ins.operands[0].reg) == register:
            if ins.mnemonic == "mov" and ins.operands[1].type == X86_OP_IMM:
                return ins.operands[1].imm
            if ins.mnemonic == "lea":
                target = exe.rip_target(ins)
                return target if target is not None else f"stack {ins.operands[1].mem.disp:#x}"
            if ins.mnemonic == "xor" and ins.operands[1].type == X86_OP_REG and \
                    ins.operands[1].reg == ins.operands[0].reg:
                return 0
            return None
    return None


# --- section 1.2: the call that asks for the device ---------------------------------------------------------
c.same("functions that call the set-up function (map: one chain, through fn 0x19f4df0)",
       exe.callers_and_jumps(SETUP), [0x19F4DF0])
thunk = direct_target(exe.at(CALL))
c.same("the call at 0x24785b7 goes to a jump through the D3D12XboxCreateDevice import",
       [thunk, imported(exe.at(thunk))], [0x5F3FD80, ("d3d12_xs.dll", "D3D12XboxCreateDevice")])
c.same("the only function that reaches that jump", exe.callers_and_jumps(thunk), [SETUP])

window_calls = [imported(ins)[1] for ins in setup if ins.address < CALL and imported(ins)]
c.same("before it, the program makes its own window",
       window_calls, ["RegisterClassExA", "CreateWindowExA", "ShowWindow"])

block = {}
for ins in setup[at[CALL] - 20:at[CALL]]:
    if ins.mnemonic == "mov" and ins.operands[0].type == X86_OP_MEM and \
            ins.reg_name(ins.operands[0].mem.base) == "rdx":
        source = ins.operands[1]
        if source.type == X86_OP_IMM:
            block[ins.operands[0].mem.disp] = source.imm
        else:
            block[ins.operands[0].mem.disp] = "from the program's settings"
c.same("the creation block: the fields the program sets (the rest of its 64 bytes zeroed)",
       block, {0: 0x20004, 8: 0x100000, 0x20: "from the program's settings", 0x24: "from the program's settings",
               0x28: 1, 0x3C: 0x20})
zeroed = sorted(ins.operands[0].mem.disp for ins in setup[at[CALL] - 20:at[CALL]]
                if ins.mnemonic == "vmovups" and ins.operands[0].type == X86_OP_MEM)
c.same("the block is cleared with three 32-byte stores first (at +0, +0x20, +0x30: 64 bytes)", zeroed, [0, 0x20, 0x30])
c.same("the call's arguments: adapter, block, interface id, where the device arrives",
       [argument_before(CALL, "rcx") if argument_before(CALL, "rcx") is not None else
        argument_before(CALL, "ecx"), argument_before(CALL, "rdx"), argument_before(CALL, "r8"),
        argument_before(CALL, "r9")],
       [0, "stack 0x1b0", 0x61E0638, DEVICE])
after = setup[at[CALL] + 1:at[CALL] + 8]
c.same("the result goes to the program's result check (fn 0x4e9b660) and a failure leaves the set-up",
       [direct_target(after[2]), after[4].mnemonic], [0x4E9B660, "js"])
copies = sorted(exe.rip_target(ins) for ins in setup[at[CALL]:at[CALL] + 25]
                if ins.mnemonic == "mov" and ins.operands[0].type == X86_OP_MEM and exe.rip_target(ins) is not None)
# Read 2026-11-27: besides global 0x8759d00 (the map's "keeps both") the device is copied into ten more globals.
c.same("where the device is copied right after", copies,
       [0x75DEFF8 + 8 * k for k in range(10)] + [KEPT])

# --- what the set-up asks of the device ---------------------------------------------------------------------
calls = method_calls(setup, {KEPT, DEVICE})
by_slot = {}
for address, slot in calls:
    by_slot.setdefault(slot, []).append(address)
c.same("custom heap properties (slot 26) asked for heap types 1, 2 and 3",
       [argument_before(a, "r9d") for a in by_slot.get(26, [])], [1, 2, 3])
c.same("the pool queue (slot 8): one call, the queue arriving in global 0x8759ff8",
       [argument_before(a, "r9") for a in by_slot.get(8, [])], [0x8759FF8])
# Read 2026-11-27 (the map said five): four calls of slot 48, one object each.
c.same("slot 48: four calls, each object in its own global",
       [argument_before(a, "r9") for a in by_slot.get(48, [])], [0x75E7038, 0x75E7040, 0x75E7048, 0x75E7050])
# Read 2026-11-27 (the map said one): five command signatures, each without a root signature.
c.same("command signatures (slot 41): five calls, root signature none, each kept in its own global",
       [[argument_before(a, "r8d"), argument_before(a, "r9")] for a in by_slot.get(41, [])],
       [[0, 0x61E0678]] * 5)
c.same("slot 49: one call, with 0x11f and 1, after a version comparison",
       [[argument_before(a, "edx"), argument_before(a, "r8d")] for a in by_slot.get(49, [])], [[0x11F, 1]])
version = [ins for ins in setup[at[by_slot[49][0]] - 12:at[by_slot[49][0]]]
           if ins.mnemonic == "movabs" and ins.operands[1].type == X86_OP_IMM]
c.same("the version the program compares with before slot 49 (four 16-bit parts)",
       [(version[0].operands[1].imm >> s) & 0xFFFF for s in (48, 32, 16, 0)] if version else None,
       [10, 0, 26100, 9628])
order = [slot for _, slot in calls if slot in (8, 26, 41, 48, 49)]
c.same("the order of these calls", order, [49, 26, 26, 26, 8, 48, 48, 48, 48, 41, 41, 41, 41, 41])

# --- section 4: the builder library ---------------------------------------------------------------------------
builder_imports = dict(exe.imports())
methods = [name for name in builder_imports.get("XboxBvhBuilder.dll", [])]
c.same("methods imported from XboxBvhBuilder.dll", len(methods), 7)


def import_slots(library):
    """{function name: address of its import slot} for one library, read from the import directory."""
    pe = struct.unpack_from("<I", exe.data, 0x3C)[0]
    rva = struct.unpack_from("<I", exe.data, pe + 24 + 112 + 8)[0]
    slots = {}
    while True:
        lookup, _time, _chain, name, address = struct.unpack("<IIIII", exe.read(rva, 20))
        if name == 0:
            return slots
        if exe.cstring(name) == library:
            k = 0
            while True:
                value = struct.unpack("<Q", exe.read((lookup or address) + 8 * k, 8))[0]
                if not value:
                    break
                if not value >> 63:
                    slots[exe.cstring((value & 0x7FFFFFFF) + 2)] = address + 8 * k
                k += 1
        rva += 20


builder_slots = import_slots("XboxBvhBuilder.dll")


def sites(name_part):
    """Call sites of the builder method whose decorated name holds name_part, as (address, function)."""
    slot = [address for name, address in builder_slots.items() if name_part in name]
    return [(address, function) for address, function, ins in exe.rip_references(slot[0]) if ins.mnemonic == "call"]


wanted = {"??0XboxBvhBuilder": "constructor", "??1XboxBvhBuilder": "destructor",
          "Set64BitMortonCodes": "Set64BitMortonCodes",
          "GetRaytracingAccelerationStructurePrebuildInfo": "prebuild info",
          "BuildRaytracingAccelerationStructureBatched2X": "build",
          "RehydrateRaytracingAccelerationStructureBatched2X": "rehydrate",
          "CopyRaytracingAccelerationStructure": "copy"}
found = {label: sites(part) for part, label in wanted.items()}
c.same("constructor: one call, at 0x247944f in the set-up function", found["constructor"], [(0x247944F, SETUP)])
c.same("destructor: one call, at 0x248b591 in fn 0x248b570", found["destructor"], [(0x248B591, 0x248B570)])
c.same("Set64BitMortonCodes: 8 calls in 4 functions",
       [len(found["Set64BitMortonCodes"]), sorted({f for _, f in found["Set64BitMortonCodes"]})],
       [8, [0x75480, 0xFE4E50, 0x2490110, 0x56AC100]])
c.same("prebuild info: 3 calls, in fns 0x75480, 0x4ee2a00, 0x56ac100",
       sorted(f for _, f in found["prebuild info"]), [0x75480, 0x4EE2A00, 0x56AC100])
c.same("build: 6 calls in 2 functions",
       [len(found["build"]), sorted({f for _, f in found["build"]})], [6, [0xFE4E50, 0x2490110]])
c.same("rehydrate: 4 calls in fn 0xea6c60 and fn 0x2490a20",
       [len(found["rehydrate"]), sorted({f for _, f in found["rehydrate"]})], [4, [0xEA6C60, 0x2490A20]])
c.same("copy: 2 calls", len(found["copy"]), 2)

ctor = setup[at[0x247944F] - 8:at[0x247944F] + 2]
c.same("the builder object: 0x18 bytes asked for before the constructor",
       [ins.op_str for ins in ctor if ins.mnemonic == "mov" and ins.op_str.startswith("ecx")], ["ecx, 0x18"])
c.same("the constructor gets the device and false, and the object is kept in global 0x75df0a8",
       [exe.rip_target(ctor[5]), ctor[7].op_str, exe.rip_target(ctor[9])], [DEVICE, "r8d, r8d", BUILDER])
dtor = exe.instructions(*exe.function_of(0x248B591))
c.same("the destructor is called on the object read from global 0x75df0a8",
       [exe.rip_target(ins) for ins in dtor if ins.address < 0x248B591 and exe.rip_target(ins) is not None],
       [BUILDER])

c.done()
