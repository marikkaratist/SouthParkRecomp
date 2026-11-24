#include "southpark_launcher.h"
#include <atomic>

void southpark::launcher_animation_setup(recompui::LauncherMenu *menu) {
    auto context = recompui::get_current_context();
    recompui::Element *background_container = menu->get_background_container();
    background_container->set_background_color({ 0x00, 0x00, 0x00, 0xFF });

    (void)context;
}

void southpark::launcher_animation_update(recompui::LauncherMenu *menu) {
}
