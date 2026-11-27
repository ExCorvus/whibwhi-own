"""Repeats the audio measurements of FINDINGS.md (2026-11-27, 'the program's audio setup' and its follow-ups).

    python measure/audio.py "<your game folder>\\game_scarlett_master.exe"

Reads the executable read-only and prints, for each statement, 'same' or 'DIFFERS' against what FINDINGS.md says.
Only addresses and the numbers found there are kept here; what they mean is said in words.
"""
from pe_image import Checks, open_from_arguments

m = open_from_arguments(__doc__)
c = Checks("audio: the sound device, its records, and the stream channel")
n, o = m.number, m.offset


def users(target):
    return sorted({function for _a, function, _i in m.rip_references(target)})


# The setup (fn 0x43aec40)
c.same("the setup asks for 350 sound records (the count it stores at 0x9ae98e0)", (o(0x43aec68), n(0x43aec68)),
       (0x9ae98e0, 350))
writes = [a for a, _f, _i in m.rip_references(0x9ae98c8) if m.writes_memory(a)]
c.same("the status word 0x9ae98c8 is written by the program only at", writes, [0x43aec56])
c.same("  ... with the value", n(0x43aec56), 0)
c.same("setup command: opcode", n(0x43aee0e), 2)
c.same("setup command: its magic value", n(0x43aee08), 0xF0F0F0F0)
c.same("setup command: the one value in its block", n(0x43aee19), 0xC0)

# The XMA2 packet walk (fn 0x55cbd10)
c.same("packet walk: keeps the low 3 bits of byte +2", (o(0x55cbd23), n(0x55cbd27)), (2, 7))
c.same("packet walk: requires them to be", n(0x55cbd2a), 1)
c.same("packet walk: top 6 bits of byte +0 (shift right by)", n(0x55cbd3b), 2)
c.same("packet walk: times 512 samples (shift left by)", n(0x55cbd41), 9)
c.same("packet walk: byte +3 times 2048 (shift left by)", (o(0x55cbd33), n(0x55cbd3e)), (3, 11))
c.same("packet walk: plus one packet of", n(0x55cbd44), 0x800)
c.same("packet walk is called by the voice start fn 0x785d0", 0x785D0 in m.callers(0x55CBD10), True)

# The records
c.same("functions that use the records' pointer 0x9ae9908", users(0x9AE9908), [0x78120, 0x785D0, 0x43AEC40])
c.same("voice start: rate class bounds (Hz)", [n(0x786C3), n(0x786E0), n(0x786FE)], [24000, 32000, 44100])
c.same("voice start: ring size 0x17 x 256 bytes, in bits 22-26 of +0", n(0x78763), 0x17 << 22)
c.same("voice start: XMA2 data must be aligned to", n(0x78AF8), 0x800)
c.same("voice start: XMA2 data address stored into record fields", [o(0x78B0E), o(0x78B52)], [0x14, 0x18])
c.same("refill reads the device's position in record field", o(0x78205), 0x24)
c.same("refill reads the device's other position from bits 27-31 of +0 (shift by)", n(0x78214), 0x13)
record_writes = []
for begin in (0x78120, 0x785D0):
    for ins in m.instructions(begin):
        if ins.operands and m.writes_memory(ins.address) and m.offset(ins.address) == 0x24 and ins.operands[0].mem.index:
            record_writes.append(ins.address)
c.same("writes to record field +0x24 in the refill and the voice start", record_writes, [])
c.same("refill: the ring wraps at", n(0x784B6), 0x1700)
c.same("refill: positions counted per channel (object field)", o(0x784D4), 0x2004)
c.same("refill: data command opcode", n(0x78518), 0x12)
c.same("refill: data command magic value", n(0x784FD), 0xF1F1F1F1)
c.same("ring read: decoded-up-to position = bits 27-31 of record +0, in 256-byte units", [n(0x78214), n(0x7821B)],
       [0x13, 0x1F00])
c.same("ring read: taken-out-up-to position = low 5 bits of record +0x24, in 256-byte units",
       [o(0x78205), n(0x78211), n(0x78218)], [0x24, 0x1F, 8])
c.same("ring read: ring size = bits 22-26 of record +0", [n(0x7822E), n(0x78231)], [0xE, 0x1F00])
c.same("ring read: both positions equal -> the sign of record +4 decides empty or full",
       [o(0x782E6), m.at(0x782EB).mnemonic], [4, "js"])

# Start and stop
c.same("start command (fn 0x43a73e0): opcode and magic value", [n(0x43A73F2), n(0x43A73EC)], [5, 0xF6F6F6F6])
c.same("start command: voice table field +0x10 becomes", (o(0x43A7428), n(0x43A7428)), (0x10, 2))
c.same("stop command (fn 0x925e0): opcode", n(0x925F6), 7)
c.same("stop command: voice table field +0x18 becomes", (o(0x9262C), n(0x9262C)), (0x18, 1))
c.same("who stops voices", m.callers(0x925E0), [0x78120, 0x92580, 0x50F2AE0])

# The stream channel
c.same("stream submit fn 0x4f20ad0 is called from", m.callers(0x4F20AD0), [0x4F20990])
c.same("stream submit: opcode", n(0x4F20B25), 3)
c.same("stream submit: samples per packet", n(0x4F20B3E), 960)
c.same("stream submit: output area per stream (bytes)", n(0x4F20B6B), 0x1E00)
c.same("stream submit: output bytes per channel", n(0x4F20B91), 0xF00)
c.same("functions that use the payload area 0x9ae98a8", users(0x9AE98A8),
       [0x43AEE50, 0x4E64B10, 0x4F20AD0, 0x5A87480])
c.same("  ... fn 0x5a87480 (start-up initialisation of this data) only writes it", m.writes_memory(0x5A8797C), True)
c.same("stream output: the scale constant at 0x615b954", m.float32(0x615B954), 32767.0)
c.same("stream output: float samples read from the output area at offset", o(0x4E64B97), 0x14010)
c.same("stream output: packed to signed 16-bit", m.at(0x4E64BC5).mnemonic, "vpackssdw")
c.same("stream output: into the program's own ring at object field", o(0x4E64BD8), 0x100)

# What the voice start (fn 0x785d0) hands the chip
c.same("voice start: the data address must be 2048-aligned (low bits tested)", n(0x785E6), 0x7FF)
c.same("voice start: rate classes by the stream's sample rate (object field +0xc)",
       [o(0x786BD), n(0x786C3), n(0x786E0), n(0x786FE)], [0xC, 24000, 32000, 44100])
c.same("voice start: rate class written into bits 27-28 of record +4", [n(0x786F0), n(0x7870C), n(0x78714)],
       [0x8000000, 0x10000000, 0x18000000])
c.same("voice start: channel bit 29 of record +4 from the object's channel count",
       [o(0x7867C), n(0x786A1), n(0x786A6)], [0x2004, 0x1D, 0x20000000])
c.same("voice start: ring size 0x17 into bits 22-26 of record +0", n(0x78763), 0x5C00000)
c.same("voice start: ring and second area handed as chip addresses (record +0x1c, +0x20)",
       [o(0x787A1), o(0x787F4)], [0x1C, 0x20])
c.same("voice start: walks the XMA2 packets, finds the loop, counts the samples",
       [m.at(a).op_str for a in (0x78831, 0x78850, 0x78931)], ["0x55cbd10", "0x43a7440", "0x55cbd70"])
c.same("voice start: length in 2048-byte packets into the low 12 bits of record +0", [n(0x78AC5), n(0x78ADB)],
       [0x7FF800, 0xB])
c.same("voice start: the data address into record +0x14 and +0x18", [o(0x78B0E), o(0x78B52)], [0x14, 0x18])
c.same("voice start: loop count 255 (bits 12-19 of record +0) when the loop count is over 4", n(0x78919), 0xFF000)
c.same("voice start: first frame's bit position into the low 26 bits of record +8", n(0x78BA5), 0xFC000000)
c.done()
