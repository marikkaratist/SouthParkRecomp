**English** | [Русский](README.ru.md)

# South Park: Recompiled

A native PC port of South Park (Nintendo 64, USA) made by static recompilation, built on
[N64Recomp](https://github.com/N64Recomp/N64Recomp), N64ModernRuntime, RT64 and RecompFrontend.

The ROM is not included and never will be: you need your own dump of the game.

## Features

- **Widescreen** done properly: no black bars, the 3D view fills the window, and objects at the
  sides are drawn (the game's view frustum is widened to the window's shape). The sky covers the
  full width, and the HUD can sit at the window's edges or in the middle as on the console
  (General → HUD Placement).
- **60 updates per second.** The game reads the controls and moves the world 60 times a second
  instead of 30, at the original speed (General → Game Update Rate). On top of that, RT64 draws
  in-between frames at your display's refresh rate with per-object interpolation, so motion and
  camera cuts stay clean.
- **Modern controls**, as in today's console shooters: the left stick walks and strafes, the
  right stick or the mouse turns and looks, and the view stays where you leave it; the left mouse
  button fires (General → Control Scheme, Invert Look, Mouse Sensitivity). The Original scheme
  keeps the Nintendo 64 layout.
- **Draw distance**: Original, or Far, where the world is drawn twice as far and the fog starts
  twice as far away (General → Draw Distance). The engine's per-frame limits on models, materials
  and deferred objects were lifted so that distant buildings don't drop out.
- A launcher with controller and keyboard navigation, controller remapping, and Controller Pak
  saves stored next to the settings.

Mod support is not a goal of this project; the launcher's Mods page comes with the frontend.

## Installation

1. Download `SouthParkRecompiled-v0.9.0-windows-x64.zip` from [Releases](../../releases) and
   unpack it anywhere.
2. Run `SouthParkRecompiled.exe`.
3. In the launcher, pick your South Park (USA) dump in `.z64` format. If yours is `.v64` or
   `.n64`, convert it first with `tools/rom_prepare.py`. The ROM is checked by its hash (SHA1 below).

Requires Windows 10/11 x64 and a GPU with DirectX 12 or Vulkan.

## ROM

| | |
|---|---|
| Game | South Park (USA), code `NDTE`, revision 0 |
| Size | 16 MB |
| SHA1 (.z64) | `42d7350d1f9040c5546f481c4d650b99a23aadbc` |
| Header CRC | `7ECBE939 3C331795` |
| CIC | 6102 |
| Entry point | `0x80000400` |
| libultra | 2.0D |
| Microcode | F3DEX.NoN fifo 2.05, L3DEX fifo 2.05 |
| Data compression | RNC (~5850 blocks, from `0x9C9E34`) |

Preparing the ROM from any format (.z64 / .v64 / .n64); the original file is left untouched:

```
python tools/rom_prepare.py "<path to dump>" rom/southpark.us.z64
```

## HD textures (make your own pack)

Texture packs are `.rtz` mods, like the HD packs for Zelda 64: Recompiled or Banjo: Recompiled.
`tools/make_texture_pack.py` builds one from your own copy of the game with the Real-ESRGAN upscaler
([realesrgan-ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN/releases)):

1. Play with texture dumping on; every texture the game shows is saved once:
   ```
   SouthParkRecompiled.exe --dump-textures C:\sp\dump
   ```
2. Decode, upscale 4x and pack (Python with numpy and Pillow):
   ```
   python tools/make_texture_pack.py all C:\sp\dump C:\sp\work --esrgan <path to realesrgan-ncnn-vulkan.exe> --packer build\texture_packer.exe
   ```
3. Copy `C:\sp\work\southpark_hd.rtz` to `%LOCALAPPDATA%\SouthParkRecompiled\mods` and enable it under Mods.

Textures are matched by their RT64 hash, so play through every place you want covered before
building; anything not dumped keeps its original look. Tiling textures stay seamless and
transparent edges get no halo. The pack is made from your own copy of the game: keep it for
your own use.

## Repository layout

- `src/`, `include/`, `assets/`, `icons/` — the application: launcher, settings, game code (`src/game/`)
- `patches/` — MIPS code built into the game (currently a placeholder)
- `symbols/` — symbol file for N64Recomp and function names
- `southpark.us.toml`, `aspMain.us.toml`, `patches.toml` — recompiler configs
- `n64recomp-patches/` — changes to N64Recomp needed to get through this ROM
- `runtime-patches/` — changes to N64ModernRuntime, rt64 and RecompFrontend, applied to the submodules
- `tools/` — ROM analysis, artwork and debugging scripts
- `docs/` — memory map, symbols, how the runtime works
- `lib/` — submodules: N64ModernRuntime, rt64, RecompFrontend
- `external/N64Recomp` — the recompiler with our changes (in .gitignore)
- `rom/`, `references/`, `.venv/` — ROM, other projects for reference, Python environment (in .gitignore)

## Building (Windows)

You need:

- Visual Studio Build Tools with the "Desktop development with C++" workload and the Windows SDK;
- LLVM 20 or newer — `clang-cl` for the program itself (the MSVC 14.51 standard library rejects older versions);
- an LLVM with the MIPS target — `clang` for `patches/` (LLVM 23 Windows builds lack MIPS; 18.1.8 works);
- `make` (for example from w64devkit);
- N64Recomp built in `external/N64Recomp/build` with the patches from `n64recomp-patches/`. Its
  version must match the one in `lib/N64ModernRuntime/N64Recomp` (currently commit `2b6f056`):
  newer versions write section tables in a format this runtime doesn't know;
- your ROM: `python tools/rom_prepare.py "<dump>" rom/southpark.us.z64`.

Tool locations go in `build.local.ps1` (see the parameters at the top of `build.ps1`):

```
.\build.ps1              # recompile on first run, configure CMake, build
.\build.ps1 -Recompile   # run N64Recomp and RSPRecomp again
```

Submodules: `git submodule update --init --recursive`, then apply the changes from `runtime-patches/`:

```
git -C lib/N64ModernRuntime apply ../../runtime-patches/0001-n64modernruntime-fix-thread-queue-remove.patch ../../runtime-patches/0004-n64modernruntime-rsp-done-after-display-list.patch
git -C lib/rt64 apply ../../runtime-patches/0002-rt64-acclaim-f3dex2-as-f3dex2.patch ../../runtime-patches/0003-rt64-msaa-resolve-without-color-target.patch
git -C lib/RecompFrontend apply ../../runtime-patches/0005-recompfrontend-mouse-controller-focus-race.patch ../../runtime-patches/0006-recompfrontend-dump-textures-env.patch
```

## Milestones

- [x] 0. Tools, project setup, verified ROM
- [x] 1. ROM analysis: memory map, loader, TLB code paging — see [docs/LAYOUT.md](docs/LAYOUT.md)
- [x] 2. Symbols: function discovery, names carried over from Turok 2, first N64Recomp run — see [docs/SYMBOLS.md](docs/SYMBOLS.md)
- [x] 3. Recompilation and build: the program builds and opens the launcher — see [docs/RUNTIME.md](docs/RUNTIME.md)
- [x] 4. First boot and debugging: the game plays through the intro — see [docs/RUNTIME.md](docs/RUNTIME.md)
- [x] 5. Enhancements: widescreen, 60 updates per second, modern controls, draw distance — see [docs/RUNTIME.md](docs/RUNTIME.md#stage-5-polish)
- [ ] 6. Polish: remaining rough edges before 1.0

## License

GPL-3.0 — see [LICENSE](LICENSE). South Park and all rights to the game belong to their owners;
this project contains no game data.
