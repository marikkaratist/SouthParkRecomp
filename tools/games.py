"""Memory layout profiles for the ROMs this project works with.

Both games use the Iguana/Acclaim engine layout: code runs TLB-mapped in useg
(0x002xxxxx engine, 0x004xxxxx paged module), data sits in KSEG0.
See docs/LAYOUT.md.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class Region:
    name: str
    rom: int
    vram: int
    size: int

    @property
    def rom_end(self) -> int:
        return self.rom + self.size

    @property
    def vram_end(self) -> int:
        return self.vram + self.size

    def contains(self, vram: int) -> bool:
        return self.vram <= vram < self.vram_end

    def to_rom(self, vram: int) -> int:
        return vram - self.vram + self.rom


@dataclass(frozen=True)
class Game:
    name: str
    rom_path: Path
    code: tuple[Region, ...]          # executable regions
    data: tuple[Region, ...]          # initialised data the code points into (jump tables, pointers)
    obj_align: int = 16               # padding between object files in .text
    extra: dict = field(default_factory=dict)

    def code_region(self, vram: int) -> Region | None:
        return next((r for r in self.code if r.contains(vram)), None)

    def data_region(self, vram: int) -> Region | None:
        return next((r for r in self.data if r.contains(vram)), None)


SOUTHPARK = Game(
    name="southpark.us",
    rom_path=ROOT / "rom" / "southpark.us.z64",
    code=(
        Region(".code", 0x1100, 0x00200500, 0xA7ED8 - 0x1100),
        Region(".virtual", 0xC4000, 0x00400000, 0x14AA0),
    ),
    data=(
        Region(".data", 0xA7ED8, 0x800A72D8, 0xC2C00 - 0xA7ED8),
        # Menu descriptor records of the paged module; VM init maps 0x16F10 bytes.
        Region(".virtual_data", 0xC4000 + 0x14AA0, 0x00414AA0, 0x16F10 - 0x14AA0),
    ),
    obj_align=8,
)

# Turok 2 (USA 1.0), the ROM turok2-decomp targets. Used to validate the tools
# against a known answer and as the source of function names.
TUROK2 = Game(
    name="turok2.us",
    rom_path=ROOT / "rom" / "turok2.us.z64",
    code=(
        Region(".code", 0x1100, 0x00200500, 0xA5DF8 - 0x1100),
        Region(".code_tail_funcs", 0xC2ED8, 0x002C22D8, 0xC3074 - 0xC2ED8),
        Region(".virtual", 0x14A000, 0x00400000, 0x3C000),
        Region(".virtual_1", 0x186C14, 0x00403C14, 0x1BFA00 - 0x186C14),
    ),
    data=(
        Region(".code_rodata", 0xA5DF8, 0x800A51F8, 0xC2ED8 - 0xA5DF8),
    ),
)

GAMES = {g.name: g for g in (SOUTHPARK, TUROK2)}
