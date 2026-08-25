// TasomachiVR - where the eye sits, and what stops it entering a wall.
//
// This logic used to live in the Lua script. It moved here because the wall test has to
// happen in the same place as the eye is computed, and because C++ already owns the view
// rotation, the body and the arms - the Lua half had shrunk to a liability.
//
// Two things it does:
//
//   FOLLOWS THE HEAD BONE, filtered. The bone carries the walk cycle, and head bob is a
//   reliable way to make people ill in VR. But anchoring on something rigid instead is
//   wrong once the body is visible: the body is animated relative to any rigid anchor, so
//   it slides while walking and overtakes the eye on a jump. So the eye tracks the bone
//   and the bob is removed by a low-pass with a HARD LIMIT - the filtered eye may never
//   sit further than a few centimetres from where the head really is. Small oscillations
//   are damped away; a jump or a crouch exceeds the limit at once and is followed exactly.
//   A plain low-pass cannot do both, and the clamp is what separates the two regimes.
//
#pragma once

#include <uevr/API.hpp>

namespace tasomachivr {

class Eye {
public:
    struct Settings {
        bool  stabilise{true};
        // Higher follows faster. Vertical carries nearly all of the bob, so it is damped
        // harder than horizontal.
        float bob_damping{9.0f};
        float sway_damping{22.0f};
        // Centimetres. Past this the filter gives up and tracks the head exactly.
        float sway_limit{4.0f};

        // One-euro filter on the ANCHOR. Cutoff, in hertz, is min_cutoff + beta * speed, so
        // smoothing is heavy while you are still and nearly absent while you move. A single
        // fixed amount of smoothing cannot do both: enough to kill tracking jitter is also
        // enough to leave the view dragging behind you when walking.
        float anchor_min_cutoff{0.5f};
        float anchor_beta{0.10f};

        // While airborne, freeze where the head sits INSIDE the body. The jump animation
        // curls the character up, dragging the head bone down and forward through the
        // chest; that is pose, not head movement, and following it put the camera in the
        // torso. Frozen, the view rides the capsule, which is what a jump should feel like.
        bool  airborne{false};

        // Centimetres the eye is raised while airborne, eased in and out.
        //
        // The camera is already in the right place during a jump - the anchor tracks the
        // capsule exactly and the head offset is held - so what still comes into view is the
        // ANIMATION: the character tucks, and knees and chest rise into a viewpoint that
        // never moved. Lifting the eye passes over them. It is a cosmetic correction and
        // makes no pretence otherwise; too much of it feels like floating.
        float air_lift{0.0f};
        float air_lift_speed{12.0f};
    };

    // Call once per tick with the mesh carrying the character. Does nothing useful until
    // the head bone can be read.
    void update(uevr::API::UObject* pawn, uevr::API::UObject* mesh, float delta,
                bool gameplay, const Settings& settings);

    // Writes the computed eye into the stereo view position. False if there is nothing to
    // write yet, in which case the game's own camera position is left alone.
    bool apply(float out[3]) const;

private:
    uevr::API::UObject* m_pawn{nullptr};
    float m_eye[3]{};
    // The head's position RELATIVE to the mesh component - what actually gets filtered.
    float m_off[3]{};
    // The filtered component position, plus the state the one-euro filter needs.
    float m_anchor[3]{};
    float m_prev[3]{};
    float m_rate[3]{};
    bool  m_have_anchor{false};
    bool  m_air_seen{false};
    float m_lift{0.0f};
    float m_air_drop{0.0f};
    bool  m_was_air{false};
    bool  m_have_eye{false};
    bool  m_reported{false};
};

} // namespace tasomachivr
