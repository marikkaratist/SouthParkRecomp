// Video mode adjustment for South Park.
//
// RT64 finds the framebuffer to show from VI_ORIGIN and assumes, as libultra's
// stock video modes have it, that the origin sits one row into the buffer. The
// game builds its own modes (Video_ResizeViMode) for the 480-wide display, and
// those start exactly at the buffer: origin offset 0. RT64 then looks one row
// above the buffer, finds nothing and shows black.
//
// The runtime keeps a pointer to the mode it was given and reads it on every
// retrace. These replacements hand it a private copy of the game's mode whose
// field origins are moved one row in, and refresh that copy whenever the game
// sets a mode or swaps buffers, so later edits to the game's own table still
// take effect.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "recomp.h"
#include "recompui/recompui.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

namespace {
    // Guest address of the private copy: the last page of the VM mirror pool
    // (src/game/boot.cpp), which the pool never reaches.
    constexpr int32_t mode_copy_address = static_cast<int32_t>(0x80FFFF00);

    int32_t game_mode_address = 0;
    bool reported = false;

    void sync_mode(uint8_t* rdram) {
        if (game_mode_address == 0) {
            return;
        }
        const OSViMode* source = TO_PTR(OSViMode, game_mode_address);
        OSViMode* copy = TO_PTR(OSViMode, mode_copy_address);
        *copy = *source;

        const uint32_t pixel_bytes = (source->comRegs.ctrl & 0x3) == 0x3 ? 4 : 2;
        const uint32_t row_bytes = source->comRegs.width * pixel_bytes;
        for (OSViFieldRegs& field : copy->fldRegs) {
            if (field.origin < row_bytes) {
                field.origin += row_bytes;
            }
        }

        if (!reported && source->fldRegs[0].origin < row_bytes) {
            reported = true;
            std::printf("VI mode at 0x%08X: width %u, origin offsets %u/%u moved one row (%u bytes) in for RT64\n",
                        static_cast<uint32_t>(game_mode_address), source->comRegs.width,
                        source->fldRegs[0].origin, source->fldRegs[1].origin, row_bytes);
        }
    }
}

// void osViSetMode(OSViMode* mode)
extern "C" void sp_osViSetMode(uint8_t* rdram, recomp_context* ctx) {
    game_mode_address = static_cast<int32_t>(ctx->r4);
    sync_mode(rdram);
    osViSetMode(rdram, mode_copy_address);
}

// void osViSwapBuffer(void* framebuffer)
extern "C" void sp_osViSwapBuffer(uint8_t* rdram, recomp_context* ctx) {
    sync_mode(rdram);
    osViSwapBuffer(rdram, static_cast<int32_t>(ctx->r4));
}

// Hook in CCamera__Update, just before the corners of the view frustum are moved into world
// space. The game works out what is in view from this frustum (Frustum_FromCorners,
// BoundingRect_FromPoints) and does not draw the rest. The frustum is built for the 4:3 picture;
// in a wider window RT64 shows more to the sides, where objects would pop out of existence as
// soon as they left the 4:3 part. The five points are on the stack at 0x3F8: the eye, then four
// far corners (x, y, z); this widens the corners to the window's shape, with a little to spare.
extern "C" void sp_widen_view_frustum(uint8_t* rdram, recomp_context* ctx) {
    int width = 0, height = 0;
    recompui::get_window_size(width, height);
    if (width <= 0 || height <= 0) {
        return;
    }
    const float ratio = (static_cast<float>(width) / static_cast<float>(height)) / (4.0f / 3.0f) * 1.05f;
    if (ratio <= 1.0f) {
        return;
    }
    const gpr sp = ctx->r29;
    for (int32_t offset : { 0x404, 0x410, 0x41C, 0x428 }) {
        float x;
        uint32_t bits = static_cast<uint32_t>(MEM_W(offset, sp));
        std::memcpy(&x, &bits, sizeof(x));
        x *= ratio;
        std::memcpy(&bits, &x, sizeof(bits));
        MEM_W(offset, sp) = bits;
    }
}

// Hook at the start of View_DrawBorder(View* view): returns nonzero when the
// border should not be drawn. A view's rectangle is four floats: width 0x2A8,
// height 0x2AC, left 0x2B0, top 0x2B4. Split-screen views are at most half the
// screen in one direction, so anything at the corner and at least 320x240 is
// the single full-screen view.
extern "C" int sp_skip_view_border(uint8_t* rdram, recomp_context* ctx) {
    const int32_t view = static_cast<int32_t>(ctx->r4);
    const auto read_float = [&](int32_t offset) {
        return *TO_PTR(float, view + offset);
    };
    return read_float(0x2B0) <= 0.0f && read_float(0x2B4) <= 0.0f &&
           read_float(0x2A8) >= 320.0f && read_float(0x2AC) >= 240.0f;
}
