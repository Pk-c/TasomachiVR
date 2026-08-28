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
    float forward_offset{25.0f};
    float up_offset{0.0f};
    // Eye height while piloting the boat, in place of up_offset. On the page for the same
    // reason as the rest of the offsets: it is judged from the pilot's seat, in a headset.
    float ship_up_offset{-40.0f};
    // Eye forward while piloting, in place of forward_offset. On the page for the same reason
    // as its pair: it is judged from the pilot's seat.
    float ship_forward_offset{25.0f};
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

    // Centimetres the eye is raised while airborne. On the page because it is judged by eye,
    // in a headset, mid-jump - which is not something a number in a file can be tuned by.
    float air_lift{40.0f};
    // Pushed forward while airborne as well: the tuck brings the chest up AND
    // forward, and height alone does not always clear it.
    float air_forward{0.0f};

    // Seconds the head stays hidden after landing. On the page because the right value is
    // whatever stops it flashing back while the recovery animation is still unwinding, and
    // that is only visible from inside the headset.
    float head_hide_linger{0.30f};

    // Rendering, driven live through console commands rather than the game's ini - so both
    // are judged in the headset and neither needs a restart.
    //
    // detail     how far away meshes keep their high LOD. 1 is the game's own choice; higher
    //            pushes the switch further out, which is what stops things visibly popping.
    // supersample  render scale in percent. The bluntest clarity gain there is in VR, and the
    //            most expensive - it is the one to back off if the framerate suffers.
    float detail{4.0f};
    float supersample{120.0f};

};

} // namespace tasomachivr
