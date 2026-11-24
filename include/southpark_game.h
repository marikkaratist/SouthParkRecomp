#ifndef __SOUTHPARK_GAME_H__
#define __SOUTHPARK_GAME_H__

#include <cstdint>
#include <span>
#include <vector>
#include "recomp.h"
#include "ultramodern/ultra64.h"

namespace southpark {
    void game_on_init(uint8_t* rdram, recomp_context* ctx);
    void on_thread_create(uint8_t* rdram, recomp_context* ctx);

    // Frame interpolation hints for RT64 (src/game/interpolation.cpp): tags every
    // matrix in a graphics task with an identity before the task is rendered.
    void add_interpolation_hints(uint8_t* rdram, OSTask* task);
    void set_interpolation_hints(bool on);
    void trace_camera(bool on);
    // Widescreen HUD: pin the left and right halves of the in-game HUD to the window's edges.
    void set_hud_to_edges(bool on);};

#endif
