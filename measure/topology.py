"""Repeats the topology measurements of FINDINGS.md (2026-11-27: 'the program's topology numbers 0x11 and 0x13 are
the AMD rectangle list and quad list', 'can the PC reach the GPU's own rectangle and quad modes?', and the class
the draw path makes for the pipeline key).

    python measure/topology.py <path to the executable in your copy>

Only the executable's part is repeated here; the parts read in AMD's public code and in the installed driver
depend on the reader's own PC and are described in FINDINGS.md.
"""
import re

from pe_image import Checks, open_from_arguments

m = open_from_arguments(__doc__)
c = Checks("topology: the renderer's topology number, rectangles (0x11) and quads (0x13)")
n = m.number

# Every instruction that addresses the context field +0x1237d (the four displacement bytes, then decode back).
name, va, vsize, raw, rawsize = m.section(".text")
code = m.data[raw:raw + rawsize]
places = {}
for hit in re.finditer(rb"\x7d\x23\x01\x00", code):
    for back in range(2, 9):
        start = va + hit.start() - back
        try:
            ins = m.at(start)
        except ValueError:
            continue
        if "+ 0x1237d]" in ins.op_str and ins.address + ins.size >= va + hit.start() + 4:
            places[ins.address] = ins
            break
c.same("instructions that address the topology field +0x1237d", len(places), 430)

writes = {}
stores = 0
for a, ins in places.items():
    if ins.mnemonic == "mov" and ins.op_str.startswith("byte ptr [") and m.writes_memory(a):
        stores += 1
        try:
            writes[a] = n(a)
        except ValueError:
            pass
values = list(writes.values())
c.same("  ... of them storing a byte into it", stores, 211)
c.same("  ... of those storing a fixed number", len(writes), 189)
c.same("places that write 0x11 (rectangle list)", values.count(0x11), 39)
c.same("places that write 0x13 (quad list)", values.count(0x13), 2)

by_function = {}
for a, v in writes.items():
    f = m.function_of(a)
    by_function.setdefault(f[0] if f else None, []).append(v)
special = {f: vs for f, vs in by_function.items() if 0x11 in vs or 0x13 in vs}
c.same("functions that set 0x11 or 0x13", len(special), 37)
c.same("  ... of them setting no other number", sum(1 for vs in special.values() if set(vs) <= {0x11, 0x13}), 33)
c.same("fn 0x5178b90 sets a triangle list and a rectangle list", (writes.get(0x5178E69), writes.get(0x5179495)),
       (4, 0x11))

# The rectangle drawn at 0x124995: three vertices of 24 bytes asked from fn 0x517a290.
site = [ins for ins in m.instructions(0x124995, 0x124995 + 0x100) if ins.mnemonic == "call"][0]
c.same("after setting 0x11 at 0x124995 the program asks a vertex area from", int(site.op_str, 16), 0x517A290)
args = {}
for ins in m.instructions(0x124995, site.address):
    if ins.mnemonic == "mov" and ins.op_str.split(",")[0] in ("edx", "r8d"):
        args[ins.op_str.split(",")[0]] = n(ins.address)
c.same("  ... with a count and a vertex size of", (args.get("edx"), args.get("r8d")), (3, 0x18))

# The class for the pipeline key (fn 0x32520, 0x32710-0x3273e): the numbers it compares the topology with.
compared = sorted({n(i.address) for i in m.instructions(0x32710, 0x3273F) if i.mnemonic == "cmp"})
c.same("draw path: the topology numbers the pipeline-key class compares with", compared, [1, 2, 0x11, 0x20])
c.same("draw path: the description request (fn 0xb5a5d0) is called at 0x3275f",
       int(m.at(0x3275F).op_str, 16), 0xB5A5D0)
c.done()
