#ifndef __SOUTHPARK_CONFIG_H__
#define __SOUTHPARK_CONFIG_H__

#include <cstdint>
#include <filesystem>
#include <string>

namespace southpark {
    inline const std::u8string program_id = u8"SouthParkRecompiled";
    inline const std::string program_name = "South Park: Recompiled";

    namespace configkeys {
        namespace general {
            inline const std::string control_scheme = "control_scheme";
            inline const std::string invert_look = "invert_look";
            inline const std::string hud_placement = "hud_placement";
            inline const std::string update_rate = "update_rate";
            inline const std::string draw_distance = "draw_distance";
        }
    }

    // Control options (general tab), read by src/game/controls.cpp.
    bool modern_controls();         // left stick moves, right stick and mouse look, the view does not recentre
    bool invert_look();
    bool mouse_look_enabled();      // mouse sensitivity above zero
    bool hud_at_edges();            // widescreen HUD: pinned to the window's edges rather than the 4:3 middle
    bool double_update_rate();      // the game runs 60 updates a second instead of 30 (src/game/update_rate.cpp)
    float draw_distance_scale();    // the cameras' far clip and fog, times this (src/game/draw_distance.cpp)

    // TODO: Move loading configs to the runtime once we have a way to allow per-project customization.
    void init_config();
};

#endif
