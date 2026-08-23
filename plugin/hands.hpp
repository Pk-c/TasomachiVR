// TasomachiVR - where the player's hands are, in world space.
//
// The physics-driven arms need a target per hand, in UE world coordinates. Getting there
// from vr->get_pose() would mean deriving the mapping between OpenXR space and UE world
// space by hand - the same class of convention that had to be measured for the head yaw,
// and the biggest source of risk in the whole plan.
//
// UEVR already solves it. UObjectHook can attach any component to a motion controller and
// drives its transform itself, conversion included. So instead of computing anything, we
// hand it a spare component per hand and read that component's world location back. Both
// pawns carry unused ArrowComponents (Arrow1, Arrow2) which render nothing in a shipping
// build, so they cost nothing to borrow.
//
// This starts as a probe: attach, then log the two positions next to the head bone, which
// is the only way to tell whether the numbers are sane before anything is built on them.
#pragma once

#include <uevr/API.hpp>

namespace tasomachivr {

class Hands {
public:
    struct Vec3 {
        float x{};
        float y{};
        float z{};
    };

    // Where the wrist sits relative to the controller, in centimetres along the
    // CONTROLLER's own axes: forward, right, up. A controller is held in the fist, while
    // the hand bone is at the wrist, so the two are never in the same place - and the gap
    // is a constant, nothing to do with arm length.
    //
    // The axes come from the engine (GetForwardVector and friends on the component UEVR
    // drives) rather than from rebuilding a basis out of the rotator here. Same principle
    // as everywhere else in this mod: when a convention can be asked for, ask.
    void set_wrist_offset(float forward, float right, float up) {
        m_offset[0] = forward;
        m_offset[1] = right;
        m_offset[2] = up;
    }

    // Attaches the borrowed components on the first call for a pawn, then reports.
    // Returns false while nothing usable was found.
    bool update(uevr::API::UObject* pawn, bool log, float head_yaw);

    // World transforms of the two controllers, read back from the components UEVR drives.
    // Valid only while tracked() is true. The rotation matters as much as the position:
    // Two Bone IK places the hand but never orients it, so without this the hand keeps
    // whatever rotation the game's animation gave it - which reads as a dislocated wrist.
    const Vec3& left_position() const { return m_left_pos; }
    const Vec3& right_position() const { return m_right_pos; }
    const Vec3& left_rotation() const { return m_left_rot; }
    const Vec3& right_rotation() const { return m_right_rot; }
    bool tracked() const { return m_tracked; }

private:
    // Where the controller is, worked out from the VR poses rather than asked of UEVR:
    // the fallback if the UObjectHook attachment cannot be made to drive anything.
    bool computed(bool right, float head_yaw, const Vec3& head, Vec3& out, Vec3& raw) const;

    // UObjectHook needs to be enabled for the attachment to be driven at all, which is a
    // profile setting rather than something the plugin can switch on.
    bool attach(uevr::API::UObject* component, uint32_t hand);

    uevr::API::UObject* m_pawn{nullptr};
    uevr::API::UObject* m_left{nullptr};
    uevr::API::UObject* m_right{nullptr};
    uevr::API::UObject* m_mesh{nullptr};
    Vec3 m_left_pos{};
    Vec3 m_right_pos{};
    Vec3 m_left_rot{};   // FRotator: pitch, yaw, roll
    Vec3 m_right_rot{};
    float m_offset[3]{0.0f, 0.0f, 0.0f};   // forward, right, up
    bool m_tracked{false};
    bool m_attached{false};
    int  m_log_age{0};
};

} // namespace tasomachivr
