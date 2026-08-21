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
    float up_offset{0.0f};
    float yaw_offset{0.0f};
    int   pause_button{3};
};

} // namespace tasomachivr
