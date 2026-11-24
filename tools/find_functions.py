"""Find function boundaries in the code regions of a ROM.

Usage: python tools/find_functions.py <game> [--out file.json] [--check dump.toml]

Works without symbols. Evidence used:
  * `jal` targets (certain function entries);
  * control flow: a function ends after an unconditional transfer (`jr ra`,
    `j`, `b`, table `jr`) once no forward branch is still pending;
  * `switch` jump tables read from rodata, so case labels placed after the
    last `jr ra` stay inside their function;
  * words in the data region that point at entry-like code (callbacks, vtables);
  * SN64 pads each object file to 16 bytes with junk; junk is told from code
    by decoding it (invalid instructions, registers read before being set).
"""
from __future__ import annotations

import argparse
import json
import re
import struct
from dataclasses import dataclass

import rabbitizer

from games import GAMES, Game, Region

JR_RA = 0x03E00008
ENTRY_DEFINED = frozenset({0, 4, 5, 6, 7, 28, 29, 31})  # zero, a0-a3, gp, sp, ra


def simm(x: int) -> int:
    return x - 0x10000 if x & 0x8000 else x


def is_prologue(w: int) -> bool:
    return (w & 0xFFFF8000) == 0x27BD8000  # addiu sp, sp, -X


def is_jr(w: int) -> bool:
    return w >> 26 == 0 and (w & 0x001FFFFF) == 0x8


def branch_target(w: int, pc: int) -> int | None:
    """Target of a PC-relative branch at pc, else None."""
    op = w >> 26
    rt = (w >> 16) & 31
    rs = (w >> 21) & 31
    if op in (4, 5, 20, 21):                              # beq bne (+ likely)
        pass
    elif op in (6, 7, 22, 23) and rt == 0:                # blez bgtz (+ likely)
        pass
    elif op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19):  # bltz bgez ... bgezall
        pass
    elif op == 17 and rs == 8 and rt & 0x1C == 0:         # bc1f / bc1t (+ likely)
        pass
    else:
        return None
    return pc + 4 + (simm(w & 0xFFFF) << 2)


def is_uncond_branch(w: int) -> bool:
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    return (op == 4 and rs == rt) or (op == 1 and rt in (1, 17) and rs == 0)  # beq r,r / bgez zero


def is_terminator(w: int) -> bool:
    return w >> 26 == 2 or is_jr(w) or is_uncond_branch(w)


@dataclass
class Func:
    vram: int
    size: int
    region: str
    called: bool = False       # target of a jal
    pointed: bool = False      # address found in data
    prologue: bool = False
    jtbl: int = 0              # number of jump tables
    kind: str = "func"         # func | filler (object padding) | data (does not decode)
    asm: bool = False          # hand-written assembly with several entry points


class Finder:
    def __init__(self, game: Game):
        self.game = game
        self.rom = game.rom_path.read_bytes()
        self.words = struct.unpack(f">{len(self.rom) // 4}I", self.rom)
        self.seeds: set[int] = set()
        self.data_ptrs: set[int] = set()
        self.jtbl_targets: set[int] = set()
        self.j_out: set[int] = set()         # `j` targets that leave their function
        self.tail_entries: set[int] = set()  # entries reached only by `j`
        self.mid_entries: set[int] = set()   # jal targets that land inside a function
        self._valid: dict[int, bool] = {}
        self.code_ptrs: set[int] = set()     # entries whose address is built by lui/addiu
        self._collect()

    # -- helpers ------------------------------------------------------------
    def w(self, region: Region, vram: int) -> int:
        return self.words[region.to_rom(vram) // 4]

    def data_word(self, vram: int) -> int | None:
        r = self.game.data_region(vram)
        return self.words[r.to_rom(vram) // 4] if r and vram % 4 == 0 else None

    def valid(self, w: int) -> bool:
        v = self._valid.get(w)
        if v is None:
            ins = rabbitizer.Instruction(w)
            v = self._valid[w] = bool(ins.isValid() and ins.isImplemented())
        return v

    def entry_code(self, region: Region, start: int, end: int) -> bool:
        """Can the words in [start, end) be the first instructions of a function?

        At entry only the argument registers, sp, ra and gp hold values, so
        compiled code never reads anything else before writing it (saving a
        callee-saved register to the stack is the one exception).
        """
        defined = set(ENTRY_DEFINED)
        for pc in range(start, min(end, region.vram_end), 4):
            w = self.w(region, pc)
            if w == 0:
                continue
            if not self.valid(w):
                return False
            ins = rabbitizer.Instruction(w)
            rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            if ins.readsRs() and rs not in defined:
                return False
            if ins.readsRt() and rt not in defined and not (ins.doesStore() and rs == 29):
                return False
            if ins.readsRd() and rd not in defined:
                return False
            if ins.modifiesRt():
                defined.add(rt)
            if ins.modifiesRd():
                defined.add(rd)
            if ins.isBranch() or ins.isJump():
                break
        return True

    def _entry_like(self, vram: int) -> bool:
        """True if code at vram sits where a function can begin: region start,
        or shortly after an unconditional transfer (allowing object padding)."""
        r = self.game.code_region(vram)
        if vram == r.vram:
            return True
        if not self.entry_code(r, vram, vram + 12):
            return False
        align = self.game.obj_align
        back = range(8, 8 + align, 4) if vram % align == 0 else (8,)
        return any(vram - b >= r.vram and is_terminator(self.w(r, vram - b)) for b in back)

    def _collect(self) -> None:
        for r in self.game.code:
            for pc in range(r.vram, r.vram_end, 4):
                w = self.w(r, pc)
                if w >> 26 == 3:
                    t = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                    if self.game.code_region(t):
                        self.seeds.add(t)
        # Function addresses built in code (callbacks passed as arguments).
        for r in self.game.code:
            for pc in range(r.vram, r.vram_end, 4):
                w = self.w(r, pc)
                if w >> 26 != 0x0F or not self.game.code_region((w & 0xFFFF) << 16 | 0x8000) and                         not self.game.code_region(((w & 0xFFFF) << 16) - 0x8000 & 0xFFFFFFFF) and                         not self.game.code_region((w & 0xFFFF) << 16):
                    continue
                reg = (w >> 16) & 31
                for q in range(pc + 4, min(pc + 36, r.vram_end), 4):
                    x = self.w(r, q)
                    op, rs, rt = x >> 26, (x >> 21) & 31, (x >> 16) & 31
                    if op in (0x09, 0x0D) and rs == reg:
                        v = ((w & 0xFFFF) << 16) + (simm(x & 0xFFFF) if op == 0x09 else x & 0xFFFF) & 0xFFFFFFFF
                        if v % 4 == 0 and self.game.code_region(v) and self._entry_like(v):
                            self.data_ptrs.add(v)
                            self.code_ptrs.add(v)
                        break
                    if (op == 0x0F and rt == reg) or op in (2, 3) or is_jr(x):
                        break
        for r in self.game.data:
            for rom in range(r.rom, r.rom_end - 3, 4):
                v = self.words[rom // 4]
                if v % 4 == 0 and self.game.code_region(v) and self._entry_like(v):
                    self.data_ptrs.add(v)

    # -- jump tables --------------------------------------------------------
    def jump_table(self, region: Region, fstart: int, pc: int) -> list[int]:
        """Case labels of the switch whose `jr` sits at pc, or []."""
        reg = (self.w(region, pc) >> 21) & 31
        lo = None
        bound = None
        hi: dict[int, int] = {}
        addends: set[int] = set()
        p = pc - 4
        steps = 0
        while p >= fstart and steps < 24:
            w = self.w(region, p)
            op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            if lo is None:
                if op == 0x23 and rt == reg:                 # lw reg, lo(base)
                    lo = simm(w & 0xFFFF)
                    addends = {rs}
            else:
                if op == 0 and (w & 0x7FF) == 0x21 and rd in addends:   # addu
                    addends |= {rs, rt}
                elif op == 0x0F and rt in addends and rt not in hi:     # lui
                    hi[rt] = (w & 0xFFFF) << 16
                elif op in (0x0B, 0x0A) and bound is None:              # sltiu / slti
                    bound = w & 0xFFFF
            p -= 4
            steps += 1
        if lo is None or not hi:
            return []
        table = (next(iter(hi.values())) + lo) & 0xFFFFFFFF
        entries: list[int] = []
        limit = bound if bound else 512
        addr = table
        while len(entries) < limit:
            v = self.data_word(addr)
            if v is None or v % 4 or not region.contains(v) or v < fstart or v > pc + 0x10000:
                break
            entries.append(v)
            addr += 4
        return entries

    # -- main sweep ---------------------------------------------------------
    def _is_entry(self, vram: int) -> bool:
        return vram in self.seeds or vram in self.data_ptrs

    def scan_region(self, region: Region) -> list[Func]:
        funcs: list[Func] = []
        pc = region.vram
        end = region.vram_end
        while pc < end:
            fstart = pc
            if not self.valid(self.w(region, pc)) and not self._is_entry(pc):
                # Not code: skip to the next place a function can begin.
                pc += 4
                while pc < end and not (self._is_entry(pc) or (is_prologue(self.w(region, pc)) and pc % self.game.obj_align == 0)):
                    pc += 4
                funcs.append(Func(fstart, pc - fstart, region.name, kind="data"))
                continue
            far = pc              # farthest pending target of a branch or switch
            far_j = pc            # farthest pending target of a forward `j`
            ntables = 0
            junk_end = False
            i = pc
            fend = end
            while i < end:
                if i > fstart and far < i and self._is_entry(i):
                    fend = i      # ran into a known entry: previous code ends here
                    break
                w = self.w(region, i)
                if not self.valid(w):
                    fend = i      # ran into something that is not code
                    break
                op = w >> 26
                t = branch_target(w, i)
                terminator = False
                if t is not None:
                    if t > far and region.contains(t):
                        far = t
                    terminator = is_uncond_branch(w)
                elif op == 2:                                   # j
                    t = (i & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                    if region.contains(t) and t not in self.seeds and i < t < i + 0x8000:
                        far_j = max(far_j, t)
                    elif not fstart <= t <= i:
                        self.j_out.add(t)
                    terminator = True
                elif is_jr(w):
                    if w != JR_RA:
                        cases = self.jump_table(region, fstart, i)
                        if cases:
                            ntables += 1
                            self.jtbl_targets.update(cases)
                            far = max(far, max(cases))
                    terminator = True
                if terminator and max(far, far_j) <= i + 4:
                    # A delay slot that is itself a known entry means this
                    # "transfer" is padding junk in front of a real function.
                    fend = i + 4 if self._is_entry(i + 4) else min(i + 8, end)
                    junk_end = fend == i + 4
                    break
                i += 4
            if fend == fstart:
                fend = fstart + 4
            funcs.append(Func(fstart, fend - fstart, region.name, called=fstart in self.seeds,
                              pointed=fstart in self.data_ptrs, prologue=is_prologue(self.w(region, fstart)),
                              jtbl=ntables, kind="filler" if junk_end and fend - fstart <= 12 else "func"))
            pc = self._next_start(region, fend, funcs)
        return funcs

    def _next_start(self, region: Region, fend: int, funcs: list[Func]) -> int:
        """Skip object-alignment filler after a function."""
        align = self.game.obj_align
        if fend >= region.vram_end or fend % align == 0 or self._is_entry(fend):
            return fend
        aligned = (fend + align - 1) & ~(align - 1)
        if aligned >= region.vram_end:
            return fend
        filler = [self.w(region, a) for a in range(fend, aligned, 4)]
        plausible = any(filler) and self.entry_code(region, fend, fend + 16)
        if plausible and (self._is_entry(aligned) or
                          (is_prologue(self.w(region, aligned)) and not is_prologue(self.w(region, fend)))):
            # Only a complete tiny function can sit in front of another entry.
            plausible = aligned - fend >= 8 and is_terminator(self.w(region, aligned - 8))
        if plausible:
            return fend
        funcs.append(Func(fend, aligned - fend, region.name, kind="filler"))
        return aligned

    def run(self) -> list[Func]:
        for _ in range(6):
            self.j_out.clear()
            out: list[Func] = []
            for r in self.game.code:
                out += self.scan_region(r)
            starts = {f.vram for f in out if f.kind == "func"}
            new = {t for t in self.j_out if self.game.code_region(t) and t not in starts and t not in self.seeds}
            if not new:
                break
            self.seeds |= new
            self.tail_entries |= new
        self.mid_entries = {s for s in self.seeds if s not in starts}
        # A jal into the middle of a function only happens in hand-written
        # assembly (several entry points sharing code). Split there so every
        # call target is a function of its own.
        for entry in sorted(self.mid_entries):
            i = next(k for k, f in enumerate(out) if f.vram < entry < f.vram + f.size)
            f = out[i]
            tail = Func(entry, f.vram + f.size - entry, f.region, kind=f.kind, asm=True)
            f.size = entry - f.vram
            f.asm = True
            out.insert(i + 1, tail)
        for f in out:
            f.called = f.vram in self.seeds and f.vram not in self.tail_entries
        return out


def load_reference(path: str) -> dict[str, dict[int, tuple[str, int]]]:
    """Functions from an N64Recomp context dump: {section: {vram: (name, size)}}."""
    ref: dict[str, dict[int, tuple[str, int]]] = {}
    section = None
    for line in open(path, encoding="utf-8"):
        m = re.match(r'name = "(.+)"', line)
        if m:
            section = m.group(1)
            ref[section] = {}
            continue
        m = re.search(r'name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+)', line)
        if m and section:
            ref[section][int(m.group(2), 16)] = (m.group(1), int(m.group(3), 16))
    return ref


def load_names(path: str) -> dict[int, str]:
    """Function names from a splat symbol_addrs file."""
    names: dict[int, str] = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"(\S+) = (0x[0-9A-Fa-f]+); // type:func", line)
        if m:
            names[int(m.group(2), 16)] = m.group(1)
    return names


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("game", choices=sorted(GAMES))
    ap.add_argument("--out")
    ap.add_argument("--check-names", help="splat symbol_addrs file with known function entries")
    args = ap.parse_args()

    finder = Finder(GAMES[args.game])
    funcs = finder.run()
    for r in finder.game.code:
        fs = [f for f in funcs if f.region == r.name and f.kind == "func"]
        other = [f for f in funcs if f.region == r.name and f.kind != "func"]
        print(f"{r.name:18s} functions={len(fs):5d} called={sum(f.called for f in fs):5d} "
              f"pointed-only={sum(f.pointed and not f.called for f in fs):4d} "
              f"unreferenced={sum(not f.called and not f.pointed for f in fs):4d} "
              f"with-switch={sum(1 for f in fs if f.jtbl):4d} "
              f"filler={sum(f.kind == 'filler' for f in other)} "
              f"data={sum(f.size for f in other if f.kind == 'data')}B")
    print(f"entries reached only by `j`: {len(finder.tail_entries)}; "
          f"jal targets inside a function: {len(finder.mid_entries)}")
    if finder.mid_entries:
        print("   mid-function entries:", " ".join(f"{v:08X}" for v in sorted(finder.mid_entries)[:40]))

    if args.check_names:
        names = load_names(args.check_names)
        for r in finder.game.code:
            known = {v: n for v, n in names.items() if r.contains(v)}
            if not known:
                continue
            mine = [f for f in funcs if f.region == r.name]
            starts = {f.vram for f in mine if f.kind == "func"}
            missed = sorted(v for v in known if v not in starts)
            print(f"-- {r.name}: known entries found {len(known) - len(missed)}/{len(known)} "
                  f"({100 * (len(known) - len(missed)) / len(known):.2f}%)")
            for v in missed[:20]:
                c = next((f for f in mine if f.vram <= v < f.vram + f.size), None)
                where = f"{c.kind} {c.vram:08X}+{c.size}" if c else "nowhere"
                print(f"     missed {v:08X} {known[v][:40]:40s} inside {where}")

    if args.out:
        json.dump({"game": args.game, "functions": [f.__dict__ for f in funcs],
                   "mid_entries": sorted(finder.mid_entries), "tail_entries": sorted(finder.tail_entries)},
                  open(args.out, "w"), indent=0)
        print(f"written {args.out}")


if __name__ == "__main__":
    main()
