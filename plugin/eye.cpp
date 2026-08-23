#include "eye.hpp"

#include "ucall.hpp"

#include <cmath>
#include <cstring>
#include <string>

namespace tasomachivr {
namespace {

using API = uevr::API;
namespace uc = ucall;
using Vector = uc::Vec3;

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

Vector lerp(const Vector& a, const Vector& b, float t) {
    return Vector{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

float length(const Vector& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// The channel argument is an ETraceTypeQuery, which is an INDEX into the project's list of
// trace channels - not an ECollisionChannel. UCollisionProfile::ConvertToCollisionChannel
// looks it up in TraceTypeMapping, and an out-of-range index returns ECC_MAX, i.e. a trace
// that can never hit anything. The engine's two built-in trace channels come first, so
// index 0 is Visibility and index 1 is Camera. This code spent a whole session sending 2,
// which is neither: it was a silent no-op, and that is why nothing ever stopped the head.
//
// Visibility is also the right one on the merits - the Pawn and CharacterMesh collision
// profiles both ignore it, so her own capsule and body cannot block the view.
//
// Confirmed against this build rather than assumed - the engine reported:
//   WorldContextObject@0, Start@8, End@20, Radius@32, TraceChannel@36, bTraceComplex@37,
//   ActorsToIgnore@40, DrawDebugType@56, OutHit@60, bIgnoreSelf@196, ... ReturnValue@236
// which is exactly the order the positional writes below assume. So the arguments are
// right, and a trace that never reports a hit is a channel question, not a layout one.
//
// Arguments go in by POSITION, as everywhere in this mod. ActorsToIgnore is left as the
// zeroed TArray the blob already contains, which is a valid empty array and needs no
// allocation. OutHit is deliberately never read: see eye.hpp on why bisection beats
// guessing FHitResult's field offsets.
bool blocked(API::UObject* context, const Vector& start, const Vector& end, float radius,
             int channel) {
    auto* library = API::get()->find_uobject<API::UObject>(
        L"KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
    if (library == nullptr || context == nullptr) {
        return false;
    }

    uc::Call call{library, L"SphereTraceSingle"};
    if (!call.ok) {
        return false;
    }

    // Reported once. The arguments go in by position, and if that order is not what this
    // build declares then the trace quietly tests nonsense and never reports a hit - which
    // is precisely the symptom. Reading the real signature out of the engine beats assuming
    // it a second time.
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        std::string params;
        for (auto* f = call.fn->get_child_properties(); f != nullptr; f = f->get_next()) {
            auto* prop = reinterpret_cast<API::FProperty*>(f);
            const auto name = f->get_fname() != nullptr
                ? uc::narrow(f->get_fname()->to_string())
                : std::string{"?"};
            std::string type{"?"};
            if (auto* fc = f->get_class(); fc != nullptr) {
                type = uc::narrow(fc->get_name());
            }
            if (!params.empty()) {
                params += ", ";
            }
            params += type + " " + name + "@" + std::to_string(prop->get_offset());
        }
        API::get()->log_info("[TasomachiVR] EYE | SphereTraceSingle blob=%d : %s",
                             call.fn->get_properties_size(), params.c_str());
    }

    const bool ok = uc::put(call, 0, context) && uc::put(call, 1, start) &&
                    uc::put(call, 2, end) && uc::put(call, 3, radius) &&
                    uc::put(call, 4, static_cast<uint8_t>(channel)) &&
                    uc::put(call, 5, false) &&      // bTraceComplex
                    uc::put(call, 7, static_cast<uint8_t>(0)) &&   // DrawDebugType: None
                    uc::put(call, 9, true);         // bIgnoreSelf
    if (!ok) {
        return false;
    }

    library->process_event(call.fn, call.bytes.data());
    bool hit = false;
    uc::result(call, hit);
    return hit;
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

    if (!settings.collide) {
        return;
    }

    // From inside the body out to the eye. The neck is the anchor rather than the capsule
    // centre: starting at the feet would sweep through the floor on every frame.
    Vector inside{};
    if (!uc::socket_location(mesh, L"Neck", inside)) {
        return;
    }

    Vector wanted{m_eye[0], m_eye[1], m_eye[2]};
    if (length(Vector{wanted.x - inside.x, wanted.y - inside.y, wanted.z - inside.z}) < 1.0f) {
        return;
    }
    if (!blocked(pawn, inside, wanted, settings.probe_radius, settings.trace_channel)) {
        return;
    }

    // Blocked: find how far along the segment is still clear. Six halvings on a 40 cm
    // neck-to-eye span lands inside a centimetre, which is far below what anyone can
    // perceive as the view stopping short.
    float clear = 0.0f;
    float hit = 1.0f;
    for (int i = 0; i < 6; ++i) {
        const float mid = (clear + hit) * 0.5f;
        if (blocked(pawn, inside, lerp(inside, wanted, mid), settings.probe_radius,
                    settings.trace_channel)) {
            hit = mid;
        } else {
            clear = mid;
        }
    }

    const Vector held = lerp(inside, wanted, clear);
    const Vector towards{wanted.x - inside.x, wanted.y - inside.y, wanted.z - inside.z};
    const float span = length(towards);
    const float pull = span > 0.01f ? settings.wall_margin / span : 0.0f;

    // Held just short of where it was stopped, so the eye is never flush against a
    // surface and clipping into it.
    m_eye[0] = held.x - towards.x * pull;
    m_eye[1] = held.y - towards.y * pull;
    m_eye[2] = held.z - towards.z * pull;
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
