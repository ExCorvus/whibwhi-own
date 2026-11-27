"""Repeats the call sites that docs/console-libs/ names for the console libraries besides sound (that is in
audio.py): chad.dll (chad.md), XCRDAPI.dll (xcrdapi-dstorage.md), xg_xs.dll (xg-xs.md), the network libraries
(network.md, sections 0-2) and the run-time loaded xgameruntime.dll (xgameruntime.md, section 1):

- which functions each library is asked for, through which jump, from which function and address;
- the chains of small jumps in front of some of them, and the program's own check right after where the map
  names one.

    python measure/console_libs.py "<your game folder>\\game_scarlett_master.exe"

Reads the executable read-only; nothing is written.
"""
import struct

import numpy as np
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from pe_image import Checks, open_from_arguments

exe = open_from_arguments(__doc__)
c = Checks("console libraries: chad, storage, texture layout, network, game runtime - read in the executable")

va, code = exe._code()


def import_slots(library):
    """{function name or '#ordinal': address of its import slot} for one library, from the import directory."""
    pe = struct.unpack_from("<I", exe.data, 0x3C)[0]
    rva = struct.unpack_from("<I", exe.data, pe + 24 + 112 + 8)[0]
    slots = {}
    while True:
        lookup, _time, _chain, name, address = struct.unpack("<IIIII", exe.read(rva, 20))
        if name == 0:
            return slots
        if exe.cstring(name).lower() == library.lower():
            k = 0
            while True:
                value = struct.unpack("<Q", exe.read((lookup or address) + 8 * k, 8))[0]
                if not value:
                    break
                key = f"#{value & 0xFFFF}" if value >> 63 else exe.cstring((value & 0x7FFFFFFF) + 2)
                slots[key] = address + 8 * k
                k += 1
        rva += 20


def _targets(positions, skip):
    """For candidate byte positions: position + skip + 4 + the signed 32-bit number at position + skip."""
    b = [code[positions + skip + k].astype(np.int64) for k in range(4)]
    rel = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)
    rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
    return va + positions + skip + 4 + rel


_limit = len(code) - 6
_direct_at = np.nonzero((code[:_limit] == 0xE8) | (code[:_limit] == 0xE9))[0]
_direct_target = _targets(_direct_at, 1)
_indirect_at = np.nonzero((code[:_limit] == 0xFF) & ((code[1:_limit + 1] == 0x15) | (code[1:_limit + 1] == 0x25)))[0]
_slot_target = _targets(_indirect_at, 2)


def through_slot(slot):
    """[(address, 'call' or 'jmp')] of every call/jump through an import slot (FF 15 / FF 25), in all code."""
    hits = _indirect_at[_slot_target == slot].tolist()
    return sorted((va + h, "call" if code[h + 1] == 0x15 else "jmp") for h in hits)


def sites_to(target):
    """[(address, 'call' or 'jmp')] of every direct call/jump (E8 / E9) to target, in all code, each decoded."""
    out = []
    for h in _direct_at[_direct_target == target].tolist():
        ins = exe.at(va + h)
        if ins is not None and ins.address == va + h and ins.mnemonic in ("call", "jmp") and \
                ins.operands[0].type == X86_OP_IMM and ins.operands[0].imm == target:
            out.append((va + h, ins.mnemonic))
    return out


def in_function(sites):
    """[(address, function start)] for a list of sites; None where the site lies in no function of .pdata."""
    return [(a, (exe.function_of(a) or (None,))[0]) for a, _ in sites]


def size(function):
    begin, end = exe.function_of(function)
    return end - begin if begin == function else None


def imports_of(library):
    return dict(exe.imports()).get(library, [])


def immediates(function):
    return {ins.operands[-1].imm for ins in exe.instructions(*exe.function_of(function))
            if ins.operands and ins.operands[-1].type == X86_OP_IMM}


def calls_in(function):
    """[(address, target)] of the direct calls in a function, in order."""
    return [(ins.address, ins.operands[0].imm) for ins in exe.instructions(*exe.function_of(function))
            if ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM]


def guid(rva):
    a, b, c_, d = struct.unpack("<IHH8s", exe.read(rva, 16))
    return f"{a:08x}-{b:04x}-{c_:04x}-{d[:2].hex()}-{d[2:].hex()}"


# --- chad.dll (chad.md) --------------------------------------------------------------------------------------
chad = import_slots("chad.dll")
c.same("chad.dll: the five functions the executable imports", sorted(chad),
       ["XDspActivate", "XDspConnectWithMaximumStreamLimit", "XDspDeactivate", "XDspDisconnect", "XDspSubmitCommand"])
thunk = {name: [a for a, kind in through_slot(slot) if kind == "jmp"] for name, slot in chad.items()}
c.same("chad.dll: each is reached through one jump (the map's import thunks)", thunk,
       {"XDspActivate": [0x5F3FEF0], "XDspConnectWithMaximumStreamLimit": [0x5F3FF00],
        "XDspDeactivate": [0x5F3FF10], "XDspDisconnect": [0x5F3FF20], "XDspSubmitCommand": [0x5F3FF30]})

c.same("XDspActivate: one call, at 0x11d475d in fn 0x11d4750 (49 bytes)",
       [in_function(sites_to(0x5F3FEF0)), size(0x11D4750)], [[(0x11D475D, 0x11D4750)], 49])
pdata_hits = exe.data.count(struct.pack("<I", 0x11D4750))
# Read 2026-11-27 (the map said no referrer at all, dead code): a small function at 0x11d4720 with no .pdata entry
# ends in a jump to the wrapper, and two functions call it.
c.same("XDspActivate: its wrapper is reached by one jump, from 0x11d4740; its address is in the file once (.pdata)",
       [sites_to(0x11D4750), pdata_hits], [[(0x11D4740, "jmp")], 1])
c.same("XDspActivate: that jump ends the function at 0x11d4720 (after the int3 filler), which picks a record of "
       "0x4c8 bytes at +0x488",
       [exe.at(0x11D471F).mnemonic, [ins.address for ins in exe.instructions(0x11D4720, 0x11D4745)][-1],
        0x4C8 in [ins.operands[-1].imm for ins in exe.instructions(0x11D4720, 0x11D4745)
                  if ins.operands and ins.operands[-1].type == X86_OP_IMM]], ["int3", 0x11D4740, True])
c.same("XDspActivate: fn 0x11d4720 is called at 0x3b97e (in fn 0x3b770) and 0x43b3966 (in fn 0x43b3920)",
       in_function(sites_to(0x11D4720)), [(0x3B97E, 0x3B770), (0x43B3966, 0x43B3920)])
handed = []
for site in (0x3B97E, 0x43B3966):
    body = exe.instructions(*exe.function_of(site))
    k = [i for i, ins in enumerate(body) if ins.address == site][0]
    r9 = [ins.operands[1].mem.disp for ins in body[k - 8:k]
          if ins.mnemonic == "lea" and ins.reg_name(ins.operands[0].reg) == "r9"]
    result = [ins.operands[0].mem.disp for ins in body[k + 1:k + 2]
              if ins.mnemonic == "mov" and ins.operands[0].type == X86_OP_MEM]
    handed.append([r9, result])
c.same("XDspActivate: the callers hand the address of the stream's +0x70 (fn 0x3b770) or +0x38 (fn 0x43b3920) "
       "and keep the result at +0x60 / +0x28", handed, [[[0x70], [0x60]], [[0x38], [0x28]]])
c.same("XDspActivate: the counter at 0x9aea514 is used only by that wrapper",
       sorted({f for _, f, _ in exe.rip_references(0x9AEA514)}), [0x11D4750])

# Read 2026-11-27: between 0x1166020 and the library there is one more jump, the import thunk 0x5f3ff10.
c.same("XDspDeactivate: 0x1165fd7 -> jump 0x1166010 -> jump 0x1166020 -> thunk 0x5f3ff10",
       [exe.at(0x1165FD7).operands[0].imm, exe.at(0x1166010).operands[0].imm, exe.at(0x1166020).operands[0].imm],
       [0x1166010, 0x1166020, 0x5F3FF10])
c.same("XDspDeactivate: nothing else reaches that chain",
       [sites_to(0x5F3FF10), sites_to(0x1166020), in_function(sites_to(0x1166010))],
       [[(0x1166020, "jmp")], [(0x1166010, "jmp")], [(0x1165FD7, 0x1165F80)]])
c.same("XDspDeactivate: its caller fn 0x1165f80 (0x84 bytes) is called by fn 0x46030 and fn 0x161cc50",
       [size(0x1165F80), exe.callers_and_jumps(0x1165F80)], [0x84, [0x46030, 0x161CC50]])

c.same("XDspConnectWithMaximumStreamLimit: one call, at 0x5b5262e in fn 0x5b52600",
       in_function(sites_to(0x5F3FF00)), [(0x5B5262E, 0x5B52600)])
c.same("XDspConnect: fn 0x5b525d0 is reached from fn 0x5b52510", exe.callers_and_jumps(0x5B525D0), [0x5B52510])
c.same("XDspConnect: fn 0x5b52600 is reached only from fn 0x5b525d0, by a jump",
       [(exe.function_of(a)[0], kind) for a, kind in sites_to(0x5B52600)], [(0x5B525D0, "jmp")])
c.same("XDspConnect: fn 0x5b52510 is called once, by the audio start fn 0x43ad8b0",
       in_function(sites_to(0x5B52510)), [(0x43ADA52, 0x43AD8B0)])
c.same("XDspConnect: 24 streams with 10 MiB, or 30 streams with 30 MiB",
       {24, 30, 10 << 20, 30 << 20} <= immediates(0x5B52510), True)

# fn 0x43ad8b0: its order (chad.md section 0).
start = exe.instructions(*exe.function_of(0x43AD8B0))
order = []
for ins in start:
    if ins.mnemonic != "call":
        continue
    target = exe.rip_target(ins)
    if target is not None and exe.import_at(target):
        order.append(exe.import_at(target)[1])
    elif ins.operands[0].type == X86_OP_IMM and ins.operands[0].imm in (0x43AEC40, 0x43AEE50, 0x5B52510):
        order.append(hex(ins.operands[0].imm))
    elif ins.operands[0].type == X86_OP_IMM:
        inner = exe.at(ins.operands[0].imm)
        t = exe.rip_target(inner) if inner is not None and inner.mnemonic == "jmp" else None
        if t is not None and exe.import_at(t):
            order.append(exe.import_at(t)[1])
wanted = [x for x in order if x in ("CoInitializeEx", "ApuCreateHeap", "0x43aec40", "0x43aee50", "0x5b52510")]
c.same("the audio start fn 0x43ad8b0: CoInitializeEx, ApuCreateHeap, fn 0x43aec40, fn 0x43aee50, fn 0x5b52510",
       wanted, ["CoInitializeEx", "ApuCreateHeap", "0x43aec40", "0x43aee50", "0x5b52510"])

c.same("XDspSubmitCommand: wrapper fn 0x4e5b200 (28 bytes) calls the thunk at 0x4e5b20d",
       [size(0x4E5B200), in_function(sites_to(0x5F3FF30))], [28, [(0x4E5B20D, 0x4E5B200)]])
c.same("XDspSubmitCommand: the wrapper is reached through the jump at 0x4e5b010 only",
       sites_to(0x4E5B200), [(0x4E5B010, "jmp")])
c.same("XDspSubmitCommand: six calls of that jump, in three functions", in_function(sites_to(0x4E5B010)),
       [(0x43B2D45, 0x43B2A40), (0x43B3A63, 0x43B3980), (0x43B3B02, 0x43B3980), (0x43B3BA2, 0x43B3980),
        (0x43B3C49, 0x43B3980), (0x4E5AF74, 0x4E5ACC0)])

disconnect = []
target = 0x5F3FF20
for _ in range(4):
    s = sites_to(target)
    disconnect.append([(a, kind, (exe.function_of(a) or (None,))[0]) for a, kind in s])
    if len(s) != 1 or exe.function_of(s[0][0]):
        break
    target = s[0][0]
c.same("XDspDisconnect: reached from the teardown fn 0x43ae950",
       [f for a, kind, f in disconnect[-1]], [0x43AE950])
c.same("XDspDisconnect: fn 0x43ae950 is reached from the teardown fn 0x43ada70",
       [exe.function_of(a)[0] for a, _ in sites_to(0x43AE950) if exe.function_of(a)], [0x43ADA70])

# --- XCRDAPI.dll (xcrdapi-dstorage.md section 1) ---------------------------------------------------------------
storage = import_slots("XCRDAPI.dll")
c.same("XCRDAPI.dll: one function imported", sorted(storage), ["DStorageGetFactory"])
storage_thunk = [a for a, kind in through_slot(storage["DStorageGetFactory"]) if kind == "jmp"]
c.same("DStorageGetFactory: one call, at 0x1d8e4f0 in fn 0x1d8e4d0 (86 bytes)",
       [[in_function(sites_to(t)) for t in storage_thunk], size(0x1D8E4D0)], [[[(0x1D8E4F0, 0x1D8E4D0)]], 86])
setup = exe.instructions(*exe.function_of(0x1D8E4D0))
leas = {ins.reg_name(ins.operands[0].reg): exe.rip_target(ins) for ins in setup
        if ins.mnemonic == "lea" and ins.address < 0x1D8E4F0 and exe.rip_target(ins) is not None}
c.same("DStorageGetFactory: asked with the interface id at 0x62fc7d8, the factory arriving in 0x78f5e78",
       [leas.get("rcx"), leas.get("rdx")], [0x62FC7D8, 0x78F5E78])
# Read 2026-11-27 (the map had ...-4888-... in its third group): the bytes 84 48 give 0x4884.
c.same("the interface id there (not the PC's IID_IDStorageFactory 6924ea0c-...)", guid(0x62FC7D8),
       "62959d11-d45f-4884-a0a3-f7862ea243ae")
stores = [ins.address for ins in setup
          if ins.mnemonic == "mov" and ins.operands[0].type == X86_OP_MEM and exe.rip_target(ins) == 0x78F5E80]
table_lea = [exe.rip_target(ins) for ins in setup if ins.mnemonic == "lea" and exe.rip_target(ins) == 0x62FC550]
c.same("before it, the table 0x62fc550 is written into 0x78f5e80", [len(stores), table_lea], [1, [0x62FC550]])
c.same("no failure path: an int3 follows the checks",
       any(ins.mnemonic == "int3" for ins in setup), True)
c.same("the factory's method +0x40 is called with 0x2000000 (32 MiB)",
       [any(ins.mnemonic == "call" and ins.operands[0].type == X86_OP_MEM and ins.operands[0].mem.disp == 0x40
            for ins in setup), 0x2000000 in immediates(0x1D8E4D0)], [True, True])
c.same("the once-only guard fn 0x1d8e470 in front of it has six callers",
       [[exe.function_of(a)[0] for a, _ in sites_to(0x1D8E4D0) if exe.function_of(a)],
        len(exe.callers_and_jumps(0x1D8E470))], [[0x1D8E470], 6])

# --- xg_xs.dll (xg-xs.md sections 1, 2.1, 4) ---------------------------------------------------------------------
xg = import_slots("xg_xs.dll")
xg_thunk = {name: [a for a, kind in through_slot(slot) if kind == "jmp"] for name, slot in xg.items()}
c.same("xg_xs.dll: two functions, through the thunks 0x5f3ff40 and 0x5f3ff50", xg_thunk,
       {"XGCreateTextureComputer": [0x5F3FF40], "XGSuggestSwizzleMode": [0x5F3FF50]})
c.same("XGCreateTextureComputer: four calls", in_function(sites_to(0x5F3FF40)),
       [(0x344235, 0x3441F0), (0x979C75, 0x979C40), (0x16F7141, 0x16F7100), (0x5121A5E, 0x5121980)])
c.same("XGSuggestSwizzleMode: two calls", sorted(in_function(sites_to(0x5F3FF50))),
       [(0xC4B3CE, 0xC4B030), (0x4ECCCA2, 0x4ECC940)])
shape = []
for site in (0x4ECCCA2, 0xC4B3CE):
    body = exe.instructions(*exe.function_of(site))
    k = [i for i, ins in enumerate(body) if ins.address == site][0]
    edx_zero = any(ins.mnemonic == "xor" and ins.op_str == "edx, edx" for ins in body[max(0, k - 3):k])
    # r8 is formed earlier in the function as rcx + 0x2c (directly, or through rax and "mov r8, rax").
    r8 = sorted({ins.operands[1].mem.disp for ins in body[:k] if ins.mnemonic == "lea"
                 and ins.reg_name(ins.operands[0].reg) in ("r8", "rax") and ins.reg_name(ins.operands[1].mem.base) == "rcx"
                 and ins.operands[1].mem.disp == 0x2C})
    after = [ins.operands[0].imm for ins in body[k + 1:k + 14] if ins.mnemonic == "jmp" and
             ins.operands[0].type == X86_OP_IMM][:1]
    shape.append([edx_zero, r8, after])
c.same("XGSuggestSwizzleMode: both calls pass 0 and the description's +0x2c, then jump to fn 0x4e9b660",
       shape, [[True, [0x2C], [0x4E9B660]]] * 2)
c.same("the callers of the four texture-computer functions",
       [exe.callers_and_jumps(f) for f in (0x3441F0, 0x979C40, 0x5121980)],
       [[0x51FB680], [0x979BC0], [0x5121690, 0x555E9E0]])
c.same("fn 0x16f7100: called at 0x193df4b, and its alias 0x16f70f0 at 0x193e26f",
       [in_function(sites_to(0x16F7100)), in_function(sites_to(0x16F70F0))],
       [[(0x16F70F0, None), (0x193DF4B, 0x193DE10)], [(0x193E26F, 0x193DE10)]])

# --- the network libraries (network.md sections 0-2) -----------------------------------------------------------------
ws2 = import_slots("WS2_32.dll")
c.same("WS2_32.dll: 25 functions, 23 by ordinal",
       [len(ws2), sum(1 for k in ws2 if k.startswith("#"))], [25, 23])
c.same("WINHTTP.dll and libHttpClient.GDK.dll: 15 and 38 functions",
       [len(imports_of("WINHTTP.dll")), len(imports_of("libHttpClient.GDK.dll"))], [15, 38])


def ws2_calls(ordinal):
    return [(a, exe.function_of(a)[0] if exe.function_of(a) else None)
            for a, kind in through_slot(ws2[f"#{ordinal}"]) if kind == "call"]


start_up = ws2_calls(115)
c.same("WSAStartup (#115): one call, at 0x5a61172 in fn 0x5a61140 (71 bytes)",
       [start_up, size(0x5A61140)], [[(0x5A61172, 0x5A61140)], 71])
after = exe.instructions(0x5A61172, 0x5A61172 + 0x20, 4)
c.same("WSAStartup: asked for version 2; test eax / jne to 0x5a61184, which is an int3",
       [2 in immediates(0x5A61140), [ins.mnemonic for ins in after[1:3]], after[2].operands[0].imm,
        exe.at(0x5A61184).mnemonic], [True, ["test", "jne"], 0x5A61184, "int3"])
chain = [exe.callers_and_jumps(f) for f in (0x5A61140, 0x5BF0410, 0x266EA0, 0x266CF0, 0x266BE0)]
c.same("WSAStartup is reached from 0x5bf0410 <- 0x266ea0 <- 0x266cf0 <- 0x266be0 <- (0x2669c0, 0x2208b90)",
       chain, [[0x5BF0410], [0x266EA0], [0x266CF0], [0x266BE0], [0x2669C0, 0x2208B90]])
c.same("socket (#23): four calls", ws2_calls(23),
       [(0x28C734F, 0x28C7320), (0x41E2FE1, 0x41E2FC0), (0x41E66A6, 0x41E6680), (0x58AAFAB, 0x58AAF80)])
c.same("bind (#2): three calls; listen (#13): one; accept (#1): one, in fn 0x28c73b0",
       [[a for a, _ in ws2_calls(2)], [a for a, _ in ws2_calls(13)], ws2_calls(1)],
       [[0x28C736C, 0x41E6742, 0x41E67A9], [0x28C737F], [(0x28C73DF, 0x28C73B0)]])
c.same("connect (#4): two calls", ws2_calls(4), [(0x41E2E20, 0x41E2DD0), (0x58AAFC8, 0x58AAF80)])
c.same("fn 0x41e2dd0 tolerates 10035 and 10036 after a connect", {10035, 10036} <= immediates(0x41E2DD0), True)

# --- xgameruntime.dll, loaded at run time (xgameruntime.md section 1) ----------------------------------------------
c.same("xgameruntime.dll is not an import", any(lib.lower() == "xgameruntime.dll" for lib, _ in exe.imports()), False)
c.same("fn 0x55e29dc: load flags 0xa00 or 0x800, failure 0x89240101",
       {0xA00, 0x800, 0x89240101} <= immediates(0x55E29DC), True)
resolved = sorted({exe.rip_target(ins) for ins in exe.instructions(*exe.function_of(0x55E2A70))
                   if exe.rip_target(ins) is not None and 0x6E04E80 <= exe.rip_target(ins) <= 0x6E04ED8})
c.same("fn 0x55e2a70: the lock 0x6e04e80 and the six cached exports 0x6e04eb0..0x6e04ed8",
       [0x6E04E80 in resolved, [g for g in resolved if g >= 0x6E04EB0]],
       [True, [0x6E04EB0 + 8 * k for k in range(6)]])
c.same("the unload path fn 0x55dcf80 has one caller, fn 0x5a91bd0", exe.callers_and_jumps(0x55DCF80), [0x5A91BD0])
wrappers = exe.callers(0x55DE048)
c.same("the query helper fn 0x55de048: 126 bytes, called by 161 functions, all in 0x55dca58..0x55e2ebb",
       [size(0x55DE048), len(wrappers), all(0x55DCA58 <= w <= 0x55E2EBB for w in wrappers)], [126, 161, True])
c.same("it rewrites 0x80040154 to 0x89240107", {0x80040154, 0x89240107} <= immediates(0x55DE048), True)
poke = exe.instructions(*exe.function_of(0x55DDFF0))
# The methods are called through the program's indirect-call helper fn 0x56a2070, with the method in rax.
slots_called = [poke[i - 1].operands[1].mem.disp for i, ins in enumerate(poke)
                if ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM and ins.operands[0].imm == 0x56A2070
                and poke[i - 1].mnemonic == "mov" and poke[i - 1].operands[1].type == X86_OP_MEM]
c.same("fn 0x55ddff0 calls method +0xe8, then Release (+0x10)", slots_called, [0xE8, 0x10])
c.same("58 of the 161 wrappers call fn 0x55ddff0", len(set(exe.callers(0x55DDFF0)) & set(wrappers)), 58)

c.done()
