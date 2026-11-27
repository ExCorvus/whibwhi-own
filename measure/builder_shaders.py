"""Repeats the measurements of FINDINGS.md about the installation's ray-tracing builder library and its shaders
(2026-11-26/27: the builder's root signature bits, insertvalue, the console shader compiler's functions, the lane
count switch).

    python measure/builder_shaders.py "<your game folder>\\XboxBvhBuilder.dll"

Reads the library read-only, in memory; prints counts and names only.
"""
import collections
import sys

from dxil_bitcode import containers
from pe_image import Checks

if len(sys.argv) < 2:
    print(__doc__)
    sys.exit(2)
with open(sys.argv[1], "rb") as f:
    data = f.read()
c = Checks("builder shaders: the 52 shaders inside the installation's XboxBvhBuilder.dll")
shaders = containers(data)
c.same("shader containers in the library", len(shaders), 52)
c.same("their parts, the same in every one", sorted({",".join(s.order) for s in shaders}),
       ["SFI0,ISG1,OSG1,PSV0,RTS0,STAT,ILDN,HASH,DXIL"])
c.same("root signature flags carried by every one", sorted({s.root_signature_flags() for s in shaders}), [0x3000000])
c.same("shader feature flags (value: shaders)", sorted(collections.Counter(s.feature_flags() for s in shaders).items()),
       [(0, 20), (0x8000, 20), (0xC000, 9), (0x44000, 2), (0x4C000, 1)])
stages = [s.stage_and_threads() for s in shaders]
c.same("shader stage of every one (5 = compute)", sorted({st[0] for st in stages}), [5])
c.same("thread-group width (width: shaders)", sorted(collections.Counter(st[3][0] for st in stages).items()),
       [(1, 1), (32, 35), (64, 4), (256, 1), (512, 4), (1024, 7)])

modules = [s.module() for s in shaders]
c.same("bitcode read in every shader", sum(1 for x in modules if x is not None), 52)
insert = sorted((s.size, mod.function_records[27]) for s, mod in zip(shaders, modules) if mod.function_records[27])
c.same("shaders using insertvalue (record 27): size and count", insert,
       [(5136, 13), (5256, 8), (11052, 16), (11840, 16), (16184, 64)])
# The function-record kinds dxil-spirv's reader took before the change (bc/module.cpp, vkd3d-proton 71cae09).
handled = {1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 15, 16, 19, 20, 26, 28, 29, 33, 34, 35, 38, 43, 44, 46, 56}
used = set().union(*(set(mod.function_records) for mod in modules))
c.same("record kinds used that the reader did not take", sorted(used - handled), [27])
console = sorted(set().union(*({n for n in mod.declared if n.startswith("xbox.")} for mod in modules)))
c.same("distinct console shader compiler functions declared", len(console), 33)
c.same("shaders declaring none of them",
       sum(1 for mod in modules if not any(n.startswith("xbox.") for n in mod.declared)), 9)
wave32 = sum(1 for mod in modules if "__XBOX_ENABLE_WAVE32" in mod.metadata_strings)
c.same("shaders built with the console compiler's 32-lane switch", wave32, 42)
compiler = sorted({x for mod in modules for x in mod.metadata_strings if x.startswith("dxcxbox")})
c.same("the compiler named in every shader", compiler, ["dxcxbox 2504.2509.11601.10011"])
c.done()
