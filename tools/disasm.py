"""Disassemble a South Park function by name or address, with call targets named.

Usage: python tools/disasm.py <name|0xADDRESS> [max instructions]
"""
import bisect
import re
import struct
import sys

import rabbitizer

from games import ROOT, SOUTHPARK

rom = SOUTHPARK.rom_path.read_bytes()
words = struct.unpack(f">{len(rom) // 4}I", rom)
syms: dict[int, tuple[str, int]] = {}
for line in open(ROOT / "symbols" / "southpark.us.syms.toml", encoding="utf-8"):
    m = re.search(r'name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+)', line)
    if m:
        syms[int(m.group(2), 16)] = (m.group(1), int(m.group(3), 16))
starts = sorted(syms)
by_name = {n: a for a, (n, _) in syms.items()}


def main() -> None:
    arg = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    if arg in by_name:
        start = by_name[arg]
    else:
        v = int(arg, 16)
        start = starts[bisect.bisect_right(starts, v) - 1]
    name, size = syms[start]
    region = SOUTHPARK.code_region(start)
    print(f"--- {name} @{start:08X} size {size}")
    for a in range(start, start + min(size, limit * 4), 4):
        w = words[region.to_rom(a) // 4]
        text = re.sub(" +", " ", rabbitizer.Instruction(w, vram=a).disassemble())
        if w >> 26 in (2, 3):
            t = (a & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            text = f"{'jal' if w >> 26 == 3 else 'j'} {syms.get(t, (f'{t:08X}',))[0]}"
        print(f"   {a:08X} {text}")


if __name__ == "__main__":
    main()
