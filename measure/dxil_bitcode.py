"""Reads shader containers (DXBC) and the LLVM bitcode in their DXIL parts, for the measurement scripts.

Our own small reader: it walks the bitcode's blocks and keeps only counts and names, read-only and in memory.
Nothing of a shader is written anywhere.
"""
import collections
import struct


class Bits:
    def __init__(self, data):
        self.v = int.from_bytes(data, "little")
        self.n = len(data) * 8
        self.p = 0

    def read(self, w):
        x = (self.v >> self.p) & ((1 << w) - 1)
        self.p += w
        return x

    def vbr(self, w):
        out, shift = 0, 0
        while True:
            x = self.read(w)
            out |= (x & ((1 << (w - 1)) - 1)) << shift
            shift += w - 1
            if not x >> (w - 1):
                return out

    def align32(self):
        self.p = (self.p + 31) & ~31


def _scalar(b, op):
    kind, val = op
    if kind == "lit":
        return val
    if kind == 1:
        return b.read(val)
    if kind == 2:
        return b.vbr(val)
    if kind == 4:  # char6: a-z, A-Z, 0-9, '.', '_'
        return ord("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._"[b.read(6)])
    raise ValueError(kind)


def _abbrev_definition(b):
    ops = []
    for _ in range(b.vbr(5)):
        if b.read(1):
            ops.append(("lit", b.vbr(8)))
        else:
            e = b.read(3)
            ops.append((e, b.vbr(5) if e in (1, 2) else 0))
    return ops


def _abbrev_record(b, ops):
    vals = []
    i = 0
    while i < len(ops):
        k = ops[i][0]
        if k == 3:  # array
            count = b.vbr(6)
            vals.extend(_scalar(b, ops[i + 1]) for _ in range(count))
            i += 2
            continue
        if k == 5:  # blob
            count = b.vbr(6)
            b.align32()
            b.p += count * 8
            b.align32()
            vals.append(count)
        else:
            vals.append(_scalar(b, ops[i]))
        i += 1
    return vals[0], vals[1:]


class Module:
    """What one shader's bitcode holds: record kinds of its function bodies, declared functions, metadata text."""

    def __init__(self, bitcode):
        self.function_records = collections.Counter()  # record code -> count (function blocks, id 12)
        self.declared = set()  # names of functions declared without a body
        self.metadata_strings = []
        if bitcode[:4] != b"BC\xc0\xde":
            raise ValueError("not LLVM bitcode")
        self._values = []
        self._names = {}
        self._blockinfo = collections.defaultdict(list)
        self.b = Bits(bitcode[4:])
        self._block(2, -1, [], None)
        for vid, (kind, prototype) in enumerate(self._values):
            if kind == "f" and prototype and vid in self._names:
                self.declared.add(self._names[vid])

    def _block(self, width, bid, abbrevs, parent):
        b = self.b
        current = None
        while b.p < b.n:
            a = b.read(width)
            if a == 0:
                b.align32()
                return
            if a == 1:
                inner = b.vbr(8)
                inner_width = b.vbr(4)
                b.align32()
                b.read(32)
                self._block(inner_width, inner, list(self._blockinfo[inner]), bid)
                continue
            if a == 2:
                d = _abbrev_definition(b)
                if bid == 0:
                    self._blockinfo[current].append(d)
                else:
                    abbrevs.append(d)
                continue
            if a == 3:
                code = b.vbr(6)
                ops = [b.vbr(6) for _ in range(b.vbr(6))]
            else:
                code, ops = _abbrev_record(b, abbrevs[a - 4])
            if bid == 0 and code == 1:
                current = ops[0]
            elif bid == 12:
                self.function_records[code] += 1
            elif bid == 8 and code == 7:
                self._values.append(("g", None))
            elif bid == 8 and code == 8:
                self._values.append(("f", ops[2]))
            elif bid == 14 and parent == 8 and code == 1:
                self._names[ops[0]] = "".join(chr(x) for x in ops[1:])
            elif bid == 15 and code == 1:
                self.metadata_strings.append("".join(chr(x) if 32 <= x < 127 else "?" for x in ops))


class Container:
    def __init__(self, data, offset):
        self.offset = offset
        self.size, count = struct.unpack_from("<II", data, offset + 24)
        self.parts = {}
        self.order = []
        for o in struct.unpack_from(f"<{count}I", data, offset + 32):
            tag = bytes(data[offset + o:offset + o + 4]).decode("ascii", "replace")
            length = struct.unpack_from("<I", data, offset + o + 4)[0]
            self.parts[tag] = (offset + o + 8, length)
            self.order.append(tag)
        self.data = data

    def root_signature_flags(self):
        if "RTS0" not in self.parts:
            return None
        p, _ = self.parts["RTS0"]
        return struct.unpack_from("<I", self.data, p + 20)[0]

    def feature_flags(self):
        if "SFI0" not in self.parts:
            return None
        p, _ = self.parts["SFI0"]
        return struct.unpack_from("<Q", self.data, p)[0]

    def stage_and_threads(self):
        """From PSV0: (shader stage, minimum lanes, maximum lanes, thread-group size)."""
        if "PSV0" not in self.parts:
            return None
        p, _ = self.parts["PSV0"]
        info = struct.unpack_from("<I", self.data, p)[0]
        i = p + 4
        low, high = struct.unpack_from("<II", self.data, i + 16)
        stage = self.data[i + 24] if info >= 36 else None
        threads = struct.unpack_from("<III", self.data, i + 36) if info >= 48 else None
        return stage, low, high, threads

    def module(self):
        if "DXIL" not in self.parts:
            return None
        h, _ = self.parts["DXIL"]
        start, length = struct.unpack_from("<II", self.data, h + 16)
        return Module(bytes(self.data[h + 8 + start:h + 8 + start + length]))


def containers(data):
    out = []
    pos = 0
    while True:
        pos = data.find(b"DXBC", pos)
        if pos < 0:
            return out
        try:
            size, count = struct.unpack_from("<II", data, pos + 24)
        except struct.error:
            return out
        if not (32 < size < 64 << 20) or not (0 < count < 64):
            pos += 4
            continue
        out.append(Container(data, pos))
        pos += size
