#include "southpark_launcher.h"
#include <chrono>
#include <cmath>
#include <random>
#include <vector>

// Launcher look in the show's flat cut-paper style: green mountains, snowy pines, a crooked
// title board on the left, the menu on the right in outlined sign lettering, falling snow.
//
// Layout is in dp (1 dp = 1/1080 of the window height). The background picture is 1920x1200
// and is pinned to the bottom of the window, so at 16:9 one picture pixel is one dp. All the
// artwork is drawn by tools/make_launcher_art.py.

namespace {
    using namespace recompui;

    const Color sky_color{ 0x8F, 0xD8, 0xF8, 0xFF };   // SKY in make_launcher_art.py
    const Color ink{ 0x11, 0x11, 0x11, 0xFF };
    const Color title_accent{ 0xC0, 0x1E, 0x1E, 0xFF };
    const Color option_color{ 0xFF, 0xFF, 0xFF, 0xFF };
    constexpr const char* sign_font = "Suplexmentary Comic NC";
    constexpr const char* option_outline = "outline(4dp #10301f)";

    // Title board: 900x760 picture units at 0.8 dp per unit, tilted like a hammered-in sign.
    constexpr float title_scale = 0.8f;
    constexpr float title_w = 900.0f * title_scale, title_h = 760.0f * title_scale;
    // The bottom puts the post's foot about 90 dp into the snowfield (its edge is ~260 dp up).
    constexpr float title_left = 120.0f, title_bottom = 150.0f, title_tilt = -4.0f;

    // Menu on the right, right-aligned like a classic title screen.
    constexpr float menu_right = 150.0f, menu_bottom = 300.0f, menu_width = 620.0f;

    // recompui has no setter for some RCSS properties (font-effect). Style::set_property is a
    // protected virtual; a pointer to it taken through a subclass of Style is legal, and calling
    // it on an element dispatches to Element::set_property, which updates the RmlUi element.
    struct PropertyAccess : Style {
        static void set(Element* element, const std::string& name, const std::string& value) {
            Rml::PropertyDictionary properties;
            if (!Rml::StyleSheetSpecification::ParsePropertyDeclaration(properties, name, value)) {
                return;
            }
            void (Style::*setter)(Rml::PropertyId, const Rml::Property&) = &PropertyAccess::set_property;
            Style* style = element;
            for (const auto& [id, property] : properties.GetProperties()) {
                (style->*setter)(id, property);
            }
        }
    };

    struct Flake {
        Element* element;
        float x, y;     // percent of the window
        float fall;     // percent of the height per second
        float sway, phase;
    };

    std::vector<Flake> flakes;
    std::chrono::steady_clock::time_point start_time;

    Element* make_box(ContextId context, Element* parent) {
        Element* e = context.create_element<Element>(parent, 0, "div", false);
        e->set_position(Position::Absolute);
        return e;
    }

    Label* sign_text(ContextId context, Element* parent, const std::string& text, float top, float size, Color color) {
        Label* label = context.create_element<Label>(parent, text, theme::Typography::Header1);
        label->set_position(Position::Absolute);
        label->set_left(0.0f);
        label->set_width(100.0f, Unit::Percent);
        label->set_top(top);
        label->set_text_align(TextAlign::Center);
        label->set_font_family(sign_font);
        label->set_font_size(size);
        label->set_line_height(size * 1.1f);
        label->set_font_weight(400);
        label->set_letter_spacing(size * 0.05f);
        label->set_color(color);
        return label;
    }

    float wrap(float value, float range) {
        return value - range * std::floor(value / range);
    }
}

void southpark::launcher_animation_setup(recompui::LauncherMenu *menu) {
    auto context = recompui::get_current_context();
    start_time = std::chrono::steady_clock::now();
    flakes.clear();

    menu->remove_default_title();

    Element* background = menu->get_background_container();
    background->set_background_color(sky_color);

    Svg* scenery = context.create_element<Svg>(background, "launcher/background.svg");
    scenery->set_position(Position::Absolute);
    scenery->set_left(0.0f);
    scenery->set_bottom(0.0f);
    scenery->set_width(100.0f, Unit::Percent);
    scenery->set_height_auto();

    // Title board; the text rotates with it. The board spans y 20..600 of the picture.
    Element* title = make_box(context, background);
    title->set_left(title_left);
    title->set_bottom(title_bottom);
    title->set_width(title_w);
    title->set_height(title_h);
    title->set_rotation(title_tilt);
    {
        Svg* art = context.create_element<Svg>(title, "launcher/title_sign.svg");
        art->set_position(Position::Absolute);
        art->set_left(0.0f);
        art->set_top(0.0f);
        art->set_width(title_w);
        art->set_height(title_h);
        sign_text(context, title, "SOUTH", 40.0f, 140.0f, ink);
        sign_text(context, title, "PARK", 178.0f, 140.0f, ink);
        sign_text(context, title, "RECOMPILED", 340.0f, 62.0f, title_accent);
    }

    // Falling snow, behind the menu text.
    std::mt19937 rng{ 1997 };
    std::uniform_real_distribution<float> unit{ 0.0f, 1.0f };
    for (int i = 0; i < 90; i++) {
        float size = 4.0f + 7.0f * unit(rng);
        Element* flake = make_box(context, background);
        flake->set_width(size);
        flake->set_height(size);
        flake->set_border_radius(size * 0.5f);
        flake->set_background_color(Color{ 0xFF, 0xFF, 0xFF, static_cast<uint8_t>(170 + 85 * unit(rng)) });
        // Bigger flakes are closer, so they fall faster.
        flakes.push_back({ flake, 100.0f * unit(rng), 110.0f * unit(rng), 3.0f + size * 0.9f, 0.4f + 0.8f * unit(rng), 6.28f * unit(rng) });
    }

    // Menu: outlined sign lettering, no boxes. The focused option pulses (GameOption does that);
    // the background stays clear so nothing has to line up with the text.
    GameOptionsMenu* options = menu->get_game_options_menu();
    if (options != nullptr) {
        options->set_right(menu_right);
        options->set_bottom(menu_bottom);
        options->set_width(menu_width);
        options->set_gap(6.0f);
        for (GameOption* option : options->get_options()) {
            option->set_justify_content(JustifyContent::FlexEnd);
            option->set_padding(6.0f);
            option->set_padding_left(0.0f);
            option->set_padding_right(0.0f);
            option->set_color(option_color);
            // The focused option pulses between orange and pale orange (GameOption does that),
            // which can read as white; growing it a little keeps the choice obvious.
            option->hover_style.set_background_color(theme::color::Transparent);
            option->hover_style.set_color(theme::color::Secondary);
            option->hover_style.set_scale_2D(1.08f, 1.08f);
            option->focus_style.set_background_color(theme::color::Transparent);
            option->focus_style.set_scale_2D(1.08f, 1.08f);
            Label* label = option->get_label();
            label->set_font_family(sign_font);
            label->set_font_size(64.0f);
            label->set_line_height(72.0f);
            label->set_font_weight(400);
            label->set_letter_spacing(3.0f);
            PropertyAccess::set(label, "font-effect", option_outline);
        }
    }

    launcher_animation_update(menu);
}

void southpark::launcher_animation_update(recompui::LauncherMenu *menu) {
    (void)menu;
    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count();

    for (const Flake& flake : flakes) {
        float y = wrap(flake.y + flake.fall * t, 110.0f) - 5.0f;
        float x = flake.x + flake.sway * std::sin(t * 0.9f + flake.phase);
        flake.element->set_top(y, Unit::Percent);
        flake.element->set_left(x, Unit::Percent);
    }
}
