// Controller Pak for South Park.
//
// The game keeps its saves on a Controller Pak. The runtime has no Controller
// Pak: its osPfs* functions all answer "no pak". Here the pak is a 32 KiB file
// next to the settings, and only the lowest layer is native: reading and
// writing one 32-byte page, and the "is a pak plugged in" queries. The file
// system on top of that is libultra's own code from the ROM, recompiled like
// the rest of the game (the sp_osPfs* names in symbols/manual_names), so notes,
// checksums and repair behave exactly as on the console and the file has the
// same layout emulators use (.mpk).
//
// One pak, in controller 1. Other ports report an empty slot.

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "recomp.h"
#include "librecomp/game.hpp"

namespace {
    constexpr size_t page_size = 32;
    constexpr size_t page_count = 1024;
    constexpr size_t pak_size = page_size * page_count;
    constexpr int32_t pfs_err_nopack = 1;

    std::array<uint8_t, pak_size> pak{};
    std::filesystem::path pak_path;
    bool loaded = false;

    // A freshly formatted pak, as libultra's own formatter leaves it.
    void format(std::array<uint8_t, pak_size>& image) {
        image.fill(0);
        // Page 0: label area, then four copies of the ID block.
        for (size_t i = 0; i < page_size; i++) {
            image[i] = static_cast<uint8_t>(i);
        }
        image[0] = 0x81;
        std::array<uint8_t, page_size> id{};
        const uint8_t serial[] = { 'S', 'o', 'u', 't', 'h', 'P', 'a', 'r', 'k', 'R', 'e', 'c', 'o', 'm', 'p', 0 };
        for (size_t i = 0; i < sizeof(serial); i++) {
            id[i] = serial[i];
        }
        id[0x19] = 0x01;    // device id: bit 0 set = a 32 KiB pak
        id[0x1A] = 0x01;    // one bank
        uint16_t sum = 0;
        for (size_t i = 0; i < 0x1C; i += 2) {
            sum = static_cast<uint16_t>(sum + ((id[i] << 8) | id[i + 1]));
        }
        const uint16_t inverted = static_cast<uint16_t>(0xFFF2 - sum);
        id[0x1C] = static_cast<uint8_t>(sum >> 8);
        id[0x1D] = static_cast<uint8_t>(sum);
        id[0x1E] = static_cast<uint8_t>(inverted >> 8);
        id[0x1F] = static_cast<uint8_t>(inverted);
        for (size_t offset : { 0x20, 0x60, 0x80, 0xC0 }) {
            std::copy(id.begin(), id.end(), image.begin() + offset);
        }
        // Pages 8-15 and 16-23: the page table and its backup. Pages 5..127 free (3).
        for (size_t table : { 0x100, 0x200 }) {
            uint8_t table_sum = 0;
            for (size_t entry = 5; entry < 128; entry++) {
                image[table + entry * 2 + 1] = 0x03;
                table_sum = static_cast<uint8_t>(table_sum + 0x03);
            }
            image[table + 1] = table_sum;
        }
        // Pages 24-39: the note table, empty.
    }

    void save() {
        std::error_code error;
        std::filesystem::create_directories(pak_path.parent_path(), error);
        std::filesystem::path temp_path = pak_path;
        temp_path += ".tmp";
        {
            std::ofstream file{ temp_path, std::ios::binary | std::ios::trunc };
            file.write(reinterpret_cast<const char*>(pak.data()), pak.size());
            if (!file) {
                std::fprintf(stderr, "Controller Pak: cannot write %s\n", temp_path.string().c_str());
                return;
            }
        }
        std::filesystem::rename(temp_path, pak_path, error);
        if (error) {
            std::fprintf(stderr, "Controller Pak: cannot replace %s: %s\n", pak_path.string().c_str(), error.message().c_str());
        }
    }

    void load() {
        if (loaded) {
            return;
        }
        loaded = true;
        pak_path = recomp::get_config_path() / "saves" / "controller_pak_1.mpk";
        std::ifstream file{ pak_path, std::ios::binary };
        if (file && file.read(reinterpret_cast<char*>(pak.data()), pak.size())) {
            std::printf("Controller Pak: loaded %s\n", pak_path.string().c_str());
            return;
        }
        format(pak);
        save();
        std::printf("Controller Pak: created %s\n", pak_path.string().c_str());
    }
}

// s32 __osContRamRead(OSMesgQueue* queue, int channel, u16 page, u8* buffer)
extern "C" void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t channel = static_cast<uint32_t>(ctx->r5);
    const uint32_t page = static_cast<uint32_t>(ctx->r6) & 0xFFFF;
    const gpr buffer = ctx->r7;
    if (channel != 0) {
        ctx->r2 = pfs_err_nopack;
        return;
    }
    load();
    for (size_t i = 0; i < page_size; i++) {
        // Pages past the end are the bank-select register: reads back as zero.
        MEM_B(i, buffer) = page < page_count ? pak[page * page_size + i] : 0;
    }
    ctx->r2 = 0;
}

// s32 __osContRamWrite(OSMesgQueue* queue, int channel, u16 page, u8* buffer, int force)
extern "C" void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t channel = static_cast<uint32_t>(ctx->r5);
    const uint32_t page = static_cast<uint32_t>(ctx->r6) & 0xFFFF;
    const gpr buffer = ctx->r7;
    const int32_t force = MEM_W(0x10, ctx->r29);
    if (channel != 0) {
        ctx->r2 = pfs_err_nopack;
        return;
    }
    ctx->r2 = 0;
    // As in libultra: the ID area is only written when the caller insists.
    if (force != 1 && page < 7 && page != 0) {
        return;
    }
    if (page >= page_count) {
        return;     // bank select: there is one bank
    }
    load();
    bool changed = false;
    for (size_t i = 0; i < page_size; i++) {
        const uint8_t value = static_cast<uint8_t>(MEM_B(i, buffer));
        changed |= pak[page * page_size + i] != value;
        pak[page * page_size + i] = value;
    }
    if (changed) {
        save();
    }
}

// s32 __osPfsGetStatus(OSMesgQueue* queue, int channel)
extern "C" void __osPfsGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = static_cast<uint32_t>(ctx->r5) == 0 ? 0 : pfs_err_nopack;
}

// s32 osPfsIsPlug(OSMesgQueue* queue, u8* pattern): one bit per port with a pak.
extern "C" void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx) {
    MEM_B(0, ctx->r5) = 0x01;
    ctx->r2 = 0;
}

// The SI bus lock. Game threads never run at the same time here.
extern "C" void __osSiGetAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
}

extern "C" void __osSiRelAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
}
