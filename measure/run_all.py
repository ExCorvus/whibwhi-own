"""Runs every measurement in measure/ on your own copy of the game and prints one short report.

    python measure/run_all.py "<your game folder>" [--report <file>] [--pipelines <builder_pipelines.exe> [--validate]]

Each script reads its file from your game folder read-only, in memory, and says for each statement of FINDINGS.md
whether it is the same on your copy. This script runs them side by side, then prints which of them hold and which
differ. Nothing of the game is written anywhere: the report holds the scripts' verdict lines (addresses, numbers and
words), the size and SHA-256 of each file read, and the versions of the tools. It can be posted as it is.

The one measurement that needs the graphics card (builder_pipelines.cpp) runs only when you pass the program you
built from it with --pipelines (measure/README.md says how to build it). Without it the report says it was not run.

Exit code 0 when every measurement that ran holds, 1 when one differs or could not run, 2 for a wrong call.
"""

import concurrent.futures
import hashlib
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# The same rule the game test uses (scripts/game_test.ps1): a measurement names the file it reads in its usage line.
USAGE = re.compile(r'game folder>\\+([A-Za-z0-9_.]+)')
CHECKS = re.compile(r'(?m)(\.same\(|^\s*same\(")')


def fingerprint(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return path.stat().st_size, digest.hexdigest()


def tool_versions():
    lines = [f"Python {platform.python_version()} on {platform.system()} {platform.release()} {platform.machine()}"]
    for name in ("numpy", "capstone"):
        try:
            module = __import__(name)
            version = getattr(module, "__version__", None)
            if version is None and name == "capstone":
                version = ".".join(str(x) for x in module.cs_version())
            lines.append(f"{name} {version}")
        except ImportError:
            lines.append(f"{name} MISSING (pip install {name})")
    return lines


def measurements():
    """(script, the file of the game folder it reads), and the scripts that check results but name no file."""
    found, unnamed = [], []
    for script in sorted(HERE.glob("*.py")):
        if script.name == Path(__file__).name:
            continue
        text = script.read_text(encoding="utf-8")
        usage = USAGE.search(text)
        if usage:
            found.append((script, usage.group(1)))
        elif CHECKS.search(text):
            unnamed.append(script)
    return found, unnamed


def verdict_lines(output):
    """The lines a report keeps: the DIFFERS lines and the last line (how many are the same)."""
    lines = [line.rstrip() for line in output.splitlines() if line.strip()]
    differs = [line.strip() for line in lines if line.strip().startswith("DIFFERS")]
    last = lines[-1].strip() if lines else ""
    return differs, last


def run_one(command):
    done = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return done.returncode, done.stdout + done.stderr


def main(argv):
    args = list(argv)
    report_path, pipelines, validate = None, None, False
    if "--report" in args:
        i = args.index("--report")
        report_path = Path(args[i + 1])
        del args[i:i + 2]
    if "--pipelines" in args:
        i = args.index("--pipelines")
        pipelines = Path(args[i + 1])
        del args[i:i + 2]
    if "--validate" in args:
        args.remove("--validate")
        validate = True
    if len(args) != 1:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    game = Path(args[0])
    if not game.is_dir():
        print(f"not a folder: {game}")
        return 2

    report = ["# measure/run_all.py report", ""]
    report += tool_versions()
    found, unnamed = measurements()
    inputs = sorted({name for _, name in found} | ({"XboxBvhBuilder.dll"} if pipelines else set()))
    report.append("")
    report.append("Files read from the game folder (size, SHA-256):")
    present = {}
    for name in inputs:
        path = game / name
        if path.is_file():
            size, sha = fingerprint(path)
            present[name] = path
            report.append(f"  {name}: {size} bytes, {sha}")
        else:
            report.append(f"  {name}: NOT FOUND")
    report.append("")

    results = []  # (name, state, last line, DIFFERS lines)
    for script in unnamed:
        results.append((script.name, "could not run", "checks results but names no file of the game folder", []))

    jobs = {}
    workers = max(1, min(len(found), os.cpu_count() or 1))
    print(f"running {len(found)} measurements on {game}, {workers} at a time (the slowest takes some minutes) ...")
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for script, name in found:
            if name not in present:
                results.append((script.name, "could not run", f"{name} is not in the game folder", []))
                continue
            jobs[pool.submit(run_one, [sys.executable, str(script), str(present[name])])] = script.name
        if pipelines:
            if not pipelines.is_file() or "XboxBvhBuilder.dll" not in present:
                results.append(("builder_pipelines.cpp", "could not run",
                                "the built program or XboxBvhBuilder.dll is missing", []))
            else:
                command = [str(pipelines), str(present["XboxBvhBuilder.dll"])] + (["--validate"] if validate else [])
                jobs[pool.submit(run_one, command)] = "builder_pipelines.cpp"
        for job in concurrent.futures.as_completed(jobs):
            name = jobs[job]
            code, output = job.result()
            differs, last = verdict_lines(output)
            state = "holds" if code == 0 else ("differs" if differs else "could not run")
            if state == "could not run" and not last:
                last = f"exit {code}"
            results.append((name, state, last, differs))
            print(f"  {name}: {state} - {last}")
    if not pipelines:
        results.append(("builder_pipelines.cpp", "not run",
                        "needs the graphics card and a build: pass --pipelines (measure/README.md)", []))

    report.append("Measurements:")
    for name, state, last, differs in sorted(results):
        report.append(f"  {name}: {state} - {last}")
        for line in differs:
            report.append(f"      {line}")
    bad = [r for r in results if r[1] in ("differs", "could not run")]
    ran = [r for r in results if r[1] != "not run"]
    report.append("")
    if bad:
        report.append(f"VERDICT: {len(bad)} of {len(ran)} measurements do not hold on this copy: "
                      + ", ".join(r[0] for r in sorted(bad)))
    else:
        report.append(f"VERDICT: all {len(ran)} measurements that ran hold on this copy")

    text = "\n".join(report) + "\n"
    print()
    print(text, end="")
    if report_path:
        report_path.write_text(text, encoding="utf-8")
        print(f"(report written to {report_path})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
