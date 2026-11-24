"""Recover the code layout of the ROM from the code itself.

Usage: python tools/layout_scan.py rom/southpark.us.z64

Method (same as turok2-decomp's docs/VRAM-LAYOUT.md): `jal` stores its target
absolutely, so the call graph tells where each code block is linked. Function
prologues (`addiu sp, sp, -X`) and `jr ra` tell where code physically sits.
"""
from __future__ import annotations

import collections
import struct
import sys

JR_RA = 0x03E00008


def is_prologue(word: int) -> bool:
    return (word & 0xFFFF8000) == 0x27BD8000  # addiu sp, sp, -X


def main() -> None:
    rom = open(sys.argv[1], "rb").read()
    words = struct.unpack(f">{len(rom) // 4}I", rom)

    # 1. Code density per 16 KB block: prologues, jr ra, jal
    print("== code density per 0x4000 block (prologues / jr ra / jal), blocks with code only")
    block = 0x4000 // 4
    runs = []
    for b in range(0, len(words), block):
        chunk = words[b:b + block]
        pro = sum(1 for w in chunk if is_prologue(w))
        jr = sum(1 for w in chunk if w == JR_RA)
        jal = sum(1 for w in chunk if w >> 26 == 3)
        code = pro >= 8 and jr >= 8
        runs.append((b * 4, pro, jr, jal, code))
    start = None
    for off, pro, jr, jal, code in runs + [(len(rom), 0, 0, 0, False)]:
        if code and start is None:
            start = off
        elif not code and start is not None:
            tot = [r for r in runs if start <= r[0] < off]
            print(f"  code 0x{start:06X}-0x{off:06X}  ({(off - start) // 1024} KB)  "
                  f"prologues={sum(r[1] for r in tot)} jr_ra={sum(r[2] for r in tot)} jal={sum(r[3] for r in tot)}")
            start = None

    # 2. jal targets by 1 MB bin, per code region
    def jal_bins(lo: int, hi: int) -> collections.Counter:
        c = collections.Counter()
        for i in range(lo // 4, hi // 4):
            w = words[i]
            if w >> 26 == 3:
                c[((w & 0x03FFFFFF) << 2) >> 20] += 1
        return c

    for name, lo, hi in (("main 0x1000-0xC2C00", 0x1000, 0xC2C00), ("0xC2C00-0x200000", 0xC2C00, 0x200000)):
        print(f"== jal bins in {name}")
        for k, v in jal_bins(lo, hi).most_common(6):
            print(f"  0x_{k:02X}xxxxx : {v}")

    # 3. Verify engine mapping: vram 0x00200500 <-> ROM 0x1100 (ROM = v - 0x1FF400)
    targets = collections.Counter()
    for i in range(0x1000 // 4, 0x200000 // 4):
        w = words[i]
        if w >> 26 == 3:
            targets[(w & 0x03FFFFFF) << 2] += 1
    eng = [t for t in targets if 0x00200000 <= t < 0x00400000]
    hit = sum(1 for t in eng if is_prologue(words[(t - 0x1FF400) // 4]))
    print(f"== engine: {len(eng)} unique jal targets in 0x002/0x003, range 0x{min(eng):08X}-0x{max(eng):08X}")
    print(f"   prologue at ROM = v - 0x1FF400: {hit}/{len(eng)} ({100 * hit / len(eng):.1f}%)")

    # 4. Locate the paged ('virtual') code: vote deltas for 0x004xxxxx targets
    virt = sorted(t for t in targets if 0x00400000 <= t < 0x00500000)
    print(f"== virtual: {len(virt)} unique jal targets, range 0x{virt[0]:08X}-0x{virt[-1]:08X}")
    pro_offs = [i * 4 for i, w in enumerate(words[:0x400000 // 4]) if is_prologue(w)]
    votes = collections.Counter()
    vset = virt
    for p in pro_offs:
        for t in vset:
            d = p - (t - 0x00400000)
            if 0 <= d < 0x400000:
                votes[d] += 1
    for d, n in votes.most_common(6):
        print(f"   ROM base 0x{d:06X} for vram 0x00400000: {n}/{len(virt)} targets on a prologue ({100 * n / len(virt):.1f}%)")

    # 5. TLB / exception opcodes
    names = {0x42000001: "tlbr", 0x42000002: "tlbwi", 0x42000006: "tlbwr", 0x42000008: "tlbp", 0x42000018: "eret"}
    print("== TLB / eret opcodes")
    for i, w in enumerate(words[:0x400000 // 4]):
        if w in names:
            print(f"   ROM 0x{i * 4:06X}  {names[w]}")


if __name__ == "__main__":
    main()
