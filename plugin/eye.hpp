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
    bool  m_have_eye{false};
    bool  m_reported{false};
};

} // namespace tasomachivr
