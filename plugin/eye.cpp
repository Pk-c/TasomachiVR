#include "eye.hpp"

#include "ucall.hpp"

#include <cmath>

namespace tasomachivr {
namespace {

using API = uevr::API;
namespace uc = ucall;
using Vector = uc::Vec3;

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

void Eye::update(API::UObject* pawn, API::UObject* mesh, float delta, bool gameplay,
                 const Settings& settings) {
    if (pawn != m_pawn) {
        m_pawn = pawn;
        // The eye belongs to the pawn: boarding the boat teleports the head, and carrying
        // the old filtered position over would show as a slide into place.
        m_have_eye = false;
    }

    if (!gameplay || mesh == nullptr) {
        m_have_eye = false;
        return;
    }

    Vector head{};
    if (!uc::socket_location(mesh, L"Head", head)) {
        if (!m_reported) {
            m_reported = true;
            API::get()->log_error("[TasomachiVR] EYE | cannot read the Head bone");
        }
        m_have_eye = false;
        return;
    }

    if (!m_have_eye || !settings.stabilise) {
        m_eye[0] = head.x;
        m_eye[1] = head.y;
        m_eye[2] = head.z;
        m_have_eye = true;
    } else {
        const float dt = delta > 0.0f ? delta : 0.016f;
        const float a_xy = clampf(dt * settings.sway_damping, 0.0f, 1.0f);
        const float a_z = clampf(dt * settings.bob_damping, 0.0f, 1.0f);

        m_eye[0] += (head.x - m_eye[0]) * a_xy;
        m_eye[1] += (head.y - m_eye[1]) * a_xy;
        m_eye[2] += (head.z - m_eye[2]) * a_z;

        // The clamp is the point: it is what lets one filter both damp the walk cycle and
        // follow a jump. Smoothing hard enough to kill the bob is also enough to let the
        // body outrun the camera whenever the character really moves.
        const float limit = settings.sway_limit;
        m_eye[0] = clampf(m_eye[0], head.x - limit, head.x + limit);
        m_eye[1] = clampf(m_eye[1], head.y - limit, head.y + limit);
        m_eye[2] = clampf(m_eye[2], head.z - limit, head.z + limit);
    }

}

bool Eye::apply(float out[3]) const {
    if (!m_have_eye) {
        return false;
    }
    out[0] = m_eye[0];
    out[1] = m_eye[1];
    out[2] = m_eye[2];
    return true;
}

} // namespace tasomachivr
