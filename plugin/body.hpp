// TasomachiVR - what the player sees of their own character.
//
// This used to live in the Lua script, which cannot read the ini and so could not be
// configured. Moving it to C++ is what lets it become a row on the VR settings page.
//
// UE4 visibility is per primitive, never per bone, so a first person body is always a
// compromise:
//
//   whole mesh hidden  Nothing of the character is drawn. bCastHiddenShadow keeps it
//                      casting a complete, correct shadow. No neck seam, no clipping
//                      through the chest when you look down - and no body either.
//   headless body      HideBoneByName collapses the head bone in the skinning itself,
//                      so every pass sees the change alike. The whole hair chain
//                      (Head_001..Head_006) is parented to Head and goes with it. The
//                      cost is that the shadow loses its head too: there is no way to
//                      drop one bone from the base pass only.
//
// Either way the mesh is restored during cutscenes, which frame the character and need
// her whole.
#pragma once

#include <string>

#include <uevr/API.hpp>

namespace tasomachivr {

class Body {
public:
    enum Mode {
        Hidden = 0,
        Headless = 1,
        // Whole, head included. Used when the camera has moved far enough from the head that
        // you would be looking at a headless character rather than out of her eyes - which is
        // the only reason the head was ever hidden.
        Whole = 2,
    };

    // Call every tick, including outside gameplay - that is how the body comes back for
    // cutscenes. Does nothing while the wanted state is already applied.
    void apply(uevr::API::UObject* pawn, int mode, bool gameplay);
    // Forget what was applied, so the next apply() puts it back on.
    //
    // Needed because re-initialising the animation resets the component underneath us: the
    // hidden head bone comes back and, with it, the physics bodies - which on this character
    // are only LeftArm and RightArm. That is why forcing an anim rebuild made the head and
    // the arms shake, and why toggling BodyMode by hand fixed it.
    void invalidate() { m_applied = -2; }

private:
    // The skeletal mesh that actually carries the character: the one with a "Head" bone.
    // On foot that is CharacterMesh0, on the flying boat it is SK_Pc_01, and asking the
    // component whether the bone exists settles it without naming either.
    uevr::API::UObject* find_head_mesh(uevr::API::UObject* pawn);

    uevr::API::UObject* m_pawn{nullptr};
    // Identity by name, so an address handed back after a zone unload is not mistaken
    // for the pawn that used to live there.
    std::wstring m_pawn_id{};
    uevr::API::UObject* m_mesh{nullptr};
    // The SkeletalMesh asset currently on the component. The game swaps it for costume
    // changes, which rebuilds the skeleton and undoes a hidden bone, so a change here
    // has to re-apply.
    uevr::API::UObject* m_asset{nullptr};
    int m_applied{-2}; // -1 = restored, otherwise a Mode
    int m_cycle_stage{0};
};

} // namespace tasomachivr
