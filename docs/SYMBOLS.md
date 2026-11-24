**English** | [Русский](SYMBOLS.ru.md)

# South Park (USA) symbols

Result of stage 2: the function list, their names and the first successful N64Recomp run.

## Result

| | |
|---|---|
| Functions found | 2913 (engine 2553, VM module 360) |
| Identified from Turok 2 | 1117, of which 1041 with a real name |
| Named by hand | 24 |
| libultra and libaudio | 359 functions, 19 left unnamed (mostly assembly) |
| N64Recomp | processes the whole ROM, exit code 0 |
| Stubbed | 20 functions: 14 in assembly, 6 Controller Pak internals |
| Output | 58 C files, 20 MB, 2845 functions |

## Reproducing it

You need `rom/southpark.us.z64` and, for carrying names over, `rom/turok2.us.z64`
(Turok 2 USA 1.0, SHA1 `fb0400f2…1006f1`) and `references/turok2-decomp`.

```
python tools/find_functions.py southpark.us --out rom/southpark.us.funcs.json
python tools/find_functions.py turok2.us --out rom/turok2.us.funcs.json
python tools/match_turok2.py
python tools/make_symbols.py
external/N64Recomp/build/N64Recomp southpark.us.toml
```

`symbols/names.southpark.us.json` and `symbols/southpark.us.syms.toml` are in the repository,
so a normal build does not need the Turok 2 ROM.

N64Recomp must be built with `n64recomp-patches/0001-jump-table-in-another-section.patch`
(see below).

## Finding functions (`tools/find_functions.py`)

The ROM has no symbols; function boundaries are recovered from the code:

- `jal` targets are reliable entry points;
- a function ends after an unconditional jump (`jr ra`, `j`, `b`) unless a branch target
  beyond it has not been reached yet;
- `switch` tables are read from rodata so that `case` labels after the last `jr ra`
  stay inside the function;
- code pointers in data and addresses built with `lui`/`addiu` pairs (callbacks);
- padding between object files is told apart from code by decoding: invalid instructions
  and reads of registers that were never written.

South Park's object files are aligned to 8 bytes, Turok 2's to 16
(`obj_align` in `tools/games.py`). With the wrong value, 49 calls landed inside functions.

Check against Turok 2, where the answer is known from the decompilation: 2333 of 2337
hand-named engine functions were found (99.83%).

Known limitations:

- A function with no call or pointer to it, preceded by padding that looks like code,
  may get a start 4 bytes too early. It shows up at run time as "function not found"
  and is fixed with an entry in `symbols/manual_names.southpark.us.txt`.
- 544 engine functions are never referenced. They are dead code, or entry points whose
  addresses are computed in a way the tool can't see.

## Carrying names over (`tools/match_turok2.py`)

Both games were built by the same compiler from the same engine, so shared functions match
instruction for instruction once the linker-filled parts (call targets, `lui`/`lo` address
halves) are masked.

| Pass | Found | Meaning |
|---|---|---|
| exact | 793 | masked code is identical and unique in both games |
| callsite | 98 | called from the same place in two already matched functions |
| order | 226 | lies between two matched neighbours and its code is similar |

By region: system libraries (`0x0029…`, `0x002A…`) are identified almost entirely, game logic
(`0x0020…`, `0x0023…`, `0x0025…`) at 3–20%, which is expected: that is South Park's own code.
The VM module is identified at 11%.

The virtual memory manager got all its names: `VM_Initialize`, `VM_HandleTLBMiss`,
`VM_Alloc`, `VM_ManagerThread`, `VM_ReaderThread` and others (`0x0028FBA0–0x00291254`).

Names matter beyond readability: N64Recomp uses them to decide which libultra functions to
replace with the runtime's implementation. Of those found in South Park it replaces 57,
ignores 124 and renames 11.

## N64Recomp patch

N64Recomp looks for a jump table in the same segment as the function
(`rom = vram + func.rom − func.vram`). In this engine code is linked in useg
(`0x002xxxxx`) and rodata in KSEG0 (`0x800Axxxx`), so that calculation points outside the ROM.

The patch searches the other non-relocatable segments. Data segments are declared in the
symbol file without functions (`.data`, `.virtual_data`) only for this purpose.

## Stubs

Assembly that N64Recomp does not translate (coprocessor 0 registers, TLB, cache).
Each function was identified by its counterpart in Turok 2 (`turok2-decomp`, `docs/ASM_ORIGIN.md`).

| Function | Address | What it does | What happens to it |
|---|---|---|---|
| `boot_STVM_MapTLB_tail` | `0x00200500` | tail of the boot stub's TLB write | not needed: entry goes through `game_entry` |
| `STVM_MapTLB`, `STVM_UnmapTLB`, `STVM_DumpTLB` | `0x00291198…` | TLB writes and reads for the VM manager | replaced with our own code in stages 3–4 |
| `__osUnmapTLBAll` | `0x002A0450` | clears the TLB | an empty stub is enough |
| `__osException_handler` | `0x0029E1C0` | libultra exception handler | threads and interrupts are the runtime's job |
| `Debug_SetWatch` | `0x0027D704` | debug Watch registers | an empty stub is enough |
| `SNDebug_*` (7) | `0x002A51B0…` | SN64 debugger: cache flush, TLB and context dumps | an empty stub is enough |

## Left for the next stages

- **useg addresses in the runtime.** Generated code accesses memory through the `MEM_*`
  macros, which expect `0x800…`. `0x002…` and `0x004…` need translating, and function
  pointers in data look like `0x002xxxxx`.
- **Replacing the VM manager.** `VM_Initialize`, `VM_HandleTLBMiss` and ROM window requests
  through `VM_Alloc` must work without a TLB.
- **Entry point.** Not set in the config: the runtime must set the `0x803FFFC0` stack itself
  and call `game_entry`.
