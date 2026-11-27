"""Read-only access to the executable for the measurement scripts.

Everything here works on a file the caller names (the user's own copy). The file is read once into memory and
never written; nothing of it is written anywhere. Function bounds come from the executable's own exception
directory (.pdata). Needs Python 3.10+, numpy and capstone (installed separately: pip install numpy capstone).
"""
import bisect
import struct
import sys

import numpy as np
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP


class Image:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: not a PE file")
        sections = struct.unpack_from("<H", d, pe + 6)[0]
        optional = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", d, opt)[0] != 0x20B:
            raise ValueError(f"{path}: not a 64-bit image")
        self.image_base = struct.unpack_from("<Q", d, opt + 24)[0]
        directories = opt + 112
        self.exception_rva, self.exception_size = struct.unpack_from("<II", d, directories + 3 * 8)
        self.sections = []
        table = opt + optional
        for i in range(sections):
            s = table + i * 40
            name = d[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", d, s + 8)
            self.sections.append((name, va, max(vsize, rawsize), raw, rawsize))
        self._functions()
        self._dis = Cs(CS_ARCH_X86, CS_MODE_64)
        self._dis.detail = True

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        raise KeyError(name)

    def read(self, rva, size):
        for _name, va, vsize, raw, rawsize in self.sections:
            if va <= rva < va + vsize:
                off = rva - va
                have = max(0, min(size, rawsize - off))
                return self.data[raw + off:raw + off + have] + b"\0" * (size - have)
        raise ValueError(f"rva {rva:#x} lies in no section")

    def dword(self, rva):
        return struct.unpack("<I", self.read(rva, 4))[0]

    def float32(self, rva):
        return struct.unpack("<f", self.read(rva, 4))[0]

    # ---- functions, from .pdata --------------------------------------------------------------------------
    def _functions(self):
        raw = self.read(self.exception_rva, self.exception_size)
        entries = np.frombuffer(raw[:len(raw) // 12 * 12], dtype="<u4").reshape(-1, 3)
        entries = entries[entries[:, 0] != 0]
        self._begins = entries[:, 0].astype(np.int64)
        self._ends = entries[:, 1].astype(np.int64)
        self._unwind = entries[:, 2].astype(np.int64)
        order = np.argsort(self._begins)
        self._begins, self._ends, self._unwind = self._begins[order], self._ends[order], self._unwind[order]
        self._begin_list = self._begins.tolist()

    def _primary(self, index):
        """A .pdata entry whose unwind information is chained belongs to the function of the entry it chains to."""
        for _ in range(64):
            info = self._unwind[index]
            if info & 1:  # an indirect entry (unwind field points at another RUNTIME_FUNCTION)
                begin = self.dword(int(info & ~1))
                index = bisect.bisect_right(self._begin_list, begin) - 1
                continue
            head = self.read(int(info), 4)
            if not (head[0] >> 3) & 4:  # UNW_FLAG_CHAININFO
                return index
            codes = head[2]
            chained = int(info) + 4 + 2 * ((codes + 1) & ~1)
            begin = self.dword(chained)
            index = bisect.bisect_right(self._begin_list, begin) - 1
        return index

    def function_of(self, rva):
        """(begin, end) of the function holding rva, or None. end is that entry's own end."""
        i = bisect.bisect_right(self._begin_list, rva) - 1
        if i < 0 or not (self._begins[i] <= rva < self._ends[i]):
            return None
        p = self._primary(i)
        return int(self._begins[p]), int(self._ends[i])

    # ---- code ---------------------------------------------------------------------------------------------
    def instructions(self, begin, end=None, limit=None):
        """Disassembles from begin to end (default: the end of the .pdata range holding begin)."""
        if end is None:
            f = self.function_of(begin)
            i = bisect.bisect_right(self._begin_list, begin) - 1
            end = int(self._ends[i]) if f else begin + 0x400
        code = self.read(begin, end - begin)
        out = []
        for ins in self._dis.disasm(code, begin):
            out.append(ins)
            if limit and len(out) >= limit:
                break
        return out

    @staticmethod
    def rip_target(ins):
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                return ins.address + ins.size + op.mem.disp
        return None

    def text(self, ins):
        t = self.rip_target(ins)
        line = f"{ins.address:8x}  {ins.mnemonic:8} {ins.op_str}"
        return line + (f"  ; rva {t:#x}" if t is not None else "")

    def at(self, address):
        ins = self.instructions(address, address + 16, limit=1)
        if not ins or ins[0].address != address:
            raise ValueError(f"no instruction starts at {address:#x}")
        return ins[0]

    def number(self, address):
        """The immediate value of the instruction at address (the last one if there are several)."""
        from capstone.x86 import X86_OP_IMM
        values = [(op.imm, op.size) for op in self.at(address).operands if op.type == X86_OP_IMM]
        if not values:
            raise ValueError(f"the instruction at {address:#x} holds no number")
        value, size = values[-1]
        return value & ((1 << (8 * size)) - 1) if value < 0 else value

    def offset(self, address):
        """The displacement of the memory operand of the instruction at address (rip-relative: the target rva)."""
        ins = self.at(address)
        t = self.rip_target(ins)
        if t is not None:
            return t
        for op in ins.operands:
            if op.type == X86_OP_MEM:
                return op.mem.disp
        raise ValueError(f"the instruction at {address:#x} has no memory operand")

    def writes_memory(self, address):
        ins = self.at(address)
        return bool(ins.operands) and ins.operands[0].type == X86_OP_MEM and ins.mnemonic.startswith(
            ("mov", "and", "or", "add", "sub", "xor", "inc", "dec", "vmov"))

    def _code(self):
        if not hasattr(self, "_code_cache"):
            name, va, vsize, raw, rawsize = self.section(".text")
            self._code_cache = (va, np.frombuffer(self.data[raw:raw + rawsize], dtype=np.uint8))
        return self._code_cache

    def callers(self, target):
        """Starts of the functions that call target directly (E8 rel32)."""
        va, b = self._code()
        rel = (b[1:-3].astype(np.int64) | (b[2:-2].astype(np.int64) << 8) | (b[3:-1].astype(np.int64) << 16)
               | (b[4:].astype(np.int64) << 24))
        rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
        site = np.arange(len(rel), dtype=np.int64) + va
        hit = np.nonzero((b[:-4] == 0xE8) & (site + 5 + rel == target))[0]
        out = set()
        for h in hit.tolist():
            f = self.function_of(va + h)
            if f:
                out.add(f[0])
        return sorted(out)

    def rip_references(self, target):
        """(instruction address, function start, instruction) for every instruction that addresses target
        rip-relatively. Candidates come from a byte scan; each is confirmed by disassembling its function."""
        va, b = self._code()
        disp = (b[:-3].astype(np.int64) | (b[1:-2].astype(np.int64) << 8) | (b[2:-1].astype(np.int64) << 16)
                | (b[3:].astype(np.int64) << 24))
        disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
        pos = np.arange(len(disp), dtype=np.int64) + va
        ranges = set()
        for extra in (0, 1, 2, 4):
            for h in np.nonzero(pos + 4 + extra + disp == target)[0].tolist():
                i = bisect.bisect_right(self._begin_list, va + h) - 1
                if i >= 0 and self._begins[i] <= va + h < self._ends[i]:
                    ranges.add((int(self._begins[i]), int(self._ends[i]), int(self._begins[self._primary(i)])))
        out = []
        for begin, end, function in sorted(ranges):
            for ins in self.instructions(begin, end):
                if self.rip_target(ins) == target:
                    out.append((ins.address, function, ins))
        return out


class Checks:
    """Each script states what FINDINGS.md says and compares it with what it reads now."""

    def __init__(self, title):
        self.title = title
        self.results = []
        print(title)

    def same(self, what, measured, expected):
        ok = measured == expected
        self.results.append(ok)
        def shown(v):
            if isinstance(v, bool):
                return repr(v)
            if isinstance(v, int):
                return hex(v)
            if isinstance(v, (list, tuple)):
                return "[" + ", ".join(shown(x) for x in v) + "]"
            return repr(v)
        print(f"  {'same   ' if ok else 'DIFFERS'}  {what}: {shown(measured)}"
              + ("" if ok else f" (FINDINGS: {shown(expected)})"))
        return ok

    def done(self):
        bad = self.results.count(False)
        print(f"{len(self.results) - bad} of {len(self.results)} the same as FINDINGS.md")
        sys.exit(1 if bad else 0)


def open_from_arguments(usage):
    if len(sys.argv) < 2:
        print(usage)
        sys.exit(2)
    return Image(sys.argv[1])
