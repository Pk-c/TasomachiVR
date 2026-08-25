#include "roomscale.hpp"

#include "ucall.hpp"

#include <cmath>
#include <cstring>

namespace tasomachivr {
namespace {

using API = uevr::API;
namespace uc = ucall;

using Vector = uc::Vec3;

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
// Below this the delta is tracking noise, not a step.
// Below this the body is close enough to under the head, and chasing further would only
// jitter. It is also why the body used to tremble: a per-frame delta has no resting state,
// so tracking noise moved the character every single frame.
constexpr float kDeadzone = 1.5f;   // centimetres

// OpenXR is right-handed with +Y up and metres, UE left-handed with +Z up and centimetres.
// The same mapping as the hand targets, which was confirmed against a deliberate gesture:
// holding an arm out to the side put the movement in the raw X component.
Vector vr_to_ue(float dx, float dy, float dz) {
    (void)dy;   // vertical is deliberately dropped: crouching must not walk the body
    return Vector{-dz * 100.0f, dx * 100.0f, 0.0f};
}

Vector rotate_yaw(const Vector& v, float degrees) {
    const float r = degrees * kDegToRad;
    const float c = std::cos(r);
    const float s = std::sin(r);
    return Vector{v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

float length2d(const Vector& v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

bool actor_location(API::UObject* actor, Vector& out) {
    uc::Call call{actor, L"K2_GetActorLocation"};
    if (!call.ok) {
        return false;
    }
    actor->process_event(call.fn, call.bytes.data());
    return uc::result(call, out);
}

// K2_AddActorWorldOffset(FVector DeltaLocation, bool bSweep, FHitResult& SweepHitResult,
// bool bTeleport). The hit result is an out parameter we do not read; the blob carries it
// and the achieved movement is measured from the actor instead, which is the only thing
// that tells us how far it really got.
bool add_world_offset(API::UObject* actor, const Vector& delta) {
    uc::Call call{actor, L"K2_AddActorWorldOffset"};
    if (!call.ok) {
        return false;
    }
    if (!uc::put(call, 0, delta) || !uc::put(call, 1, true)) {
        return false;
    }
    actor->process_event(call.fn, call.bytes.data());
    return true;
}

} // namespace

void Roomscale::update(API::UObject* pawn, bool active, float snap_yaw, float view_yaw,
                       const Settings& settings) {
    const float yaw = settings.yaw_source == 1 ? view_yaw : snap_yaw;
    const float max_step = settings.max_step;
    m_pawn = pawn;
    if (!active || pawn == nullptr) {
        return;
    }

    const auto* vr = API::get()->param()->vr;
    if (vr == nullptr) {
        return;
    }

    UEVR_Vector3f head{};
    UEVR_Quaternionf ignored{};
    vr->get_pose(vr->get_hmd_index(), &head, &ignored);

    UEVR_Vector3f origin{};
    vr->get_standing_origin(&origin);

    // The RESIDUAL, not a frame delta: how far the headset currently sits from the standing
    // origin. That gap IS the distance between where the player's head is and where the
    // body is, so driving it to zero walks the body back underneath the head - however the
    // gap came to be.
    //
    // The first version integrated per-frame deltas, which cannot self-correct: every step
    // the limiter discarded, every move a wall refused, every recentre left an error that
    // stayed forever. Working from the absolute offset converges instead, which is exactly
    // what "the character should come back under my head when it can" asks for.
    const Vector residual = rotate_yaw(vr_to_ue(head.x - origin.x, 0.0f, head.z - origin.z),
                                       yaw);
    const float distance = length2d(residual);
    if (distance < kDeadzone) {
        return;
    }

    // Walked off at a bounded rate rather than in one jump, so a large gap - a recentre, a
    // tracking glitch - is closed over several frames instead of teleporting the character
    // across the room. Nothing is thrown away: whatever is not covered this frame is still
    // in the residual on the next one.
    // Eased rather than closed outright: see Settings::gain. What is not covered now stays
    // in the residual and is covered next frame.
    const float gain = settings.gain > 0.0f && settings.gain < 1.0f ? settings.gain : 1.0f;
    Vector wanted{residual.x * gain, residual.y * gain, 0.0f};
    const float distance_now = length2d(wanted);
    if (distance_now > max_step) {
        const float k = max_step / distance_now;
        wanted = Vector{wanted.x * k, wanted.y * k, 0.0f};
    }

    Vector before{};
    Vector after{};
    const bool measured = actor_location(pawn, before);
    if (!add_world_offset(pawn, wanted)) {
        if (!m_reported) {
            m_reported = true;
            API::get()->log_error("[TasomachiVR] ROOMSCALE | K2_AddActorWorldOffset not "
                                  "callable - roomscale cannot work");
        }
        return;
    }
    if (!measured || !actor_location(pawn, after)) {
        return;
    }

    // What the capsule actually managed, which is less than asked for against a wall.
    const Vector achieved{after.x - before.x, after.y - before.y, 0.0f};

    // Back into tracking space, so the origin moves by exactly what the body managed and
    // no more. Against a wall the body stops, the origin barely moves, and the residual
    // simply persists - which is the right behaviour: leaning past a wall must not drag
    // the view through it.
    if (settings.compensate == 0) {
        return;
    }
    const Vector unrotated = rotate_yaw(achieved, -yaw);
    const float achieved_dx = unrotated.y / 100.0f;
    const float achieved_dz = -unrotated.x / 100.0f;

    const float sign = settings.compensate < 0 ? -1.0f : 1.0f;
    origin.x += achieved_dx * sign;
    origin.z += achieved_dz * sign;
    vr->set_standing_origin(&origin);
}

} // namespace tasomachivr
