# measure/ — repeat the measurements yourself

Every result in [FINDINGS.md](../FINDINGS.md) that was read from the executable should be repeatable by anyone
who has their own copy of the game. The scripts here do that: each one reads the executable **read-only, in
memory**, checks the statements of one group of FINDINGS entries, and prints `same` or `DIFFERS` for each.
Nothing of the game is written anywhere, and the scripts themselves hold only addresses and the numbers found
there (rule 2 in [AGENTS.md](../AGENTS.md)).

These are measuring tools. They are not part of the weave's build and not part of the game test.

## What you need

- Python 3.10 or newer, and `pip install numpy capstone` (capstone is the disassembler; it is installed
  separately and not part of this repository).
- Your own copy of the game, version 1.0.2.1. You pass the path of its executable to each script.

## Running

```
python measure/audio.py "<your game folder>\game_scarlett_master.exe"
```

The last line says how many statements are the same as FINDINGS.md; the exit code is 0 when all are.

## The scripts

| script | repeats |
| --- | --- |
| `pe_image.py` | not a measurement: the shared reader (sections, functions from the executable's own `.pdata`, callers, rip-relative references, the number or offset in one instruction) |
| `audio.py` | FINDINGS 2026-11-27, "the program's audio setup" and its follow-ups: the 350 sound records, the setup/start/stop/data commands, the XMA2 packet walk, what the device writes back, the stream channel's float output turned into 16-bit samples |

More scripts are added as the earlier results are turned into repeatable steps.
