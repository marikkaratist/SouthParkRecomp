**English** | [Русский](RUNTIME.ru.md)

# How the game lives in a runtime without a TLB

Design decisions from stage 3 onwards. The code is in `src/game/boot.cpp` and the other files in `src/game/`.

## The problem

On the console, code runs at useg addresses (`0x002…` engine, `0x004…` module) which the TLB
maps onto physical memory, while data lives in KSEG0 (`0x800…`). N64ModernRuntime knows
nothing of the TLB: it assumes every address is `0x80000000 + offset into memory`.

## Memory

Generated code and the runtime itself access memory the same way:

```
host address = rdram + (address − 0xFFFFFFFF80000000)
```

For a useg address `U` that is `rdram + 0x80000000 + U`, i.e. 2 GB above "normal" memory.
The runtime reserves 4 GB of address space for `rdram` and commits the first 512 MB.
We additionally commit two ranges in the same reservation:

| Game addresses | What is there | Filled with |
|---|---|---|
| `0x00200000–0x003FFFFF` | the engine (view of the first 2 MB) | a 1 MB copy from ROM `0x1000` at `0x00200400` |
| `0x00400000–0x0041FFFF` | VM module | `0x16F10` bytes from ROM `0xC4000` |

No changes to the runtime or to the `MEM_*` macros are needed for this.

Limitation: the `0x002…` region is a separate copy, not a second view of the same memory as on
the console. A write to a `0x800…` address does not show up in it. The engine never reads data
through `0x002…` (all data references go through `0x800…`); the copy only exists in case code is read.

## Functions by address

The runtime looks up functions by address in a table filled by `load_overlays`. At start it
registers the first megabyte of ROM at `0x80000400`, as IPL3 does, so the engine code ends up
registered at `0x80000500+`. The game, however, calls functions through pointers like
`0x002xxxxx`. `game_on_init` registers the sections a second time, at their real addresses:

- `.code`: ROM `0x1100` → `0x00200500`
- `.virtual`: ROM `0xC4000` → `0x00400000`

## Boot

Not recompiled and replaced by `recomp_entrypoint`:

- **the boot stub** (`0x80000400`): TLB setup;
- **`game_entry`** (`0x0022BCE8`): re-reads 1 MB of cartridge directly through the PI registers
  (`0xA4600010`, `0xB0001000`), clears BSS `0x800C2000–0x8011E000`, calls `osInitialize`
  and `game_main`. Direct cartridge reads are impossible in the runtime, and it has already
  loaded the data needed.

`recomp_entrypoint` sets FR, clears BSS (after the runtime's load it holds ROM bytes),
sets the stack to `0x803FFFC0 − 0x18` and calls `osInitialize_recomp` and `game_main`.

**The FR bit.** The code uses the odd FPU registers as independent registers. That is
Status.FR = 1 mode, which IPL3 leaves on the console. The bit is set on entry and on creation of
every thread (`on_thread_create`); otherwise reading odd registers yields zero.

## Virtual memory manager

The original uses it for two things:

1. **Module code** at `0x00400000` — page loading on TLB misses (or whole with an Expansion Pak).
2. **ROM windows for data** through `VM_Alloc(size, 0, rom_offset)`. There are two calls:

| Caller | Size | ROM | What it is |
|---|---|---|---|
| `CAudioWorld__Initialize` | `0x36DCC` | `0x100000` | instrument bank (ctl); the wave table follows it |
| `CAudioWorld__LoadSfxDataFile` | `0x10CD48` | `0x8BC1B4` | sound effects file |

Replacement:

- `VM_Initialize` — empty. The module is already in memory; no pager threads are created.
- `VM_Alloc` — allocates space in the `0x80C00000–0x81000000` pool and copies the whole ROM
  window there. Returns a normal KSEG0 address, not a useg one.

Why that pool: it is above the 8 MB the game sees and below 16 MB — the RSP addresses memory
with 24 bits, and the audio microcode reads ADPCM tables from these banks. The range
`0x80801000–0x80C00000` is taken by the patch binary (`patches/patches.ld`).

Risk: if the engine somewhere checks that a window address is below `0x80000000`, that check
will behave differently. The other `VM_*` functions remain recompiled, but nothing calls them:
the manager has only three external calls (`VM_Initialize`, `VM_Alloc` ×2).

## Controller Pak

The Controller Pak library (`pfs*.c`) is linked into the VM module (`0x0040F318–0x00412090`).
The runtime implements `osPfsInitPak`, `osPfsReadWriteFile`, `osPfsAllocateFile` and the other
top-level functions itself, and treats the internal ones (`__osContRamRead`, `__osPfsRWInode`, …)
as nonexistent. Therefore:

- names must be exact. Eleven functions came from Turok 2 with a `_2` suffix
  (the library is there twice); `tools/make_symbols.py` strips it;
- `osPfsAllocateFile` and `__osPfsRequestOneChannel` were named by hand;
- six internal functions missing from N64Recomp's lists (`__osPfsDeclearPage`,
  `__osClearPage`, `__osPfsReleasePages`, `__osBlockSum`, `__osSumcalc`, `__osIdCheckSum`)
  are stubbed: nothing calls them any more, and they reference removed SI functions.

## Build: what had to be worked around

- **Versions.** The submodules sit at the commits the template was tested with: N64ModernRuntime
  `ca568b6`, RecompFrontend `b3b7ebb`, rt64 `f0d8c9f`. N64Recomp must be the version embedded in
  the runtime (`2b6f056`).
- **SDL and Clang 19+.** `SDL_endian.h` from SDL 2.26 and 2.30.3 defines `_m_prefetch`, which is a
  builtin in newer Clang. `CMakeLists.txt` defines `__PRFCHWINTRIN_H`, which disables that definition.
- **MIPS.** LLVM 23 Windows builds lack the MIPS target, so `patches/` is compiled with
  Clang 18.1.8 and linked with `ld.lld` from LLVM 23.

## What the first boot revealed (stage 4)

Each item was a separate crash or hang, found from the log and the call stack.

### ROM windows must sit at the original addresses

The instrument bank contains pointers already relocated for address `0x00480000`. So
`VM_Alloc` reproduces the original `VM_AllocRegion` placement: windows follow the module one after
another (page `0x400`, `0x80` pages), each starting on an even page: `0x00480000`, then `0x004B8000`.
Their memory is committed the same way as the module's.

The RSP reads ADPCM tables from these banks and only accepts physical addresses below 16 MB.
So every window has a second copy in `0x80C00000–0x81000000`, and `osVirtualToPhysical` is
replaced (`sp_osVirtualToPhysical`) to return the copy's address for window addresses. The copies
are not synchronized, on the assumption that the game only reads this data after loading.

### Cartridge reads

The engine does not use libultra's PI manager. The loader thread `CLoader__Main` calls
`osPiRawStartDma` itself and waits for the PI interrupt message on its own queue
(`loader + 0xA48`, message `0x7D1`). The runtime has neither raw DMA nor a PI interrupt. The
replacement `sp_osPiRawStartDma` copies the data at once and posts the message to that queue
itself. The queue address is taken from the caller's `$s0` — the replacement is written for the
single call site.

### RNC unpacker

Written in assembly: its body (`0x0027D7F0`) starts in the delay slot of the last `jr ra` of the
`RNC_UnpackMethod2` wrapper and is called through a pointer. The boundary was fixed by hand
(`symbols/manual_funcs.southpark.us.txt`).

### Audio

The `__amMain` thread generates audio frames on every AI interrupt until `osAiGetStatus()`
reports "queue full". In the runtime that function always returns 0, so the loop never ended and
starved lower-priority threads. The replacements in `src/game/audio.cpp` report "full" when two
buffers' worth of audio is queued for playback.

### A bug in the runtime's scheduler

`ultramodern::thread_queue_remove` only removed a thread from a queue if it was first. The engine
changes other threads' priorities (`osSetThreadPri`); the runtime then takes the thread out of the
queue and re-inserts it — and the ready queue got corrupted: the idle thread fell out
("No threads left to run!") and the list became circular. Fix:
`runtime-patches/0001-n64modernruntime-fix-thread-queue-remove.patch`.

### Graphics

- **Microcode.** The game uses Acclaim's variant: "F3DEX2.NoN.fifo 2.05 (Acclaim)". RT64
  recognizes it but marks it unknown in its database, so geometry commands were not executed and
  parsing ran past the end of the list. Patch `0002` gives it the plain F3DEX2 handler. Acclaim's
  lighting differences are not implemented.
- **MSAA.** The game clears the depth buffer by setting it as the color buffer. Such a buffer pair
  has no color target, and RT64 with MSAA on accessed it unchecked. Patch `0003`.
- **Video mode.** RT64 finds the framebuffer from `VI_ORIGIN` and assumes it is one row into the
  buffer, as in libultra's stock modes. The game builds its own modes for a 480-pixel-wide display
  with offset 0, and RT64 showed a black screen. `src/game/video.cpp` passes the runtime a copy of
  the mode shifted by one row.

### A race between the game and rendering

The runtime told the game the RSP had finished the graphics task before RT64 even started reading
the display list (a comment in `ultramodern/src/events.cpp` calls this an assumption that holds
for most games). This engine reuses the display list and dynamic data buffers right after the RSP
signal. RT64 read memory the game was already overwriting. The symptoms were intermittent:
"exploded" animated geometry, flicker, a crash on a garbage palette address, an endless display
list (a hang in the comet cutscene).

Patch `0004` moves the RSP signal to after the list is processed — as on the console, where the
RSP interrupt arrives once the whole list has been read.

### Audio speed

Audio played 27% fast and higher in pitch. `src/main/main.cpp` (template code) guards against
latency build-up: if more than 100 ms of audio is queued on the device, some samples are dropped.
The queue length was computed at the game's rate (22050 Hz) although the queue holds audio already
converted to the output rate (48000 Hz), so it was overestimated 2.2× and the guard fired all the
time. It is now computed at the output rate. A measurement in `src/game/audio.cpp` logs the
requested and actual rate: 22050 and ~22080 frames per second.

## Submodule patches

They live in `runtime-patches/` and are applied to the working copies in `lib/` (so the submodules
show as "dirty"). After `git submodule update` they must be applied again:

```
git -C lib/N64ModernRuntime apply ../../runtime-patches/0001-n64modernruntime-fix-thread-queue-remove.patch
git -C lib/rt64 apply ../../runtime-patches/0002-rt64-acclaim-f3dex2-as-f3dex2.patch
git -C lib/rt64 apply ../../runtime-patches/0003-rt64-msaa-resolve-without-color-target.patch
git -C lib/N64ModernRuntime apply ../../runtime-patches/0004-n64modernruntime-rsp-done-after-display-list.patch
git -C lib/RecompFrontend apply ../../runtime-patches/0005-recompfrontend-mouse-controller-focus-race.patch
git -C lib/RecompFrontend apply ../../runtime-patches/0006-recompfrontend-dump-textures-env.patch
```

The first and third are bug fixes worth offering upstream. The fifth stops mouse motion from
reaching the launcher's UI while the controller or keyboard owns the focus (the hover and the
focus used to fight over the menu), and clears the hover left under the hidden cursor.

## Debugging

Command line switches (`include/southpark_dev.h`): `--rom`, `--autostart`, `--log`, `--dump-dl`, `--watchdog`.
Watchdog (`--watchdog N`): if no graphics task arrives for N seconds, the stacks of all threads and
the display list rendering is stuck on are written to the log.
On a crash, `abort` or an uncaught exception, the call stack is written to the log.

- `tools/run_game.ps1` — run the game for a given time and show the log with function names.
- `tools/screenshot.ps1` — the same with window captures (`-Launcher` stays in the launcher).
- `tools/crash_symbols.py` — function names from the linker map; for a build with a `.pdb`
  (`.\build.ps1 -Config RelWithDebInfo -BuildDir build-dbg`), source lines too.
- `tools/disasm.py` — disassemble a game function by name or address.

## Tested and untested (as of stage 4)

Tested by running (Release and RelWithDebInfo builds):

- the game starts from the launcher and from the command line;
- the Controller Pak screen, copyright screen, Iguana logo, warning, intro cutscene — 85 seconds
  without crashes;
- 3D geometry, textures and text are displayed, including in widescreen.

Not tested at that point:

- audio by ear after the speed fix (the measured rate is correct);
- controls and everything past the intro: menus, levels;
- saves: the game reports "No Controller Pak found";
- the hypothesis about the unused second VM module.

Known picture defect at that point: vertical lines along the 4:3 boundary in widescreen.

## Stage 5: polish

### Black border at the screen edges

`View_DrawBorder` (`0x0025E124`) draws a two-pixel black border around each view at the end of the
frame: on a TV it hid the ragged picture edge, in a wide window it shows as two vertical lines along
the 4:3 boundary. A hook in `southpark.us.toml` returns from the function if the view covers the
whole screen (`sp_skip_view_border` in `src/game/video.cpp`); between split-screen views the border
stays. Cutscene letterboxing is drawn by other code and is unaffected.

### Controller Pak (saves)

The game saves to a Controller Pak, while in N64ModernRuntime every `osPfs*` answers "no pak".
The pak file system is kept native to the game: libultra's functions from the ROM are renamed to
`sp_osPfs*` (`symbols/manual_names`) so that N64Recomp doesn't replace them with stubs, and are
recompiled like the rest of the game. Only the lowest layer is native (`src/game/pak.cpp`): reading
and writing a 32-byte page (`__osContRamRead`, `__osContRamWrite`), answering "pak inserted"
(`__osPfsGetStatus`, `osPfsIsPlug`) and empty `__osSiGetAccess`/`__osSiRelAccess`. There is one pak,
in the first controller; the file `saves/controller_pak_1.mpk` sits next to the settings, in the
same format emulators use. Tested: the file is created, the game starts. Not tested: actually
saving in the game.

### Frame interpolation (60 fps and above)

Above the native rate RT64 draws in-between frames and has to know which object in the new frame
matches which object in the old one. Without hints it guesses, and on camera cuts and changes in
drawing order models tear and the background jitters.

How the engine draws: the camera is one combined "view × projection" matrix
(`G_MTX PROJECTION|LOAD` at the start of the frame); every object and every model part has its own
world matrix (`G_MTX MODELVIEW|LOAD`). For fixed objects it lives at a constant address, and the
address serves as the object's identity. Animated models keep their matrices in per-frame scratch
memory (two alternating buffers) reached through segment 1, so there the identity is the location of
the `G_MTX` command itself in the model's list (model and part); RT64 tells instances of one model
apart by position (`auto` ordering).

Before a display list goes to RT64, `src/game/interpolation.cpp` replaces each `G_MTX` command with
a call to a small three-command list in spare memory (`0x80F80000`): an RT64 matrix group with the
identity, the original `G_MTX`, and a return. Each distinct `G_MTX` command gets one slot for good,
because the game keeps some lists between frames. A camera cut is detected from the camera
(a rotation of more than ~25° in a frame, or a jump in position); on such a frame all identities
change and nothing is carried across the cut. `--no-hints` turns the hints off for comparison.

Smoothness check: `tools/motion_probe.ps1` captures a narrow strip of the window about 60 times a
second, and `tools/motion_probe.py` prints the picture shift between neighbouring captures. Smooth
motion means shifts of one sign; a sign change means judder. The first version (one slot for all
commands with the same words) gave a "+44 +44 −65" pattern; after the fix the user sees the cutscene
as smooth. The numeric measurement after the fix was not completed; gameplay and split screen were
not checked. `--trace-camera` prints camera motion per frame.

### Modern controls (free look, mouse)

The player's input is gathered by `CPlayer__ReadInput` (`0x0022BDA0`, the same code as in Turok 2):
once a frame, for each player, it fills the structure at `player + 0xAF4`: `+0x04` turn (a speed,
−1..1), `+0x08` look (the pitch itself, −1..1: the movement code pulls the pitch `player + 0xB60`
towards this value every frame), `+0x0C` strafe, `+0x10` forward, buttons from `+0x18`. With the
default control type, look equals the stick position, so the view returns to level as soon as the
stick is released. Turning changes the player object's angle `+0x70`.

`src/game/controls.cpp` installs two hooks (`southpark.us.toml`):
- after `osContGetReadData` in `Controller_ReadPads` (`0x0022C1F8`): accumulates mouse motion,
  unbinds the right stick from the C buttons; the left mouse button gives Z (fire);
- after the call to `CPlayer__ReadInput` in `CPlayerList__Update` (`0x00235680`, player in `$s0`):
  the left stick writes forward and strafe, the right stick and the mouse write turn, and look comes
  from a value the file keeps for each player and changes at a rate rather than by position.

Settings (General tab): Control Scheme (Modern by default / Original), Invert Look,
Mouse Sensitivity (0 = mouse off). Works only with the game's default control type.
Tested: the build, running the cutscene without crashes. Not tested in game at the time: axis
signs, look and mouse speeds (constants `stick_look_rate`, `mouse_turn_scale`, `mouse_look_scale`).

### Widescreen: HUD at the edges and object culling

**HUD.** The game draws it after the 3D scene, under a projection without the camera, in two ways:
texture rectangles (`G_TEXRECT`) when it is still, and quads (the radar disc always, the rest while
sliding and rotating). Everything else flat is drawn in the same part of the frame: messages, the
pause menu. Some elements are queued by the game and drawn later, so "the stretch of the list from
CPlayer__DrawHud" doesn't cover everything.

The solution in `src/game/interpolation.cpp` (the same pass over the list as for the interpolation
hints):
- hooks `sp_hud_begin` / `sp_hud_end` around `CPlayer__DrawHud` (`0x0022FBC8`) report that the frame
  has a game HUD at all and that the view covers the whole screen;
- in such a frame every rectangle and quad of the flat part at the top level of the list becomes a
  candidate; its screen position is computed from the current matrices;
- candidates that sit in one row on screen and closer than `hud_gap` are merged into one element
  (the face and the health number, the radar and its rim, a line of text);
- an element entirely left or right of the middle is pinned to that edge of the window: for quads
  with RT64's viewport alignment (`G_EX_SETVIEWPORTALIGN` and a repeated viewport command), for
  rectangles with `G_EX_SETRECTALIGN`; while it is drawn the scissor is opened to the whole window.
  An element crossing the middle (the pause menu, a centred message) is left alone.
The commands are inserted through short lists in memory at `0x80FE0000` (they live for one frame).
Setting: General → HUD Placement (Edges / Middle).

**Culling.** `CCamera__Update` (`0x0025B68C`) builds the view frustum from five points (the eye and
four far corners) for a 4:3 picture; the game decides what to draw from it (`Frustum_FromCorners`,
`BoundingRect_FromPoints`). The hook `sp_widen_view_frustum` (`src/game/video.cpp`) stretches the
corners horizontally to the window's shape (with a 5% margin) before they are turned into world
coordinates.

Helper: `--mash <sec>` presses START and A alternately so that an automatic start reaches a level
by itself (`tools/run_game.ps1 -Mash`, `tools/screenshot.ps1 -GameArgs`).

Tested with level captures in a 16:9 window: the portrait, radar and health move to the left edge,
ammunition and score to the right, the pause menu stays centred. Not tested at the time: culling
(the captures show enough objects at the edges, but the effect itself is only visible in motion),
split screen, other levels and screens. Known then: the sky's cloud band stayed 4:3 wide; the fade
when a level appears also covered only 4:3.

**Sky and fades.** The game draws the sky as a grid of 48x48 texture tiles under the scene camera;
no tile spans the full width, so RT64 left them in the 4:3 middle. Now each tile is replaced by an
RT64 extended rectangle (`G_EX_TEXRECT`) whose left and right edges are anchored to the window point
matching their place in the 4:3 picture (`stretched_edge`), and the whole grid is stretched evenly
across the window. Condition: the rectangle is at the top level of the list and the current
projection is the scene camera (not the third, end-of-frame projection the pause menu is drawn under).

Full-screen fills (fades, cutscene letterboxing) are stretched by RT64 itself as long as the
rectangle covers the frame's common scissor. An important rule found through a bug: RT64 widens the
3D scene only if its scissor is not narrower than any other scissor in the same frame. The game's
scissor ends at 0x77C (479 pixels); the "wide" scissor this code sets while drawing pinned elements
must keep the game's coordinates and change only the anchor points (`emit_wide_scissor`), otherwise
the scene in cutscenes shrinks to 4:3.

`tools/screenshot.ps1` captures the game window through PrintWindow: other windows don't get into
the picture and the game doesn't need to be in front.

Tested with captures: full-width sky in a level, an undistorted pause menu, wide cutscenes.
Not tested: the fade itself when a level appears (the moment wasn't captured).

**Game update rate (mouse).** Measurement (`--trace-input`): the game updates 30 times a second
(time step 0.5, where 1.0 = 1/15 s), with two controller polls per update. The path from mouse
motion to turning is linear, without smoothing: `Math_EaseToward` moves the heading by
turn * k * step. Mouse "heaviness" is latency: motion reaches the game at the next update, and RT64
shows in-between frames between the last two updates, i.e. lags one more. At 30 updates that is up
to 60-70 ms.

Two limits hold the game at 30: each of the three framebuffers (0x800C2760, stride 0x108) has a
field at +0x104 — the minimum number of vertical blanks (VIs) on screen, 2; the scheduler
(func_00258618) holds a finished frame back until they pass. The main loop (`Game_RunFrame`,
0x002074B0) averages the VI counts of the last four frames, leaving out the longest, into 0x800CD380
and raises the result to at least 2 (3 with three or four players), then step = VI * 15 / 60.

Setting Game Update Rate (30 by default at first, as in the original). At 60 the hook
`sp_update_rate` (src/game/update_rate.cpp, before 0x002078A8) sets the framebuffers' minimum to 1
and removes the lower limit of 2 from the average. Measured: 60 updates a second, step 0.25. By
calculation the mouse latency is halved (the latency itself was not measured). Game speed is kept:
the screens after the first one and the Iguana cutscene run at the same speed (the time offset is
constant). Exception: the first copyright screen stays twice as long, about 8 s instead of 4 — it
counts updates, not time. There may be other such places, so at first 60 was not the default.

Debugging: `--trace-input`, `--fake-mouse <n>` (n mouse counts per poll), `--update-rate 30|60`.

**Crosshair.** Setting Crosshair: Off, Dot, Cross (Cross by default). src/game/crosshair.cpp
appended to the end of the game's display list (the pointer to free space is 0x800CD9B8) right after
the player's HUD, in the `sp_hud_end` hook, only for a full-screen view. The crosshair was filled
rectangles in the primitive color in one-cycle mode, white with a one-pixel black outline, in the
centre of the 480x360 picture. The game's render mode was saved and restored with RT64's extended
commands (PUSH/POP OTHERMODE, COMBINE, PRIMCOLOR), so the game's following commands were unaffected;
the extended commands themselves are enabled at the start of each task (interpolation.cpp), so with
`--no-hints` no crosshair was drawn. The crosshair was appended after the recorded HUD stretch, and
pinning the HUD to the edges did not touch it. Tested with captures: crosshair centred, hidden with
the HUD on pause, nothing else changed. (Removed later, see below.)

**Fixes after review (2026-11-16).**
- The "Expansion Pak found" text was distorted: its letters are drawn with the same rectangles under
  the scene camera as the sky and were stretched with it, showing neighbouring glyphs of the font.
  Now only rows of tiles that together cover the picture edge to edge are stretched.
- On pause the mouse and right stick moved the view: the game polls the controls on pause too, but
  doesn't move the player if the pause flag 0x800CD360 is set or a menu is open (+0x538) on the
  player's camera (+0xA54), like `CPlayer__IsPausedOrInMenu` in Turok 2. In that case
  `sp_controls_after_read_input` changes nothing and mouse motion during the pause is discarded.
  Verified by gaps in `--trace-input` during pauses.
- The radar fell apart on pause (the map left its ring): the HUD slides off screen, and some
  elements were dropped by the "off screen" filter. The filter now drops only large elements and
  very distant ones. A pinned element's scissor, if smaller than the picture, moves with it
  (`emit_item_scissor`) instead of opening to the whole window.
- The weapon icon (a 48x48 rectangle at the bottom right, by the hand) moves to the right edge: right
  edge at 0.95, mirroring the radar on the left. The player field +0xC4C turned out not to be it.
- A smaller, thinner crosshair: lines one picture pixel wide, a one-pixel outline. RT64 rounds the
  edges of such rectangles to whole pixels, so fractions of a pixel don't work.
- Game Update Rate is 60 by default; 30 is kept for places that count frames.

**Draw distance.** Setting Draw Distance: Original, Far (x2), Farthest (x4), Original by default.
The camera's far clip is the float at camera + 0x4F0: the far plane of the perspective
(guPerspectiveF in `CCamera__Update`) and of the view frustum the game culls against. Fog is set with
`gSPFogPosition(camera + 0x506, 1000)` (0x0025C8A0), the start in thousandths of the far clip and the
end at the far plane, so the fog moves out with the draw distance. The game sets the far clip from
the zone the camera is in (a smooth transition from the previous value at +0x4F4, func_0025AFD4), and
in cutscenes every frame. So a hook at the start of `CCamera__Update` (`sp_draw_distance_restore`)
gives the game back its own value if ours is in the field, and a hook at 0x0025BEE8
(`sp_draw_distance_apply`, where all paths after the write meet) multiplies it; the rest of the camera
update and the drawing see the larger value. Debugging: `--draw-distance <k>`.
Tested with captures in the first level (x1, x2, x4): the fog recedes, distant houses and mountains
are visible; the sky's cloud layer, almost hidden by fog in the original, is fully visible, with a
repeating pattern. Not tested: objects appearing at a distance in motion, other levels, cutscenes.

**Fixes after reviewing draw distance and the crosshair (2026-11-16).**
- Our crosshair is removed: the game's own options have a CROSSHAIRS entry. src/game/crosshair.cpp
  and the Crosshair setting are deleted; the crosshair description above is obsolete.
- Draw Distance: only Original and Far (x2). Farthest (x4) is removed: with it the ground and
  textures disappeared. The draw distance code moved to src/game/draw_distance.cpp.
- Level grid sections (`CScene__LoadVisibleGridSections`, 0x00201878, up to 128 of them, in the world
  array +0x127C, counter +0x1278) at Far are loaded nearest first (`sp_load_grid_sections`) rather
  than by grid rows. Measured on the first street: 1-4 sections at x2 and about 12 at x4; the limit is
  not reached.
- The game is told it has 12 MiB of memory instead of 8 (`sp_report_memory_size`, a hook in
  `InitializeSystemResources` 0x0021DB18 after osGetMemSize): the heap, and with it the model and
  texture cache, ends at 0x80B80000 instead of 0x80780000. The patch binary moved to the free top
  512 KiB, at 0x80B81000 (patches/patches.ld); the VM windows stay from 0x80C00000. The game only
  compares the memory size against 4 MiB; it puts the framebuffers and depth buffer at the top of the
  heap. The frame's display list now lies above 8 MiB (0x809BFFE0), and RT64 reads it. `--memory 8`
  restores 8 MiB.
- Found but not reproduced on the first street: objects at the side vanishing at Far. On this street
  at x2 the game requests models for six times as many static objects, with a handful of cache
  refusals (`--trace-input`, a counter in func_00227738); the frame's list is 0x9BF8 bytes of 0x20000.
  The likely cause in busy places is cache shortage, which is what the extra memory is for; left for
  the user to check.
- Debugging: `--trace-input` now also prints the camera rectangle sizes, grid sections, model
  readiness of static objects and the size of the frame's list.

**Draw distance: buildings by the school and the sky (2026-11-16).**
- Deferred objects. While drawing the level (`CScene__DrawLevel` 0x00201E40, `CScene__DrawLevelViews`
  0x0020234C, func_00203890), the game defers objects flagged "draw last" (static ones with flag 4,
  level ones with 0x20; apparently translucent: trees, windows) into a 32-entry list in the world
  (+0xFB4 counter, +0xFB8 pointers) and draws it afterwards from far to near (`CScene__DrawDeferred`
  0x00200A34). When the list is full, the object is simply not drawn. At Far more trees are in view,
  the list filled up, and houses dropped out depending on where the camera looked. Five hooks
  `sp_deferred_full_1..5` before the fullness check: if there is no room, the object is drawn at once
  with the same call as normal ones (func_00227738, `CLevelActor__Draw`, func_00218ED8), preserving
  the game's registers. Such an object is out of far-to-near order. Active at any draw distance. On the
  first street there are no overflows even at x2 (`--trace-input` counts them); it can only be checked
  by the school.
- The sky. The cloud layer (`CEmitter__DrawLayer` 0x00281D50, CEmitter__Draw in Turok 2) is a plane
  at the layer's height under the view frustum's corners (camera + 0x26C: the eye and four corners),
  transparent towards the edges, with texture coordinates from world coordinates. At x2 the plane
  grew, near the horizon the texture coordinates exceeded the N64 limit, and the clouds were distorted.
  The hook `sp_draw_sky_layer` substitutes the corners at the original distance while the layer is
  drawn and calls the function itself. Tested with a capture: the sky at Far looks as in the original.
- Checked and ruled out as causes: the 128 grid section limit (there are 1-12), cache evictions (none
  in the level), the frame's list buffer (0x9BF8 of 0x20000 bytes).

**Frame capture (F9 / F10 / Insert / Home or L + R + D-pad down).** src/game/debug_capture.cpp writes
one frame of level drawing to %LOCALAPPDATA%\SouthParkRecompiled\debug_capture.txt: the player, the
camera (eye, far clip, frustum corners and planes, view rectangle), every grid section and every
object with the results of the checks (mask, view rectangle, frustum; for level objects also the load
rectangle) and a "drawn / deferred" mark. In single player the level is drawn by the
`CScene__DrawLevelViews` branch (0x0020234C) for one camera, in split screen by `CScene__DrawLevel`.
A Windows sound plays at the moment of capture. `--capture-at <s>` does the same on a timer.

**Analysis of the captures by the school (Far).** Culling works correctly: the culled school walls
lie beyond the screen edge (46–52° from the view direction with the visible edge at about 43°; the
game's frustum is 40.8° from the axis, and for a 16:9 screen with a vertical angle of about 47° it is
38°). For the brown house the body is drawn in both frames; the textured parts (roof, windows)
disappear: textures also live in the shared cache (`CCache__Request`), and the log by the school shows
evictions during play. The cause is cache memory.

**14 MiB.** The video chip and the RSP only read the first 16 MiB. The VM window copies took a 4 MiB
reserve, while the game allocates two windows during loading for the whole game, 1.3 MiB (confirmed by
logs). The copies moved to 0x80E00000–0x80F80000 (1.5 MiB, src/game/boot.cpp; if it runs out the
runtime stops with a message), the patch binary to 0x80D81000, and the game is told it has 14 MiB: the
heap goes up to 0x80D80000, 2 MiB more than at 12 and 6 MiB more than on the console. Tested: loading,
the level, audio.

**Draw distance: fog (2026-11-17).** The captures by the school showed: the school walls are marked
"drawn" even in a frame where they aren't visible, and "model not loaded" never appeared. It wasn't
memory, it was fog. The RSP computes fog from depth after perspective, z/w = A - B/w, not from
distance. With a near plane of 16 this value is almost 1 for everything beyond a couple of hundred
units, so doubling only the far plane pushed the fog back by only about a third. The fog start in the
first level is 995 of 1000. Buildings at 2000-2500 units stood in solid sky-coloured fog and appeared
when you turned a few degrees away: the depth along the view direction is smaller then. The fog
multiplier can't be doubled: 25600 doesn't fit in 16 bits. Now `sp_draw_distance_near` (a hook in
`CCamera__Update` at 0x0025BFB0, before guPerspectiveF) multiplies the near plane too (the constant
0x800A94F8 = 16) by the same factor. z/w at depth w is then the same as in the original at w / k: the
fog looks as before, just k times farther. The game's microcode is F3DEX2.NoN, so nothing is clipped
by the near plane. Tested with captures of the first street (x1 and x2): at x2 the crossing at the end
of the street is clearly visible, the hand and nearby houses are undistorted. Not tested: by the
school, right up against the walls.

**Draw distance: the limit of 256 models a frame (2026-11-21).** New captures by the school and the
brown house: in frames where a building is missing, the level code still marks it "drawn", the models
are loaded, and there are zero cache evictions at that time (rom/school.log, the last ~80 s). The cause
is the draw queue: `func_0021013C` and `func_00210290` (queue a model instance) lock each new model of
the frame in the cache and write it to a 256-entry list (0x800D2CB0, counter 0x800D2CA8).
`func_0020FB74` at the end of the scene releases them (`func_00221B74`, `CCache__Unlock`), and
`Render_ResetBatches` zeroes the counter. When the list is full, the queue functions silently return,
and every model after the 256th in that frame is not drawn. At x2 more than 256 models get into the
frame, and what the game draws last disappeared, for example the section with the school. A small turn
changed the number of models on the right, so the building appeared and vanished. Now
`sp_frame_models_room` (hooks at 0x002101CC and 0x002103B8, before the comparison with 256) moves the
last entry into its own list when the list is full, freeing a slot. `sp_frame_models_release`
(0x0020FC00) releases its list the same way the game releases its own, and `sp_frame_models_reset`
(0x0020FB48) clears it together with the game's. Check: on the first street with `SP_MODEL_LIMIT=32`
(overflow from the 32nd model) the frame is complete, nothing missing and no crashes. Not tested at
the school itself.

## Draw distance: material and draw item limits per frame (2026-11-21)

The 256 model limit turned out not to be the only one. Flushing a batch (func_0020ED10) sorts drawing
by material: no more than 100 material nodes a frame (0x800D2028, counter 0x800CD9C4) and no more than
500 draw items (0x800CD9D8, counter 0x800CD9B4). Beyond 100 materials the game drops the rest of the
batch, beyond 500 items every following instance. At Far, right at the start of the first street,
102–106 materials are needed, so the school and houses disappeared although the level sent them to be
drawn. Only the CPU reads these nodes, so beyond the limit they are taken from memory above 16 MiB
(0x81000000: 2000 materials, 0x81010000: 20000 items). The game compares the counters for equality,
so at the limit the counter jumps one ahead. Check with lowered limits:
SP_MATERIAL_LIMIT=30 SP_ITEM_LIMIT=100, the frame is complete. With --trace-input, once a second
"materials in a frame" / "draw items in a frame" is printed if the game's limit is exceeded.

## Texture packs (2026-11-25)

`--dump-textures <folder>` sets `RT64_DUMP_TEXTURES`, which the renderer reads when it starts
(`runtime-patches/0006`) and turns on RT64's texture dump, as the inspector's "Dump textures" does:
for every texture, once, the whole TMEM (`<hash>.v<version>.tmem`), the tile parameters
(`.tile.json`) and the RDRAM it came from (`.rice.*`, for Rice hashes).

`tools/make_texture_pack.py` turns that into a pack:
- **decode**: samples the TMEM dump exactly like RT64's `sampleTMEM` (`shaders/TextureDecoder.hlsli`):
  texel address `y * line*8 + (x << shift) / 2` from `tmem*8`; on odd rows the 32-bit words of a row
  are swapped in pairs; RGBA32 takes R,G from the lower and B,A from the upper TMEM half; with a TLUT
  the index reads a 16-bit RGBA16 or IA16 entry from `0x800 + index*8` (CI4: `+ palette*128`). TMEM is
  stored in N64 byte order. Check: on 644 textures the mean change between rows is the same for
  even and odd rows (7.0 and 6.8), which a wrong row swap would break.
- **upscale**: Real-ESRGAN ncnn-vulkan, `realesrgan-x4plus-anime` x4 by default. Each texture is padded
  by 8 texels as the game samples it (`cms`/`cmt`: wrap, mirror, or clamp when clamped or unmasked),
  colour under fully transparent texels is bled in from the neighbours, and alpha goes through the
  upscaler as a separate grey image; alpha that was only 0/255 is thresholded back to 0/255.
- **pack**: `rt64.json` with `autoPath: rt64` and each texture matched by its RT64 hash (no Rice hashes
  needed), `mod.json` with `game_id: southpark`, then `texture_packer --create-low-mip-cache` and
  `--create-pack` (the packer insists on the cache and writes `<folder>.rtz` inside the folder).

Measured: one minute from boot to the first level dumps 710 textures (formats: RGBA16, CI4 with RGBA16
or IA16 palettes, CI8, I4, I8); upscaling 644 of them (smaller than 8 texels are left alone) takes
38 s; the pack is 17 MB. In game the log shows "Loading mod southpark_hd_textures" and the level draws
with the replaced textures, nothing missing or scrambled.
