#include "southpark_config.h"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "southpark_support.h"
#include "southpark_dev.h"
#include "ultramodern/config.hpp"
#include "librecomp/files.hpp"
#include "librecomp/config.hpp"
#include "util/file.h"
#include <filesystem>
#include <fstream>
#include <iomanip>

#if defined(_WIN32)
#include <Shlobj.h>
#elif defined(__linux__)
#include <unistd.h>
#include <pwd.h>
#elif defined(__APPLE__)
#include "apple/rt64_apple.h"
#endif

enum class ControlScheme : uint32_t { Modern, Original };
enum class HudPlacement : uint32_t { Edges, Middle };
enum class UpdateRate : uint32_t { Original, Double };
enum class DrawDistance : uint32_t { Original, Far };

static void add_general_options(recomp::config::Config &config) {
    using EnumOptionVector = const std::vector<recomp::config::ConfigOptionEnumOption>;

    static EnumOptionVector control_scheme_options = {
        {ControlScheme::Modern, "Modern"},
        {ControlScheme::Original, "Original"},
    };
    config.add_enum_option(
        southpark::configkeys::general::control_scheme,
        "Control Scheme",
        "<recomp-color primary>Modern</recomp-color>: the left stick walks and strafes, the right stick and the mouse turn and look, and the view stays where you leave it."
        "<br />"
        "<br />"
        "<recomp-color primary>Original</recomp-color>: as on the Nintendo 64. The stick turns and looks, the C buttons walk and strafe, and the view returns to level when the stick is released."
        "<br />"
        "<br />"
        "Modern applies to the game's default control type. To look with the mouse, raise Mouse Sensitivity above zero; the left mouse button then fires.",
        control_scheme_options,
        ControlScheme::Modern
    );
    static EnumOptionVector hud_placement_options = {
        {HudPlacement::Edges, "Edges"},
        {HudPlacement::Middle, "Middle"},
    };
    config.add_enum_option(
        southpark::configkeys::general::hud_placement,
        "HUD Placement",
        "Where the in-game display (portrait, radar, health, ammunition, score) sits in a widescreen window."
        "<br />"
        "<br />"
        "<recomp-color primary>Edges</recomp-color>: at the left and right edges of the window."
        "<br />"
        "<recomp-color primary>Middle</recomp-color>: where a 4:3 picture would have it, as in the original.",
        hud_placement_options,
        HudPlacement::Edges
    );
    config.add_bool_option(
        southpark::configkeys::general::invert_look,
        "Invert Look",
        "Pushing the right stick or the mouse forward looks down instead of up. Modern control scheme only.",
        false
    );
    static EnumOptionVector update_rate_options = {
        {UpdateRate::Original, "30 (Original)"},
        {UpdateRate::Double, "60"},
    };
    config.add_enum_option(
        southpark::configkeys::general::update_rate,
        "Game Update Rate",
        "How many times a second the game reads the controls and moves the world. The picture is smoothed to the display's frame rate either way."
        "<br />"
        "<br />"
        "<recomp-color primary>60</recomp-color>: the view follows the mouse and the sticks one frame sooner. The game keeps its speed, but it was not made to run this way: if something runs slower or faster than it should, try 30."
        "<br />"
        "<recomp-color primary>30</recomp-color>: as on the Nintendo 64.",
        update_rate_options,
        UpdateRate::Double
    );
    static EnumOptionVector draw_distance_options = {
        {DrawDistance::Original, "Original"},
        {DrawDistance::Far, "Far"},
    };
    config.add_enum_option(
        southpark::configkeys::general::draw_distance,
        "Draw Distance",
        "How far the world is drawn before the fog closes in."
        "<br />"
        "<br />"
        "<recomp-color primary>Original</recomp-color>: as on the Nintendo 64."
        "<br />"
        "<recomp-color primary>Far</recomp-color>: twice as far; the fog starts twice as far away. The game loads the nearest parts of the level first, so if it runs out of room, what is missing is far away.",
        draw_distance_options,
        DrawDistance::Original
    );
}

float southpark::draw_distance_scale() {
    const float forced = southpark::dev::forced_draw_distance();
    if (forced > 0.0f) {
        return forced;
    }
    switch (static_cast<DrawDistance>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(
        southpark::configkeys::general::draw_distance)))) {
        case DrawDistance::Far:
            return 2.0f;
        case DrawDistance::Original:
        default:
            return 1.0f;
    }
}

bool southpark::double_update_rate() {
    const int forced = southpark::dev::forced_update_rate();
    if (forced != 0) {
        return forced >= 60;
    }
    return static_cast<UpdateRate>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(
        southpark::configkeys::general::update_rate))) == UpdateRate::Double;
}

bool southpark::hud_at_edges() {
    return static_cast<HudPlacement>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(
        southpark::configkeys::general::hud_placement))) == HudPlacement::Edges;
}

bool southpark::modern_controls() {
    return static_cast<ControlScheme>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(
        southpark::configkeys::general::control_scheme))) == ControlScheme::Modern;
}

bool southpark::invert_look() {
    return std::get<bool>(recompui::config::get_general_config().get_option_value(southpark::configkeys::general::invert_look));
}

bool southpark::mouse_look_enabled() {
    return recompui::config::general::has_mouse_sensitivity_option() && recompui::config::general::get_mouse_sensitivity() != 0;
}

template <typename T = uint32_t>
T get_general_config_enum_value(const std::string& option_id) {
    return static_cast<T>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(option_id)));
}

template <typename T = uint32_t>
T get_general_config_number_value(const std::string& option_id) {
    return static_cast<T>(std::get<double>(recompui::config::get_general_config().get_option_value(option_id)));
}

template <typename T = uint32_t>
T get_graphics_config_enum_value(const std::string& option_id) {
    return static_cast<T>(std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(option_id)));
}

static void add_graphics_options(recomp::config::Config &config) {
    // User Task: Add game-specific graphics options.
}

static void set_control_defaults() {
    using namespace recompinput;

    // TODO default controls overrides
}

static void set_control_descriptions() {
    // @User Task: Set control descriptions for each game input. These descriptions will be shown in the UI when the
    // user is rebinding controls, so they should be clear and concise.
    recompinput::set_game_input_description(recompinput::GameInput::Y_AXIS_POS, "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::Y_AXIS_NEG, "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::X_AXIS_NEG, "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::X_AXIS_POS, "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::A,          "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::B,          "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::Z,          "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::L,          "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::R,          "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::START,      "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::C_UP,       "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::C_DOWN,     "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::C_LEFT,     "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::C_RIGHT,    "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::DPAD_UP,    "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::DPAD_DOWN,  "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::DPAD_LEFT,  "Add description");
    recompinput::set_game_input_description(recompinput::GameInput::DPAD_RIGHT, "Add description");
}

void southpark::init_config() {
    std::filesystem::path recomp_dir = recompui::file::get_app_folder_path();

    if (!recomp_dir.empty()) {
        std::filesystem::create_directories(recomp_dir);
    }

    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = true;

    auto &general_config = recompui::config::create_general_tab(general_options);
    add_general_options(general_config);

    auto &graphics_config = recompui::config::create_graphics_tab();
    add_graphics_options(graphics_config);

    set_control_defaults();
    set_control_descriptions();
    recompui::config::create_controls_tab();

    recompui::config::create_sound_tab();

    recompui::config::create_mods_tab();

    recompui::config::finalize();

}
