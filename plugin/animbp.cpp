#include "animbp.hpp"

#include "ucall.hpp"

#include <cmath>
#include <cstring>
#include <set>
#include <string>

namespace tasomachivr {
namespace {

using API = uevr::API;
namespace uc = ucall;

// --- the contract with the Blueprint ---------------------------------------------
// Class path of our post-process AnimBP, with the _C suffix because it is the generated
// class we want, not the Blueprint asset.
constexpr const wchar_t* kClassPath = L"/Game/TasomachiVR/ABP_VRArms.ABP_VRArms_C";

// Variables the Blueprint must expose, spelled exactly like this.
constexpr const wchar_t* kVarEnabled   = L"VR_Enabled";
constexpr const wchar_t* kVarAlpha     = L"VR_Alpha";
constexpr const wchar_t* kVarLeft      = L"VR_LeftHandTarget";
constexpr const wchar_t* kVarRight     = L"VR_RightHandTarget";
constexpr const wchar_t* kVarLeftOn    = L"VR_LeftHandValid";
constexpr const wchar_t* kVarRightOn   = L"VR_RightHandValid";
constexpr const wchar_t* kVarDebugTilt = L"VR_DebugTilt";
constexpr const wchar_t* kVarLeftRot    = L"VR_LeftHandRotation";
constexpr const wchar_t* kVarRightRot   = L"VR_RightHandRotation";
constexpr const wchar_t* kVarLeftElbow  = L"VR_LeftElbowTarget";
constexpr const wchar_t* kVarRightElbow = L"VR_RightElbowTarget";
constexpr const wchar_t* kVarLeftOffset  = L"VR_LeftHandOffset";
constexpr const wchar_t* kVarRightOffset = L"VR_RightHandOffset";

// TSoftClassPtr is TPersistentObjectPtr<FSoftObjectPath>:
//     FWeakObjectPtr WeakPtr    @0   (two int32)
//     int32          TagAtLastTest @8
//     <4 bytes padding>
//     FSoftObjectPath ObjectID  @16  (FName AssetPathName @16, FString SubPathString @24)
// Total 40 bytes, which is exactly the argument size the probe reported for
// LoadClassAsset_Blocking (blob=48 with ReturnValue@40). The layout is derived rather
// than guessed, and a wrong derivation shows up as a null return, which is logged.
struct SoftClassPtr {
    int32_t weak_index{0};
    int32_t weak_serial{0};
    int32_t tag_at_last_test{0};
    int32_t padding{0};
    API::FName asset_path{};
    uc::EngineString sub_path{};
};
static_assert(sizeof(SoftClassPtr) == 40, "TSoftClassPtr is expected to be 40 bytes");

using Vector = uc::Vec3;

float length(const Vector& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vector subtract(const Vector& a, const Vector& b) {
    return Vector{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector add(const Vector& a, const Vector& b) {
    return Vector{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vector scale(const Vector& v, float k) {
    return Vector{v.x * k, v.y * k, v.z * k};
}

// The hand goes where the player's hand is, and only the clamp intervenes: past her
// reach the target is pulled back onto the sphere of what she can actually touch, keeping
// the direction exactly. Without the clamp the solver locks the elbow straight and the
// shoulder starts to shear.
Vector retarget(const Vector& shoulder, const Vector& raw, float arm_length,
                float reach_scale) {
    const Vector v = subtract(raw, shoulder);
    const float len = length(v);
    if (len < 0.01f || arm_length <= 0.0f) {
        return raw;
    }

    const float wanted = len * reach_scale;
    const float limit = arm_length * 0.98f;
    return add(shoulder, scale(v, (wanted < limit ? wanted : limit) / len));
}

Vector normalise(const Vector& v) {
    const float len = length(v);
    return len > 0.0001f ? scale(v, 1.0f / len) : Vector{0.0f, 1.0f, 0.0f};
}

// Where the elbow should point. A Two Bone IK with no joint target lets the solver settle
// the elbow wherever it likes, which is most of what makes an arm look broken even when
// the hand is in the right place.
//
// The hint is placed along HER OWN shoulder axis, which is the vector between the two
// shoulder bones. The first version used world axes - x minus twenty, y plus twenty-five -
// which silently assumed she always faced world +X. The moment she turned, "outwards"
// pointed somewhere arbitrary, and about half the time that was into her own chest.
// Nothing about her facing is assumed here, and nothing about her scale either.
Vector elbow_hint(const Vector& shoulder, const Vector& hand, const Vector& right_axis,
                  bool right, float out, float down) {
    const Vector mid = scale(add(shoulder, hand), 0.5f);
    const float side = right ? 1.0f : -1.0f;
    return add(add(mid, scale(right_axis, out * side)), Vector{0.0f, 0.0f, -down});
}

// ComposeRotators is a UFunction, so the engine can be asked to compose two rotators
// for us. That matters: composing FRotators is not adding their components, and deriving
// UE's rotation convention by hand is the one thing this project has repeatedly paid for.
// Doing it here rather than in the Blueprint also means the order is a setting instead of
// a graph edit.
bool compose_rotators(const Vector& a, const Vector& b, Vector& out) {
    auto* library = API::get()->find_uobject<API::UObject>(
        L"KismetMathLibrary /Script/Engine.Default__KismetMathLibrary");
    if (library == nullptr) {
        return false;
    }
    uc::Call call{library, L"ComposeRotators"};
    if (!call.ok || !uc::put(call, 0, a) || !uc::put(call, 1, b)) {
        return false;
    }
    library->process_event(call.fn, call.bytes.data());
    return uc::result(call, out);
}

std::set<std::wstring>& reported() {
    static std::set<std::wstring> names;
    return names;
}

} // namespace

// Is the class default object there? NewObject builds every instance from it, so a class
// without one is inert - the engine's NewObject returns null exactly as ours does.
//
// Bounded rather than absolute, on purpose. If this lookup string is ever wrong, an absolute
// check would refuse to load the class forever and cost us the arms for a reason that has
// nothing to do with the arms. So it delays acceptance for a couple of seconds and then
// gives up gracefully, which is no worse than the behaviour that used to work.
bool AnimBp::cdo_ready() {
    if (API::get()->find_uobject<API::UObject>(
            L"ABP_VRArms_C /Game/TasomachiVR/ABP_VRArms.Default__ABP_VRArms_C") != nullptr) {
        return true;
    }
    if (++m_cdo_wait < 120) {
        return false;
    }
    if (m_cdo_wait == 120) {
        API::get()->log_info("[TasomachiVR] ANIMBP | the CDO never turned up by name - "
                             "accepting the class anyway rather than blocking on a lookup "
                             "that may simply be spelled wrong");
    }
    return true;
}

bool AnimBp::ensure_class() {
    if (m_class != nullptr) {
        return true;
    }
    if (m_load_failed) {
        return false;
    }

    // Retried a few times rather than once: the pak is mounted early, but asking before
    // the engine is ready to load would fail for a reason that has nothing to do with
    // the path being wrong.
    if (++m_retry > 300) {
        m_load_failed = true;
        API::get()->log_error("[TasomachiVR] ANIMBP | gave up loading %s",
                              uc::narrow(kClassPath).c_str());
        return false;
    }

    // Already loaded? Cheaper than asking the engine to load it again, and it is what
    // happens on every call after the first success.
    if (auto* existing = API::get()->find_uobject<API::UClass>(
            (std::wstring{L"BlueprintGeneratedClass "} + kClassPath).c_str())) {
        // Same CDO condition as below: finding the class is not the same as it being usable.
        if (!cdo_ready()) {
            return false;
        }
        m_class = existing;
        API::get()->log_info("[TasomachiVR] ANIMBP | class already loaded (CDO present)");
        return true;
    }

    auto* library = API::get()->find_uobject<API::UObject>(
        L"KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
    if (library == nullptr) {
        return false;
    }

    uc::Call call{library, L"LoadClassAsset_Blocking"};
    if (!call.ok) {
        m_load_failed = true;
        API::get()->log_error("[TasomachiVR] ANIMBP | LoadClassAsset_Blocking not found");
        return false;
    }

    SoftClassPtr soft{};
    soft.asset_path = API::FName{kClassPath};
    if (!uc::put(call, 0, soft)) {
        m_load_failed = true;
        API::get()->log_error("[TasomachiVR] ANIMBP | could not write the soft class path");
        return false;
    }

    library->process_event(call.fn, call.bytes.data());

    API::UObject* loaded = nullptr;
    uc::result(call, loaded);
    if (loaded == nullptr) {
        // Not fatal yet: the retry above covers a pak that is mounted but not yet ready.
        return false;
    }

    // The class is not accepted until its CDO exists. NewObject builds every instance from
    // the class default object, so a class loaded WITHOUT one is inert: the engine's own
    // NewObject inside InitializeAnimScriptInstance returns null exactly as ours does, the
    // post-process instance stays null, and there are no arms.
    //
    // That is what went wrong. A previous version loaded this from the pre-tick the moment
    // the plugin woke up - on the menu map, sixteen seconds before the character existed -
    // and the CDO never got built in that context. The UClass pointer looked perfectly
    // healthy, so it was cached for the rest of the session and every later attempt was
    // doomed. Checking here turns a permanent silent failure into a retry.
    if (!cdo_ready()) {
        API::get()->log_info("[TasomachiVR] ANIMBP | loaded the class but its CDO is not "
                             "there yet - not caching it, will retry");
        return false;
    }

    m_class = reinterpret_cast<API::UClass*>(loaded);
    API::get()->log_info("[TasomachiVR] ANIMBP | loaded %s (CDO present)",
                         uc::narrow(m_class->get_full_name()).c_str());
    return true;
}

// GUARDED. An earlier version wrote unconditionally and a later read came back holding a
// MATERIAL - M_Bg_w_costume06 - so the write is conditional on the read making sense: the
// only states worth writing over are "empty" and "already ours". Anything else means the
// pointer is not the property it claims to be, and refusing beats corrupting a loaded asset.
bool AnimBp::write_slot(API::UObject* asset) {
    if (asset == nullptr || m_class == nullptr) {
        return false;
    }

    auto** slot = asset->get_property_data<API::UClass*>(L"PostProcessAnimBlueprint");
    if (slot == nullptr) {
        return false;
    }

    if (*slot == m_class) {
        return true;   // already done
    }

    if (*slot != nullptr) {
        if (!m_refused) {
            m_refused = true;
            API::get()->log_error("[TasomachiVR] ANIMBP | REFUSING to write: the slot on %s "
                                  "holds %s, which is not an AnimBP class. The property "
                                  "pointer is not what it claims - not writing.",
                                  uc::object_name(asset).c_str(),
                                  uc::narrow((*slot)->get_full_name()).c_str());
        }
        return false;
    }

    *slot = m_class;
    API::get()->log_info("[TasomachiVR] ANIMBP | assigned to %s",
                         uc::object_name(asset).c_str());
    return true;
}

bool AnimBp::ensure_assigned(API::UObject* mesh) {
    auto* asset = uc::property_object(mesh, L"SkeletalMesh");
    if (asset == nullptr || m_class == nullptr) {
        return false;
    }

    // The property lives on the mesh ASSET, not on the component, so one write covers every
    // component using it. It has to be the asset read off the LIVE component rather than one
    // resolved by path: a level load replaces the object, and writing to the stale one is
    // both useless and, once it has been collected, a crash.
    //
    // Nothing else is needed. The engine builds the instance during its own InitAnim, which
    // it runs a few milliseconds after this lands - measured at 3 ms in the first working
    // session and 4 ms in the one that fixed it. Every attempt to force that rebuild by hand
    // was chasing a failure that was really the class having no CDO, and all of it is gone.
    if (asset != m_asset) {
        m_asset = asset;
        API::get()->log_info("[TasomachiVR] ANIMBP | live asset is %s (write %s)",
                             uc::object_name(asset).c_str(),
                             write_slot(asset) ? "ok" : "FAILED");
    }
    return true;
}

// Asks the engine to rebuild its animation, because the assignment alone is a RACE.
//
// Writing the class into the mesh asset only takes effect the next time the component runs
// InitAnim, and whether that happens right after the write is pure timing. It did in the two
// sessions that worked - 3 ms and 4 ms later - and it did not in the next one, which is what
// "the arms are back" and "you just broke the arms again" actually measured.
//
// SetAnimationMode toggled away and back calls InitializeAnimScriptInstance(true), which is
// verified rather than assumed: the main anim instance comes back as a different object
// afterwards. Nothing else is touched - in particular not the mesh, which is what made the
// earlier SetSkeletalMesh(nullptr) attempt throw the arms around the room.
//
// This was deleted once as dead machinery, on the grounds that it had never been seen to
// produce an instance. It never COULD: at the time the class had no CDO, so NewObject
// returned null no matter who asked. Removing it took away the safety net the moment it
// started being able to work.
void AnimBp::nudge_rebuild(API::UObject* mesh) {
    if (--m_wait > 0 || ++m_nudges > 5) {
        return;
    }
    m_wait = 45;

    // TEnumAsByte<EAnimationMode::Type>: a whole byte, not a bitfield, so reading it is
    // safe. 0 = AnimationBlueprint, 1 = AnimationSingleNode.
    auto* mode = mesh->get_property_data<uint8_t>(L"AnimationMode");
    const uint8_t current = mode != nullptr ? *mode : 0;
    uc::call_one(mesh, L"SetAnimationMode", (uint8_t)(current == 0 ? 1 : 0));
    uc::call_one(mesh, L"SetAnimationMode", current);
    API::get()->log_info("[TasomachiVR] ANIMBP | no instance yet - asked the engine to "
                         "rebuild (%d)", m_nudges);
}

API::UObject* AnimBp::find_instance(API::UObject* mesh) {
    // The property first, since it costs nothing.
    if (auto* instance = uc::property_object(mesh, L"PostProcessAnimInstance")) {
        return instance;
    }

    uc::Call call{mesh, L"GetPostProcessInstance"};
    if (!call.ok) {
        return nullptr;
    }
    mesh->process_event(call.fn, call.bytes.data());
    API::UObject* instance = nullptr;
    uc::result(call, instance);
    return instance;
}

template <typename T>
void AnimBp::write(API::UObject* instance, const wchar_t* name, const T& value) {
    auto* slot = instance->get_property_data<T>(name);
    if (slot == nullptr) {
        if (reported().insert(name).second) {
            API::get()->log_error("[TasomachiVR] ANIMBP | variable MISSING: %s",
                                  uc::narrow(name).c_str());
        }
        return;
    }
    if (reported().insert(name).second) {
        API::get()->log_info("[TasomachiVR] ANIMBP | variable ok: %s", uc::narrow(name).c_str());
    }
    *slot = value;
}

void AnimBp::update(API::UObject* pawn, const Targets& targets, const Tuning& tuning) {
    if (pawn == nullptr) {
        return;
    }

    if (pawn != m_pawn) {
        m_pawn = pawn;
        m_mesh = uc::property_object(pawn, L"Mesh");
        if (m_mesh == nullptr) {
            m_mesh = uc::property_object(pawn, L"SK_Pc_01");
        }
        // A new pawn means a new component, so the instance has to be found again. The
        // assignment lives on the shared asset and does not.
        m_instance = nullptr;
        m_arm_length = 0.0f;
        // Half a second before the first nudge: the engine usually gets there on its own,
        // and rebuilding when it was about to anyway is churn for nothing.
        m_wait = 30;
        m_nudges = 0;
    }

    if (m_mesh == nullptr || !ensure_class() || !ensure_assigned(m_mesh)) {
        return;
    }

    // Re-checked rather than cached for good: the game calls PlayAnimation for its
    // interactions, which switches the component to single-node mode and rebuilds its
    // animation - and with it the post-process instance. Holding the old pointer is
    // exactly what made the arms freeze after touching something.
    if (++m_instance_age >= 30 || m_instance == nullptr) {
        m_instance_age = 0;
        auto* current = find_instance(m_mesh);
        if (current != m_instance) {
            if (current == nullptr) {
                // It comes back on its own: the game's next InitAnim rebuilds it, exactly
                // as it built it the first time.
                API::get()->log_info("[TasomachiVR] ANIMBP | instance went away");
                m_instance = nullptr;
                m_wait = 30;
                m_nudges = 0;
                return;
            }
            API::get()->log_info("[TasomachiVR] ANIMBP | post-process instance is %s",
                                 uc::class_name(current).c_str());
            m_instance = current;
        }
    }
    if (m_instance == nullptr) {
        nudge_rebuild(m_mesh);
        return;
    }

    if (m_arm_length <= 0.0f) {
        Vector arm{};
        Vector fore{};
        Vector hand{};
        if (uc::socket_location(m_mesh, L"LeftArm", arm) &&
            uc::socket_location(m_mesh, L"LeftForeArm", fore) &&
            uc::socket_location(m_mesh, L"LeftHand", hand)) {
            m_arm_length = length(subtract(fore, arm)) + length(subtract(hand, fore));
            API::get()->log_info("[TasomachiVR] ANIMBP | her arm reaches %.1f cm, so targets "
                                 "clamp there", m_arm_length);
        }
    }

    Vector left_shoulder{};
    Vector right_shoulder{};
    const bool have_shoulders = uc::socket_location(m_mesh, L"LeftArm", left_shoulder) &&
                                uc::socket_location(m_mesh, L"RightArm", right_shoulder);

    Vector left{targets.left[0], targets.left[1], targets.left[2]};
    Vector right{targets.right[0], targets.right[1], targets.right[2]};
    if (have_shoulders && m_arm_length > 0.0f) {
        left = retarget(left_shoulder, left, m_arm_length, tuning.reach_scale);
        right = retarget(right_shoulder, right, m_arm_length, tuning.reach_scale);
    }

    write(m_instance, kVarEnabled, true);
    write(m_instance, kVarAlpha, tuning.alpha);
    write(m_instance, kVarDebugTilt, tuning.debug_tilt);
    write(m_instance, kVarLeftOn, targets.have_left);
    write(m_instance, kVarRightOn, targets.have_right);
    write(m_instance, kVarLeft, left);
    write(m_instance, kVarRight, right);
    // Composed here, then handed over already corrected. The Blueprint still runs its
    // Combine Rotators node, but against a zero offset, so it passes this through
    // untouched - which is what lets the order be a setting rather than a graph edit.
    const Vector left_raw{targets.left_rotation[0], targets.left_rotation[1],
                          targets.left_rotation[2]};
    const Vector right_raw{targets.right_rotation[0], targets.right_rotation[1],
                           targets.right_rotation[2]};
    const Vector left_off{tuning.left_hand_offset[0], tuning.left_hand_offset[1],
                          tuning.left_hand_offset[2]};
    const Vector right_off{tuning.right_hand_offset[0], tuning.right_hand_offset[1],
                           tuning.right_hand_offset[2]};

    // ComposeRotators(A, B) returns FQuat(B) * FQuat(A) - A first, then B applied in the
    // outer frame. The wrist correction is expressed in the CONTROLLER's own frame, so it
    // has to be the inner term: FQuat(controller) * FQuat(offset), which is A=offset,
    // B=controller.
    //
    // The other way round applies the correction in world space, and then the hands turn
    // with the room instead of with your wrists - the correction looked right facing one
    // way and wrong facing another. This was a live setting for a while, which only made it
    // possible to be wrong in two directions; there is one correct order and this is it.
    Vector left_rot = left_raw;
    Vector right_rot = right_raw;
    compose_rotators(left_off, left_raw, left_rot);
    compose_rotators(right_off, right_raw, right_rot);

    write(m_instance, kVarLeftRot, left_rot);
    write(m_instance, kVarRightRot, right_rot);

    // Zero: the correction has already been applied above.
    write(m_instance, kVarLeftOffset, Vector{});
    write(m_instance, kVarRightOffset, Vector{});

    if (have_shoulders) {
        const Vector right_axis = normalise(subtract(right_shoulder, left_shoulder));
        write(m_instance, kVarLeftElbow,
              elbow_hint(left_shoulder, left, right_axis, false, tuning.elbow_out,
                         tuning.elbow_down));
        write(m_instance, kVarRightElbow,
              elbow_hint(right_shoulder, right, right_axis, true, tuning.elbow_out,
                         tuning.elbow_down));
    }
}

} // namespace tasomachivr
