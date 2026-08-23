// TasomachiVR - where the eye sits, and what stops it entering a wall.
//
// This logic used to live in the Lua script. It moved here because the wall test has to
// happen in the same place as the eye is computed, and because C++ already owns the view
// rotation, the body, the arms and the roomscale - the Lua half had shrunk to a liability.
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
//   KEEPS OUT OF WALLS. The roomscale move sweeps the capsule, so the body cannot walk
//   through geometry - but nothing stops the player leaning their head through a
//   partition. A sphere trace from the body to the wanted eye position finds the wall and
//   the eye is held at it.
//
//   The wall distance is found by BISECTION rather than by reading the hit result:
//   FHitResult is a large struct whose field offsets would have to be guessed, and this
//   project has paid for enough guessed layouts. A blocked-or-not boolean is all the
//   engine has to tell us, and six halvings put the eye within a centimetre.
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

        bool  collide{true};
        // Radius of the sphere swept towards the eye. Roughly a head.
        float probe_radius{12.0f};
        // How far short of the wall the eye is held.
        float wall_margin{3.0f};
        // ETraceTypeQuery: 1 is Visibility, 2 is Camera. Camera by default because the
        // game's own collision profiles have the Pawn channel ignore Camera, so the trace
        // cannot be blocked by the player's own capsule.
        int   trace_channel{2};
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
