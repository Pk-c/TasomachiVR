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

    // ANCHORED ON THE MESH COMPONENT, not on the world, and this is the whole trick.
    //
    // Filtering the head's world position cannot tell two very different motions apart:
    // the head bobbing inside the walk cycle, which must be damped, and the character
    // travelling, which must be followed exactly. It damped both, so walking left the view
    // pinned behind where it belonged and a jump put it inside the chest.
    //
    // The component is carried by the capsule and never animates, so all real movement is
    // in the anchor and only the animation is left in the offset.
    Vector anchor{};
    const bool anchored = uc::component_location(mesh, anchor);

    // ONE-EURO FILTER on the anchor. It has to carry real movement with no lag, which is
    // why it was briefly left unfiltered - and that let the engine's own jitter straight
    // through to the view, which is what the shimmering was. A deadzone quantised real
    // movement into steps; hysteresis removed the steps but copies raw jitter the moment it
    // decides you are moving, and nothing is ever still enough to leave that state. Neither
    // is a filter, and rejecting noise WHILE following motion is what a filter is for.
    if (anchored) {
        const float dt = delta > 0.0f ? delta : 0.016f;
        const float raw[3]{anchor.x, anchor.y, anchor.z};

        if (!m_have_anchor) {
            for (int i = 0; i < 3; ++i) {
                m_anchor[i] = m_prev[i] = raw[i];
                m_rate[i] = 0.0f;
            }
            m_have_anchor = true;
        } else {
            const auto alpha = [dt](float cutoff) {
                const float tau = 1.0f / (6.2831853f * cutoff);
                return 1.0f / (1.0f + tau / dt);
            };
            for (int i = 0; i < 3; ++i) {
                // Speed, itself smoothed at a fixed 1 Hz so one noisy frame cannot convince
                // the filter that you have started running.
                const float speed = (raw[i] - m_prev[i]) / dt;
                m_prev[i] = raw[i];
                m_rate[i] += (speed - m_rate[i]) * alpha(1.0f);

                const float cutoff = settings.anchor_min_cutoff +
                                     settings.anchor_beta * std::fabs(m_rate[i]);
                m_anchor[i] += (raw[i] - m_anchor[i]) * alpha(cutoff);
            }
        }
        anchor = Vector{m_anchor[0], m_anchor[1], m_anchor[2]};
    } else {
        m_have_anchor = false;
    }

    const Vector offset{head.x - anchor.x, head.y - anchor.y, head.z - anchor.z};

    if (settings.airborne && m_have_eye && anchored) {
        // Held at its last grounded value: the view follows the capsule and nothing else.
    } else if (!m_have_eye || !settings.stabilise || !anchored) {
        m_off[0] = offset.x;
        m_off[1] = offset.y;
        m_off[2] = offset.z;
        m_have_eye = true;
    } else {
        const float dt = delta > 0.0f ? delta : 0.016f;
        const float a_xy = clampf(dt * settings.sway_damping, 0.0f, 1.0f);
        const float a_z = clampf(dt * settings.bob_damping, 0.0f, 1.0f);

        m_off[0] += (offset.x - m_off[0]) * a_xy;
        m_off[1] += (offset.y - m_off[1]) * a_xy;
        m_off[2] += (offset.z - m_off[2]) * a_z;

        // Still clamped, but the clamp now bounds how far the view may sit from the head
        // WITHIN the body, so it can no longer be spent on travel.
        const float limit = settings.sway_limit;
        m_off[0] = clampf(m_off[0], offset.x - limit, offset.x + limit);
        m_off[1] = clampf(m_off[1], offset.y - limit, offset.y + limit);
        m_off[2] = clampf(m_off[2], offset.z - limit, offset.z + limit);
    }

    if (anchored) {
        m_eye[0] = anchor.x + m_off[0];
        m_eye[1] = anchor.y + m_off[1];
        m_eye[2] = anchor.z + m_off[2];
    } else {
        m_eye[0] = head.x;
        m_eye[1] = head.y;
        m_eye[2] = head.z;
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
