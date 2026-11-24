"""Turn the addresses in a crash report into function names.

Usage: python tools/crash_symbols.py <log file> [--build build] [--symbolizer llvm-symbolizer.exe]

The crash handler in src/game/dev_tools.cpp prints lines such as
`  #03 SouthParkRecompiled.exe+0x1336B4`. Each offset is looked up in the
linker map of the given build folder. If that build has a .pdb and
llvm-symbolizer is available, source files and line numbers are added too.
Recompiled functions keep their guest names, so the result reads as the game's
own call stack.
"""
from __future__ import annotations

import argparse
import bisect
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = "SouthParkRecompiled"


def load_map(path: Path) -> tuple[list[int], list[str]]:
    base = 0x140000000
    syms: dict[int, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"\s*Preferred load address is ([0-9A-Fa-f]+)", line)
        if m:
            base = int(m.group(1), 16)
            continue
        m = re.match(r"\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{16})\s+(.*)$", line)
        if m:
            rva = int(m.group(2), 16) - base
            if rva > 0:
                syms.setdefault(rva, f"{m.group(1)}  [{m.group(3).strip()}]")
    addrs = sorted(syms)
    return addrs, [syms[a] for a in addrs]


def source_lines(symbolizer: str, exe: Path, rvas: list[int]) -> dict[int, str]:
    """file:line for each offset, innermost inlined frame first."""
    if not rvas:
        return {}
    args = [symbolizer, f"--obj={exe}", "--relative-address", "--inlines", "--functions=short", "--output-style=LLVM"]
    out = subprocess.run(args + [hex(r) for r in rvas], capture_output=True, text=True).stdout
    result: dict[int, str] = {}
    for rva, block in zip(rvas, out.split("\n\n")):
        frames = []
        lines = [l for l in block.splitlines() if l.strip()]
        for name, where in zip(lines[0::2], lines[1::2]):
            where = where.replace("\\", "/")
            where = where.split("SouthParkRecomp/", 1)[-1]
            frames.append(f"{name} at {where}")
        if frames:
            result[rva] = " <- ".join(frames)
    return result


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--build", default="build")
    ap.add_argument("--symbolizer", default="")
    args = ap.parse_args()

    build = ROOT / args.build
    addrs, names = load_map(build / f"{EXE}.map")
    text = Path(args.log).read_text(encoding="utf-8", errors="replace").splitlines()
    pattern = re.compile(re.escape(EXE) + r"\.exe\+0x([0-9A-Fa-f]+)")
    rvas = sorted({int(m.group(1), 16) for line in text for m in [pattern.search(line)] if m})
    sources: dict[int, str] = {}
    if args.symbolizer and (build / f"{EXE}.pdb").exists():
        sources = source_lines(args.symbolizer, build / f"{EXE}.exe", rvas)

    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    for line in text:
        m = pattern.search(line)
        if m:
            rva = int(m.group(1), 16)
            i = bisect.bisect_right(addrs, rva) - 1
            if i >= 0:
                line += f"   {names[i]} +0x{rva - addrs[i]:X}"
            if rva in sources:
                line += f"\n        {sources[rva]}"
        print(line)


if __name__ == "__main__":
    main()
