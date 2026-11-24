// Audio interface status for South Park's audio manager.
//
// The game's audio thread (__amMain) is driven by the AI interrupt. On each one
// it generates audio frames until the AI reports its two-buffer FIFO as full:
//
//     while (!(osAiGetStatus() & AI_STATUS_FIFO_FULL)) {
//         __amHandleFrameMsg(...);     // builds a frame, ends in osAiSetNextBuffer()
//     }
//
// The runtime's osAiGetStatus always returns 0, so that loop never ends and the
// audio thread starves every thread below it. These replacements report the
// FIFO as full once two buffers' worth of audio is waiting to be played.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

namespace {
    constexpr uint32_t ai_status_fifo_full = 0x80000000;
    constexpr uint32_t ai_status_dma_busy = 0x40000000;

    // Size of the most recent buffer handed to the AI, in bytes.
    std::atomic<uint32_t> last_buffer_bytes{ 0 };

    // Bookkeeping for the rate report below.
    uint32_t declared_frequency = 0;
    uint64_t bytes_in_window = 0;
    uint32_t buffers_in_window = 0;
    int reports_left = 8;
    std::chrono::steady_clock::time_point window_start;

    // Prints, a few times, how much audio the game actually produces per second
    // next to the rate it asked the AI for. The two must agree, or the sound
    // plays at the wrong speed and pitch.
    void note_buffer(uint32_t bytes) {
        auto now = std::chrono::steady_clock::now();
        if (buffers_in_window == 0 && bytes_in_window == 0) {
            window_start = now;
        }
        bytes_in_window += bytes;
        buffers_in_window++;
        double elapsed = std::chrono::duration<double>(now - window_start).count();
        if (reports_left > 0 && elapsed >= 5.0) {
            reports_left--;
            std::printf("audio: asked for %u Hz; produced %.0f stereo frames/s in %u buffers of %u bytes (%.1f buffers/s)\n",
                        declared_frequency, bytes_in_window / 4.0 / elapsed, buffers_in_window, bytes, buffers_in_window / elapsed);
            bytes_in_window = 0;
            buffers_in_window = 0;
        }
    }
}

// s32 osAiSetFrequency(u32 frequency)
extern "C" void sp_osAiSetFrequency(uint8_t*, recomp_context* ctx) {
    declared_frequency = static_cast<uint32_t>(ctx->r4);
    std::printf("audio: osAiSetFrequency(%u)\n", declared_frequency);
    ultramodern::set_audio_frequency(declared_frequency);
    ctx->r2 = declared_frequency;
}

// s32 osAiSetNextBuffer(void* buffer, u32 size)
extern "C" void sp_osAiSetNextBuffer(uint8_t* rdram, recomp_context* ctx) {
    uint32_t size = static_cast<uint32_t>(ctx->r5);
    last_buffer_bytes.store(size);
    note_buffer(size);
    ultramodern::queue_audio_buffer(rdram, static_cast<int32_t>(ctx->r4), size);
    ctx->r2 = 0;
}

// u32 osAiGetStatus(void)
extern "C" void sp_osAiGetStatus(uint8_t*, recomp_context* ctx) {
    uint32_t buffer = last_buffer_bytes.load();
    uint32_t remaining = ultramodern::get_remaining_audio_bytes();
    uint32_t status = 0;
    if (remaining > 0) {
        status |= ai_status_dma_busy;
    }
    // One buffer playing and one queued behind it: nothing more fits.
    if (buffer != 0 && remaining >= buffer * 2) {
        status |= ai_status_fifo_full;
    }
    ctx->r2 = status;
}
