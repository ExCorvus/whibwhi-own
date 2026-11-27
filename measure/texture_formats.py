"""Repeats the texture format measurement of FINDINGS.md (2026-11-27: 'the console format 118: its bits per pixel
from the program's own table').

    python measure/texture_formats.py "<your game folder>\\game_scarlett_master.exe"

fn 0x4d9f3c0 takes a format number and gives its bits per pixel from a table of 32-bit numbers (entry = format - 1;
a format above 127 gives 0). The public formats are checked first, so that the table is known to be that list; then
the console's own numbers next to 118, which the weave's texture library (weave/xg_xs.cpp) takes from here.
"""
import struct

from pe_image import Checks, open_from_arguments

m = open_from_arguments(__doc__)
c = Checks("texture formats: bits per pixel in the program's own table (fn 0x4d9f3c0)")

lea = m.at(0x4D9F3C6)
c.same("fn 0x4d9f3c0 loads its table with", (lea.mnemonic, lea.op_str.split(",")[0]), ("lea", "rdx"))
table = 0x4D9F3CD + struct.unpack("<i", m.read(0x4D9F3C9, 4))[0]
c.same("the table sits at", hex(table), hex(0x61ED5B8))


def bits(fmt):
    return struct.unpack("<I", m.read(table + 4 * (fmt - 1), 4))[0]


public = {2: 128, 10: 64, 24: 32, 28: 32, 40: 32, 55: 16, 61: 8, 71: 4, 77: 8, 98: 8, 115: 16}
c.same("public formats 2, 10, 24, 28, 40, 55, 61, 71, 77, 98, 115", {f: bits(f) for f in public}, public)
c.same("console formats 116 to 120", [bits(f) for f in range(116, 121)], [0, 0, 16, 16, 16])
c.same("console formats 121 to 127", [bits(f) for f in range(121, 128)], [4, 4, 4, 4, 0, 0, 8])
c.done()
