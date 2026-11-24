"""Find every function N64Recomp cannot translate.

Usage: python tools/recomp_trial.py

N64Recomp stops at the first function it fails on. This runs it repeatedly,
stubbing each failing function in a scratch config, until the whole ROM goes
through. The result is the list of functions that need a decision (stub,
ignore, or replace with native code) in southpark.us.toml.
"""
from __future__ import annotations

import re
import subprocess
import tomllib

from games import ROOT

RECOMP = ROOT / "external" / "N64Recomp" / "build" / "N64Recomp.exe"
BASE = ROOT / "southpark.us.toml"
SCRATCH = ROOT / "rom" / "trial.toml"


def main() -> None:
    base = tomllib.loads(BASE.read_text(encoding="utf-8"))
    known = list(base.get("patches", {}).get("stubs", []))
    failed: list[tuple[str, str]] = []
    for attempt in range(400):
        stubs = known + [n for n, _ in failed]
        SCRATCH.write_text(
            "[input]\n"
            'symbols_file_path = "../symbols/southpark.us.syms.toml"\n'
            'rom_file_path = "southpark.us.z64"\n'
            'output_func_path = "trial_out"\n\n'
            "[patches]\nstubs = [\n" + "".join(f'    "{s}",\n' for s in stubs) + "]\n",
            encoding="utf-8")
        p = subprocess.run([str(RECOMP), str(SCRATCH)], capture_output=True, text=True, cwd=ROOT)
        out = p.stdout + p.stderr
        m = re.search(r"Error recompiling (\S+)", out)
        if not m:
            print(f"finished after {attempt + 1} runs, exit code {p.returncode}")
            tail = [l for l in out.splitlines() if l.strip()][-6:]
            print("\n".join("   " + l[:200] for l in tail))
            break
        lines = [l.strip() for l in out.splitlines() if l.strip()]
        at = next(k for k, l in enumerate(lines) if l.startswith("Error in recompiling") or l.startswith("Error recompiling"))
        reason = lines[at - 1] if at and not lines[at - 1].startswith("[Info]") else "?"
        failed.append((m.group(1), reason))
        print(f"{len(failed):3d}. {m.group(1):40s} {reason[:110]}")
    (ROOT / "rom" / "trial_failed.txt").write_text("".join(f"{n}\t{r}\n" for n, r in failed), encoding="utf-8")


if __name__ == "__main__":
    main()
