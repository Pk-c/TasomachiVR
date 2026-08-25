// TasomachiVR - the body follows the player's own steps.
//
// The counter-intuitive part, and the reason this is not three lines: moving the pawn by
// the headset's physical displacement makes the movement count TWICE. The view already
// follows the head - that is the whole point of VR - so displacing the body as well moves
// the player twice as far as they walked.
//
// The fix is to shift UEVR's standing origin by the same amount, so the headset's position
// RELATIVE TO THAT ORIGIN is unchanged. The view then stays put while the body carries it,
// which is what walking feels like. UEVR exposes get_standing_origin and
// set_standing_origin for exactly this.
//
// The second subtlety is walls. The pawn is moved with a swept move so its capsule
// collides, which means it does not always travel as far as asked. Compensating by the
// requested delta rather than the achieved one would let the player's view walk straight
// through geometry while their body stayed behind. So the achieved displacement is
// measured after the move and converted back into tracking space, and only that is
// compensated.
//
// On foot only. The flying boat is a pawn too, and displacing it would sail the boat with
// the player's footsteps.
#pragma once

#include <uevr/API.hpp>

namespace tasomachivr {

class Roomscale {
public:
    struct Settings {
        float max_step{25.0f};

        // Fraction of the remaining residual to cover THIS frame, already scaled by the
        // frame time by the caller. Closing the whole gap at once makes the body jump in
        // discrete hops, and the standing origin - which UEVR anchors the interface panel
        // to - hops with it, so the HUD twitches every time you shift your weight. An
        // exponential approach still converges, because the residual is recomputed from the
        // absolute offset every frame rather than accumulated.
        float gain{1.0f};
        // Which yaw maps tracking space onto the world.
        //
        // The first version used the VIEW yaw, which is wrong: the view yaw contains the
        // headset's own yaw, and that is part of the POSE, not part of the mapping between
        // the two spaces. The mapping is the world rotation the mod itself applies, which
        // is the snap-turn yaw alone. The mistake made the direction of a step depend on
        // where the player happened to be looking.
        //
        // It is a setting because the earlier evidence could not tell the two apart: every
        // yaw in that session sat between -5 and +7 degrees, where they are identical.
        // 0 = snap yaw (the reasoned answer), 1 = view yaw (what was there before).
        int   yaw_source{0};
        // 1 normal, -1 inverted, 0 disables the compensation entirely so the raw double
        // movement can be seen. Three settings, three distinct symptoms, one run.
        int   compensate{1};
    };

    // Call once per tick. `active` false resets the reference so that re-enabling, or
    // stepping back into gameplay, cannot produce one huge catch-up jump.
    void update(uevr::API::UObject* pawn, bool active, float snap_yaw, float view_yaw,
                const Settings& settings);

private:
    uevr::API::UObject* m_pawn{nullptr};
    bool  m_reported{false};
};

} // namespace tasomachivr
