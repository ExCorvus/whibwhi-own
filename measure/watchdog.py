"""Repeats what FINDINGS 2026-11-26, "the program's progress reports, read during a run", stands on: the
program's own hang watchdog as docs/exe-map/MAP.md section 4 describes it (its verified parts) - who writes the
progress stamp, the one-second pass, the two time limits, the 30 passes, the stop, the watch scopes, the run loop.

    python measure/watchdog.py "<your game folder>\\game_scarlett_master.exe"

Reads the executable read-only; nothing is written. The run results of that entry (how often the program reported
during a game test) come from runs, not from the file, and are not repeated here.
"""
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from pe_image import Checks, open_from_arguments

exe = open_from_arguments(__doc__)
c = Checks("watchdog: the program's own hang watchdog (MAP.md section 4)")

WATCHDOG = 0x2215A60
REPORTER = 0x22159F0
SLEEP_THUNK = 0x92ECA0
NOT_A_WRITE = ("cmp", "test", "bt", "comis", "ucomis", "vcomis", "vucomis", "push", "call", "j", "prefetch")


def writes(ins):
    mnemonic = ins.mnemonic.replace("lock ", "")
    return (bool(ins.operands) and ins.operands[0].type == X86_OP_MEM and not mnemonic.startswith(NOT_A_WRITE)
            and not mnemonic.startswith("lea"))


def writers(address, besides=()):
    return sorted({function for _a, function, ins in exe.rip_references(address)
                   if writes(ins) and function not in besides})


def readers(address):
    return sorted({function for _a, function, _ins in exe.rip_references(address)})


# The state the watchdog keeps (MAP.md 4.2), written by whom (the watchdog's own clears not counted).
c.same("functions besides the watchdog that write the progress stamp 0x71b6338", writers(0x71B6338, (WATCHDOG,)),
       [REPORTER])
c.same("the watchdog reads the progress stamp", WATCHDOG in readers(0x71B6338), True)
# MAP.md's table said "fn 0x22159f0 only" until 2026-11-27; the watchdog also stores it when it re-arms.
c.same("stores of the progress stamp by the watchdog itself (re-arming)",
       sum(1 for _a, f, ins in exe.rip_references(0x71B6338) if f == WATCHDOG and writes(ins)), 4)
c.same("functions besides the watchdog that write the kick byte 0x71b6331", writers(0x71B6331, (WATCHDOG,)), [])
c.same("functions that write the open-scope count 0x71b6334", writers(0x71B6334, (WATCHDOG,)),
       [0xB5B6B0, 0xB5B6F0, 0x22161B0])
c.same("functions that write the pause byte 0x71b6330", writers(0x71B6330, (WATCHDOG,)), [0x22161B0])
c.same("functions that write the stop byte 0x71b6340", writers(0x71B6340, (WATCHDOG,)), [0x22161B0])
c.same("functions with a .pdata entry that write the crash callback 0x71b6348", writers(0x71B6348, (WATCHDOG,)), [])
# fn 0x22161f0 is a leaf without a .pdata entry, so the scan above cannot see it; its store is read directly.
installer = exe.instructions(0x22161F0, 0x2216217)
c.same("fn 0x22161f0 (no .pdata entry) stores its argument into the crash callback",
       [(ins.address, ins.op_str.endswith("rcx")) for ins in installer if exe.rip_target(ins) == 0x71B6348
        and writes(ins)], [(0x221620C, True)])

# The two time limits (MAP.md 4.2), and that the watchdog is what reads them.
c.same("the overrun limit at 0x6159bd4 (seconds)", exe.float32(0x6159BD4), 2.0)
c.same("the watch-scope limit at 0x6159e20 (seconds)", exe.float32(0x6159E20), 300.0)
c.same("the watchdog reads both limits", [WATCHDOG in readers(0x6159BD4), WATCHDOG in readers(0x6159E20)],
       [True, True])

# One pass (MAP.md 4.3): Sleep(1000), 30 passes re-armed, the stop.
body = exe.instructions(*exe.function_of(WATCHDOG))
sleep_args = []
for i, ins in enumerate(body):
    if ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM and ins.operands[0].imm == SLEEP_THUNK:
        for before in reversed(body[max(0, i - 4):i]):
            if before.mnemonic == "mov" and before.op_str.startswith("ecx,"):
                sleep_args.append(exe.number(before.address))
                break
c.same("what the watchdog passes to Sleep (milliseconds)", sorted(set(sleep_args)), [1000])
# The countdown lives in a register, not in memory: every re-arm puts 30 into one.
thirty = [ins.address for ins in body if ins.mnemonic == "mov" and len(ins.operands) == 2
          and ins.operands[0].type != X86_OP_MEM and ins.operands[1].type == X86_OP_IMM and ins.operands[1].imm == 30]
c.same("places where the watchdog sets its countdown to 30 passes", len(thirty) > 0, True)
stop = exe.at(0x2215C35)
stop_mem = [op for op in stop.operands if op.type == X86_OP_MEM]
c.same("the stop at 0x2215c35: a store of 0 to address 0",
       [stop.mnemonic, stop_mem[0].mem.disp if stop_mem else None, exe.number(0x2215C35)], ["mov", 0, 0])
c.same("the instruction after the stop is int3", exe.at(0x2215C35 + stop.size).mnemonic, "int3")

# The reporter and the run loop (MAP.md 4.5, 4.6).
reporter = exe.function_of(REPORTER)
# MAP.md said 18 bytes until 2026-11-27; what it does (one time query, one store) was right.
c.same("the reporter fn 0x22159f0, its size in bytes", reporter[1] - reporter[0], 35)
calls = [ins for ins in exe.instructions(*reporter) if ins.mnemonic == "call"]
c.same("the reporter's one call goes to", [exe.import_at(exe.rip_target(ins))[1] for ins in calls],
       ["QueryPerformanceCounter"])
c.same("the reporter's stores into the progress stamp",
       sum(1 for ins in exe.instructions(*reporter) if exe.rip_target(ins) == 0x71B6338 and writes(ins)), 1)
for site in (0x1C82B67, 0x1C82BC1, 0x120799):
    ins = exe.at(site)
    c.same(f"the call at {site:#x} goes to the reporter",
           ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM and ins.operands[0].imm == REPORTER, True)
c.same("functions that call the run loop fn 0x1c82b20 or jump to it", len(exe.callers_and_jumps(0x1C82B20)), 23)
loop = exe.instructions(*exe.function_of(0x1C82B20))
handler = any(exe.rip_target(ins) == 0x258B000 and ins.mnemonic == "lea" and ins.op_str.startswith("rcx")
              for ins in loop)
c.same("the run loop hands 0x258b000 to the callback installer", handler, True)
marker = exe.at(0x258B000)
c.same("0x258b000: a store of 0x2ef1e94b to address 0, then ret",
       [marker.mnemonic, exe.number(0x258B000), exe.at(0x258B000 + marker.size).mnemonic],
       ["mov", 0x2EF1E94B, "ret"])

# The watch scopes (MAP.md 4.4): the functions that open one.
c.same("functions that open a watch scope (call fn 0xb5b6b0)", exe.callers(0xB5B6B0),
       sorted([0xB59E40, 0xCC44B0, 0x1D83480, 0x1EE1200, 0x22185A0, 0x248E070, 0x32B5E50, 0x374F2D0, 0x50070F0,
               0x5A9B5B0, 0x2474500]))
c.done()
