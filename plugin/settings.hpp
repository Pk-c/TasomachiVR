// TasomachiVR - the settings the in-game page is allowed to edit.
//
// Its own header so that the page and the plugin share one definition without either
// depending on the other, and so that adding a loader-side setting cannot accidentally
// appear on the page.
#pragma once

namespace tasomachivr {

struct MenuSettings {
    int   turn_mode{0};        // 0 snap, 1 smooth
    float snap_angle{45.0f};
    float snap_cooldown{0.3f};
    float smooth_speed{90.0f};
    float turn_deadzone{0.2f};
    float forward_offset{10.0f};
    float up_offset{6.0f};
    float yaw_offset{0.0f};
    int   pause_button{3};
    int   body_mode{1};       // 0 whole mesh hidden, 1 headless body

    // How big UEVR draws the game's interface plane. On the page because it has to be found
    // by eye - too large and it runs past the edge of the headset's field of view, too small
    // and the text is unreadable, and where that line falls depends on the headset.
    float menu_size{1.3f};

    // Keep the HUD counters on screen instead of fading them in on the left trigger. Off by
    // default: the point of the fade is a clear view, and this is the escape hatch for
    // moments when you would rather just read the numbers.
    bool  hud_always_on{false};
};

} // namespace tasomachivr
