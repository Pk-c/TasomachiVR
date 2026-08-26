#include "poses.hpp"

#include "ucall.hpp"

#include <algorithm>

namespace tasomachivr {
namespace {

using API = uevr::API;
namespace uc = ucall;

// EAnimationMode::Type. AnimationBlueprint is 0 and AnimationSingleNode is 1, which is the
// order the enum has had since UE4 shipped - and the order the game's own bench code relies
// on, since it calls SetAnimationMode with a literal.
constexpr uint8_t kModeBlueprint = 0;
constexpr uint8_t kModeSingleNode = 1;

// The single-node instance's asset, read as a PROPERTY rather than through
// GetAnimationAsset - a property either exists or does not, while a function may simply not
// be exposed. This is the same reading current_anim_name() makes in the plugin, and it is
// how the bench pose is recognised at all.
API::UObject* playing_asset(API::UObject* mesh) {
    uc::Call node{mesh, L"GetAnimInstance"};
    if (!node.ok) {
        return nullptr;
    }
    mesh->process_event(node.fn, node.bytes.data());
    API::UObject* instance = nullptr;
    uc::result(node, instance);
    if (instance == nullptr) {
        return nullptr;
    }
    return uc::property_object(instance, L"CurrentAsset");
}

// AnimationMode is a TEnumAsByte, a whole byte of its own - not one of the packed bitfields
// that made a plain byte write so dangerous in body.cpp - so it reads and writes straight.
uint8_t animation_mode(API::UObject* mesh) {
    if (auto* slot = mesh->get_property_data<uint8_t>(L"AnimationMode")) {
        return *slot;
    }
    return kModeBlueprint;
}

} // namespace

void Poses::forget() {
    m_assets.clear();
    m_names.clear();
    m_index = -1;
    m_current.clear();
    m_posed = false;
    m_was_photo = false;
    m_prior_mode = kModeBlueprint;
    m_prior_asset = nullptr;
    m_prior_name.clear();
    m_mesh = nullptr;
}

// THE SWEEP. By class POINTER, not by class name, and the difference is worth a line:
// fire_button_events had to fall back to names because a Blueprint-generated class object
// is not the one find_uobject hands back. AnimSequence is native, so there is exactly one
// class object for it and every instance carries that pointer - which turns a wide string
// conversion per object into a compare per object.
void Poses::rescan(const std::string& prefix) {
    m_assets.clear();
    m_names.clear();

    auto* array = API::FUObjectArray::get();
    if (array == nullptr) {
        return;
    }
    auto* klass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.AnimSequence");

    std::vector<std::pair<std::string, API::UObject*>> found;
    const int32_t count = array->get_object_count();
    for (int32_t i = 0; i < count; ++i) {
        auto* candidate = array->get_object(i);
        if (candidate == nullptr) {
            continue;
        }
        auto* c = candidate->get_class();
        if (c == nullptr) {
            continue;
        }
        if (klass != nullptr) {
            if (c != klass) {
                continue;
            }
        } else if (uc::class_name(candidate) != "AnimSequence") {
            continue;
        }
        const auto name = uc::object_name(candidate);
        if (!prefix.empty() && name.rfind(prefix, 0) != 0) {
            continue;
        }
        // The archetype is not an animation anyone can play.
        if (name.rfind("Default__", 0) == 0) {
            continue;
        }
        found.emplace_back(name, candidate);
    }

    // Alphabetical, so the cycle is the same order every session and the button becomes
    // something you can learn rather than something you rediscover. The prefix groups them
    // usefully on its own: action_, gesture_, Idle_, Interact_, Sitting_.
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (auto& entry : found) {
        m_names.push_back(entry.first);
        m_assets.push_back(entry.second);
    }
}

// A name from the last sweep back to the object it named. The sweep is what makes this
// safe: an entry that survived it was alive a moment ago and is still referenced by the
// array we read it from.
API::UObject* Poses::find(const std::string& name) const {
    for (size_t i = 0; i < m_names.size(); ++i) {
        if (m_names[i] == name) {
            return m_assets[i];
        }
    }
    return nullptr;
}

void Poses::remember(API::UObject* mesh) {
    m_prior_mode = kModeBlueprint;
    m_prior_asset = nullptr;
    if (mesh == nullptr) {
        return;
    }
    m_prior_mode = animation_mode(mesh);
    m_prior_name.clear();
    if (m_prior_mode == kModeSingleNode) {
        m_prior_asset = playing_asset(mesh);
        // The NAME as well as the pointer, because the pointer stops being safe the moment
        // we play something else: the mesh was the only thing referencing that asset, and
        // once it lets go the collector may take it before the pose is ever put back.
        if (m_prior_asset != nullptr) {
            m_prior_name = uc::object_name(m_prior_asset);
        }
    }
}

// PlayAnimation alone, no SetAnimationMode beside it. The engine's implementation is
// SetAnimationMode(AnimationSingleNode) followed by SetAnimation and Play, so calling both
// - as the game's Blueprint does - re-initialises the instance twice for one pose.
bool Poses::apply(API::UObject* mesh, API::UObject* asset) {
    if (mesh == nullptr || asset == nullptr) {
        return false;
    }
    return uc::call_two(mesh, L"PlayAnimation", asset, true);
}

bool Poses::restore(API::UObject* mesh) {
    m_posed = false;
    m_index = -1;
    m_current.clear();
    if (mesh == nullptr) {
        return false;   // she is gone; her animation mode went with her
    }
    // Re-found by name rather than trusted: see remember(). If it has gone, the AnimBP is
    // the honest answer - a wrong pose restored from a collected pointer is a crash.
    if (m_prior_mode == kModeSingleNode && !m_prior_name.empty()) {
        if (auto* asset = find(m_prior_name)) {
            return apply(mesh, asset);
        }
    }
    return uc::call_one(mesh, L"SetAnimationMode", kModeBlueprint);
}

bool Poses::tick(API::UObject* character, bool photo_mode, unsigned requests,
                 const std::string& prefix) {
    // Derived fresh every tick, and preferred over the remembered one: after a zone change
    // the remembered pointer is a freed component that this class would otherwise call into.
    auto* mesh = character != nullptr ? uc::property_object(character, L"Mesh") : nullptr;

    bool changed = false;

    if (photo_mode && !m_was_photo) {
        rescan(prefix);
        remember(mesh);
        m_index = -1;
        m_current.clear();
        m_mesh = mesh;

        API::get()->log_info("[TasomachiVR] poses: %d loaded animation(s) match '%s'",
                             (int)m_names.size(), prefix.c_str());
        for (size_t i = 0; i < m_names.size(); ++i) {
            API::get()->log_info("[TasomachiVR] poses:   %2d %s", (int)i, m_names[i].c_str());
        }
        if (m_prior_mode == kModeSingleNode) {
            API::get()->log_info("[TasomachiVR] poses: she arrived posed - that pose is what "
                                 "the cycle returns her to");
        }
    }

    if (!photo_mode && m_was_photo) {
        // The pose belongs to photo mode. Leaving it with the character still frozen in a
        // wave is not a feature: the AnimBP is off, so she would slide through the town in
        // whatever frame the shot ended on.
        if (m_posed) {
            // Swept once more before the restore, for the same reason the cycle sweeps: the
            // pose she arrived in is looked up by name, and the list it is looked up in
            // must be one that was true a moment ago.
            if (m_prior_mode == kModeSingleNode) {
                rescan(prefix);
            }
            changed = restore(mesh != nullptr ? mesh : m_mesh);
            API::get()->log_info("[TasomachiVR] poses: photo mode closed - animation restored");
        }
        m_assets.clear();
        m_names.clear();
        m_mesh = nullptr;
    }

    m_was_photo = photo_mode;

    if (!photo_mode || requests == 0) {
        return changed;
    }
    if (mesh == nullptr) {
        return changed;
    }
    m_mesh = mesh;

    // SWEPT AGAIN, on every press rather than kept from the entry. The camera flies far
    // enough to stream a zone out from under the list, and an animation whose last
    // reference went with it is a collected object: playing that is a crash, not a wrong
    // pose. One walk of the object array per button press is a cost the event binds
    // already pay.
    rescan(prefix);
    if (m_names.empty()) {
        API::get()->log_info("[TasomachiVR] poses: nothing to cycle - no loaded animation "
                             "matches '%s'", prefix.c_str());
        return changed;
    }

    // Where the cycle stands is held by NAME, because a sweep that gained or lost an entry
    // shifts every index after it - and stepping from a stale index would jump about the
    // list rather than move one along it.
    const int size = static_cast<int>(m_names.size());
    m_index = -1;
    if (!m_current.empty()) {
        for (int i = 0; i < size; ++i) {
            if (m_names[static_cast<size_t>(i)] == m_current) {
                m_index = i;
                break;
            }
        }
    }

    // Presses are counted, not sampled, so a double tap while the game was busy moves two
    // steps rather than one. The list ends on -1: one press past the last pose gives her
    // back the animation she came in with, without leaving photo mode to get it.
    for (unsigned n = 0; n < requests; ++n) {
        m_index = m_index + 1 >= size ? -1 : m_index + 1;
    }

    if (m_index < 0) {
        changed = restore(mesh) || changed;
        API::get()->log_info("[TasomachiVR] poses: back to her own animation");
        return changed;
    }

    if (!m_posed) {
        // The first pose of a session is what turns the AnimBP off, and that is the change
        // the body has to hear about. Later ones only swap the asset.
        changed = true;
    }
    if (!apply(mesh, m_assets[m_index])) {
        API::get()->log_error("[TasomachiVR] poses: PlayAnimation failed for %s",
                              m_names[m_index].c_str());
        return changed;
    }
    m_posed = true;
    m_current = m_names[m_index];
    API::get()->log_info("[TasomachiVR] poses: %d/%d %s", m_index + 1, size, m_current.c_str());
    return changed;
}

} // namespace tasomachivr
