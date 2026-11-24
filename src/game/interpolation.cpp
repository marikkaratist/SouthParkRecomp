// Frame interpolation hints for RT64.
//
// At framerates above the game's own, RT64 draws extra frames between two game
// frames. For that it has to know which object in the new frame is which object
// in the old one. Left to guess, it pairs them up by drawing order and by how
// similar they look, and guesses wrong whenever the order changes or the camera
// cuts: body parts swap places, scenery slides across the screen for a frame.
//
// The engine makes the right answer easy to state:
//  - the camera is one combined view-projection matrix, loaded with
//    G_MTX(PROJECTION | LOAD) at the top of the frame;
//  - scenery and other fixed objects load a world matrix that lives at an
//    address of its own for as long as the object exists: the address is the
//    object's identity;
//  - animated models keep their matrices in per-frame scratch memory that
//    swaps between two buffers, reached through segment 1. Those addresses
//    mean nothing from one frame to the next. The G_MTX commands, however, sit
//    in the model's own display list, which the game keeps, so the location of
//    the command names the model and the part. Several instances of one model
//    then share an identity, and RT64 pairs them up by where they are.
// Before a display list goes to RT64, each G_MTX command in it is replaced by a
// call to a three-command list kept in spare memory: RT64's "matrix group"
// command carrying the identity, the original G_MTX, and a return. On a frame
// where the camera jumps, all identities change, so nothing is carried across
// the cut.
//
// The game keeps some display lists from frame to frame, so the replacement
// has to stay valid: every distinct G_MTX command gets one slot for good, and a
// call that is already in place just has its slot refreshed. A slot must never
// be shared by commands with different identities: the first version keyed
// slots by the command words alone, all models' "part 1" commands landed in one
// slot, and RT64 paired unrelated parts, which showed as the picture jumping
// backwards between game frames.

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "southpark_game.h"
#include "southpark_dev.h"
#include "southpark_config.h"

namespace {
    // Spare guest memory: the last 512 KiB below 16 MiB, after the copies of the
    // VM windows (src/game/boot.cpp, 0x80E00000-0x80F80000). RT64 takes display
    // list addresses up to 16 MiB.
    constexpr uint32_t region_start = 0x80F80000;
    constexpr uint32_t region_end = 0x80FE0000;
    // Above the slots: room for lists that only live for one frame (the HUD's). Ends where
    // video.cpp keeps its VI mode copy.
    constexpr uint32_t frame_scratch_start = 0x80FE0000;
    constexpr uint32_t frame_scratch_end = 0x80FFFF00;
    constexpr uint32_t header_size = 0x20;
    // The game's pointer to the next free display list command.
    constexpr uint32_t display_list_head = 0x800CD9B8;
    constexpr uint32_t slot_size = 0x20;
    constexpr uint32_t slots_start = region_start + header_size;
    constexpr uint32_t slot_limit = (region_end - slots_start) / slot_size;

    // F3DEX2 and RT64 extended commands.
    constexpr uint32_t op_vtx = 0x01;
    constexpr uint32_t op_tri1 = 0x05, op_tri2 = 0x06, op_quad = 0x07;
    constexpr uint32_t op_movemem = 0xDC;
    constexpr uint32_t op_mtx = 0xDA;
    constexpr uint32_t op_moveword = 0xDB;
    constexpr uint32_t op_dl = 0xDE;
    constexpr uint32_t op_enddl = 0xDF;
    constexpr uint32_t ex_opcode = 0x64;
    constexpr uint32_t ex_enable_w0 = (0xE0u << 24) | 0x525464;
    constexpr uint32_t ex_enable_w1 = (0x1u << 28) | ex_opcode;
    constexpr uint32_t ex_matrix_group_w0 = (ex_opcode << 24) | 0x00000C;
    constexpr uint32_t ex_viewport_align_w0 = (ex_opcode << 24) | 0x000007;
    constexpr uint32_t ex_scissor_w0 = (ex_opcode << 24) | 0x000005;
    constexpr uint32_t ex_rect_align_w0 = (ex_opcode << 24) | 0x000006;
    constexpr uint32_t ex_texrect_w0 = (ex_opcode << 24) | 0x000002;
    constexpr uint32_t ex_fillrect_w0 = (ex_opcode << 24) | 0x000003;
    constexpr uint32_t op_fillrect = 0xF6, op_othermode_h = 0xE3;
    constexpr uint32_t op_texrect = 0xE4, op_setscissor = 0xED, op_spnoop = 0xE0;
    constexpr uint32_t origin_left = 0x000, origin_right = 0x400, origin_none = 0x800;

    constexpr uint32_t skip = 0, interpolate = 1, automatic = 2;
    constexpr uint32_t group_params(uint32_t projection, uint32_t decompose, uint32_t position, uint32_t rotation,
                                    uint32_t scale, uint32_t skew, uint32_t perspective, uint32_t vertex, uint32_t tile,
                                    uint32_t lookat) {
        return (projection << 1) | (decompose << 2) | (position << 3) | (rotation << 5) | (scale << 7) | (skew << 9) |
               (perspective << 11) | (vertex << 13) | (tile << 15) | (lookat << 24);
    }
    // Objects: move, turn and scale smoothly, matched in drawing order within one identity.
    constexpr uint32_t model_params = group_params(0, 1, interpolate, interpolate, interpolate, interpolate, skip,
                                                   automatic, automatic, interpolate);
    // Several instances may share one identity: RT64 pairs them up by position (order "auto").
    constexpr uint32_t instanced_params = model_params | (1u << 17);
    // No identity: every decision is RT64's.
    constexpr uint32_t auto_params = group_params(0, 0, automatic, automatic, automatic, automatic, automatic,
                                                  automatic, automatic, automatic) | (1u << 17);
    // Camera: plain blending of position and direction.
    constexpr uint32_t camera_params = group_params(1, 0, interpolate, interpolate, skip, skip, interpolate,
                                                    interpolate, interpolate, interpolate);

    bool enabled = true;
    bool trace = false;     // --trace-camera: one line per frame on stdout
    uint32_t frame_number = 0;
    std::unordered_map<uint64_t, uint32_t> slot_of_command;
    uint32_t slots_used = 0;
    bool slots_full_reported = false;

    // Changes whenever the camera cuts. Part of every identity.
    uint32_t generation = 0;

    struct Camera {
        bool valid = false;
        float forward[3]{};
        float eye[3]{};
    };
    Camera previous_camera;
    float previous_step = 0.0f;

    uint32_t read_word(uint8_t* rdram, uint32_t address) {
        return static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(address))));
    }

    void write_word(uint8_t* rdram, uint32_t address, uint32_t value) {
        MEM_W(0, static_cast<gpr>(static_cast<int32_t>(address))) = value;
    }

    bool in_slots(uint32_t address) {
        return address >= slots_start && address < slots_start + slots_used * slot_size;
    }

    void read_matrix(uint8_t* rdram, uint32_t address, float m[4][4]) {
        for (int index = 0; index < 16; index++) {
            uint32_t high = read_word(rdram, address + (index / 2) * 4);
            uint32_t low = read_word(rdram, address + 0x20 + (index / 2) * 4);
            int shift = (index & 1) ? 0 : 16;
            int32_t fixed = static_cast<int32_t>((((high >> shift) & 0xFFFF) << 16) | ((low >> shift) & 0xFFFF));
            m[index / 4][index % 4] = fixed / 65536.0f;
        }
    }

    // --- Widescreen HUD ------------------------------------------------------
    // In a wide window the game's HUD keeps to the 4:3 middle. The game draws it after the 3D
    // scene, under a projection with no camera in it, in two forms: textured rectangles when it
    // is at rest, and quads (the radar's disc always, the rest while it slides or spins). The
    // same part of the frame holds whatever else is 2D: messages, the pause menu.
    // CPlayer__DrawHud (hooked, see below) says that a frame shows a game HUD at all. In such a
    // frame every rectangle and quad of that 2D part is a candidate. Candidates that touch or
    // nearly touch each other on screen are taken as one item (a number and its icon, the radar
    // and its rim, a line of text). An item that lies wholly left or right of the middle is
    // pinned to that edge of the window (RT64's viewport and rectangle alignment); an item that
    // crosses the middle, such as a centred message or the pause menu, stays where it is.
    struct HudRange {
        uint32_t begin, end;
    };
    std::mutex hud_mutex;
    std::vector<HudRange> hud_ranges;       // filled by the game thread, used by the next task
    uint32_t hud_open_begin = 0;
    bool hud_to_edges = true;
    struct HudQuad {
        bool rectangle;                     // a G_TEXRECT (three commands) rather than a quad
        uint32_t vertex_command;            // the G_VTX, or the G_TEXRECT
        uint32_t last_triangle;             // the last triangle command drawn from it
        uint32_t viewport_w0, viewport_w1;  // state in force at the quad
        uint32_t scissor_w0, scissor_w1;
        float left, right, top, bottom;     // on screen, -1..1
        int group;
    };

    constexpr float hud_middle = 0.06f;     // half-width of the strip that counts as "the middle"
    constexpr float hud_gap = 0.12f;        // items closer than this, side by side, are one item
    // Where the weapon icon's right edge goes: as far in from the right as the radar's left edge
    // (-0.95 at rest) is from the left.
    constexpr float weapon_icon_right = 0.95f;

    // The screen extent of a set of vertices. False if it cannot be worked out.
    bool screen_extent(uint8_t* rdram, uint32_t address, uint32_t count, uint32_t model, uint32_t projection, HudQuad& quad) {
        if (count == 0 || model == 0 || projection == 0) {
            return false;
        }
        float mv[4][4], pr[4][4];
        read_matrix(rdram, model, mv);
        read_matrix(rdram, projection, pr);
        quad.left = quad.top = 1e9f;
        quad.right = quad.bottom = -1e9f;
        for (uint32_t i = 0; i < count; i++) {
            const gpr vertex = static_cast<gpr>(static_cast<int32_t>(address + i * 16));
            const float in[4] = { static_cast<float>(MEM_H(0, vertex)), static_cast<float>(MEM_H(2, vertex)),
                                  static_cast<float>(MEM_H(4, vertex)), 1.0f };
            float eye[4], clip_x = 0.0f, clip_y = 0.0f, clip_w = 0.0f;
            for (int column = 0; column < 4; column++) {
                eye[column] = in[0] * mv[0][column] + in[1] * mv[1][column] + in[2] * mv[2][column] + in[3] * mv[3][column];
            }
            for (int row = 0; row < 4; row++) {
                clip_x += eye[row] * pr[row][0];
                clip_y += eye[row] * pr[row][1];
                clip_w += eye[row] * pr[row][3];
            }
            if (std::fabs(clip_w) < 1e-6f) {
                return false;
            }
            quad.left = std::min(quad.left, clip_x / clip_w);
            quad.right = std::max(quad.right, clip_x / clip_w);
            quad.top = std::min(quad.top, clip_y / clip_w);
            quad.bottom = std::max(quad.bottom, clip_y / clip_w);
        }
        // Only things that are on the screen and are not most of it: backdrops and anything that
        // reaches far off screen are not HUD items, and would swallow real ones into their group.
        // Off to the side is allowed: the HUD slides out of the picture when the game is paused,
        // and an item that stopped being pinned half way out would come apart (the radar's map
        // left its rim).
        constexpr float far = 3.0f;
        if (quad.right - quad.left > 1.2f || quad.bottom - quad.top > 1.2f ||
            quad.bottom < -far || quad.top > far || quad.right < -far || quad.left > far) {
            return false;
        }
        return true;
    }

    // A projection with no camera in it: the end-of-frame pass and the radar use such.
    bool is_plain_projection(uint8_t* rdram, uint32_t address) {
        float m[4][4];
        read_matrix(rdram, address, m);
        return m[0][1] == 0.0f && m[0][2] == 0.0f && m[0][3] == 0.0f && m[1][0] == 0.0f && m[1][2] == 0.0f &&
               m[1][3] == 0.0f && m[2][0] == 0.0f && m[2][1] == 0.0f;
    }

    // RT64 extended commands used for pinning.
    uint32_t emit_viewport_align(uint8_t* rdram, uint32_t at, uint32_t origin) {
        write_word(rdram, at + 0x00, ex_viewport_align_w0);
        write_word(rdram, at + 0x04, origin);
        write_word(rdram, at + 0x08, 0);
        write_word(rdram, at + 0x0C, 0);
        return at + 0x10;
    }

    // The width of the picture in quarter pixels. The game's scissor stops one pixel short of it
    // (0x77C for 480 pixels), so round up to a whole number of 8-pixel units.
    int32_t picture_width(uint32_t scissor_w1) {
        return static_cast<int32_t>((((scissor_w1 >> 12) & 0xFFF) + 8) / 32 * 32);
    }

    // The game's scissor with its left edge counted from the window's left edge and its right
    // edge from the window's right edge. The coordinates themselves must stay the game's: RT64
    // widens the 3D scene only while the scene's scissor is as wide as every other scissor used
    // on the same picture, and a scissor that reached even one pixel further stops that.
    uint32_t emit_wide_scissor(uint8_t* rdram, uint32_t at, uint32_t scissor_w0, uint32_t scissor_w1) {
        const int32_t right = static_cast<int32_t>((scissor_w1 >> 12) & 0xFFF) - picture_width(scissor_w1);
        write_word(rdram, at + 0x00, ex_scissor_w0);
        write_word(rdram, at + 0x04, ((scissor_w1 >> 24) & 0x3) | (origin_left << 2) | (origin_right << 14));
        write_word(rdram, at + 0x08, (((scissor_w0 >> 12) & 0xFFF) << 16) | (scissor_w0 & 0xFFF));
        write_word(rdram, at + 0x0C, ((static_cast<uint32_t>(right) & 0xFFFF) << 16) | (scissor_w1 & 0xFFF));
        return at + 0x10;
    }

    // The scissor for an item pinned to one edge. One that covers the picture opens to the window
    // (above). A smaller one clips the item itself (the radar keeps its map inside the rim with it):
    // it keeps its size and moves to the same edge as the item, or what it should hide shows.
    uint32_t emit_item_scissor(uint8_t* rdram, uint32_t at, uint32_t scissor_w0, uint32_t scissor_w1, uint32_t origin) {
        const int32_t width = picture_width(scissor_w1);
        const int32_t left = static_cast<int32_t>((scissor_w0 >> 12) & 0xFFF);
        const int32_t right = static_cast<int32_t>((scissor_w1 >> 12) & 0xFFF);
        if (left <= 0 && right >= width - 8) {
            return emit_wide_scissor(rdram, at, scissor_w0, scissor_w1);
        }
        const int32_t shift = origin == origin_right ? -width : 0;
        write_word(rdram, at + 0x00, ex_scissor_w0);
        write_word(rdram, at + 0x04, ((scissor_w1 >> 24) & 0x3) | (origin << 2) | (origin << 14));
        write_word(rdram, at + 0x08, ((static_cast<uint32_t>(left + shift) & 0xFFFF) << 16) | (scissor_w0 & 0xFFF));
        write_word(rdram, at + 0x0C, ((static_cast<uint32_t>(right + shift) & 0xFFFF) << 16) | (scissor_w1 & 0xFFF));
        return at + 0x10;
    }

    uint32_t emit_rect_align(uint8_t* rdram, uint32_t at, uint32_t origin, int32_t x_offset) {
        write_word(rdram, at + 0x00, ex_rect_align_w0);
        write_word(rdram, at + 0x04, origin | (origin << 12));
        write_word(rdram, at + 0x08, (static_cast<uint32_t>(x_offset) & 0xFFFF) << 16);
        write_word(rdram, at + 0x0C, (static_cast<uint32_t>(x_offset) & 0xFFFF) << 16);
        return at + 0x10;
    }

    // --- Widescreen backdrop -------------------------------------------------
    // RT64 keeps 2D rectangles in the 4:3 middle of a wide window, except a rectangle that spans
    // the whole picture (a fade, the cutscene bars), which it stretches itself. The backdrop the
    // game draws under the scene's camera (the sky) is a grid of textured tiles, none of which
    // spans the picture, so it stays 4:3 wide. It is stretched here.
    // RT64's extended rectangles take, for the left and for the right edge, an "origin": a point
    // across the window (0 = left edge, 0x400 = right edge) from which the coordinate counts.
    // Giving each edge the origin that matches its own place across the 4:3 picture stretches
    // the rectangle, or a whole grid of them, evenly over the window.
    struct StretchedEdge {
        uint32_t origin;
        int32_t coordinate;
    };

    StretchedEdge stretched_edge(int32_t x, int32_t width) {
        const int32_t origin = std::clamp((x * 0x400 + width / 2) / width, 0, 0x400);
        return { static_cast<uint32_t>(origin), x - (origin * width) / 0x400 };     // RT64 adds origin * width / 0x400 back
    }

    // A G_TEXRECT and the two half-commands after it, as one extended rectangle.
    uint32_t emit_stretched_texrect(uint8_t* rdram, uint32_t at, uint32_t command, int32_t width) {
        const uint32_t w0 = read_word(rdram, command), w1 = read_word(rdram, command + 4);
        const StretchedEdge left = stretched_edge((w1 >> 12) & 0xFFF, width);
        const StretchedEdge right = stretched_edge((w0 >> 12) & 0xFFF, width);
        write_word(rdram, at + 0x00, ex_texrect_w0);
        write_word(rdram, at + 0x04, ((w1 >> 24) & 0x7) | (left.origin << 3) | (right.origin << 15));
        write_word(rdram, at + 0x08, ((static_cast<uint32_t>(left.coordinate) & 0xFFFF) << 16) | (w1 & 0xFFF));
        write_word(rdram, at + 0x0C, ((static_cast<uint32_t>(right.coordinate) & 0xFFFF) << 16) | (w0 & 0xFFF));
        write_word(rdram, at + 0x10, read_word(rdram, command + 0x0C));     // s, t
        write_word(rdram, at + 0x14, read_word(rdram, command + 0x14));     // ds/dx, dt/dy
        return at + 0x18;
    }

    uint32_t emit_command(uint8_t* rdram, uint32_t at, uint32_t w0, uint32_t w1) {
        write_word(rdram, at + 0x00, w0);
        write_word(rdram, at + 0x04, w1);
        return at + 0x08;
    }

    // Reads the camera out of a view-projection matrix (row vectors: clip = world * M).
    // Column 3 gives the view direction, columns 0 and 1 the scaled right and up axes.
    Camera read_camera(uint8_t* rdram, uint32_t address) {
        float m[4][4];
        read_matrix(rdram, address, m);
        Camera camera;
        float along[3];     // eye position measured along right, up and forward
        float axes[3][3];
        const int columns[3] = { 0, 1, 3 };
        for (int axis = 0; axis < 3; axis++) {
            int column = columns[axis];
            float length = std::sqrt(m[0][column] * m[0][column] + m[1][column] * m[1][column] + m[2][column] * m[2][column]);
            if (length < 1e-6f) {
                return camera;      // not a perspective camera (2D overlay)
            }
            for (int i = 0; i < 3; i++) {
                axes[axis][i] = m[i][column] / length;
            }
            along[axis] = -m[3][column] / length;
        }
        for (int i = 0; i < 3; i++) {
            camera.forward[i] = axes[2][i];
            camera.eye[i] = axes[0][i] * along[0] + axes[1][i] * along[1] + axes[2][i] * along[2];
        }
        camera.valid = true;
        return camera;
    }

    // True if the camera did not get here by moving: a cut between shots.
    bool is_cut(const Camera& camera) {
        if (!camera.valid || !previous_camera.valid) {
            previous_step = 0.0f;
            return camera.valid != previous_camera.valid;
        }
        float dot = 0.0f, step = 0.0f;
        for (int i = 0; i < 3; i++) {
            dot += camera.forward[i] * previous_camera.forward[i];
            float delta = camera.eye[i] - previous_camera.eye[i];
            step += delta * delta;
        }
        step = std::sqrt(step);
        // A turn of more than about 25 degrees in one frame, or a jump several
        // times longer than the camera was travelling per frame.
        bool cut = dot < 0.9f || (step > 60.0f && step > previous_step * 6.0f + 30.0f);
        if (trace) {
            std::printf("camera %u: step %.1f dot %.4f eye %.0f %.0f %.0f%s\n", frame_number, step, dot,
                        camera.eye[0], camera.eye[1], camera.eye[2], cut ? " CUT" : "");
        }
        previous_step = cut ? 0.0f : step;
        return cut;
    }

    // Bits 0-22 say which object, bit 23 which kind of identity, the top byte which shot.
    uint32_t model_id(bool by_command, uint32_t address) {
        return ((generation % 100 + 1) << 24) | (by_command ? 0x00800000 : 0) | (address & 0x007FFFFF);
    }

    uint32_t camera_id(uint32_t ordinal) {
        return 0xF0000000 | ((generation % 100) << 16) | (ordinal & 0xFFFF);
    }

    enum class Identity {
        Camera,     // a view-projection matrix
        Address,    // the matrix has an address of its own: that address
        Command,    // the matrix is addressed through a segment: the command that loads it
        None,       // nothing stable to go by: left to RT64
    };

    struct MatrixCommand {
        uint32_t command_address;
        uint32_t slot;
        Identity identity;
        uint32_t matrix_address;
    };
}

void southpark::set_interpolation_hints(bool on) {
    enabled = on;
}

void southpark::set_hud_to_edges(bool on) {
    hud_to_edges = on;
}

// Hook at the start of CPlayer__DrawHud(player, view): remember where in the display list
// this player's HUD starts. Only for a view that fills the screen; in split screen each
// player's HUD belongs inside that player's part of the picture.
extern "C" void sp_hud_begin(uint8_t* rdram, recomp_context* ctx) {
    const int32_t view = static_cast<int32_t>(ctx->r5);
    auto view_float = [&](int32_t offset) {
        uint32_t bits = read_word(rdram, static_cast<uint32_t>(view + offset));
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    const bool full_screen = view != 0 && view_float(0x2B0) <= 0.0f && view_float(0x2B4) <= 0.0f &&
                             view_float(0x2A8) >= 320.0f && view_float(0x2AC) >= 240.0f;
    hud_open_begin = full_screen ? read_word(rdram, display_list_head) : 0;
}

// Hook in CPlayerList__DrawHuds, right after the call to CPlayer__DrawHud.
extern "C" void sp_hud_end(uint8_t* rdram, recomp_context* ctx) {
    if (hud_open_begin == 0) {
        return;
    }
    const uint32_t end = read_word(rdram, display_list_head);
    if (end > hud_open_begin) {
        std::lock_guard lock{ hud_mutex };
        hud_ranges.push_back({ hud_open_begin, end });
    }
    hud_open_begin = 0;
}

void southpark::trace_camera(bool on) {
    trace = on;
}

void southpark::add_interpolation_hints(uint8_t* rdram, OSTask* task) {
    if (!enabled || task->t.type != 1) {    // 1 = M_GFXTASK
        return;
    }

    // Pass 1: find every matrix command and give it a slot.
    static std::vector<MatrixCommand> commands;
    static std::vector<uint32_t> stack;
    commands.clear();
    stack.clear();
    uint32_t segments[16] = {};
    auto resolve = [&](uint32_t address) {
        if (address >= 0x80000000) {
            return address;
        }
        return 0x80000000 | ((segments[(address >> 24) & 0xF] + (address & 0x00FFFFFF)) & 0x00FFFFFF);
    };

    const uint32_t start = resolve(static_cast<uint32_t>(task->t.data_ptr));
    const uint32_t frame_list_size = task->t.data_size;
    if (southpark::dev::trace_input()) {
        static uint32_t tasks = 0, largest = 0;
        largest = std::max(largest, frame_list_size);
        if (++tasks % 60 == 0) {
            std::printf("frame list: %08X, 0x%X bytes, largest 0x%X\n", start, frame_list_size, largest);
            largest = 0;
        }
    }
    uint32_t pc = start;
    uint32_t camera_matrix = 0;
    bool seen_model = false;

    // The HUD stretches recorded for this frame's list (and none left over from older ones).
    static std::vector<HudRange> hud;
    hud.clear();
    {
        std::lock_guard lock{ hud_mutex };
        for (auto it = hud_ranges.begin(); it != hud_ranges.end();) {
            if (it->begin >= start && it->end <= start + frame_list_size) {
                hud.push_back(*it);
                it = hud_ranges.erase(it);
            }
            else {
                ++it;
            }
        }
        if (hud_ranges.size() > 16) {
            hud_ranges.clear();
        }
    }
    auto in_hud = [&](uint32_t address) {
        for (const HudRange& range : hud) {
            if (address >= range.begin && address < range.end) {
                return true;
            }
        }
        return false;
    };
    uint32_t scratch = frame_scratch_start;
    uint32_t viewport_w0 = 0, viewport_w1 = 0;      // the last viewport command
    uint32_t current_model = 0, current_projection = 0;
    uint32_t scissor_w0 = 0, scissor_w1 = 0;        // the last scissor command
    bool plain_projection = false;                  // the current projection has no camera in it
    static std::vector<HudQuad> quads;
    quads.clear();
    static std::vector<uint32_t> backdrop;          // textured rectangles drawn before the 3D scene
    backdrop.clear();
    bool fill_mode = false;                         // the RDP is in fill (or copy) mode: clears, not drawing
    auto in_frame_list = [&](uint32_t address) {
        return address >= start && address < start + frame_list_size;
    };
    int open_quad = -1;                             // the quad whose triangles are being drawn
    const bool pin_hud = hud_to_edges && southpark::hud_at_edges() && !hud.empty();
    const bool widescreen = hud_to_edges;           // off with --no-hints style switches for comparison
    for (int count = 0; count < 200000; count++) {
        if (pc < 0x80000000 || pc >= 0x81000000) {
            break;
        }
        const uint32_t w0 = read_word(rdram, pc);
        const uint32_t w1 = read_word(rdram, pc + 4);
        const uint32_t opcode = w0 >> 24;
        const uint32_t command_address = pc;
        pc += 8;

        // What kind of identity a matrix command has. Animated models keep their matrices in
        // per-frame scratch memory, reached through a segment register that is set once per
        // model instance; the address is different every frame. The command, though, sits in
        // the model's own display list, which the game keeps: it names the model and the part,
        // and RT64 is left to tell apart several instances of one model by where they are.
        auto identity_of = [&](uint32_t mtx_w0, uint32_t mtx_w1) {
            if (mtx_w0 & 0x04) {    // parameter byte, push bit inverted; bit 2 is "projection"
                return Identity::Camera;
            }
            if (mtx_w1 >= 0x80000000) {
                return Identity::Address;
            }
            const bool in_frame_list = command_address >= start && command_address < start + frame_list_size;
            return in_frame_list ? Identity::None : Identity::Command;
        };

        uint32_t mtx_w0 = 0, mtx_w1 = 0, slot = 0;
        bool is_matrix = false;
        if (opcode == op_mtx) {
            // One slot per distinct command; for "Command" identities, per command location.
            uint64_t key = (static_cast<uint64_t>(w0 & 0xFF) << 32) | w1;
            if (identity_of(w0, w1) == Identity::Command) {
                key |= static_cast<uint64_t>(command_address & 0x00FFFFFF) << 40;
            }
            auto found = slot_of_command.find(key);
            if (found != slot_of_command.end()) {
                slot = found->second;
            }
            else if (slots_used < slot_limit) {
                slot = slots_start + slots_used * slot_size;
                slots_used++;
                slot_of_command.emplace(key, slot);
                write_word(rdram, slot + 0x10, w0);
                write_word(rdram, slot + 0x14, w1);
                write_word(rdram, slot + 0x18, op_enddl << 24);
                write_word(rdram, slot + 0x1C, 0);
            }
            else {
                if (!slots_full_reported) {
                    slots_full_reported = true;
                    std::fprintf(stderr, "interpolation hints: out of slots, further objects are left to RT64's guess\n");
                }
                continue;
            }
            write_word(rdram, command_address, op_dl << 24);
            write_word(rdram, command_address + 4, slot);
            mtx_w0 = w0;
            mtx_w1 = w1;
            is_matrix = true;
        }
        else if (opcode == op_dl && in_slots(resolve(w1))) {
            // Replaced on an earlier frame; the list has been kept.
            slot = resolve(w1);
            mtx_w0 = read_word(rdram, slot + 0x10);
            mtx_w1 = read_word(rdram, slot + 0x14);
            is_matrix = true;
        }

        if (is_matrix) {
            const Identity identity = identity_of(mtx_w0, mtx_w1);
            const uint32_t matrix_address = resolve(mtx_w1);
            (identity == Identity::Camera ? current_projection : current_model) = matrix_address;
            if (identity == Identity::Camera) {
                plain_projection = is_plain_projection(rdram, matrix_address);
            }
            commands.push_back({ command_address, slot, identity, matrix_address });
            if (identity == Identity::Camera && !seen_model) {
                camera_matrix = matrix_address;
            }
            seen_model |= identity != Identity::Camera;
        }
        else if (opcode == op_movemem && (w0 & 0xFF) == 0x08) {
            viewport_w0 = w0;
            viewport_w1 = w1;
        }
        else if (opcode == op_setscissor) {
            scissor_w0 = w0;
            scissor_w1 = w1;
        }
        else if (opcode == op_othermode_h && (w0 & 0xFFFF) == 0x0A01) {    // the cycle type field
            fill_mode = (w1 & 0x00200000) != 0;
        }
        else if (opcode == op_texrect && widescreen && camera_matrix != 0 && current_projection == camera_matrix &&
                 !fill_mode && stack.empty() && scissor_w1 != 0) {
            backdrop.push_back(command_address);
        }
        else if (opcode == op_vtx) {
            open_quad = -1;
            if (pin_hud && viewport_w0 != 0 && scissor_w0 != 0 && plain_projection && stack.empty()) {
                HudQuad quad{};
                if (screen_extent(rdram, resolve(w1), (w0 >> 12) & 0xFF, current_model, current_projection, quad)) {
                    quad.vertex_command = command_address;
                    quad.viewport_w0 = viewport_w0;
                    quad.viewport_w1 = viewport_w1;
                    quad.scissor_w0 = scissor_w0;
                    quad.scissor_w1 = scissor_w1;
                    open_quad = static_cast<int>(quads.size());
                    quads.push_back(quad);
                }
            }
        }
        else if (opcode == op_tri1 || opcode == op_tri2 || opcode == op_quad) {
            if (open_quad >= 0) {
                quads[open_quad].last_triangle = command_address;
            }
        }
        else if (opcode == op_texrect && pin_hud && scissor_w0 != 0 && plain_projection && stack.empty()) {
            // Corners in quarter pixels; the screen size is the scissor's far corner.
            const float width = static_cast<float>(picture_width(scissor_w1));
            const float height = static_cast<float>(scissor_w1 & 0xFFF);
            if (width > 0.0f && height > 0.0f) {
                HudQuad quad{};
                quad.rectangle = true;
                quad.vertex_command = command_address;
                quad.scissor_w0 = scissor_w0;
                quad.scissor_w1 = scissor_w1;
                quad.left = ((w1 >> 12) & 0xFFF) / width * 2.0f - 1.0f;
                quad.right = ((w0 >> 12) & 0xFFF) / width * 2.0f - 1.0f;
                quad.bottom = 1.0f - (w1 & 0xFFF) / height * 2.0f;      // same way up as the quads: +1 is the top
                quad.top = 1.0f - (w0 & 0xFFF) / height * 2.0f;
                quads.push_back(quad);
            }
        }
        else if (opcode == op_moveword && ((w0 >> 16) & 0xFF) == 0x06) {
            segments[((w0 & 0xFFFF) / 4) & 0xF] = w1;
        }
        else if (opcode == op_dl) {
            if (((w0 >> 16) & 0xFF) == 0) {
                stack.push_back(pc);
            }
            pc = resolve(w1);
        }
        else if (opcode == op_enddl) {
            if (stack.empty()) {
                break;
            }
            pc = stack.back();
            stack.pop_back();
        }
    }

    // The backdrop, if this frame drew a 3D scene: stretch its tiles over the window.
    if (seen_model && camera_matrix != 0 && read_camera(rdram, camera_matrix).valid) {
        const int32_t width = picture_width(scissor_w1);
        // Only rows of tiles that together reach from one edge of the picture to the other, as the
        // sky's do. Text and logos are drawn the same way (the "Expansion Pak found" title), and
        // stretched they would show the neighbouring letters of their font texture.
        struct Row {
            uint32_t top;
            int32_t left, right;
        };
        static std::vector<Row> rows;
        rows.clear();
        auto row_of = [&](uint32_t command) -> Row& {
            const uint32_t top = read_word(rdram, command + 4) & 0xFFF;
            for (Row& row : rows) {
                if (row.top == top) {
                    return row;
                }
            }
            return rows.emplace_back(Row{ top, 0x7FFFFFFF, -1 });
        };
        for (uint32_t command : backdrop) {
            Row& row = row_of(command);
            row.left = std::min(row.left, static_cast<int32_t>((read_word(rdram, command + 4) >> 12) & 0xFFF));
            row.right = std::max(row.right, static_cast<int32_t>((read_word(rdram, command) >> 12) & 0xFFF));
        }
        for (uint32_t command : backdrop) {
            if (width <= 0 || scratch + 0x38 > frame_scratch_end) {
                break;
            }
            const Row& row = row_of(command);
            if (row.left > 8 || row.right < width - 8) {
                continue;
            }
            uint32_t at = emit_wide_scissor(rdram, scratch, scissor_w0, scissor_w1);
            at = emit_stretched_texrect(rdram, at, command, width);
            at = emit_command(rdram, at, scissor_w0, scissor_w1);
            at = emit_command(rdram, at, op_enddl << 24, 0);
            write_word(rdram, command, op_dl << 24);
            write_word(rdram, command + 4, scratch);
            for (uint32_t offset = 8; offset < 24; offset += 8) {
                write_word(rdram, command + offset, op_spnoop << 24);
                write_word(rdram, command + offset + 4, 0);
            }
            scratch = at;
        }
    }

    // Group the HUD candidates into items and pin the items that sit to one side.
    for (size_t i = 0; i < quads.size(); i++) {
        quads[i].group = static_cast<int>(i);
    }
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < quads.size(); i++) {
            for (size_t j = i + 1; j < quads.size(); j++) {
                const HudQuad& a = quads[i];
                const HudQuad& b = quads[j];
                const bool same_row = std::min(std::max(a.top, a.bottom), std::max(b.top, b.bottom)) >
                                      std::max(std::min(a.top, a.bottom), std::min(b.top, b.bottom));
                const bool near = std::max(a.left, b.left) - std::min(a.right, b.right) < hud_gap;
                if (a.group != b.group && same_row && near) {
                    const int from = b.group, to = a.group;
                    for (HudQuad& quad : quads) {
                        if (quad.group == from) {
                            quad.group = to;
                        }
                    }
                    merged = true;
                }
            }
        }
    }
    // Each item's extent. The weapon icon (with the ammunition count, when there is one) sits in
    // the bottom right, beside the weapon in the hand; it goes to the right edge of the window,
    // as far in from it as the bottom-left item (the radar) is from the left edge.
    struct ItemBox {
        float left = 1e9f, right = -1e9f, highest = -1e9f;     // highest: the top edge, +1 = top
        bool has_quads = false;
    };
    static std::vector<ItemBox> items;
    items.assign(quads.size(), ItemBox{});
    for (const HudQuad& quad : quads) {
        ItemBox& item = items[quad.group];
        item.left = std::min(item.left, quad.left);
        item.right = std::max(item.right, quad.right);
        item.highest = std::max(item.highest, std::max(quad.top, quad.bottom));
        item.has_quads |= !quad.rectangle;
    }
    for (const HudQuad& quad : quads) {
        const ItemBox& item = items[quad.group];
        const float left = item.left, right = item.right;
        const int side = right < -hud_middle ? -1 : (left > hud_middle ? 1 : 0);
        // The weapon icon: moved right, in quarter pixels of the picture.
        int32_t extra = 0;
        if (side > 0 && item.highest < 0.0f && !item.has_quads) {
            const float move = weapon_icon_right - right;
            if (move > 0.0f) {
                extra = static_cast<int32_t>(move * static_cast<float>(picture_width(quad.scissor_w1)) / 2.0f);
            }
        }
        if (trace) {
            std::printf("hud quad %08X: x %.2f..%.2f y %.2f..%.2f, item x %.2f..%.2f, side %d\n", quad.vertex_command,
                        quad.left, quad.right, quad.top, quad.bottom, left, right, side);
        }
        if (side == 0 || scratch + 0x70 > frame_scratch_end) {
            continue;
        }
        if (quad.rectangle) {
            // A rectangle is three commands long; all three move into the list.
            const uint32_t command = quad.vertex_command;
            const int32_t width = picture_width(quad.scissor_w1);
            uint32_t at = scratch;
            at = emit_rect_align(rdram, at, side < 0 ? origin_left : origin_right, side < 0 ? 0 : -width + extra);
            at = emit_item_scissor(rdram, at, quad.scissor_w0, quad.scissor_w1, side < 0 ? origin_left : origin_right);
            for (uint32_t offset = 0; offset < 24; offset += 8) {
                at = emit_command(rdram, at, read_word(rdram, command + offset), read_word(rdram, command + offset + 4));
            }
            at = emit_rect_align(rdram, at, origin_none, 0);
            at = emit_command(rdram, at, quad.scissor_w0, quad.scissor_w1);
            at = emit_command(rdram, at, op_enddl << 24, 0);
            write_word(rdram, command, op_dl << 24);
            write_word(rdram, command + 4, scratch);
            for (uint32_t offset = 8; offset < 24; offset += 8) {
                write_word(rdram, command + offset, op_spnoop << 24);
                write_word(rdram, command + offset + 4, 0);
            }
            scratch = at;
            continue;
        }
        if (quad.last_triangle == 0) {
            continue;
        }
        // Before the vertices: pin the viewport, open the scissor to the whole window.
        uint32_t at = scratch;
        at = emit_viewport_align(rdram, at, side < 0 ? origin_left : origin_right);
        at = emit_item_scissor(rdram, at, quad.scissor_w0, quad.scissor_w1, side < 0 ? origin_left : origin_right);
        at = emit_command(rdram, at, quad.viewport_w0, quad.viewport_w1);
        at = emit_command(rdram, at, read_word(rdram, quad.vertex_command), read_word(rdram, quad.vertex_command + 4));
        at = emit_command(rdram, at, op_enddl << 24, 0);
        write_word(rdram, quad.vertex_command, op_dl << 24);
        write_word(rdram, quad.vertex_command + 4, scratch);
        scratch = at;
        // After its last triangle: everything back as the game had it.
        at = emit_command(rdram, at, read_word(rdram, quad.last_triangle), read_word(rdram, quad.last_triangle + 4));
        at = emit_viewport_align(rdram, at, origin_none);
        at = emit_command(rdram, at, quad.scissor_w0, quad.scissor_w1);
        at = emit_command(rdram, at, quad.viewport_w0, quad.viewport_w1);
        at = emit_command(rdram, at, op_enddl << 24, 0);
        write_word(rdram, quad.last_triangle, op_dl << 24);
        write_word(rdram, quad.last_triangle + 4, scratch);
        scratch = at;
    }

    frame_number++;
    if (trace) {
        uint32_t projections = 0;
        for (const MatrixCommand& command : commands) {
            projections += command.identity == Identity::Camera;
        }
        std::printf("frame %u: %zu matrices, %u projections, camera matrix %08X\n", frame_number, commands.size(),
                    projections, camera_matrix);
    }

    // The camera of this frame: the last view-projection loaded before the first object.
    if (camera_matrix != 0) {
        Camera camera = read_camera(rdram, camera_matrix);
        if (is_cut(camera)) {
            generation++;
        }
        previous_camera = camera;
    }

    // Pass 2: write this frame's identities.
    uint32_t camera_ordinal = 0;
    for (const MatrixCommand& command : commands) {
        write_word(rdram, command.slot + 0x00, ex_matrix_group_w0);
        uint32_t id = 0xFFFFFFFF;       // G_EX_ID_AUTO
        uint32_t params = auto_params;
        switch (command.identity) {
            case Identity::Camera:
                id = camera_id(camera_ordinal++);
                params = camera_params;
                break;
            case Identity::Address:
                id = model_id(false, command.matrix_address);
                params = model_params;
                break;
            case Identity::Command:
                id = model_id(true, command.command_address);
                params = instanced_params;
                break;
            case Identity::None:
                break;
        }
        write_word(rdram, command.slot + 0x04, id);
        write_word(rdram, command.slot + 0x08, params);
        write_word(rdram, command.slot + 0x0C, 0);
    }

    // Start the task with "enable extended commands", then continue in the game's list.
    write_word(rdram, region_start + 0x00, ex_enable_w0);
    write_word(rdram, region_start + 0x04, ex_enable_w1);
    write_word(rdram, region_start + 0x08, (op_dl << 24) | (1 << 16));     // branch, no return
    write_word(rdram, region_start + 0x0C, start);
    task->t.data_ptr = static_cast<int32_t>(region_start);
}
