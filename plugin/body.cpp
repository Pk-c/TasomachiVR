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
    uc::call_one(mesh, L"SetRenderInMainPass", render);
}

// Written directly: there is no reachable setter, and it is a plain bool on the
// component. Without it, a mesh dropped from the main pass casts no shadow at all.
void set_cast_hidden_shadow(API::UObject* mesh, bool cast) {
    if (!uc::call_one(mesh, L"SetCastHiddenShadow", cast)) {
        if (auto* flag = mesh->get_property_data<bool>(L"bCastHiddenShadow")) {
            *flag = cast;
        }
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
    if (pawn != m_pawn) {
        m_pawn = pawn;
        m_mesh = find_head_mesh(pawn);
        m_asset = nullptr;
        m_applied = -2;

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
    }

    const int wanted = gameplay ? mode : -1;
    if (wanted == m_applied) {
        return;
    }
    m_applied = wanted;

    switch (wanted) {
    case Hidden:
        unhide_bone(m_mesh, kHeadBone); // so the shadow keeps its head
        set_cast_hidden_shadow(m_mesh, true);
        render_in_main_pass(m_mesh, false);
        break;

    case Headless:
        render_in_main_pass(m_mesh, true);
        hide_bone(m_mesh, kHeadBone);
        break;

    default: // cutscenes frame the character and need her whole
        render_in_main_pass(m_mesh, true);
        unhide_bone(m_mesh, kHeadBone);
        break;
    }

    API::get()->log_info("[TasomachiVR] BODY | %s", wanted == Hidden ? "hidden"
                                                  : wanted == Headless ? "headless body"
                                                                       : "restored");
}

} // namespace tasomachivr
