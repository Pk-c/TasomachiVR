// TasomachiVR - driving our own post-process Animation Blueprint.
//
// Why this route, in one paragraph: nothing reflected can write a bone on a
// USkeletalMeshComponent, the game's own AnimBP has no IK node or montage slot to hook,
// UPoseableMeshComponent cannot be created because component registration is not
// reflected in 4.25, and the physics asset gives each arm exactly ONE body - LeftArm and
// RightArm, no forearm, no hand - so simulated arms could only swing rigidly from the
// shoulder. All four of those were measured, not assumed. What remains is to add our own
// animation node graph, which UE runs on top of the game's animation for free.
//
// Every link was confirmed by the reflection probe:
//   LoadClassAsset_Blocking    present, one 40-byte TSoftClassPtr argument
//   SkeletalMesh.PostProcessAnimBlueprint   present at +752, and currently empty
//   SkeletalMeshComponent.SetSkeletalMesh   present, with a bReinitPose flag
//
// The sequence is therefore: load our class by path, write it into the mesh ASSET, force
// the component to re-initialise so the engine builds a post-process instance, then write
// our variables onto that instance every frame.
//
// THE CONTRACT WITH THE BLUEPRINT lives here and nowhere else. Both sides have to agree
// on these names exactly; a typo shows up as a variable that silently never moves, so
// each one is reported the first time it is looked up.
#pragma once

#include <uevr/API.hpp>

namespace tasomachivr {

class AnimBp {
public:
    struct Targets {
        // World space throughout. The Two Bone IK nodes use world space effectors and the
        // hand transforms come out of UObjectHook already in world space, so nothing is
        // converted on either side.
        float left[3]{};
        float right[3]{};
        float left_rotation[3]{};   // FRotator: pitch, yaw, roll
        float right_rotation[3]{};
        bool  have_left{false};
        bool  have_right{false};
    };

    struct Tuning {
        float alpha{1.0f};
        float debug_tilt{0.0f};
        // 1.0 sends her hand exactly where the player's hand is, which is what VR asks
        // for: you see the virtual hand where you feel the real one.
        //
        // This started out as a PROPORTIONAL mapping - the player's full reach became
        // hers - which is defensible on paper and wrong in practice. She is roughly half
        // the player's arm length, so every gesture landed at half distance and the arms
        // read as stunted. The only thing still needed is the clamp at her real reach,
        // which keeps the elbow from locking straight at something out of range.
        float reach_scale{1.0f};
        // Constant correction applied to the controller rotation before it reaches the
        // wrist, because the hand bone's axes follow the rig's convention and not the
        // controller's. Composed by the engine, not here: FRotator composition is not
        // component-wise addition, and deriving UE's matrix convention blind is exactly
        // the mistake this project has already paid for several times.
        //
        // One per side, because the two hand bones of a Mixamo rig carry MIRRORED local
        // axes: a correction that squares up the right wrist puts the left one exactly
        // wrong. Measured, not assumed - the right wrist was correct with a single offset
        // while the left came out mirrored.
        // Measured in the headset. The mirrored axes show up exactly as predicted: the
        // same yaw both sides, opposite pitch.
        float left_hand_offset[3]{-90.0f, 90.0f, 0.0f};    // pitch, yaw, roll
        float right_hand_offset[3]{90.0f, 90.0f, 0.0f};
        // Which way round the two rotations are composed. Composed in the wrong order the
        // correction lands in WORLD space instead of the hand's own frame, so it looks
        // right facing one way and drifts as soon as the player turns - which is exactly
        // what happened. Live-switchable rather than guessed at a second time.
        // 0 = controller then offset, 1 = offset then controller.

        // How far the elbow hint sits out from the arm, along her own shoulder axis, and
        // how far below. Signed: negating the first flips which way the elbow folds.
        float elbow_out{25.0f};
        float elbow_down{10.0f};
    };

    // Call every tick while in gameplay. Does the loading and assignment once, then only
    // writes variables.
    void update(uevr::API::UObject* pawn, const Targets& targets, const Tuning& tuning);

    bool ready() const { return m_instance != nullptr; }

private:
    bool ensure_class();
    bool cdo_ready();
    void nudge_rebuild(uevr::API::UObject* mesh);
    // Assigns the class and asks the component to rebuild its animation. Called again
    // while no instance has appeared: latching after a single attempt was a real defect -
    // if the animation system was not ready at that instant, nothing ever retried.
    bool ensure_assigned(uevr::API::UObject* mesh);

    // Puts the class into one mesh asset's PostProcessAnimBlueprint, guarded, and reports
    // whether the slot ends up holding it. Takes the asset as an argument precisely because
    // there is more than one: the asset resolved by path at startup is not necessarily the
    // one the live component is using.
    bool write_slot(uevr::API::UObject* asset);
    uevr::API::UObject* find_instance(uevr::API::UObject* mesh);

    // Writes a named variable on the anim instance, reporting the first time each name is
    // resolved or missed. Templated on the payload so a float, a bool and a vector all
    // go through the same reporting.
    template <typename T>
    void write(uevr::API::UObject* instance, const wchar_t* name, const T& value);

    uevr::API::UClass*  m_class{nullptr};
    uevr::API::UObject* m_pawn{nullptr};
    uevr::API::UObject* m_mesh{nullptr};
    uevr::API::UObject* m_asset{nullptr};
    uevr::API::UObject* m_instance{nullptr};
    // Measured once per pawn, from LeftArm -> LeftForeArm -> LeftHand, so nothing about
    // her size has to be assumed.
    float m_arm_length{0.0f};
    int  m_instance_age{0};
    // Fallback rebuild: how long we have waited for an instance, and how many nudges we
    // have given the engine.
    int  m_wait{0};
    int  m_nudges{0};
    // How long we have been waiting for the CDO before giving it the benefit of the doubt.
    int  m_cdo_wait{0};
    bool m_refused{false};
    bool m_load_failed{false};
    int  m_retry{0};
};

} // namespace tasomachivr
