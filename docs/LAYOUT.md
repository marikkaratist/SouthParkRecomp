**English** | [Русский](LAYOUT.ru.md)

# South Park (USA) code map

Result of stage 1. Everything was taken from the ROM itself with `tools/rom_prepare.py` and
`tools/layout_scan.py`; the method comes from `turok2-decomp` (`docs/VRAM-LAYOUT.md`).

## Summary

The memory scheme is the same as in Turok 2 (Iguana engine, "VM" = virtual memory):
code runs in useg through the TLB, data and BSS live in KSEG0.

| Region | ROM | Address | Note |
|---|---|---|---|
| Header, IPL3 | `0x0000–0x1000` | — | CIC 6102 |
| Boot stub | `0x1000–0x1100` | `0x80000400` | KSEG0, only sets up the TLB |
| Engine + libultra, `.text` | `0x1100–~0xA8000` | `0x00200500–…` | useg, `ROM = address − 0x1FF400` |
| `.rodata` / `.data` | `~0xA8000–0xC2C00` | `…–0x800C2000` | KSEG0, `ROM = address − 0x7FFFF400` |
| BSS | — | `0x800C2000–0x800F3730` | cleared by the stub |
| VM module A | `0xC4000–~0xDB810` | `0x00400000` | useg, paged in from ROM |
| VM module B | `0xDC000–~0xF2320` | `0x00400000` | apparently unused (see below) |
| Resources | `0xF4000–0x1000000` | — | RNC blocks from `0x9C9E34` |

Initial stack: `0x803FFFC0`. `main` is `0x0022BCE8`.

## Boot stub

IPL3 copies 1 MB from ROM `0x1000` to `0x80000400`. The stub:

1. sets up the stack and clears BSS;
2. clears the TLB (entries 30…0);
3. maps `0x00200000–0x003FFFFF` to physical `0x000000–0x1FFFFF` with entry 31
   (PageMask `0x1FE000`, two 1 MB pages);
4. jumps to `0x0022BCE8`.

Consequence: an address `0x002xxxxx` is just a second view of `0x800xxxxx`, offset by
`0x7FE00000`. The engine is not paged; it is fully in memory from the start.

## Calls (`jal`) confirm the layout

- 9776 calls go to `0x002xxxxx/0x003xxxxx`, 783 to `0x004xxxxx`.
- There is no code in KSEG0 (`0x800xxxxx`): libultra is linked together with the engine in useg
  (TLB instructions and `eret` sit at ROM `0x91DF8–0xA64E8`). Turok 2 differs: its libultra is in KSEG0.
- 942 of 1671 unique call targets in the engine land on a function prologue (56%; the rest are
  functions without a stack frame).

## VM manager

Manager code: `0x0028FBA0–0x00291254`. It is the same `engine/Virtual/VMEMORY.C` + `VMASM.S`
as in Turok 2 (there `VM_Initialize` … `VM_FinishPageJob`, `0x0028D820–0x0028ED10`; names and
C code are in `turok2-decomp`).

- `0x0028FBA0` — initialization (like `VM_Initialize`). Remembers the cartridge base
  `0xB00C4000` (ROM `0xC4000`) for the `0x00400000` region.
- With more than `0x4FFFFF` bytes of memory (Expansion Pak present): allocates `0x16F10` bytes
  and maps `0x00400000` onto them with a single TLB entry (PageMask `0x7E000`) — the module is
  loaded whole.
- Without an Expansion Pak — demand paging through the TLB miss handler.
- `0x00291198` — TLB write (like `STVM_MapTLB`). Called by `0x0028FBA0`, `0x0028FF20`,
  `0x00290270`, `0x0029093C`.

## Two modules at one address

At ROM `0xC4000` and `0xDC000` there are two different ~96 KB code blocks, both linked at
`0x00400000` (10.6% opcode match — they are not copies).

The engine uses module A:

- the code only refers to `0xB00C4000`;
- of the 41 addresses at which the engine calls `0x004xxxxx`, 40 are function starts in module A
  and only 3 in module B;
- module B accesses data at `0x8012xxxx`, module A at `0x8010xxxx`: they were built for
  different data layouts.

Working hypothesis: module B is a leftover from another build. To be checked in stage 4 (the
runtime should see no ROM reads from `0xDC000–0xF4000`).

## What this means for recompilation

- The stub is not recompiled: the runtime enters directly at `0x0022BCE8` and sets the stack itself.
- Module A can stay in memory permanently, as with an Expansion Pak. The module overlap that
  forced Turok 2 to split banks apparently doesn't exist here.
- useg addresses must be translated in the runtime's memory: `0x002xxxxx` → `0x800xxxxx − 0x7FE00000`,
  `0x004xxxxx` → a separate window. Function pointers in data are of the form `0x002xxxxx`.
- The VM manager also hands the engine ROM windows for data (Turok 2 loads its sound banks this
  way). The TLB miss handler cannot be recompiled and has to be replaced with our own code.

## Open questions

- Exact `.text` / `.rodata` / `.data` boundaries in the main segment.
- What lies at ROM `0xC2C00–0xC4000`, `0xDAF10–0xDC000` and `0xF4000+` (tables; looks like module data).
- Which other regions, besides code, the engine requests from the VM manager.
- The ROM only contains F3DEX/L3DEX; the audio microcode has not been identified yet.
