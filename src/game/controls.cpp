// Modern controls for South Park: dual-analog movement, free look, mouse look.
//
// How the game reads the player's controls (same engine as Turok 2, where the
// function is CPlayer__ReadInput): once per frame, for each player,
// func_0022BDA0 fills a small structure inside the player object:
//
//   player + 0xAF4  PlayerInput
//     0x00  pointer to the controller state (stick as floats at 0x1C / 0x20)
//     0x04  turn    -1..1, a turning speed
//     0x08  look    -1..1, the view pitch itself: the movement code steers the
//                   pitch towards this value every frame
//     0x0C  strafe  -1..1
//     0x10  move    -1..1
//     0x18  buttons
//
// With the default control type the stick gives turn and look, the C buttons
// give strafe and move. "look" is simply the stick's position, so the view
// returns to level as soon as the stick is released.
//
// The hook below runs right after that function and rewrites the four values:
// the left stick walks and strafes, the right stick and the mouse turn, and
// "look" becomes a value this file keeps for each player and changes at a rate,
// so the view stays where it was left.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "SDL.h"

#include "recomp.h"
#include "recompinput/recompinput.h"
#include "recompinput/input_state.h"
#include "southpark_config.h"
#include "southpark_dev.h"

void southpark_debug_capture_poll(uint16_t buttons);
void southpark_debug_capture_player(uint32_t object);

namespace {
    // Game addresses.
    constexpr int32_t player_input_offset = 0xAF4;
    constexpr int32_t player_options_offset = 0xA50;    // pointer; byte 1 is the control type
    constexpr int32_t pad_array = static_cast<int32_t>(0x800E3EB0);     // four controller states
    constexpr int32_t pad_size = 0x230;
    constexpr int32_t raw_pad_array = static_cast<int32_t>(0x800E3C48); // OSContPad[4]
    constexpr int32_t frame_increment = static_cast<int32_t>(0x800AB698); // 1.0 at the game's full speed

    constexpr int32_t player_pitch_offset = 0xB60;
    // When either is set, CPlayerList__Update does not move the player (as Turok 2's
    // CPlayer__IsPausedOrInMenu): the game is paused, or a menu is open on the player's camera.
    constexpr int32_t game_paused = static_cast<int32_t>(0x800CD360);
    constexpr int32_t player_camera_offset = 0xA54;
    constexpr int32_t camera_menus_offset = 0x538;
    // The game's limits for looking: -pi/2 and +pi/2 in the ROM.
    constexpr int32_t pitch_limit_up = static_cast<int32_t>(0x800ADBD0);
    constexpr int32_t pitch_limit_down = static_cast<int32_t>(0x800ADBD4);

    constexpr uint16_t button_z = 0x2000;

    // How the game turns: per frame, the heading moves by turn * 0.349 rad * 0.9 * frame increment
    // (func_0023318C and its siblings), about 18 degrees times the frame increment at full stick.
    // The look range is 90 degrees either way, so a look rate of 0.2 would tilt exactly as fast as
    // the stick turns; a little under that feels even, since the screen is wider than it is tall.
    constexpr float stick_look_rate = 0.17f;
    // Mouse, at 100% sensitivity: one count of movement is 0.36 degrees both ways.
    constexpr float mouse_look_scale = 0.004f;                      // of the 90 degree range
    constexpr float mouse_turn_scale = 0.36f / 18.0f;               // in "turn" units for one frame increment
    // Walking: how much quicker than the game's own the player gets up to speed and stops.
    constexpr float walk_response = 3.0f;
    // Guest memory for the edited copy of the walking table: just above the video mode copy
    // that src/game/video.cpp keeps at 0x80FFFF00.
    constexpr int32_t walk_table_copy = static_cast<int32_t>(0x80FFFF80);

    std::array<float, 4> look{};            // the pitch each player has chosen, -1..1
    float mouse_x = 0.0f, mouse_y = 0.0f;   // movement gathered since the player code last ran
    int pad_reads = 0;                      // controller reads since the player code last ran (--trace-input)

    float read_float(uint8_t* rdram, int32_t address) {
        uint32_t bits = static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(address)));
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void write_float(uint8_t* rdram, int32_t address, float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        MEM_W(0, static_cast<gpr>(address)) = bits;
    }

    // The game squares its inputs for finer control near the centre.
    float curve(float value) {
        value = std::clamp(value, -1.0f, 1.0f);
        return value * std::fabs(value);
    }
}

// Hook in func_0022C1F8, right after osContGetReadData: the raw state of the four
// controllers has just been written. Runs once per controller read.
extern "C" void sp_controls_after_pad_read(uint8_t* rdram, recomp_context* ctx) {
    MEM_H(0, static_cast<gpr>(raw_pad_array)) |= southpark::dev::mashed_buttons();
    southpark_debug_capture_poll(static_cast<uint16_t>(MEM_HU(0, static_cast<gpr>(raw_pad_array))));     // src/game/debug_capture.cpp

    const bool modern = southpark::modern_controls();
    // The right stick is bound to the C buttons by default; in the modern scheme it is the camera.
    recompinput::set_right_analog_suppressed(modern);
    if (!modern || recompinput::game_input_disabled()) {
        mouse_x = mouse_y = 0.0f;
        return;
    }

    float dx = 0.0f, dy = 0.0f;
    recompinput::get_mouse_deltas(&dx, &dy);    // already scaled by the sensitivity setting; zero when it is off
    dx += southpark::dev::fake_mouse();
    pad_reads++;
    mouse_x += dx;
    mouse_y += dy;

    // Mouse buttons cannot be bound in the launcher yet: while mouse look is on, left button fires.
    if (southpark::mouse_look_enabled() && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK)) {
        MEM_H(0, static_cast<gpr>(raw_pad_array)) |= button_z;
    }
}

// Hook in func_00235680, right after the call to func_0022BDA0. $s0 is the player.
extern "C" void sp_controls_after_read_input(uint8_t* rdram, recomp_context* ctx) {
    if (!southpark::modern_controls()) {
        return;
    }
    const int32_t player = static_cast<int32_t>(ctx->r16);
    const int32_t input = player + player_input_offset;
    // The controls are read while the game is paused as well, but the player is not moved. Leave
    // the view alone then; what the mouse did in the meantime is dropped.
    const int32_t camera = MEM_W(player_camera_offset, static_cast<gpr>(player));
    if (MEM_W(0, static_cast<gpr>(game_paused)) != 0 || (camera != 0 && MEM_W(camera_menus_offset, static_cast<gpr>(camera)) != 0)) {
        mouse_x = mouse_y = 0.0f;
        return;
    }
    const int32_t options = MEM_W(player_options_offset, static_cast<gpr>(player));
    if (options == 0 || MEM_B(1, static_cast<gpr>(options)) != 0) {
        return;     // only the default control type is remapped
    }
    const int32_t pad = MEM_W(0, static_cast<gpr>(input));
    int index = (pad - pad_array) / pad_size;
    if (pad < pad_array || index < 0 || index > 3) {
        index = 0;  // the single-player game reads a merged controller
    }

    // Left stick: walk and strafe. The C buttons (and keys bound to them) still work.
    // The stick is taken from the controller state's plain bytes (0x14, 0x15: dead zone applied,
    // -80..80). The floats at 0x1C and 0x20, which the game uses for turning, are a smoothed copy
    // that takes a while to fall back to zero: walking by them carries on after the stick is let go.
    const float left_x = static_cast<int8_t>(MEM_B(0x14, static_cast<gpr>(pad))) * 0.0125f;
    const float left_y = static_cast<int8_t>(MEM_B(0x15, static_cast<gpr>(pad))) * 0.0125f;
    if (left_x != 0.0f) {
        write_float(rdram, input + 0x0C, curve(left_x));
    }
    if (left_y != 0.0f) {
        write_float(rdram, input + 0x10, curve(left_y));
    }

    // Right stick and mouse: turn and look.
    float right_x = 0.0f, right_y = 0.0f;
    recompinput::get_right_analog(index, &right_x, &right_y);
    const float invert = southpark::invert_look() ? -1.0f : 1.0f;
    const float step = read_float(rdram, frame_increment);

    float turn = curve(right_x);
    float& pitch = look[index];
    pitch += -right_y * std::fabs(right_y) * invert * stick_look_rate * step;
    if (index == 0) {
        if (southpark::dev::trace_input()) {
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            std::printf("input: %6.2f ms, step %.3f, reads %d, mouse %.1f, frame VIs %d\n",
                        std::chrono::duration<double, std::milli>(now - last).count(), step, pad_reads, mouse_x,
                        MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800CD380))));
            last = now;
            pad_reads = 0;
        }
        // "turn" is a speed, applied in proportion to the frame increment; the mouse gives a distance.
        if (step > 0.001f) {
            turn += mouse_x * mouse_turn_scale / step;
        }
        pitch += -mouse_y * invert * mouse_look_scale;
        mouse_x = mouse_y = 0.0f;
    }
    pitch = std::clamp(pitch, -1.0f, 1.0f);

    write_float(rdram, input + 0x04, turn);
    write_float(rdram, input + 0x08, pitch);

    // The movement code does not set the view pitch (player + 0xB60, radians) to "look": it moves
    // it 18% of the way there each frame, five times slower than it does the turn. That suits a
    // stick position; for a view that is already where the player put it, it is only lag. Put the
    // pitch where the movement code is heading, the same way it works the target out (look times
    // the game's own upper or lower limit), so there is nothing left for it to catch up on.
    const float limit = read_float(rdram, pitch > 0.0f ? pitch_limit_up : pitch_limit_down);
    write_float(rdram, player + player_pitch_offset, pitch * 80.0f * 0.716197133f * 0.0174532942f * std::fabs(limit) * -1.0f);
}

// Hook at the start of func_00231D50(player, object, table): the function that turns the "move"
// and "strafe" inputs into speed. The table ($a2) holds, for walking and for strafing, an
// acceleration, a top speed and a braking rate. With the game's values it takes about a quarter
// of a second to get going and as long to stop, which is fine for buttons and feels like sliding
// on a stick. In the modern scheme the function is handed a copy with quicker rates; the top
// speeds are left alone.
extern "C" void sp_controls_walk_table(uint8_t* rdram, recomp_context* ctx) {
    southpark::dev::player_object_seen(rdram, static_cast<uint32_t>(ctx->r5));
    southpark_debug_capture_player(static_cast<uint32_t>(ctx->r5));
    if (!southpark::modern_controls()) {
        return;
    }
    const int32_t table = static_cast<int32_t>(ctx->r6);
    for (int32_t offset = 0; offset < 0x24; offset += 4) {
        MEM_W(offset, static_cast<gpr>(walk_table_copy)) = MEM_W(offset, static_cast<gpr>(table));
    }
    for (int32_t offset : { 0x00, 0x08, 0x0C, 0x14 }) {    // accelerate, brake; forward, sideways
        write_float(rdram, walk_table_copy + offset, read_float(rdram, table + offset) * walk_response);
    }
    ctx->r6 = static_cast<gpr>(walk_table_copy);
}
