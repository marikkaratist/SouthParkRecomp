"""Carry function names from Turok 2 (USA 1.0) over to South Park.

Usage: python tools/match_turok2.py [--out symbols/names.southpark.us.json]

Both games are built from the same engine with the same compiler, so shared
functions are identical instruction for instruction once link-time addresses
are masked out. Passes, from most to least certain:

  exact     the masked code is identical and unique on both sides
  callsite  called from the same place in two functions already matched
  order     sits between two matched neighbours and its code is similar
"""
from __future__ import annotations

import argparse
import bisect
import collections
import difflib
import hashlib
import json
import struct
from dataclasses import dataclass, field
from pathlib import Path

from find_functions import load_names, simm
from games import GAMES, ROOT, Game

LO_OPS = frozenset({0x09, 0x0D, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E,
                    0x31, 0x35, 0x39, 0x3D, 0x37, 0x3F})
LOAD_OPS = frozenset({0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x37})


@dataclass
class F:
    vram: int
    size: int
    region: str
    words: tuple[int, ...]
    masked: tuple[int, ...] = ()
    shape: tuple[int, ...] = ()        # opcode classes only, for fuzzy comparison
    calls: tuple[tuple[int, int], ...] = ()   # (instruction index, jal target)
    key: bytes = b""
    name: str = ""
    refs: int = 0

    @property
    def end(self) -> int:
        return self.vram + self.size


def mask_function(words: tuple[int, ...], vram: int) -> tuple[tuple[int, ...], tuple[int, ...], tuple[tuple[int, int], ...]]:
    """Blank out everything the linker fills in: call targets and hi/lo address halves."""
    masked, shape, calls = [], [], []
    hi: set[int] = set()
    for i, w in enumerate(words):
        op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        m = w
        if op in (2, 3):
            m = w & 0xFC000000
            if op == 3:
                calls.append((i, ((vram + i * 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)))
                hi.clear()
        elif op == 0x0F:
            m = w & 0xFFFF0000            # keep rt, drop the address half
            hi.add(rt)
        elif op in LO_OPS and rs in hi:
            m = w & 0xFFFF0000
            if op in (0x09, 0x0D) or op in LOAD_OPS:
                hi.discard(rt)            # rt now holds a full address or a loaded value
        else:
            dest = rd if op == 0 else (rt if 8 <= op <= 15 or op in LOAD_OPS else None)
            if dest in hi:
                hi.discard(dest)
            if op == 0 and (w & 0x3F) == 9:      # jalr
                hi.clear()
        masked.append(m)
        if op == 0:
            shape.append(0x100 | (w & 0x3F))
        elif op == 1:
            shape.append(0x200 | rt)
        elif op in (16, 17, 18):
            shape.append((op << 10) | (rs << 5) | ((w & 0x3F) if rs >= 16 else 0))
        else:
            shape.append(op)
    return tuple(masked), tuple(shape), tuple(calls)


def load_functions(game: Game) -> list[F]:
    data = json.load(open(ROOT / "rom" / f"{game.name}.funcs.json"))
    rom = game.rom_path.read_bytes()
    regions = {r.name: r for r in game.code}
    out = []
    for d in data["functions"]:
        if d["kind"] != "func":
            continue
        r = regions[d["region"]]
        o = r.to_rom(d["vram"])
        words = struct.unpack(f">{d['size'] // 4}I", rom[o:o + d["size"]])
        f = F(d["vram"], d["size"], d["region"], words)
        f.masked, f.shape, f.calls = mask_function(words, f.vram)
        f.key = hashlib.sha1(struct.pack(f">{len(f.masked)}I", *f.masked)).digest()
        out.append(f)
    return out


def similarity(a: F, b: F) -> float:
    if a.masked == b.masked:
        return 1.0
    if not a.shape or not b.shape:
        return 0.0
    ratio = min(len(a.shape), len(b.shape)) / max(len(a.shape), len(b.shape))
    if ratio < 0.5:
        return 0.0
    return difflib.SequenceMatcher(None, a.shape, b.shape, autojunk=False).ratio()


class Matcher:
    def __init__(self) -> None:
        self.sp = load_functions(GAMES["southpark.us"])
        self.t2 = load_functions(GAMES["turok2.us"])
        self.sp_at = {(f.region, f.vram): f for f in self.sp}
        self.t2_at = {(f.region, f.vram): f for f in self.t2}
        self.sp_by_vram = collections.defaultdict(list)
        self.t2_by_vram = collections.defaultdict(list)
        for f in self.sp:
            self.sp_by_vram[f.vram].append(f)
        for f in self.t2:
            self.t2_by_vram[f.vram].append(f)
        names = load_names(str(ROOT / "references/turok2-decomp/versions/symbols_names.us.txt"))
        for f in self.t2:
            # the second paged module is named 0x39000 above its link address
            f.name = names.get(f.vram + 0x39000 if f.region == ".virtual_1" else f.vram, "")
        self.pairs: dict[int, tuple[F, F, str, float]] = {}   # id(sp func) -> (sp, t2, how, score)
        self.used_t2: dict[int, int] = {}                     # id(t2 func) -> id(sp func)

    def add(self, s: F, t: F, how: str, score: float) -> bool:
        if id(s) in self.pairs or id(t) in self.used_t2:
            return False
        self.pairs[id(s)] = (s, t, how, score)
        self.used_t2[id(t)] = id(s)
        return True

    # -- pass 1 -------------------------------------------------------------
    def exact(self) -> None:
        by_key_sp = collections.defaultdict(list)
        by_key_t2 = collections.defaultdict(list)
        for f in self.sp:
            by_key_sp[f.key].append(f)
        for f in self.t2:
            by_key_t2[f.key].append(f)
        self.dup_sp, self.dup_t2 = by_key_sp, by_key_t2
        for key, ss in by_key_sp.items():
            ts = by_key_t2.get(key)
            if not ts or len(ss) != 1:
                continue
            # Turok 2 carries its paged code twice; identical twins are one function.
            named = [t for t in ts if t.name] or ts
            if len({t.name for t in named}) == 1 or len(ts) == 1:
                if ss[0].size >= 16 or len(ts) == 1 and ss[0].size >= 12:
                    self.add(ss[0], named[0], "exact", 1.0)

    # -- pass 2 -------------------------------------------------------------
    def _resolve(self, table, vram: int, near: F) -> F | None:
        c = table.get(vram)
        if not c:
            return None
        return c[0] if len(c) == 1 else next((x for x in c if x.region == near.region), c[0])

    def callsites(self) -> int:
        votes: dict[tuple[int, int], int] = collections.Counter()
        objs: dict[int, F] = {}
        for s, t, how, _ in list(self.pairs.values()):
            if len(s.calls) != len(t.calls) or s.shape != t.shape:
                continue
            for (i, sv), (j, tv) in zip(s.calls, t.calls):
                if i != j:
                    break
                cs, ct = self._resolve(self.sp_by_vram, sv, s), self._resolve(self.t2_by_vram, tv, t)
                if cs and ct and id(cs) not in self.pairs:
                    votes[(id(cs), id(ct))] += 1
                    objs[id(cs)], objs[id(ct)] = cs, ct
        best: dict[int, list[tuple[int, int]]] = collections.defaultdict(list)
        for (a, b), n in votes.items():
            best[a].append((n, b))
        added = 0
        for a, cands in best.items():
            cands.sort(reverse=True)
            total = sum(n for n, _ in cands)
            n, b = cands[0]
            if n / total < 0.8:
                continue                      # callers disagree
            s, t = objs[a], objs[b]
            if id(t) in self.used_t2:
                twin = next((x for x in self.dup_t2[t.key] if id(x) not in self.used_t2), None)
                if twin is None:
                    continue
                t = twin
            sim = similarity(s, t)
            if sim >= 0.45 or (n >= 3 and sim >= 0.25):
                added += self.add(s, t, "callsite", sim)
        return added

    # -- pass 3 -------------------------------------------------------------
    def order(self, sp_region: str, t2_region: str, threshold: float = 0.7) -> int:
        sp = sorted((f for f in self.sp if f.region == sp_region), key=lambda f: f.vram)
        t2 = sorted((f for f in self.t2 if f.region == t2_region), key=lambda f: f.vram)
        t2_index = {id(f): i for i, f in enumerate(t2)}
        anchors = [(i, t2_index[id(self.pairs[id(f)][1])]) for i, f in enumerate(sp)
                   if id(f) in self.pairs and id(self.pairs[id(f)][1]) in t2_index]
        # longest increasing run of anchors = the part of both games linked in the same order
        tails: list[int] = []
        prev = [-1] * len(anchors)
        pos: list[int] = []
        for k, (_, j) in enumerate(anchors):
            p = bisect.bisect_left(tails, j)
            if p == len(tails):
                tails.append(j)
                pos.append(k)
            else:
                tails[p] = j
                pos[p] = k
            prev[k] = pos[p - 1] if p else -1
        chain = []
        k = pos[-1] if pos else -1
        while k != -1:
            chain.append(anchors[k])
            k = prev[k]
        chain.reverse()
        added = 0
        for (i0, j0), (i1, j1) in zip(chain, chain[1:]):
            gs, gt = sp[i0 + 1:i1], t2[j0 + 1:j1]
            gs = [f for f in gs if id(f) not in self.pairs]
            gt = [f for f in gt if id(f) not in self.used_t2]
            if not gs or not gt or len(gs) * len(gt) > 400:
                continue
            # align the two gaps in order (Needleman-Wunsch on code similarity)
            n, m = len(gs), len(gt)
            sim = [[similarity(a, b) for b in gt] for a in gs]
            dp = [[0.0] * (m + 1) for _ in range(n + 1)]
            for a in range(1, n + 1):
                for b in range(1, m + 1):
                    dp[a][b] = max(dp[a - 1][b], dp[a][b - 1],
                                   dp[a - 1][b - 1] + (sim[a - 1][b - 1] if sim[a - 1][b - 1] >= threshold else -1))
            a, b = n, m
            while a and b:
                sc = sim[a - 1][b - 1]
                if sc >= threshold and dp[a][b] == dp[a - 1][b - 1] + sc:
                    added += self.add(gs[a - 1], gt[b - 1], "order", sc)
                    a, b = a - 1, b - 1
                elif dp[a][b] == dp[a - 1][b]:
                    a -= 1
                else:
                    b -= 1
        return added

    def run(self) -> None:
        self.exact()
        print(f"pass exact    : {len(self.pairs)}")
        for rnd in range(8):
            a = self.callsites()
            b = self.order(".code", ".code") + self.order(".virtual", ".virtual") + self.order(".virtual", ".virtual_1")
            print(f"round {rnd}: callsite +{a}, order +{b}  (total {len(self.pairs)})")
            if not a and not b:
                break


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "symbols" / "names.southpark.us.json"))
    args = ap.parse_args()
    m = Matcher()
    m.run()

    how = collections.Counter(p[2] for p in m.pairs.values())
    named = [p for p in m.pairs.values() if p[1].name]
    for region in sorted({f.region for f in m.sp}):
        fs = [f for f in m.sp if f.region == region]
        got = [f for f in fs if id(f) in m.pairs]
        nm = [f for f in got if m.pairs[id(f)][1].name]
        print(f"{region:10s} functions {len(fs):5d}  matched {len(got):5d} ({100 * len(got) / len(fs):.1f}%)  "
              f"with a real name {len(nm):5d} ({100 * len(nm) / len(fs):.1f}%)  "
              f"code bytes matched {100 * sum(f.size for f in got) / sum(f.size for f in fs):.1f}%")
    print("by pass:", dict(how), " named:", len(named))

    out = []
    seen: collections.Counter = collections.Counter()
    for s, t, kind, score in sorted(m.pairs.values(), key=lambda p: (p[0].region, p[0].vram)):
        name = t.name
        if name:
            seen[name] += 1
            if seen[name] > 1:
                name = f"{name}_{s.vram:08X}"
        out.append({"vram": s.vram, "region": s.region, "size": s.size, "name": name,
                    "t2_vram": t.vram, "t2_region": t.region, "t2_size": t.size, "how": kind, "score": round(score, 3)})
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    json.dump(out, open(args.out, "w"), indent=0)
    print(f"written {args.out}")


if __name__ == "__main__":
    main()
