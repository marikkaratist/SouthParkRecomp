// The game's update rate: 30 updates a second, as on the Nintendo 64, or 60.
//
// The game moves everything by a frame increment (0x800AB698): the length of the last frame in
// units of a fifteenth of a second, worked out from how many vertical blanks (VIs, sixty a second)
// the last frames took. Two limits keep it at 30 updates a second or fewer:
//
//  - each of the three framebuffers (0x800C2760, 0x108 bytes apart) holds the least number of VIs
//    it stays on screen for, at +0x104: 2. The graphics scheduler (func_00258618) holds a finished
//    frame back until that many have passed;
//  - the main loop (func_002074B0) averages the VI counts of the last four frames, leaving out the
//    longest, into 0x800CD380, and raises the result to at least 2 (3 with three or four players)
//    before turning it into the frame increment.
//
// On a PC the frames are ready long before the second VI, so lowering both limits to 1 runs the
// game at 60 updates a second with half the frame increment. Everything that moves is scaled by the
// increment, so the game keeps its speed; the gain is that the controls are read twice as often and
// the picture follows them one frame sooner, which is what the mouse needs.

#include <algorithm>
#include <cstdint>

#include "recomp.h"
#include "southpark_config.h"

namespace {
    constexpr uint32_t framebuffers = 0x800C2760;
    constexpr uint32_t framebuffer_size = 0x108;
    constexpr uint32_t framebuffer_min_vis = 0x104;
    constexpr uint32_t frame_vis = 0x800CD380;          // the VI count the frame increment is made from
    // The VI counts of the last four frames, as the main loop reads them.
    constexpr uint32_t vi_history[] = { 0x800CD704, 0x800CD780, 0x800C2754, 0x800CD384 };

    constexpr int32_t original_min_vis = 2;

    bool lowered = false;

    int32_t& word(uint8_t* rdram, uint32_t address) {
        return MEM_W(0, static_cast<gpr>(static_cast<int32_t>(address)));
    }

    void set_framebuffer_minimum(uint8_t* rdram, int32_t from, int32_t to) {
        for (uint32_t i = 0; i < 3; i++) {
            int32_t& field = word(rdram, framebuffers + i * framebuffer_size + framebuffer_min_vis);
            if (field == from) {
                field = to;
            }
        }
    }
}

// Hook in func_002074B0 at 0x002078A8, where the main loop turns the VI count into the frame
// increment.
extern "C" void sp_update_rate(uint8_t* rdram, recomp_context* ctx) {
    if (!southpark::double_update_rate()) {
        if (lowered) {
            set_framebuffer_minimum(rdram, 1, original_min_vis);
            lowered = false;
        }
        return;
    }
    set_framebuffer_minimum(rdram, original_min_vis, 1);
    lowered = true;

    // The main loop's own average, without its lower limit of 2. A higher limit (split screen with
    // three or four players) is left alone.
    int32_t sum = 0, longest = 0;
    for (uint32_t address : vi_history) {
        const int32_t count = word(rdram, address);
        sum += count;
        longest = std::max(longest, count);
    }
    const int32_t average = std::max(1, (sum - longest + 2) / 3);
    int32_t& count = word(rdram, frame_vis);
    if (count == original_min_vis && average < count) {
        count = average;
    }
}
