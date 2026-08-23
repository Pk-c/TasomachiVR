#include "reflect.hpp"

#include "ucall.hpp"

namespace tasomachivr {
namespace {

using API = uevr::API;
namespace uc = ucall;

struct Subject {
    const wchar_t* klass;
    const wchar_t* functions[14];
    const wchar_t* properties[8];
};

// A superset on purpose: a name that is absent is a fact worth having in the log, and
// guessing which spelling survived into 4.25 is exactly the mistake this avoids.
const Subject kSubjects[] = {
    {L"Class /Script/Engine.PoseableMeshComponent",
     {L"CopyPoseFromSkeletalComponent", L"SetBoneTransformByName", L"SetBoneLocationByName",
      L"SetBoneRotationByName", L"SetBoneScaleByName", L"GetBoneTransformByName", nullptr},
     {nullptr}},

    // The post-process AnimBP route: our own AnimBP, shipped in a patch pak, assigned to
    // the mesh asset and run on top of the game's own animation. The IK is the easy part;
    // these are the questions that decide whether it can be plumbed in at all.
    {L"Class /Script/Engine.SkeletalMesh",
     {nullptr},
     {L"PostProcessAnimBlueprint", L"Skeleton", L"PhysicsAsset", nullptr}},

    {L"Class /Script/Engine.SkeletalMeshComponent",
     {L"GetPostProcessInstance", L"GetAnimInstance", L"LinkAnimClassLayers",
      L"SetAnimInstanceClass", L"SetAnimClass", L"SetSkeletalMesh", L"SetAnimationMode",
      L"SetAllBodiesBelowSimulatePhysics", L"SetAllBodiesBelowPhysicsBlendWeight",
      L"AddForce", nullptr},
     {L"PostProcessAnimInstance", L"AnimClass", L"AnimScriptInstance",
      L"bDisablePostProcessBlueprint", L"PhysicsAssetOverride", nullptr}},

    // Nothing in the SDK loads an asset, so the load has to come from the engine. If
    // none of these is reachable, the only way our AnimBP gets loaded is by being
    // referenced from an asset the game already loads.
    {L"Class /Script/Engine.KismetSystemLibrary",
     {L"LoadAsset_Blocking", L"LoadClassAsset_Blocking", L"LoadAsset", L"LoadClassAsset",
      L"GetClassDisplayName", nullptr},
     {nullptr}},

    // The physics fallback, measured at the same time: which bones actually have bodies.
    {L"Class /Script/Engine.PhysicsAsset",
     {nullptr},
     {L"SkeletalBodySetups", L"ConstraintSetup", nullptr}},
};

// A TArray as it sits in a property.
struct ArrayView {
    API::UObject** data{nullptr};
    int32_t num{0};
    int32_t max{0};
};

// What the character's own mesh asset carries: whether a post-process AnimBP is already
// in use, and which bones the physics asset gives bodies to. The second answers whether
// the physics route could steer a hand at all, or only a forearm.
void walk_live_mesh(API::UObject* pawn) {
    if (pawn == nullptr) {
        API::get()->log_info("[TasomachiVR] REFLECT | no pawn, live mesh not inspected");
        return;
    }

    auto* mesh = uc::property_object(pawn, L"Mesh");
    if (mesh == nullptr) {
        mesh = uc::property_object(pawn, L"SK_Pc_01");
    }
    auto* asset = uc::property_object(mesh, L"SkeletalMesh");
    if (asset == nullptr) {
        API::get()->log_info("[TasomachiVR] REFLECT | live mesh: component=%s asset=none",
                             uc::class_name(mesh).c_str());
        return;
    }

    auto* post = uc::property_object(asset, L"PostProcessAnimBlueprint");
    auto* physics = uc::property_object(asset, L"PhysicsAsset");
    API::get()->log_info("[TasomachiVR] REFLECT | live mesh %s | asset=%s | "
                         "PostProcessAnimBlueprint=%s | PhysicsAsset=%s",
                         uc::object_name(mesh).c_str(), uc::object_name(asset).c_str(),
                         post != nullptr ? uc::object_name(post).c_str() : "none",
                         physics != nullptr ? uc::object_name(physics).c_str() : "none");

    if (physics == nullptr) {
        return;
    }

    auto* bodies = physics->get_property_data<ArrayView>(L"SkeletalBodySetups");
    if (bodies == nullptr || bodies->data == nullptr) {
        API::get()->log_info("[TasomachiVR] REFLECT | physics bodies: unreadable");
        return;
    }

    std::string names;
    for (int32_t i = 0; i < bodies->num; ++i) {
        auto* body = bodies->data[i];
        if (body == nullptr) {
            continue;
        }
        auto* bone = body->get_property_data<API::FName>(L"BoneName");
        if (!names.empty()) {
            names += " ";
        }
        names += bone != nullptr ? uc::narrow(bone->to_string()) : "?";
    }
    API::get()->log_info("[TasomachiVR] REFLECT | physics bodies (%d): %s", bodies->num,
                         names.c_str());
}

void dump_property(API::UClass* klass, const wchar_t* name) {
    auto* prop = klass->find_property(name);
    if (prop == nullptr) {
        API::get()->log_info("[TasomachiVR] REFLECT |   .%-37s ABSENT", uc::narrow(name).c_str());
        return;
    }
    std::string type{"?"};
    if (auto* field_class = prop->get_class(); field_class != nullptr) {
        type = uc::narrow(field_class->get_name());
    }
    API::get()->log_info("[TasomachiVR] REFLECT |   .%-37s @%-5d %s",
                         uc::narrow(name).c_str(), prop->get_offset(), type.c_str());
}

void dump_function(API::UClass* klass, const wchar_t* name) {
    auto* fn = klass->find_function(name);
    if (fn == nullptr) {
        API::get()->log_info("[TasomachiVR] REFLECT |   %-38s ABSENT", uc::narrow(name).c_str());
        return;
    }

    std::string params;
    for (auto* field = fn->get_child_properties(); field != nullptr; field = field->get_next()) {
        auto* prop = reinterpret_cast<API::FProperty*>(field);
        const auto param_name = field->get_fname() != nullptr
            ? uc::narrow(field->get_fname()->to_string())
            : std::string{"<unnamed>"};
        std::string type{"?"};
        if (auto* field_class = field->get_class(); field_class != nullptr) {
            type = uc::narrow(field_class->get_name());
        }
        if (!params.empty()) {
            params += ", ";
        }
        params += type + " " + param_name + "@" + std::to_string(prop->get_offset());
    }

    API::get()->log_info("[TasomachiVR] REFLECT |   %-38s blob=%-4d %s",
                         uc::narrow(name).c_str(), fn->get_properties_size(),
                         params.empty() ? "(no params)" : params.c_str());
}

} // namespace

void Reflect::run(API::UObject* pawn) {
    if (m_done) {
        return;
    }
    m_done = true;

    API::get()->log_info("[TasomachiVR] REFLECT | === what this build exposes ===");

    for (const auto& subject : kSubjects) {
        auto* klass = API::get()->find_uobject<API::UClass>(subject.klass);
        if (klass == nullptr) {
            API::get()->log_info("[TasomachiVR] REFLECT | %s : CLASS NOT FOUND",
                                 uc::narrow(subject.klass).c_str());
            continue;
        }

        API::get()->log_info("[TasomachiVR] REFLECT | %s", uc::narrow(subject.klass).c_str());
        for (const wchar_t* function : subject.functions) {
            if (function == nullptr) {
                break;
            }
            dump_function(klass, function);
        }
        for (const wchar_t* property : subject.properties) {
            if (property == nullptr) {
                break;
            }
            dump_property(klass, property);
        }
    }

    walk_live_mesh(pawn);

    API::get()->log_info("[TasomachiVR] REFLECT | === end ===");
}

} // namespace tasomachivr
