// Boot sequence and virtual-memory replacement for South Park.
//
// The game runs its code TLB-mapped in useg (0x002xxxxx engine, 0x004xxxxx
// paged module) and keeps its data in KSEG0. The cartridge's boot stub and the
// engine's VM pager exist only to program the TLB, so neither is recompiled.
// This file stands in for both. See docs/LAYOUT.md.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <sys/mman.h>
#endif

#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "southpark_dev.h"
#include "southpark_game.h"

extern "C" void osInitialize_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void game_main(uint8_t* rdram, recomp_context* ctx);
extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);

namespace {
    // Cartridge layout (docs/LAYOUT.md).
    constexpr uint32_t boot_rom = 0x00001000;      // what IPL3 loads: 1 MiB to 0x80000400
    constexpr uint32_t boot_size = 0x00100000;
    constexpr uint32_t code_rom = 0x00001100;      // engine .text
    constexpr uint32_t code_size = 0x000A6DD8;
    constexpr uint32_t module_rom = 0x000C4000;    // paged module: code, then its data
    constexpr uint32_t module_text_size = 0x00014AA0;
    constexpr uint32_t module_size = 0x00016F10;

    // Address space the boot stub and VM_Initialize set up through the TLB.
    constexpr uint32_t engine_useg = 0x00200000;   // alias of physical 0..2 MiB
    constexpr uint32_t engine_useg_size = 0x00200000;
    constexpr uint32_t code_vram = 0x00200500;
    constexpr uint32_t module_useg = 0x00400000;
    constexpr uint32_t module_useg_size = 0x00020000;

    // game_entry clears more than the boot stub does (which stops at 0x800F3730).
    constexpr uint32_t bss_start = 0x800C2000;
    constexpr uint32_t bss_end = 0x8011E000;
    constexpr uint32_t initial_sp = 0x803FFFC0;
    constexpr uint32_t entry_frame = 0x18;

    // ROM windows handed out by VM_Alloc. The game needs them at the addresses
    // the original pager would pick: the first one is the audio bank file, whose
    // pointers are already relocated to 0x00480000 in the ROM. The pager's region
    // list starts with the module (page 0x400, 0x80 pages), so windows follow it.
    constexpr uint32_t vm_first_page = 0x480;
    constexpr uint32_t vm_page_limit = 0x800;      // 0x00800000: 3.5 MiB of windows
    uint32_t vm_next_page = vm_first_page;

    // The RSP reads some of this data too (ADPCM tables) and can only take
    // physical addresses below 16 MiB. Each window therefore has a second copy
    // in ordinary RAM; the two copies do not track each other: fine for data the
    // game only reads after loading. The game asks for two windows, at boot, 1.3 MiB
    // together (0x36DCC + 0x10CD48 bytes); the copies get the 1.5 MiB below the area
    // src/game/interpolation.cpp uses from 0x80F80000. Below them are the patch
    // binary (0x80D81000, patches/patches.ld) and the game's heap, which reaches
    // 0x80D80000 because the game is told it has 14 MiB (src/game/draw_distance.cpp).
    constexpr uint32_t vm_mirror_start = 0x80E00000;
    constexpr uint32_t vm_mirror_end = 0x80F80000;

    constexpr uint32_t cart_base = 0x10000000;
    constexpr uint32_t status_fr = 0x04000000;

    gpr to_gpr(uint32_t address) {
        return static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(address)));
    }

    // Host location of a guest address, the same arithmetic recomp.h's MEM_* macros use.
    // A useg address lands 2 GiB above the KSEG0 view of the same block.
    uint8_t* host_ptr(uint8_t* rdram, uint32_t address) {
        return rdram + (to_gpr(address) - 0xFFFFFFFF80000000ULL);
    }

    // The runtime reserves 4 GiB for rdram but only makes the first 512 MiB
    // accessible. Open the part of it that a useg range falls on.
    void open_useg(uint8_t* rdram, uint32_t address, uint32_t size) {
        uint8_t* start = host_ptr(rdram, address);
#ifdef _WIN32
        DWORD old_protect = 0;
        bool ok = VirtualProtect(start, size, PAGE_READWRITE, &old_protect) != 0;
#else
        bool ok = mprotect(start, size, PROT_READ | PROT_WRITE) == 0;
#endif
        if (!ok) {
            std::fprintf(stderr, "Failed to map useg range 0x%08X-0x%08X\n", address, address + size);
            std::abort();
        }
    }

    // The game expects the FPU in 64-bit register mode (Status.FR): its code uses
    // the odd-numbered registers as independent singles. IPL3 leaves FR set; the
    // recompiler's register model depends on it too.
    void enable_fr(recomp_context* ctx) {
        cop0_status_write(ctx, ctx->status_reg | status_fr);
    }
}

void southpark::game_on_init(uint8_t* rdram, recomp_context*) {
    southpark::dev::set_rdram(rdram);

    // Engine alias. Recompiled code never executes from here, but the bytes
    // are present in case something reads its own code.
    open_useg(rdram, engine_useg, engine_useg_size);
    recomp::do_rom_read(rdram, to_gpr(engine_useg + 0x400), cart_base + boot_rom, boot_size);
    load_overlays(code_rom, static_cast<int32_t>(code_vram), code_size);

    // Paged module, resident from the start. Its data tail (menu descriptors)
    // is read through useg addresses, so it has to be real memory.
    open_useg(rdram, module_useg, module_useg_size);
    recomp::do_rom_read(rdram, to_gpr(module_useg), cart_base + module_rom, module_size);
    load_overlays(module_rom, static_cast<int32_t>(module_useg), module_text_size);

    vm_next_page = vm_first_page;
}

void southpark::on_thread_create(uint8_t*, recomp_context* ctx) {
    enable_fr(ctx);
}

// Replaces the SN64 boot stub at 0x80000400 and game_entry at 0x0022BCE8.
// game_entry re-reads the first 1 MiB of the cartridge through the PI registers,
// clears BSS, then calls osInitialize and game_main.
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx) {
    enable_fr(ctx);
    // The runtime copied 1 MiB of ROM to 0x80000400 the way IPL3 does, which
    // leaves ROM contents where BSS lives.
    std::memset(host_ptr(rdram, bss_start), 0, bss_end - bss_start);
    ctx->r29 = to_gpr(initial_sp - entry_frame);
    osInitialize_recomp(rdram, ctx);
    game_main(rdram, ctx);
}

gpr get_entrypoint_address() {
    return to_gpr(0x80000400);
}

// --- VM manager -------------------------------------------------------------
// The original maps ROM windows into useg and pages them in on TLB misses.
// Here every window is copied in whole when it is requested.

// void VM_Initialize(void)
extern "C" void VM_Initialize(uint8_t*, recomp_context*) {
    // Nothing to do: the module is resident and no pager threads are needed.
}

// void* VM_Alloc(u32 size, s32 unused, u32 rom_offset)
extern "C" void VM_Alloc(uint8_t* rdram, recomp_context* ctx) {
    uint32_t size = static_cast<uint32_t>(ctx->r4);
    uint32_t rom_offset = static_cast<uint32_t>(ctx->r6);

    // Same placement as VM_AllocRegion: after the previous region, on an even page.
    uint32_t pages = (size + 0xFFF) >> 12;
    uint32_t start_page = (vm_next_page + 1) & ~1u;
    uint32_t base = start_page << 12;
    uint32_t mirror = vm_mirror_start + (base - (vm_first_page << 12));
    if (start_page + pages > vm_page_limit || mirror + (pages << 12) > vm_mirror_end) {
        std::fprintf(stderr, "VM_Alloc: out of space for 0x%X bytes from ROM 0x%X\n", size, rom_offset);
        std::abort();
    }
    vm_next_page = start_page + pages;

    open_useg(rdram, base, pages << 12);
    recomp::do_rom_read(rdram, to_gpr(base), cart_base + rom_offset, size);
    recomp::do_rom_read(rdram, to_gpr(mirror), cart_base + rom_offset, size);
    std::printf("VM_Alloc: ROM 0x%06X + 0x%X -> 0x%08X (mirror 0x%08X)\n", rom_offset, size, base, mirror);
    ctx->r2 = to_gpr(base);
}

// u32 osVirtualToPhysical(void* address)
// The runtime's version passes useg addresses through unchanged, which would
// point the RSP at unrelated game memory. Send window addresses to their mirror.
extern "C" void sp_osVirtualToPhysical(uint8_t*, recomp_context* ctx) {
    uint32_t address = static_cast<uint32_t>(ctx->r4);
    uint32_t physical;
    if (address >= 0x80000000 && address < 0xC0000000) {
        physical = address & 0x1FFFFFFF;
    }
    else if (address >= (vm_first_page << 12) && address < (vm_next_page << 12)) {
        physical = (vm_mirror_start & 0x1FFFFFFF) + (address - (vm_first_page << 12));
    }
    else {
        physical = address;
    }
    ctx->r2 = to_gpr(physical);
}

// --- Cartridge reads --------------------------------------------------------
// The engine does not use libultra's PI manager. Its loader thread
// (CLoader__Main, the only caller) starts a raw DMA and then blocks on its own
// queue, which it registered for the PI interrupt with message 0x7D1:
//
//     osPiRawStartDma(OS_READ, request->rom, request->ram, request->size);
//     osRecvMesg(&loader->pi_queue, &msg, OS_MESG_BLOCK);    // loader + 0xA48
//
// The runtime has neither raw DMA nor a PI interrupt, so the read is done on
// the spot and the "interrupt" message is posted right away.

// s32 osPiRawStartDma(s32 direction, u32 dev_address, void* ram_address, u32 size)
extern "C" void sp_osPiRawStartDma(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t rom_size = 0x01000000;
    constexpr uint32_t loader_pi_queue = 0xA48;
    constexpr uint32_t loader_pi_message = 0x7D1;

    uint32_t direction = static_cast<uint32_t>(ctx->r4);
    uint32_t dev_address = static_cast<uint32_t>(ctx->r5) & 0x0FFFFFFF;
    uint32_t ram_address = static_cast<uint32_t>(ctx->r6);
    uint32_t size = static_cast<uint32_t>(ctx->r7);
    uint32_t loader = static_cast<uint32_t>(ctx->r16);   // CLoader__Main keeps `this` in $s0

    if (direction != 0 || dev_address > rom_size || size > rom_size - dev_address) {
        std::fprintf(stderr, "osPiRawStartDma: unsupported request (direction %u, ROM 0x%X, size 0x%X)\n",
                     direction, dev_address, size);
        std::abort();
    }
    recomp::do_rom_read(rdram, to_gpr(ram_address), cart_base + dev_address, size);
    osSendMesg(rdram, static_cast<int32_t>(loader + loader_pi_queue), static_cast<OSMesg>(loader_pi_message), OS_MESG_NOBLOCK);
    ctx->r2 = 0;
}
