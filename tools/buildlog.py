"""Summarise rom/build.log (written by build.ps1 through PowerShell redirection)."""
import re
import sys
from pathlib import Path

data = (Path(__file__).resolve().parent.parent / "rom" / "build.log").read_bytes()
text = data.decode("utf-16", "replace") if data[:2] in (b"\xff\xfe", b"\xfe\xff") else data.decode("utf-8", "replace")
lines = [l for l in text.replace("\r", "").split("\n") if l.strip()]
steps = re.findall(r"\[(\d+)/(\d+)\]", text)
failed = [i for i, l in enumerate(lines) if "FAILED" in l or re.search(r"\berror\b[: ]", l) and "warning" not in l.lower()]
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
print("progress", "/".join(steps[-1]) if steps else "-", "| errors", len(failed))
if "-v" in sys.argv:
    shown = set()
    for i in failed[:8]:
        for j in range(max(0, i - 1), min(len(lines), i + 8)):
            if j not in shown:
                shown.add(j)
                print(lines[j][:300])
        print("   ...")
    print("--- tail"); print("\n".join(l[:200] for l in lines[-6:]))
