// Draw distance: how far the world is drawn before the fog closes in.
//
// A camera's far clip is the float at camera + 0x4F0: the far plane of its projection and of the
// view frustum the game culls against, and the end of the fog, which the game sets with
// gSPFogPosition(fog start, 1000), the fog start (camera + 0x506) being in thousandths of the far
// clip. That fog is taken from the depth after perspective, so it moves out with the far clip
// only when the near plane is scaled too (sp_draw_distance_near below).
//
// The game sets the far clip from the region the camera is in, blending between regions from
// the previous value it keeps (+0x4F4), and in cinematics each frame. So the game is given its
// own value back at the start of each camera update, and the scaled one is put in once the game
// has set this frame's (0x0025BEE8, where every path through that part meets), to be used by the
// rest of the update and by the drawing.
//
// The far clip also sizes the square around each camera (camera + 0x348..0x354: min x, min z,
// max x, max z, the eye plus and minus far clip / cos(half the field of view)) inside which the
// level's grid sections are loaded from the cartridge (CScene__LoadVisibleGridSections, as in
// Turok 2). The game takes at most 128 of them, row by row across the grid; with a bigger square
// the limit fell on sections next to the player as often as on far ones, which then vanished.
// With the draw distance raised the sections are taken nearest first, so what does not fit is
// what is farthest away.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "recomp.h"
#include "southpark_config.h"
#include "southpark_dev.h"

extern "C" {
    void CISet__GetBlock(uint8_t* rdram, recomp_context* ctx);
    void CISet__GetROMAddressAndSize(uint8_t* rdram, recomp_context* ctx);
    void CCache__Request(uint8_t* rdram, recomp_context* ctx);
    void CCache__Unlock(uint8_t* rdram, recomp_context* ctx);
}

namespace {
    constexpr int32_t camera_far_clip = 0x4F0;

    struct ScaledFarClip {
        uint32_t camera = 0;
        uint32_t written = 0;       // the scaled value as stored
        uint32_t original = 0;      // the game's value
    };
    ScaledFarClip scaled_far_clips[4];

    ScaledFarClip& scaled_far_clip_of(uint32_t camera) {
        for (ScaledFarClip& entry : scaled_far_clips) {
            if (entry.camera == camera) {
                return entry;
            }
        }
        for (ScaledFarClip& entry : scaled_far_clips) {
            if (entry.camera == 0) {
                entry.camera = camera;
                return entry;
            }
        }
        scaled_far_clips[0] = ScaledFarClip{ camera };
        return scaled_far_clips[0];
    }

    gpr guest(uint32_t address) {
        return static_cast<gpr>(static_cast<int32_t>(address));
    }

    uint32_t word(uint8_t* rdram, uint32_t address) {
        return static_cast<uint32_t>(MEM_W(0, guest(address)));
    }

    float real(uint8_t* rdram, uint32_t address) {
        const uint32_t bits = word(rdram, address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // Grid sections.
    constexpr uint32_t active_cameras = 0x800CBF68;     // the camera pool's active list: head, next at +4
    constexpr uint32_t visible_box = 0x800E6EB0;        // min x, min z, max x, max z
    constexpr uint32_t decompress_grid_section = 0x00413A84;
    constexpr uint32_t grid_section_name = 0x800A7530;  // "grid section"
    constexpr int32_t section_limit = 128;
    constexpr uint32_t world_level_info = 0x24, world_rom_index = 0x54, world_collision = 0x58;
    constexpr uint32_t world_section_count = 0x1278, world_sections = 0x127C;
    constexpr uint32_t cell_size = 0x14;                // min x, min z, max x, max z, empty

    struct Candidate {
        int32_t index;
        uint32_t cell;
        float distance;
    };
}

// Hook in the boot-time heap set-up (func_0021DB18) right after its osGetMemSize call. The game
// hands everything from the end of its image (0x8011E000) to the top of RDRAM, less 512 KiB, to
// its heap, and the resource cache that holds the level's models and textures lives in that heap.
// A longer draw distance brings far more objects and textures into view than an 8 MiB console's
// cache can hold: models and textures were thrown out and loaded again, and while they were gone
// buildings went missing or lost their roofs. The game is told it has 14 MiB, so the heap ends at
// 0x80D80000 instead of 0x80780000. Everything the RSP and RT64 read must stay below 16 MiB; the
// rest of it is taken by the 512 KiB the game leaves at the top (the patch binary,
// patches/patches.ld), the copies of the VM windows (0x80E00000, src/game/boot.cpp) and the area
// src/game/interpolation.cpp uses (0x80F80000). The game only ever compares the size with 4 MiB
// (Expansion Pak or not), so nothing else changes.
extern "C" void sp_report_memory_size(uint8_t* rdram, recomp_context* ctx) {
    const int forced = southpark::dev::forced_memory_mib();     // --memory 8 for comparison
    ctx->r2 = (forced == 8 ? 8 : 14) * 0x100000;
}

// Hook at the start of CCache__FreeOldestMemEntry: the cache throws out its oldest entry to make
// room. Many a second means the cache is too small for what is in view. Counted for --trace-input.
extern "C" void sp_count_actor_draw(uint8_t* rdram, recomp_context* ctx) {
    static uint32_t evictions = 0;
    static auto last = std::chrono::steady_clock::now();
    if (!southpark::dev::trace_input()) {
        return;
    }
    evictions++;
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(1)) {
        std::printf("cache evictions in the last second: %u%c", evictions, 10);
        evictions = 0;
        last = now;
    }
}

// --- Deferred objects -------------------------------------------------------
// While drawing the level (func_00201E40, func_0020234C, func_00203890) the game holds back objects
// flagged as needing to be drawn after the rest, see-through ones such as trees and windows, in a
// list of 32 in the world (+0xFB4 count, +0xFB8 pointers), and draws them afterwards, farthest
// first (func_00200A34). When the list is full it drops the object: nothing draws it that frame.
// With the draw distance raised far more trees are in view, the list filled up and buildings went
// missing depending on where the camera pointed. An object that does not fit is drawn at once
// instead, with the call the game uses for objects that are not held back. It is then not in the
// far-to-near order, which matters only where see-through parts overlap.
namespace {
    void call_game(uint8_t* rdram, recomp_context* ctx, void (*function)(uint8_t*, recomp_context*), uint32_t a0, uint32_t a1) {
        if (southpark::dev::trace_input()) {
            static uint32_t overflows = 0;
            static auto last = std::chrono::steady_clock::now();
            overflows++;
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::seconds(1)) {
                std::printf("deferred objects over the limit in the last second: %u%c", overflows, 10);
                overflows = 0;
                last = now;
            }
        }
        const recomp_context saved = *ctx;
        ctx->r4 = guest(a0);
        ctx->r5 = guest(a1);
        function(rdram, ctx);
        *ctx = saved;
    }
}

extern "C" {
    void func_00227738(uint8_t* rdram, recomp_context* ctx);       // a static object
    void CLevelActor__Draw(uint8_t* rdram, recomp_context* ctx);
    void func_00218ED8(uint8_t* rdram, recomp_context* ctx);       // a dynamic object
}

constexpr uint32_t deferred_limit = 32;

// func_00201E40 at 0x00202118: $v1 the count, $s1 the static object, the camera at sp + 0x1C.
extern "C" void sp_deferred_full_1(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<uint32_t>(ctx->r3) >= deferred_limit) {
        call_game(rdram, ctx, func_00227738, static_cast<uint32_t>(ctx->r17), static_cast<uint32_t>(MEM_W(0x1C, ctx->r29)));
    }
}

// func_00201E40 at 0x00202244: $s1 the level actor.
extern "C" void sp_deferred_full_2(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<uint32_t>(ctx->r3) >= deferred_limit) {
        call_game(rdram, ctx, CLevelActor__Draw, static_cast<uint32_t>(ctx->r17), 0);
    }
}

// func_0020234C at 0x00202664: $s0 the static object, $s3 the camera.
extern "C" void sp_deferred_full_3(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<uint32_t>(ctx->r3) >= deferred_limit) {
        call_game(rdram, ctx, func_00227738, static_cast<uint32_t>(ctx->r16), static_cast<uint32_t>(ctx->r19));
    }
}

// func_0020234C at 0x002027D0: $s0 the level actor.
extern "C" void sp_deferred_full_4(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<uint32_t>(ctx->r3) >= deferred_limit) {
        call_game(rdram, ctx, CLevelActor__Draw, static_cast<uint32_t>(ctx->r16), 0);
    }
}

// func_00203890 at 0x00203964: $a0 the dynamic object, $s5 the camera.
extern "C" void sp_deferred_full_5(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<uint32_t>(ctx->r3) >= deferred_limit) {
        call_game(rdram, ctx, func_00218ED8, static_cast<uint32_t>(ctx->r4), static_cast<uint32_t>(ctx->r21));
    }
}

// --- Models in a frame ------------------------------------------------------
// Every model the frame draws is locked in the resource cache so that it stays where the display
// list points until the frame has been drawn: func_0021013C and func_00210290 (queue an instance
// of a model) lock each new model and note it in a list of 256 (0x800D2CB0, count at 0x800D2CA8);
// func_0020FB74 (end of the scene) calls func_00221B74 and CCache__Unlock for each, and
// Render_ResetBatches empties it. When the list is full the queueing functions return without
// drawing anything: every model after the 256th was dropped for the frame. With the draw distance
// raised more than 256 come into view, and what the game draws last (whole sections such as the
// school, or a house) vanished, depending on how many other things a few degrees of turning
// brought into view. Measured from the captures: the dropped buildings were marked drawn by the
// level code in both frames of each pair. Here the list never fills: when it holds 256, the last
// entry moves to a list of our own and the slot is used again; ours is released after the game's.
extern "C" void func_00221B74(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t frame_models = 0x800D2CB0;
    constexpr uint32_t frame_model_count = 0x800D2CA8;
    constexpr uint32_t frame_model_limit = 0x100;
    std::vector<uint32_t> extra_frame_models;

    void call_with(uint8_t* rdram, recomp_context* ctx, void (*function)(uint8_t*, recomp_context*), uint32_t a0, uint32_t a1) {
        const recomp_context saved = *ctx;
        ctx->r4 = guest(a0);
        ctx->r5 = guest(a1);
        function(rdram, ctx);
        *ctx = saved;
    }
}

// Hooks in func_0021013C at 0x002101CC and func_00210290 at 0x002103B8, right before the count is
// compared with 256.
extern "C" void sp_frame_models_room(uint8_t* rdram, recomp_context* ctx) {
    // SP_MODEL_LIMIT=<n> (n < 256) moves entries out earlier, to exercise this path where few models are in view.
    static const uint32_t limit = [] {
        const char* value = std::getenv("SP_MODEL_LIMIT");
        const int n = value != nullptr ? std::atoi(value) : 0;
        return n > 0 && n < static_cast<int>(frame_model_limit) ? static_cast<uint32_t>(n) : frame_model_limit;
    }();
    const uint32_t count = word(rdram, frame_model_count);
    if (count < limit) {
        return;
    }
    const uint32_t last = frame_models + (count - 1) * 4;
    extra_frame_models.push_back(word(rdram, last));
    MEM_W(0, guest(frame_model_count)) = static_cast<int32_t>(count - 1);
    if (southpark::dev::trace_input()) {
        static auto last_report = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report > std::chrono::seconds(1)) {
            std::printf("models in a frame over the game's 256: %zu%c", extra_frame_models.size(), 10);
            last_report = now;
        }
    }
}

// Hook in func_0020FB74 at 0x0020FC00, after the game has released its list.
extern "C" void sp_frame_models_release(uint8_t* rdram, recomp_context* ctx) {
    for (uint32_t model : extra_frame_models) {
        call_with(rdram, ctx, func_00221B74, 0, model);
        call_with(rdram, ctx, CCache__Unlock, 0, model);
    }
    extra_frame_models.clear();
}

// --- Draw items and materials in a frame ---------------------------------------
// Flushing a batch (func_0020ED10) sorts what it draws by material: a tree of material nodes, at
// most 100 in a frame (32 bytes each at 0x800D2028, count at 0x800CD9C4), each with a list of draw
// items, at most 500 in a frame (36 bytes each at 0x800CD9D8, count at 0x800CD9B4: one per
// instance and part of a model). Past 100 materials the game drops the rest of the batch, past
// 500 items every further instance, so with the draw distance raised whatever came last in the
// frame was not drawn even though the level code had sent it: the school or a house, coming and
// going as a few degrees of turning brought other things into view. Only the CPU reads these
// nodes and items (the display list is built from what they point at), so the ones past the
// game's limit are taken from memory above 16 MiB. The game compares the counts for equality
// with its limits, so when a count reaches its limit it is moved one past it (that index is never
// used), and set back to the limit once the extra room is used up too, so the game stops there.
namespace {
    struct FrameArray {
        uint32_t count_address;     // the game's count
        uint32_t limit;             // the game's limit, compared for equality
        uint32_t entry_size;
        uint32_t extra_start;       // our room for the entries past the limit
        uint32_t extra_capacity;
        const char* name;
        uint32_t from;              // first index moved to our room (the limit, or lower to test)
        bool full;
        uint32_t peak;

        // Our room's slot for game index `index`, or -1 when it stays in the game's array.
        int64_t extra_slot(uint32_t index) const {
            if (index > limit) {
                return static_cast<int64_t>(index - limit - 1) + (limit - from);
            }
            if (index >= from && index < limit) {
                return index - from;
            }
            return -1;
        }

        // Before the count is compared with the limit.
        void before_compare(uint8_t* rdram) {
            uint32_t count = word(rdram, count_address);
            peak = std::max(peak, count > limit ? count - 1 : count);
            if (count == limit && !full) {
                count = limit + 1;
            }
            const int64_t slot = extra_slot(count);
            if (slot >= static_cast<int64_t>(extra_capacity)) {
                count = limit;
                full = true;
            }
            MEM_W(0, guest(count_address)) = static_cast<int32_t>(count);
        }

        // The address the game computed for entry `index`, moved to our room when it is past the limit.
        uint32_t address_for(uint32_t index, uint32_t address) const {
            const int64_t slot = extra_slot(index);
            return slot < 0 ? address : extra_start + static_cast<uint32_t>(slot) * entry_size;
        }

        void reset() {
            if (southpark::dev::trace_input() && peak > limit) {
                static auto last_report = std::chrono::steady_clock::now();
                const auto now = std::chrono::steady_clock::now();
                if (now - last_report > std::chrono::seconds(1)) {
                    std::printf("%s in a frame: %u (the game's limit %u)%s%c", name, peak, limit, full ? ", our room full too" : "", 10);
                    last_report = now;
                }
            }
            full = false;
            peak = 0;
        }
    };

    uint32_t test_limit(const char* variable, uint32_t limit) {
        const char* value = std::getenv(variable);
        const int n = value != nullptr ? std::atoi(value) : 0;
        return n > 0 && n < static_cast<int>(limit) ? static_cast<uint32_t>(n) : limit;
    }

    // Above 16 MiB, where nothing else is (src/game/boot.cpp: the runtime opens 512 MiB).
    FrameArray frame_materials{0x800CD9C4, 100, 32, 0x81000000, 2000, "materials", test_limit("SP_MATERIAL_LIMIT", 100), false, 0};
    FrameArray frame_items{0x800CD9B4, 500, 36, 0x81010000, 20000, "draw items", test_limit("SP_ITEM_LIMIT", 500), false, 0};
}

// func_0020ED10 at 0x0020EEE0, before the material count is loaded and compared with 100.
extern "C" void sp_frame_materials_room(uint8_t* rdram, recomp_context*) {
    frame_materials.before_compare(rdram);
}

// func_0020ED10 at 0x0020EF04: $a0 the material's index, $s0 its address.
extern "C" void sp_frame_material_address(uint8_t*, recomp_context* ctx) {
    ctx->r16 = guest(frame_materials.address_for(static_cast<uint32_t>(ctx->r4), static_cast<uint32_t>(ctx->r16)));
}

// func_0020ED10 at 0x0020F080, before the item count is loaded and compared with 500.
extern "C" void sp_frame_items_room(uint8_t* rdram, recomp_context*) {
    frame_items.before_compare(rdram);
}

// func_0020ED10 at 0x0020F0A0: $v1 the item's index, $a0 its address.
extern "C" void sp_frame_item_address(uint8_t*, recomp_context* ctx) {
    ctx->r4 = guest(frame_items.address_for(static_cast<uint32_t>(ctx->r3), static_cast<uint32_t>(ctx->r4)));
}

// Hook in Render_ResetBatches at 0x0020FB48, where the game empties its lists (the models without
// releasing them) and zeroes the material and item counts.
extern "C" void sp_frame_models_reset(uint8_t*, recomp_context*) {
    extra_frame_models.clear();
    frame_materials.reset();
    frame_items.reset();
}

// --- Sky --------------------------------------------------------------------
// A cloud layer (func_00281D50, Turok 2's CEmitter__Draw, (emitter, camera)) is a plane at the
// layer's height spread under the camera's view frustum: its corners are the frustum's far corners
// (camera + 0x26C: the eye, then four corners, 12 bytes each), it fades out towards them, and its
// texture coordinates come from world positions. With the far clip scaled the plane grew with it,
// and towards the horizon its texture coordinates ran past what the N64 can hold, which bent the
// clouds the wrong way. The layer is drawn with the corners the original far clip gives, so it
// looks as in the original: it is see-through at its edges anyway.
extern "C" void CEmitter__DrawLayer(uint8_t* rdram, recomp_context* ctx);

extern "C" int sp_draw_sky_layer(uint8_t* rdram, recomp_context* ctx) {
    static bool inside = false;
    const float scale = southpark::draw_distance_scale();
    if (inside || scale <= 1.0f) {
        return 0;
    }
    const uint32_t camera = static_cast<uint32_t>(ctx->r5);
    constexpr uint32_t corners = 0x26C;
    uint32_t saved[15];
    for (uint32_t i = 0; i < 15; i++) {
        saved[i] = word(rdram, camera + corners + i * 4);
    }
    auto as_float = [](uint32_t bits) {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    for (uint32_t corner = 1; corner < 5; corner++) {
        for (uint32_t axis = 0; axis < 3; axis++) {
            const float eye = as_float(saved[axis]);
            const float point = as_float(saved[corner * 3 + axis]);
            const float moved = eye + (point - eye) / scale;
            uint32_t bits;
            std::memcpy(&bits, &moved, sizeof(bits));
            MEM_W(0, guest(camera + corners + (corner * 3 + axis) * 4)) = static_cast<int32_t>(bits);
        }
    }
    inside = true;
    CEmitter__DrawLayer(rdram, ctx);    // the game's own function, this hook passing it through
    inside = false;
    for (uint32_t i = 0; i < 15; i++) {
        MEM_W(0, guest(camera + corners + i * 4)) = static_cast<int32_t>(saved[i]);
    }
    return 1;
}

// Hook at the start of CCamera__Update(camera).
extern "C" void sp_draw_distance_restore(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t camera = static_cast<uint32_t>(ctx->r4);
    if (southpark::dev::trace_input()) {
        static uint32_t calls = 0;
        if (calls++ % 60 == 0) {
            std::printf("boxes: far %.0f, eye %.0f %.0f; visible x %.0f..%.0f z %.0f..%.0f; view x %.0f..%.0f z %.0f..%.0f; "
                        "load x %.0f..%.0f z %.0f..%.0f\n",
                        real(rdram, camera + 0x4F0), real(rdram, camera + 0x26C), real(rdram, camera + 0x274),
                        real(rdram, visible_box + 0), real(rdram, visible_box + 8), real(rdram, visible_box + 4), real(rdram, visible_box + 0xC),
                        real(rdram, camera + 0x338), real(rdram, camera + 0x340), real(rdram, camera + 0x33C), real(rdram, camera + 0x344),
                        real(rdram, camera + 0x348), real(rdram, camera + 0x350), real(rdram, camera + 0x34C), real(rdram, camera + 0x354));
        }
    }
    ScaledFarClip& entry = scaled_far_clip_of(camera);
    const gpr address = guest(camera + camera_far_clip);
    if (entry.written != 0 && static_cast<uint32_t>(MEM_W(0, address)) == entry.written) {
        MEM_W(0, address) = static_cast<int32_t>(entry.original);
    }
    entry.written = 0;
}

// Hook in CCamera__Update at 0x0025BFB0, the guPerspectiveF call for the camera ($s3), its near
// plane at sp + 0x10 and far plane (the far clip) at sp + 0x14. The game sets the fog with
// gSPFogPosition(fog start, 1000), start 995 on the first level, and the RSP takes fog from the
// depth after perspective, z/w = A - B/w with A = (f+n)/(f-n), B = 2fn/(f-n). With the near plane
// at 16 that depth is close to 1 for nearly everything, so doubling only the far clip moved the
// fog out by about a third: what the larger frustum let through stood in thick fog, the colour of
// the sky. Buildings went missing when looked at straight on and came back a few degrees aside,
// where their depth along the view is smaller. Doubling the fog multiplier instead does not fit
// the RSP's 16 bits (25600 at start 995). Scaling the near plane with the far one makes z/w at
// depth w what the original gives at w / scale, so the fog keeps its look and moves out with the
// draw distance. The game's microcode is F3DEX2.NoN: nothing is clipped at the near plane, so
// nearby walls stay. Only the order of things within 32 units of the eye could suffer.
extern "C" void sp_draw_distance_near(uint8_t* rdram, recomp_context* ctx) {
    if (southpark::draw_distance_scale() <= 1.0f) {
        return;
    }
    const uint32_t camera = static_cast<uint32_t>(ctx->r19);
    for (const ScaledFarClip& entry : scaled_far_clips) {
        if (entry.camera != camera || entry.written == 0 || word(rdram, camera + camera_far_clip) != entry.written) {
            continue;
        }
        float original, scaled;
        std::memcpy(&original, &entry.original, sizeof(original));
        std::memcpy(&scaled, &entry.written, sizeof(scaled));
        const uint32_t near_address = static_cast<uint32_t>(ctx->r29) + 0x10;
        float near_plane = real(rdram, near_address) * (scaled / original);
        uint32_t bits;
        std::memcpy(&bits, &near_plane, sizeof(bits));
        MEM_W(0, guest(near_address)) = static_cast<int32_t>(bits);
        return;
    }
}

// Hook in CCamera__Update at 0x0025BEE8; $s3 is the camera.
extern "C" void sp_draw_distance_apply(uint8_t* rdram, recomp_context* ctx) {
    const float scale = southpark::draw_distance_scale();
    if (scale <= 1.0f) {
        return;
    }
    const uint32_t camera = static_cast<uint32_t>(ctx->r19);
    ScaledFarClip& entry = scaled_far_clip_of(camera);
    const gpr address = guest(camera + camera_far_clip);
    entry.original = static_cast<uint32_t>(MEM_W(0, address));
    float far_clip;
    std::memcpy(&far_clip, &entry.original, sizeof(far_clip));
    if (!(far_clip > 0.0f)) {
        return;
    }
    far_clip *= scale;
    std::memcpy(&entry.written, &far_clip, sizeof(far_clip));
    MEM_W(0, address) = static_cast<int32_t>(entry.written);
}

// Hook at the start of CScene__LoadVisibleGridSections(world): with the draw distance raised,
// does its work nearest first and returns nonzero, so the game's own loop is skipped.
extern "C" int sp_load_grid_sections(uint8_t* rdram, recomp_context* ctx) {
    if (southpark::draw_distance_scale() <= 1.0f) {
        return 0;
    }
    const uint32_t world = static_cast<uint32_t>(ctx->r4);
    const uint32_t first_camera = word(rdram, active_cameras);
    if (first_camera == 0) {
        return 1;       // as the game: nothing to load for, the count is left as it was
    }

    // Calls into the game. Arguments past the fourth go on the stack, from sp + 0x10.
    const gpr saved_sp = ctx->r29;
    ctx->r29 = saved_sp - 0x40;
    auto call = [&](void (*function)(uint8_t*, recomp_context*), uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
        ctx->r4 = guest(a0);
        ctx->r5 = guest(a1);
        ctx->r6 = guest(a2);
        ctx->r7 = guest(a3);
        function(rdram, ctx);
        return static_cast<uint32_t>(ctx->r2);
    };
    const uint32_t collision = word(rdram, world + world_collision);
    auto block = [&](uint32_t number) {
        return call(CISet__GetBlock, collision, number, 0, 0);
    };
    const uint32_t x_max_block = block(0), x_min_block = block(1), z_max_block = block(2), z_min_block = block(3);
    const uint32_t cells = block(4) + 8;
    const int32_t nx = static_cast<int32_t>(word(rdram, x_max_block + 4));
    const int32_t nz = static_cast<int32_t>(word(rdram, z_max_block + 4));
    const uint32_t x_max = x_max_block + 8, x_min = x_min_block + 8, z_max = z_max_block + 8, z_min = z_min_block + 8;

    // The columns and rows inside the visible box, as the game finds them.
    int32_t x0 = 0, x1 = nx - 1, z0 = 0, z1 = nz - 1;
    while (x0 < nx && real(rdram, x_min + x0 * 4) < real(rdram, visible_box + 0x0)) {
        x0++;
    }
    while (x1 >= 0 && real(rdram, visible_box + 0x8) < real(rdram, x_max + x1 * 4)) {
        x1--;
    }
    while (z0 < nz && real(rdram, z_min + z0 * 4) < real(rdram, visible_box + 0x4)) {
        z0++;
    }
    while (z1 >= 0 && real(rdram, visible_box + 0xC) < real(rdram, z_max + z1 * 4)) {
        z1--;
    }

    // Every section a camera's square overlaps, with its distance from the nearest camera.
    static std::vector<Candidate> candidates;
    candidates.clear();
    for (int32_t z = z0; z <= z1; z++) {
        for (int32_t x = x0; x <= x1; x++) {
            const int32_t index = x + z * nx;
            const uint32_t cell = cells + static_cast<uint32_t>(index) * cell_size;
            if (word(rdram, cell + 0x10) != 0) {
                continue;
            }
            const float min_x = real(rdram, cell + 0x0), min_z = real(rdram, cell + 0x4);
            const float max_x = real(rdram, cell + 0x8), max_z = real(rdram, cell + 0xC);
            bool found = false;
            float distance = 1e30f;
            for (uint32_t camera = first_camera; camera != 0; camera = word(rdram, camera + 4)) {
                const float c_min_x = real(rdram, camera + 0x348), c_min_z = real(rdram, camera + 0x34C);
                const float c_max_x = real(rdram, camera + 0x350), c_max_z = real(rdram, camera + 0x354);
                if (min_x < c_max_x && c_min_x < max_x && min_z < c_max_z && c_min_z < max_z) {
                    found = true;
                    // From the middle of the square (the eye) to the nearest point of the section.
                    const float eye_x = (c_min_x + c_max_x) * 0.5f, eye_z = (c_min_z + c_max_z) * 0.5f;
                    const float dx = std::max({ min_x - eye_x, 0.0f, eye_x - max_x });
                    const float dz = std::max({ min_z - eye_z, 0.0f, eye_z - max_z });
                    distance = std::min(distance, dx * dx + dz * dz);
                }
            }
            if (found) {
                candidates.push_back({ index, cell, distance });
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });

    // Request them in that order, as the game does each one.
    int32_t count = 0;
    uint32_t slot = world + world_sections;
    const gpr sp = ctx->r29;
    for (const Candidate& candidate : candidates) {
        const uint32_t size_address = static_cast<uint32_t>(sp) + 0x20;
        const uint32_t rom = call(CISet__GetROMAddressAndSize, word(rdram, world + world_rom_index),
                                  word(rdram, world + world_level_info), static_cast<uint32_t>(candidate.index), size_address);
        const uint32_t size = word(rdram, size_address);
        MEM_W(0x10, sp) = static_cast<int32_t>(world);
        MEM_W(0x14, sp) = static_cast<int32_t>(decompress_grid_section);
        MEM_W(0x18, sp) = static_cast<int32_t>(grid_section_name);
        MEM_W(0x1C, sp) = 0;
        const uint32_t handle = call(CCache__Request, 0, rom, size, 4);
        if (handle == 0) {
            continue;
        }
        if (word(rdram, word(rdram, handle)) != 0) {
            MEM_W(0, guest(slot)) = static_cast<int32_t>(handle);
            MEM_W(4, guest(slot)) = static_cast<int32_t>(candidate.cell);
            slot += 8;
            if (++count == section_limit) {
                break;
            }
        }
        else {
            call(CCache__Unlock, 0, handle, 0, 0);
        }
    }
    MEM_W(0, guest(world + world_section_count)) = count;
    ctx->r29 = saved_sp;
    if (southpark::dev::trace_input()) {
        static uint32_t calls = 0;
        if (calls++ % 60 == 0) {
            std::printf("grid sections: %zu in reach, %d loaded\n", candidates.size(), count);
        }
    }
    return 1;
}
