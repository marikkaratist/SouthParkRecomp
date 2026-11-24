// F9 (or F10, Insert, Home, or L + R + D-pad down on the controller) in the game: write down, for one frame, how the level was drawn: where the player and each
// camera are, and for every level section and every object in them whether the game's culling
// kept it and whether its model was there to draw. For finding out why something is missing from
// the picture. The file is debug_capture.txt in the configuration folder
// (%LOCALAPPDATA%\SouthParkRecompiled), one block per press.
//
// In a single-player game the level is drawn by CScene__DrawLevelViews (0x0020234C) itself, for
// its one camera ($s3), with the tests in a row: each object is written down at the top of its
// loop with the same tests worked out here (the game's own Frustum_TestBox is called), and again
// where it is handed on to be drawn or held back.
//
// With several cameras (split screen) CScene__DrawLevel does it (0x00201E40): for each loaded section, every
// camera's view rectangle (camera + 0x338: min x, min z, max x, max z) against the section's
// rectangle; then for each static object (bounds at +0x68) and each level actor (bounds at
// +0x14C), every camera's view rectangle and view frustum (Frustum_TestBox on camera + 0x2D8).
// $s2 holds "some camera sees it" at the end of each of those loops. A static object that passes
// is drawn by func_00227738, which needs its model (CStaticObject__LoadGeometry, then the unpacked
// copy from CCache__RequestDecompression) and skips the object while either is missing.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "SDL.h"

#ifdef _WIN32
#    include <windows.h>
#endif

#include "recomp.h"
#include "librecomp/game.hpp"
#include "southpark_config.h"
#include "southpark_dev.h"

namespace {
    std::FILE* file = nullptr;
    bool requested = false;     // F9 was pressed: capture the next frame
    bool capturing = false;     // inside the captured frame
    bool key_was_down = false;
    uint32_t player_object = 0;

    // A sound, so that whoever pressed the key knows the frame was written down.
    void confirm() {
#ifdef _WIN32
        MessageBeep(MB_OK);
#endif
    }

    gpr guest(uint32_t address) {
        return static_cast<gpr>(static_cast<int32_t>(address));
    }

    float real(uint8_t* rdram, uint32_t address) {
        const uint32_t bits = static_cast<uint32_t>(MEM_W(0, guest(address)));
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // A box of six floats: min x, y, z, max x, y, z.
    void print_box(uint8_t* rdram, uint32_t box) {
        std::fprintf(file, "x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f", real(rdram, box), real(rdram, box + 12),
                     real(rdram, box + 4), real(rdram, box + 16), real(rdram, box + 8), real(rdram, box + 20));
    }

    void print_cameras(uint8_t* rdram, uint32_t first) {
        int number = 0;
        for (uint32_t camera = first; camera != 0 && number < 8; camera = static_cast<uint32_t>(MEM_W(4, guest(camera))), number++) {
            std::fprintf(file, "camera %d at %08X: eye %.0f %.0f %.0f, far %.0f, view rect x %.0f..%.0f z %.0f..%.0f\n",
                         number, camera, real(rdram, camera + 0x26C), real(rdram, camera + 0x270), real(rdram, camera + 0x274),
                         real(rdram, camera + 0x4F0), real(rdram, camera + 0x338), real(rdram, camera + 0x340),
                         real(rdram, camera + 0x33C), real(rdram, camera + 0x344));
            for (int corner = 1; corner < 5; corner++) {
                const uint32_t point = camera + 0x26C + corner * 12;
                std::fprintf(file, "  far corner %d: %.0f %.0f %.0f\n", corner, real(rdram, point), real(rdram, point + 4), real(rdram, point + 8));
            }
            for (int plane = 0; plane < 6; plane++) {
                const uint32_t p = camera + 0x2D8 + plane * 16;
                std::fprintf(file, "  plane %d: %.3f %.3f %.3f %.0f\n", plane, real(rdram, p), real(rdram, p + 4), real(rdram, p + 8), real(rdram, p + 12));
            }
        }
    }
}

// Called on every controller read with controller 1's buttons (src/game/controls.cpp).
void southpark_debug_capture_poll(uint16_t buttons) {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    // Several keys, since on laptops F9 often needs Fn; and L + R + D-pad down on the controller,
    // which works while another window (a screenshot tool) has the keyboard.
    constexpr uint16_t combo = 0x0020 | 0x0010 | 0x0400;
    const bool down = (keys != nullptr && (keys[SDL_SCANCODE_F9] || keys[SDL_SCANCODE_F10] || keys[SDL_SCANCODE_INSERT] ||
                                           keys[SDL_SCANCODE_HOME])) ||
                      (buttons & combo) == combo;
    if (down && !key_was_down) {
        requested = true;
    }
    // --capture-at <s>: the same as pressing F9 that many seconds after start.
    static const auto start = std::chrono::steady_clock::now();
    static bool timed_done = false;
    const int at = southpark::dev::capture_at_seconds();
    if (at > 0 && !timed_done && std::chrono::steady_clock::now() - start > std::chrono::seconds(at)) {
        timed_done = true;
        requested = true;
    }
    key_was_down = down;
}

// Called with the player's object every update (src/game/controls.cpp).
void southpark_debug_capture_player(uint32_t object) {
    player_object = object;
}

// CScene__DrawLevel at 0x00201F78, before the loop over sections: the list of cameras is at sp + 0x1C.
extern "C" void sp_capture_level_start(uint8_t* rdram, recomp_context* ctx) {
    if (capturing) {
        capturing = false;
        if (file != nullptr) {
            std::fprintf(file, "=== end of capture ===\n\n");
            std::fclose(file);
            file = nullptr;
        }
    }
    if (!requested) {
        return;
    }
    requested = false;
    const std::filesystem::path path = recomp::get_config_path() / "debug_capture.txt";
    file = std::fopen(path.string().c_str(), "a");
    if (file == nullptr) {
        return;
    }
    capturing = true;
    confirm();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(file, "=== capture at %lld, draw distance x%.1f ===\n", static_cast<long long>(seconds), southpark::draw_distance_scale());
    if (player_object != 0) {
        std::fprintf(file, "player %08X at %.0f %.0f %.0f\n", player_object, real(rdram, player_object + 4),
                     real(rdram, player_object + 8), real(rdram, player_object + 12));
    }
    print_cameras(rdram, static_cast<uint32_t>(MEM_W(0x1C, ctx->r29)));
    std::fprintf(file, "sections loaded: %d\n", MEM_W(0x1278, ctx->r23));
}

// CScene__DrawLevel at 0x00202010: $s5 the section's slot (handle, cell), $s2 "some camera sees it".
extern "C" void sp_capture_section(uint8_t* rdram, recomp_context* ctx) {
    if (!capturing || file == nullptr) {
        return;
    }
    const uint32_t cell = static_cast<uint32_t>(MEM_W(4, ctx->r21));
    std::fprintf(file, "section %08X: x %.0f..%.0f z %.0f..%.0f -> %s\n", cell, real(rdram, cell), real(rdram, cell + 8),
                 real(rdram, cell + 4), real(rdram, cell + 12), ctx->r18 != 0 ? "in view" : "OUT OF VIEW");
}

// CScene__DrawLevel at 0x002020F4: $s1 the static object, $s2 "some camera sees it".
extern "C" void sp_capture_static(uint8_t* rdram, recomp_context* ctx) {
    if (!capturing || file == nullptr) {
        return;
    }
    const uint32_t object = static_cast<uint32_t>(ctx->r17);
    std::fprintf(file, "  static %08X flags %04X ", object, static_cast<uint32_t>(MEM_HU(0x88, guest(object))));
    print_box(rdram, object + 0x68);
    std::fprintf(file, " -> %s\n", ctx->r18 != 0 ? "in view" : "OUT OF VIEW");
}

// CScene__DrawLevel at 0x00202228: $s1 the level actor, $s2 "some camera sees it".
extern "C" void sp_capture_actor(uint8_t* rdram, recomp_context* ctx) {
    if (!capturing || file == nullptr) {
        return;
    }
    const uint32_t actor = static_cast<uint32_t>(ctx->r17);
    std::fprintf(file, "  actor %08X flags %04X ", actor, static_cast<uint32_t>(MEM_HU(0x16C, guest(actor))));
    print_box(rdram, actor + 0x14C);
    std::fprintf(file, " -> %s\n", ctx->r18 != 0 ? "in view" : "OUT OF VIEW");
}

// func_00227738 at 0x0022777C: $s2 the static object, $fp its model (0 while not loaded).
extern "C" void sp_capture_static_model(uint8_t* rdram, recomp_context* ctx) {
    if (capturing && file != nullptr && ctx->r30 == 0) {
        std::fprintf(file, "  static %08X: NO MODEL (not loaded), not drawn\n", static_cast<uint32_t>(ctx->r18));
    }
}

// func_00227738 at 0x002277E4: $s2 the static object, $s7 its unpacked model (0 when refused).
extern "C" void sp_capture_static_unpacked(uint8_t* rdram, recomp_context* ctx) {
    if (capturing && file != nullptr && ctx->r23 == 0) {
        std::fprintf(file, "  static %08X: NO UNPACKED MODEL, not drawn\n", static_cast<uint32_t>(ctx->r18));
    }
}

// --- CScene__DrawLevelViews, one camera ---------------------------------------------------------

extern "C" void Frustum_TestBox(uint8_t* rdram, recomp_context* ctx);

namespace {
    uint32_t views_camera = 0;

    bool overlaps_view_rect(uint8_t* rdram, uint32_t camera, float min_x, float min_z, float max_x, float max_z) {
        return min_x < real(rdram, camera + 0x340) && real(rdram, camera + 0x338) < max_x &&
               min_z < real(rdram, camera + 0x344) && real(rdram, camera + 0x33C) < max_z;
    }

    bool frustum_test(uint8_t* rdram, recomp_context* ctx, uint32_t camera, uint32_t box) {
        const recomp_context saved = *ctx;
        ctx->r4 = guest(camera + 0x2D8);
        ctx->r5 = guest(box);
        Frustum_TestBox(rdram, ctx);
        const bool inside = ctx->r2 != 0;
        *ctx = saved;
        return inside;
    }

    float distance_to_eye(uint8_t* rdram, uint32_t camera, uint32_t box) {
        const float x = (real(rdram, box) + real(rdram, box + 12)) * 0.5f - real(rdram, camera + 0x26C);
        const float z = (real(rdram, box + 8) + real(rdram, box + 20)) * 0.5f - real(rdram, camera + 0x274);
        return std::sqrt(x * x + z * z);
    }

    void print_camera(uint8_t* rdram, uint32_t camera) {
        std::fprintf(file, "camera %08X: eye %.0f %.0f %.0f, far %.0f, view rect x %.0f..%.0f z %.0f..%.0f\n", camera,
                     real(rdram, camera + 0x26C), real(rdram, camera + 0x270), real(rdram, camera + 0x274), real(rdram, camera + 0x4F0),
                     real(rdram, camera + 0x338), real(rdram, camera + 0x340), real(rdram, camera + 0x33C), real(rdram, camera + 0x344));
        for (int corner = 1; corner < 5; corner++) {
            const uint32_t point = camera + 0x26C + corner * 12;
            std::fprintf(file, "  far corner %d: %.0f %.0f %.0f\n", corner, real(rdram, point), real(rdram, point + 4), real(rdram, point + 8));
        }
        for (int plane = 0; plane < 6; plane++) {
            const uint32_t q = camera + 0x2D8 + plane * 16;
            std::fprintf(file, "  plane %d: %.3f %.3f %.3f %.0f\n", plane, real(rdram, q), real(rdram, q + 4), real(rdram, q + 8), real(rdram, q + 12));
        }
    }
}

// CScene__DrawLevelViews at 0x002024F4, before the loop over sections: $s3 the camera, $s7 the world.
extern "C" void sp_capture_views_start(uint8_t* rdram, recomp_context* ctx) {
    views_camera = static_cast<uint32_t>(ctx->r19);
    if (capturing) {
        capturing = false;
        if (file != nullptr) {
            std::fprintf(file, "=== end of capture ===\n\n");
            std::fclose(file);
            file = nullptr;
        }
    }
    if (!requested) {
        return;
    }
    requested = false;
    const std::filesystem::path path = recomp::get_config_path() / "debug_capture.txt";
    file = std::fopen(path.string().c_str(), "a");
    if (file == nullptr) {
        return;
    }
    capturing = true;
    confirm();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(file, "=== capture at %lld, draw distance x%.1f, single camera ===\n", static_cast<long long>(seconds),
                 southpark::draw_distance_scale());
    if (player_object != 0) {
        std::fprintf(file, "player %08X at %.0f %.0f %.0f\n", player_object, real(rdram, player_object + 4),
                     real(rdram, player_object + 8), real(rdram, player_object + 12));
    }
    print_camera(rdram, views_camera);
    std::fprintf(file, "load box x %.0f..%.0f z %.0f..%.0f\n", real(rdram, 0x800E6EB0), real(rdram, 0x800E6EB8),
                 real(rdram, 0x800E6EB4), real(rdram, 0x800E6EBC));
    std::fprintf(file, "sections loaded: %d\n", MEM_W(0x1278, ctx->r23));
}

// 0x00202508, top of the section loop: $s4 the section's slot (handle, cell).
extern "C" void sp_capture_views_section(uint8_t* rdram, recomp_context* ctx) {
    if (!capturing || file == nullptr) {
        return;
    }
    const uint32_t cell = static_cast<uint32_t>(MEM_W(4, ctx->r20));
    const bool seen = overlaps_view_rect(rdram, views_camera, real(rdram, cell), real(rdram, cell + 4), real(rdram, cell + 8), real(rdram, cell + 12));
    std::fprintf(file, "section %08X: x %.0f..%.0f z %.0f..%.0f -> %s\n", cell, real(rdram, cell), real(rdram, cell + 8),
                 real(rdram, cell + 4), real(rdram, cell + 12), seen ? "in view rect" : "OUT OF VIEW RECT (whole section skipped)");
}

// 0x002025C4, top of the static object loop: $s0 the object.
extern "C" void sp_capture_views_static(uint8_t* rdram, recomp_context* ctx) {
    if (!capturing || file == nullptr) {
        return;
    }
    const uint32_t object = static_cast<uint32_t>(ctx->r16);
    const uint32_t box = object + 0x68;
    const bool mask = (static_cast<uint32_t>(MEM_W(0x84, guest(object))) & static_cast<uint32_t>(MEM_W(0x50, guest(views_camera)))) != 0;
    const bool rect = overlaps_view_rect(rdram, views_camera, real(rdram, box), real(rdram, box + 8), real(rdram, box + 12), real(rdram, box + 20));
    const bool frustum = frustum_test(rdram, ctx, views_camera, box);
    std::fprintf(file, "  static %08X flags %04X dist %.0f ", object, static_cast<uint32_t>(MEM_HU(0x88, guest(object))),
                 distance_to_eye(rdram, views_camera, box));
    print_box(rdram, box);
    std::fprintf(file, " mask %s, rect %s, frustum %s\n", mask ? "ok" : "NO", rect ? "ok" : "NO", frustum ? "ok" : "NO");
}

// 0x002026BC, top of the level actor loop: $s0 the actor.
extern "C" void sp_capture_views_actor(uint8_t* rdram, recomp_context* ctx) {
    if (!capturing || file == nullptr) {
        return;
    }
    const uint32_t actor = static_cast<uint32_t>(ctx->r16);
    const uint32_t box = actor + 0x14C;
    const uint32_t flags = static_cast<uint32_t>(MEM_HU(0x16C, guest(actor)));
    const bool mask = (static_cast<uint32_t>(MEM_W(0x1A8, guest(actor))) & static_cast<uint32_t>(MEM_W(0x50, guest(views_camera)))) != 0;
    const bool load_box = real(rdram, box) < real(rdram, 0x800E6EB8) && real(rdram, 0x800E6EB0) < real(rdram, box + 12) &&
                          real(rdram, box + 8) < real(rdram, 0x800E6EBC) && real(rdram, 0x800E6EB4) < real(rdram, box + 20);
    const bool rect = overlaps_view_rect(rdram, views_camera, real(rdram, box), real(rdram, box + 8), real(rdram, box + 12), real(rdram, box + 20));
    const bool frustum = frustum_test(rdram, ctx, views_camera, box);
    std::fprintf(file, "  actor %08X flags %04X dist %.0f ", actor, flags, distance_to_eye(rdram, views_camera, box));
    print_box(rdram, box);
    std::fprintf(file, " hidden %s, mask %s, load box %s, rect %s, frustum %s%s\n", (flags & 1) ? "YES" : "no", mask ? "ok" : "NO",
                 load_box ? "ok" : "NO", rect ? "ok" : "NO", frustum ? "ok" : "NO", (flags & 2) ? "" : " (not tested)");
}

// 0x00202680: the static object $s0 is drawn now. 0x0020265C: held back to be drawn last.
extern "C" void sp_capture_views_static_drawn(uint8_t* rdram, recomp_context* ctx) {
    if (capturing && file != nullptr) {
        std::fprintf(file, "  static %08X: drawn\n", static_cast<uint32_t>(ctx->r16));
    }
}

extern "C" void sp_capture_views_static_held(uint8_t* rdram, recomp_context* ctx) {
    if (capturing && file != nullptr) {
        std::fprintf(file, "  static %08X: held back to draw last\n", static_cast<uint32_t>(ctx->r16));
    }
}

// 0x002027EC: the level actor $s0 is drawn now.
extern "C" void sp_capture_views_actor_drawn(uint8_t* rdram, recomp_context* ctx) {
    if (capturing && file != nullptr) {
        std::fprintf(file, "  actor %08X: drawn\n", static_cast<uint32_t>(ctx->r16));
    }
}
