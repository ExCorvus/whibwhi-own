#!/usr/bin/env python3
"""No-game-data check.

Refuses game data in the files named: archives, executables, carved shaders (DXBC / FSSM / SPIR-V / disassembly),
textures, audio, video, or binaries. Reads the first bytes of each file so a renamed file is caught by its magic,
not just its extension. Standard library only. Exit 0 = clean, 1 = game data found.

Usage: python -I scripts/check_no_game_data.py <path> ...
"""
import os
import sys

# Magic bytes of game data we must never commit (checked at the start of the file).
MAGICS = [
    (b"MZ", "PE executable / DLL"),
    (b"\x03\x02\x23\x07", "SPIR-V shader"),
    (b"\x07\x02\x23\x03", "SPIR-V shader (big-endian)"),
    (b"DXBC", "DXBC shader bytecode"),
    (b"DXIL", "DXIL shader bytecode"),
    (b"\x1bLua", "Lua bytecode"),
    (b"RPF0", "RPF archive"),
    (b"RPF2", "RPF archive"),
    (b"RPF3", "RPF archive"),
    (b"RPF4", "RPF archive"),
    (b"RPF6", "RPF archive"),
    (b"RPF7", "RPF archive"),
    (b"\x89PNG", "PNG texture"),
    (b"\xff\xd8\xff", "JPEG texture"),
    (b"DDS ", "DDS texture"),
    (b"RIFF", "RIFF audio/video"),
    (b"ID3", "MP3 audio"),
    (b"\x30\x26\xb2\x75", "WMV/WMA audio/video"),
]

# Extensions that are game data regardless of content.
BAD_EXT = {
    ".exe", ".dll", ".spv", ".dxbc", ".dxil", ".fssm", ".rpf", ".pak", ".dat",
    ".awc", ".xma", ".wav", ".mp3", ".ogg", ".wem", ".bnk",
    ".dds", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".gif", ".webp",
    ".mp4", ".webm", ".avi", ".mkv", ".bik", ".bk2",
    ".bin", ".raw", ".img", ".iso", ".zip", ".7z", ".rar", ".gz", ".xz",
    ".pdb", ".lib", ".obj", ".o", ".a", ".so", ".dylib",
}


def check(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in BAD_EXT:
        return f"{path}: game-data extension '{ext}'"
    try:
        with open(path, "rb") as f:
            head = f.read(16)
    except OSError as e:
        return f"{path}: cannot read ({e})"
    for magic, label in MAGICS:
        if head.startswith(magic):
            return f"{path}: {label} magic"
    return None


def main(argv):
    paths = argv[1:]
    if not paths:
        print("usage: python -I scripts/check_no_game_data.py <path> ...")
        return 2
    problems = []
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs if d not in (".git", "build", "__pycache__")]
                for name in files:
                    bad = check(os.path.join(root, name))
                    if bad:
                        problems.append(bad)
        else:
            bad = check(p)
            if bad:
                problems.append(bad)
    if problems:
        for line in problems:
            print("GAME DATA:", line)
        return 1
    print(f"clean: {len(paths)} path(s) checked, no game data")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
