"""Repeats the places in the executable where FINDINGS says a run stopped or reached a new call:

- the audio setup's breakpoint (FINDINGS 2026-11-26 "bugfix: undid an AI trap", boot b169, and 2026-11-27 "the next
  stop is the audio device", game test 02:57): an int3 at image+0x43ad94e, the unwind chain written there, and
  that the audio setup fn 0x43aec40 returning false leads to it;
- the pipeline store the program checks before it asks for a pipeline (same 2026-11-26 entry): fn 0x2479760
  hands the store at 0x75ddb90 to fn 0x2479f40 before device slot 86;
- the builder's construction (FINDINGS 2026-11-26, game tests 22:36-22:48): image+0x247944f calls the
  installation's XboxBvhBuilder constructor with false, and the run's stop returned to image+0x2479455;
- the program's own compute shaders (FINDINGS 2026-11-27, game test 01:34, as corrected): device slot 62 called at
  image+0x247fe66 in fn 0x247fd30, returning to image+0x247fe6d.

    python measure/stops.py "<your game folder>\\game_scarlett_master.exe"

Reads the executable read-only; nothing is written.
"""
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from pe_image import Checks, open_from_arguments

exe = open_from_arguments(__doc__)
c = Checks("stops: where the runs stopped, read in the executable")


def body(address):
    return exe.instructions(*exe.function_of(address))


def before(address):
    """The instruction that ends right where address begins, in the function holding address."""
    for ins in body(address):
        if ins.address + ins.size == address:
            return ins
    return None


def direct_target(ins):
    if ins is not None and ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM:
        return ins.operands[0].imm
    return None


def slot(ins):
    """The method-table byte offset an indirect call goes through, as a slot number (offset / 8)."""
    if ins is None or ins.mnemonic != "call" or ins.operands[0].type != X86_OP_MEM:
        return None
    mem = ins.operands[0].mem
    return None if exe.rip_target(ins) is not None else mem.disp // 8


# The audio setup's breakpoint.
STOP = 0x43AD94E
c.same("the instruction at image+0x43ad94e", exe.at(STOP).mnemonic, "int3")
c.same("the function holding it", exe.function_of(STOP)[0], 0x43AD8B0)
jumps = sorted(ins.address for ins in body(STOP) if ins.mnemonic.startswith("j") and ins.operands
               and ins.operands[0].type == X86_OP_IMM and ins.operands[0].imm == STOP)
# Read 2026-11-27 (FINDINGS names only the audio setup's path): three jumps reach the breakpoint - after the call
# at 0x43ad933 (sign test), after the audio setup (0x43ad943), and after fn 0x5b52510 returns false (0x43ada59;
# what that function is, is not read). The fourth way is falling through after fn 0x43aee50 (below).
c.same("the jumps to the breakpoint", jumps, [0x43AD93A, 0x43AD943, 0x43ADA59])
c.same("the third jump tests the result of fn 0x5b52510",
       [direct_target(exe.at(0x43ADA52)), exe.at(0x43ADA57).op_str], [0x5B52510, "al, al"])
c.same("what the second jump tests: the result of the call at 0x43ad93c",
       [direct_target(exe.at(0x43AD93C)), exe.at(0x43AD941).mnemonic, exe.at(0x43AD941).op_str], [0x43AEC40, "test", "al, al"])
c.same("the step after the audio setup, when it fails, falls through to the breakpoint",
       [direct_target(exe.at(0x43AD945)), exe.at(0x43AD94C).mnemonic, 0x43AD94C + exe.at(0x43AD94C).size],
       [0x43AEE50, "jne", STOP])
c.same("functions that call the audio setup fn 0x43aec40", exe.callers_and_jumps(0x43AEC40), [0x43AD8B0])

# The unwind chain of boot b169: each address is a return address (it follows a call).
chain = [0x43A657A, 0x25A4E47, 0x25A4C98, 0x1D857A6]
c.same("each address of the unwind chain follows a call",
       [before(a) is not None and before(a).mnemonic == "call" for a in chain], [True] * 4)
c.same("the first one is a method call (slot 17, offset 0x88); fn 0x43ad8b0 has no direct caller",
       [slot(before(0x43A657A)), exe.callers_and_jumps(0x43AD8B0)], [17, []])
c.same("the last two call the function holding the address before them",
       [direct_target(before(0x25A4C98)) == exe.function_of(0x25A4E47)[0],
        direct_target(before(0x1D857A6)) == exe.function_of(0x25A4C98)[0]], [True, True])
# Between 0x25a4e47 and 0x43a657a the call goes to fn 0x1e73fb0, not to fn 0x43a6420; the frames in between
# are not read (stated, not a claim of FINDINGS).

# The pipeline store checked before device slot 86.
PIPELINE = 0x2479760
store = [a for a, f, _ins in exe.rip_references(0x75DDB90) if f == PIPELINE]
c.same("fn 0x2479760 addresses the pipeline store 0x75ddb90", len(store) > 0, True)
code = body(PIPELINE)
store_call = next((ins.address for ins in code if store and ins.address > store[0] and direct_target(ins) == 0x2479F40),
                  None)
slot86 = [ins.address for ins in code if slot(ins) == 86]
c.same("then calls fn 0x2479f40, before its device slot 86 call (at 0x2479a87)",
       [store_call is not None, slot86, store_call is not None and slot86 and store_call < slot86[0]],
       [True, [0x2479A87], True])

# The builder's construction.
build = exe.at(0x247944F)
c.same("image+0x247944f calls the import", exe.import_at(exe.rip_target(build)),
       ("XboxBvhBuilder.dll", "??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z"))
c.same("with the constructor's bool argument (r8) set to false just before",
       [before(0x247944F).mnemonic, before(0x247944F).op_str], ["xor", "r8d, r8d"])
c.same("the call returns to image+0x2479455, in fn 0x2478440", [0x247944F + build.size, exe.function_of(0x247944F)[0]],
       [0x2479455, 0x2478440])

# The program's own compute shaders: device slot 62.
s62 = exe.at(0x247FE66)
c.same("image+0x247fe66: a method call through slot 62 (offset 0x1f0) in fn 0x247fd30",
       [slot(s62), exe.function_of(0x247FE66)[0], 0x247FE66 + s62.size], [62, 0x247FD30, 0x247FE6D])
c.done()
