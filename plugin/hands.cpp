#include "hands.hpp"

#include "ucall.hpp"

#include <cmath>
#include <cstring>

namespace tasomachivr {
namespace {

using API = uevr::API;
namespace uc = ucall;

using Vector = Hands::Vec3;

// K2_GetComponentLocation is on SceneComponent and returns an FVector.
bool component_location(API::UObject* component, Vector& out) {
    uc::Call call{component, L"K2_GetComponentLocation"};
    if (!call.ok) {
        return false;
    }
    component->process_event(call.fn, call.bytes.data());
    return uc::result(call, out);
}

// K2_GetComponentRotation returns an FRotator, which UEVR has already produced in world
// space for us - the same reason the position needs no conversion.
bool component_rotation(API::UObject* component, Vector& out) {
    uc::Call call{component, L"K2_GetComponentRotation"};
    if (!call.ok) {
        return false;
    }
    component->process_event(call.fn, call.bytes.data());
    return uc::result(call, out);
}

// The component's own axes, straight from the engine. Rebuilding a basis from the
// rotator would mean re-deriving UE's rotation convention by hand, which is the one thing
// this project has learned not to do.
bool component_axis(API::UObject* component, const wchar_t* getter, Vector& out) {
    uc::Call call{component, getter};
    if (!call.ok) {
        return false;
    }
    component->process_event(call.fn, call.bytes.data());
    return uc::result(call, out);
}

// Moves a controller position onto where the wrist should be, along the controller's own
// forward/right/up.
void apply_wrist_offset(API::UObject* component, const float offset[3], Vector& position) {
    if (offset[0] == 0.0f && offset[1] == 0.0f && offset[2] == 0.0f) {
        return;
    }

    Vector forward{};
    Vector right{};
    Vector up{};
    if (!component_axis(component, L"GetForwardVector", forward) ||
        !component_axis(component, L"GetRightVector", right) ||
        !component_axis(component, L"GetUpVector", up)) {
        return;
    }

    position.x += forward.x * offset[0] + right.x * offset[1] + up.x * offset[2];
    position.y += forward.y * offset[0] + right.y * offset[1] + up.y * offset[2];
    position.z += forward.z * offset[0] + right.z * offset[1] + up.z * offset[2];
}

// GetSocketTransform(InSocketName, TransformSpace) -> FTransform. Space 0 is world.
// Only the translation is read, which sits after the rotation quaternion.
bool bone_location(API::UObject* mesh, const wchar_t* bone, Vector& out) {
    uc::Call call{mesh, L"GetSocketTransform"};
    if (!call.ok) {
        return false;
    }
    const API::FName name{bone};
    if (!uc::put(call, 0, name) || !uc::put(call, 1, uint8_t{0})) {
        return false;
    }
    mesh->process_event(call.fn, call.bytes.data());

    auto* ret = uc::return_param(call.fn);
    if (ret == nullptr) {
        return false;
    }
    // FTransform: FQuat Rotation (16 bytes), then FVector Translation.
    const int32_t offset = ret->get_offset() + 16;
    if (offset + static_cast<int32_t>(sizeof(Vector)) >
        static_cast<int32_t>(call.bytes.size())) {
        return false;
    }
    std::memcpy(&out, call.bytes.data() + offset, sizeof(Vector));
    return true;
}

// OpenXR is right-handed with +Y up and metres; UE is left-handed with +Z up and
// centimetres. This is the conventional mapping, and it is exactly the kind of thing that
// has to be measured rather than trusted - which is why the raw tracking-space delta is
// logged next to the result.
Vector vr_to_ue(const UEVR_Vector3f& v) {
    return Vector{-v.z * 100.0f, v.x * 100.0f, v.y * 100.0f};
}

// Rotates a vector about UE's up axis.
Vector rotate_yaw(const Vector& v, float degrees) {
    const float r = degrees * 3.14159265358979323846f / 180.0f;
    const float c = std::cos(r);
    const float s = std::sin(r);
    return Vector{v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}


} // namespace

bool Hands::attach(API::UObject* component, uint32_t hand) {
    if (component == nullptr) {
        return false;
    }
    auto* state = API::UObjectHook::get_or_add_motion_controller_state(component);
    if (state == nullptr) {
        return false;
    }
    state->set_hand(hand);
    // Permanent, on the second attempt. Non-permanent states were created successfully
    // (attach left=1 right=1) and never driven: the components stayed at their local
    // offsets under the pawn root, the distances to the head bone constant to the
    // centimetre. Permanence is the only other lever the API exposes, so it gets tried
    // before falling back to computing the position ourselves.
    state->set_permanent(true);
    return true;
}

// The controller relative to the headset, in UE world space: the delta is taken in
// tracking space (so the play-space origin cancels out), converted, then turned by the
// head's world yaw. Added to the head bone, that lands where the hand should be.

bool Hands::update(API::UObject* pawn, float head_yaw) {
    if (pawn == nullptr) {
        return false;
    }

    if (pawn != m_pawn) {
        m_pawn = pawn;
        m_attached = false;
        m_left = uc::property_object(pawn, L"Arrow1");
        m_right = uc::property_object(pawn, L"Arrow2");
        m_mesh = uc::property_object(pawn, L"Mesh");
        if (m_mesh == nullptr) {
            m_mesh = uc::property_object(pawn, L"SK_Pc_01");
        }

        API::get()->log_info("[TasomachiVR] HANDS | pawn %s | Arrow1=%s Arrow2=%s mesh=%s",
                             uc::class_name(pawn).c_str(), uc::class_name(m_left).c_str(),
                             uc::class_name(m_right).c_str(), uc::class_name(m_mesh).c_str());
    }

    if (m_left == nullptr || m_right == nullptr) {
        return false;
    }

    if (!m_attached) {
        const bool left_ok = attach(m_left, 0);
        const bool right_ok = attach(m_right, 1);
        m_attached = left_ok && right_ok;
        API::get()->log_info("[TasomachiVR] HANDS | attach left=%d right=%d%s", (int)left_ok,
                             (int)right_ok,
                             m_attached ? "" : "  (UObjectHook disabled in the profile?)");
        if (!m_attached) {
            return false;
        }
    }

    // Read every tick: this is the target the arms follow, not just a diagnostic.
    Vector left{};
    Vector right{};
    Vector head{};
    const bool have_left = component_location(m_left, left);
    const bool have_right = component_location(m_right, right);
    const bool have_head = m_mesh != nullptr && bone_location(m_mesh, L"Head", head);

    apply_wrist_offset(m_left, m_offset, left);
    apply_wrist_offset(m_right, m_offset, right);

    m_left_pos = left;
    m_right_pos = right;
    component_rotation(m_left, m_left_rot);
    component_rotation(m_right, m_right_rot);
    m_tracked = have_left && have_right;

    return true;
}

} // namespace tasomachivr
