#include "body.hpp"

#include "ucall.hpp"

namespace tasomachivr {
namespace {

using API = uevr::API;
namespace uc = ucall;

constexpr const wchar_t* kHeadBone = L"Head"; // Mixamo naming, no prefix; "head" does not exist

// A TArray as it sits in a parameter blob.
struct ArrayView {
    API::UObject** data{nullptr};
    int32_t num{0};
    int32_t max{0};
};

bool socket_exists(API::UObject* component, const wchar_t* bone) {
    uc::Call call{component, L"DoesSocketExist"};
    if (!call.ok) {
        return false;
    }
    const API::FName name{bone};
    if (!uc::put(call, 0, name)) {
        return false;
    }
    component->process_event(call.fn, call.bytes.data());
    bool exists = false;
    uc::result(call, exists);
    return exists;
}

void hide_bone(API::UObject* mesh, const wchar_t* bone) {
    uc::Call call{mesh, L"HideBoneByName"};
    if (!call.ok) {
        return;
    }
    const API::FName name{bone};
    // Second argument is EPhysBodyOp; 0 is PBO_None, which leaves the physics asset be.
    if (uc::put(call, 0, name) && uc::put(call, 1, uint8_t{0})) {
        mesh->process_event(call.fn, call.bytes.data());
    }
}

void unhide_bone(API::UObject* mesh, const wchar_t* bone) {
    uc::Call call{mesh, L"UnHideBoneByName"};
    if (!call.ok) {
        return;
    }
    const API::FName name{bone};
    if (uc::put(call, 0, name)) {
        mesh->process_event(call.fn, call.bytes.data());
    }
}

void render_in_main_pass(API::UObject* mesh, bool render) {
    if (!uc::call_one(mesh, L"SetRenderInMainPass", render)) {
        mesh->set_bool_property(L"bRenderInMainPass", render);
    }
}

// THESE ARE BITFIELDS, NOT BYTES.
//
// bVisible, bRenderInMainPass, CastShadow and friends are one-bit members packed together,
// so get_property_data<bool> hands back the whole byte they share - which is how a readback
// reported "visible=97 mainpass=141 castshadow=249". Reading that way is merely useless;
// WRITING that way, as the fallback below used to, stamps a byte over the bitfield and
// silently flips every neighbouring flag with it.
//
// set_bool_property/get_bool_property know the mask and the offset, and are the only correct
// way to touch them.
void set_cast_hidden_shadow(API::UObject* mesh, bool cast) {
    if (!uc::call_one(mesh, L"SetCastHiddenShadow", cast)) {
        mesh->set_bool_property(L"bCastHiddenShadow", cast);
    }
}


} // namespace

API::UObject* Body::find_head_mesh(API::UObject* pawn) {
    auto* klass =
        API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SkeletalMeshComponent");
    if (pawn == nullptr || klass == nullptr) {
        return nullptr;
    }

    uc::Call call{pawn, L"K2_GetComponentsByClass"};
    if (!call.ok || !uc::put(call, 0, klass)) {
        return nullptr;
    }
    pawn->process_event(call.fn, call.bytes.data());

    ArrayView array{};
    if (!uc::result(call, array) || array.data == nullptr || array.num <= 0) {
        return nullptr;
    }

    API::UObject* found = nullptr;
    for (int32_t i = 0; i < array.num; ++i) {
        auto* component = array.data[i];
        if (component != nullptr && socket_exists(component, kHeadBone)) {
            found = component;
            break;
        }
    }
    if (found == nullptr && array.num > 0) {
        found = array.data[0];
    }

    // The engine allocated this array and nothing else will release it: we never let the
    // parameter blob be destroyed, so the free is ours to make. Same allocator, so this
    // is exactly the pairing that keeps FMallocBinned2 happy.
    if (auto* malloc = API::FMalloc::get(); malloc != nullptr) {
        malloc->free(array.data);
    }

    return found;
}

void Body::apply(API::UObject* pawn, int mode, bool gameplay) {
    // Same address-reuse trap as refresh_pawn, and worse here: m_mesh would be a freed
    // skeletal mesh component that this class writes bone visibility into every tick.
    const std::wstring id = pawn != nullptr ? pawn->get_full_name() : std::wstring{};
    if (pawn != m_pawn || id != m_pawn_id) {
        m_pawn = pawn;
        m_pawn_id = id;
        m_mesh = find_head_mesh(pawn);
        m_asset = nullptr;
        m_applied = -2;
        m_cycle_stage = 0;

        if (m_mesh != nullptr) {
            API::get()->log_info("[TasomachiVR] BODY | mesh %s on %s",
                                 uc::object_name(m_mesh).c_str(), uc::class_name(pawn).c_str());
        }
    }

    if (m_mesh == nullptr) {
        return;
    }

    // A costume change swaps the asset and rebuilds the skeleton, which quietly undoes a
    // hidden bone. Watching the asset pointer is cheaper than re-hiding every tick.
    auto* asset = uc::property_object(m_mesh, L"SkeletalMesh");
    if (asset != m_asset) {
        m_asset = asset;
        m_applied = -2;
        m_cycle_stage = 0;
    }

    const int wanted = gameplay ? mode : -1;
    if (wanted == m_applied) {
        return;
    }

    // ENTERING Headless goes the long way round: unhide the bone and drop the mesh out of
    // the main pass for one tick, then hide the bone on the next.
    //
    // This is not a theory, it is the only sequence measured to work. Applying Headless
    // directly leaves the head and the arms shivering; toggling BodyMode 1 -> 0 -> 1 by hand
    // clears it every time, and the difference between those two is exactly the step below.
    // An earlier version tied this to the animation rebuild, on the assumption that the
    // rebuild was the trigger - that assumption came from a guess about cutscenes that has
    // since been withdrawn, and the fix only ever fired by luck. Doing it on every entry
    // does not depend on knowing the trigger at all.
    //
    // The cost is one frame with the body not drawn, at the moment gameplay begins.
    if (wanted == Headless && m_cycle_stage == 0) {
        m_cycle_stage = 1;
        unhide_bone(m_mesh, kHeadBone);
        render_in_main_pass(m_mesh, false);
        API::get()->log_info("[TasomachiVR] BODY | cycling before headless");
        return;   // m_applied is left alone, so this runs again next tick
    }
    m_cycle_stage = 0;
    m_applied = wanted;

    switch (wanted) {
    case Hidden:
        unhide_bone(m_mesh, kHeadBone); // so the shadow keeps its head
        set_cast_hidden_shadow(m_mesh, true);
        render_in_main_pass(m_mesh, false);
        break;

    case Headless:
        // A HEAD THAT IS HIDDEN CASTS NO SHADOW EITHER, and that cannot be worked around
        // here. Hiding a bone collapses its geometry before the renderer knows which view it
        // is drawing, so it applies to the shadow as much as to the eye.
        //
        // A second copy of the character posed by SetMasterPoseComponent does not help: such
        // a copy is drawn with the MASTER's bone visibility rather than its own. All three
        // possible orderings were built and tested in game - copy casting, copy visible, and
        // the visibility set before the slaving rather than after - and the two meshes always
        // agreed about the head. The near clip plane was tried too and never reached UEVR's
        // own projection.
        //
        // What would work is a copy animated in its own right rather than slaved, which is a
        // much larger thing and risks a shadow standing idle while you run.
        render_in_main_pass(m_mesh, true);
        hide_bone(m_mesh, kHeadBone);
        break;

    case Whole:
    default: // cutscenes frame the character and need her whole
        render_in_main_pass(m_mesh, true);
        unhide_bone(m_mesh, kHeadBone);
        set_cast_hidden_shadow(m_mesh, false);
        break;
    }

    API::get()->log_info("[TasomachiVR] BODY | %s", wanted == Hidden ? "hidden"
                                                  : wanted == Headless ? "headless body"
                                                                       : "restored");
}

} // namespace tasomachivr
