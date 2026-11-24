#ifndef __SOUTHPARK_DEV_H__
#define __SOUTHPARK_DEV_H__

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ultramodern::renderer {
    class RendererContext;
}

// Development aids, driven by command-line switches:
//   --rom <path>      validate and store this ROM as if it had been picked in the launcher
//   --autostart       start the game as soon as the launcher is up
//   --log <path>      send stdout and stderr to a file
//   --dump-dl <n>     write the first n graphics display lists to stderr
//   --dump-dl-skip <n> let n display lists pass before dumping starts
//   --no-hints        leave frame interpolation to RT64's own guessing (src/game/interpolation.cpp)
//   --mash <s>        press START and A in turn for s seconds (unattended runs into a level)
//   --trace-input     one line per player update: time since the last one, frame increment, mouse
//   --fake-mouse <n>  add n counts of sideways mouse movement to every controller read
//   --update-rate <r> 30 or 60: overrides the Game Update Rate option (src/game/update_rate.cpp)
//   --draw-distance <k> scales the far clip and fog by k, overriding the Draw Distance option
//   --memory 8        tell the game it has 8 MiB, as a console with the Expansion Pak, not 14
//   --capture-at <s>  as pressing F9 s seconds after start (src/game/debug_capture.cpp)
//   --find-player     print the player object's position-like floats once
//   --teleport <x> <z> move the player 47 s after start (the game moves it back: not usable yet)
//   --watchdog <s>    if no graphics task arrives for s seconds, write every thread's call stack
//   --dump-textures [folder]  dump every texture once for tools/make_texture_pack.py
//                     (default folder: texture_dump in the settings folder)
// On Windows a crash, abort or uncaught exception writes a call stack to stderr.
namespace southpark::dev {
    void parse_args(int argc, char** argv);
    // --dump-textures [folder]: call after the program id is set (the default folder is in the
    // settings folder) and before the renderer starts.
    void apply_texture_dump();
    bool autostart_requested();
    const std::filesystem::path& rom_path();

    // Called once the runtime has allocated guest memory, so that crash reports
    // can translate host addresses back to guest ones.
    // Buttons to add to controller 1 right now (--mash).
    uint16_t mashed_buttons();
    // --trace-input, --fake-mouse <counts>, --update-rate <30|60>: see src/game/dev_tools.cpp.
    bool trace_input();
    float fake_mouse();
    int forced_update_rate();       // 0 when not given
    float forced_draw_distance();   // --draw-distance <scale>; 0 when not given
    int forced_memory_mib();        // --memory 8; 0 when not given
    int capture_at_seconds();       // --capture-at <s>: an F9 capture s seconds in; 0 when not given
    // Called with the player's object every update: --find-player prints its floats once,
    // --teleport x z moves it there once the level has run for a few seconds.
    void player_object_seen(uint8_t* rdram, uint32_t object);

    void set_rdram(uint8_t* rdram);
    void install_crash_handler();

    void set_display_list_dumps(int count);
    void start_watchdog(int seconds);
    // Called for every graphics task; feeds the watchdog.
    void note_frame();
    // Wraps the renderer so that graphics tasks can be tagged for interpolation, dumped and watched.
    std::unique_ptr<ultramodern::renderer::RendererContext> wrap_renderer(
        uint8_t* rdram, std::unique_ptr<ultramodern::renderer::RendererContext> renderer);
}

#endif
