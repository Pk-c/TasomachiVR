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

// Retired, and deliberately left as a no-op rather than deleted so the reason survives.
//
// This used to resolve the mesh by path and assign the class before gameplay, to win a race
// against the engine's own InitAnim. It was not needed - the session logs from when the arms
// worked show the assignment landing on the first gameplay tick and the instance appearing
// three milliseconds later, unaided - and it did active harm twice over. It dereferenced a
// by-path pointer every frame, which became an access violation the moment a level load
// collected the asset and silently killed the rest of the pre-tick. And by calling
// ensure_class() from the menu map it loaded the Blueprint in a context where its CDO could
// not be built, permanently poisoning the cached class.
//
// The assignment now happens in ensure_assigned, during gameplay, on the asset read straight
// off the live component. That is what worked before, and it is the only place that can know
// the correct target anyway.
void AnimBp::prepare() {
    return;
#if 0
    // Latched, and this matters far more than it looks. This used to run every single
    // frame: find the asset by path, dereference it, notice the slot already held the
    // class, return. That is fine right up until a level load collects the asset, after
    // which find_uobject hands back a pointer to freed memory and the dereference is an
    // access violation - 0xC0000005, every frame, swallowed by the host. The game kept
    // running and the pre-tick did not: the pause-menu graft, the hands and the anim
    // Blueprint update all live after this line and were silently skipped for the whole
    // session.
    //
    // Nothing is lost by stopping. This pass only exists to win a race against the
    // engine's own InitAnim at startup; from then on ensure_assigned writes to the asset
    // the live component actually holds, which is both the correct target and a pointer
    // that cannot be stale because it was just read off the component.
    if (m_prepared || !ensure_class()) {
        return;
    }
    // By path, because at this point there may be no pawn to ask.
    auto* asset = API::get()->find_uobject<API::UObject>(
        L"SkeletalMesh /Game/chr/PC/SK_Pc_01.SK_Pc_01");
    if (asset == nullptr) {
        return;
    }
    m_prepared = true;
    // Best-effort, and no longer the only write: see write_slot and ensure_assigned. If
    // this one lands before the character spawns the engine's own InitAnim picks the
    // Blueprint up and nothing further is needed.
    write_slot(asset);
#endif
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

    // THE asset the component is actually using, which is not necessarily the one prepare()
    // found. prepare() resolves /Game/chr/PC/SK_Pc_01 by path and writes there once; this
    // game swaps the character's mesh for costumes, so the component can perfectly well be
    // pointing somewhere else by the time anyone looks. That is the whole bug: the write
    // landed in one USkeletalMesh and InitializeAnimScriptInstance read PostProcessAnimBlueprint
    // off another, found it null, and set PostProcessAnimInstance to null every single time.
    //
    // Re-checked whenever the asset changes rather than latched, so a costume swap is
    // covered too.
    if (asset != m_asset) {
        m_asset = asset;
        m_reinit_attempts = 0;
        m_reinit_wait = 90;
        API::get()->log_info("[TasomachiVR] ANIMBP | live asset is %s (write %s)",
                             uc::object_name(asset).c_str(),
                             write_slot(asset) ? "ok" : "FAILED");
    }

    // The assignment itself is prepare()'s job and is guarded there. What is left here is
    // making the engine notice it.
    //
    // Assigning PostProcessAnimBlueprint after the component has already run InitAnim does
    // nothing on its own: the instance is built during initialisation and never revisited.
    // So a rebuild has to be provoked, and the choice of lever matters:
    //
    //   SetSkeletalMesh(sameMesh)   early-outs, so the version of this that "issued a
    //                               re-init" for twelve attempts was calling a no-op.
    //   SetSkeletalMesh(nullptr)    defeats the early-out and is far too violent - it
    //                               strips a live character's mesh, and that is what sent
    //                               the arms flying in every direction.
    //   SetAnimationMode(other)     early-outs on the same value too, but TOGGLING it does
    //                               a genuine ClearAnimScriptInstance + re-initialise and
    //                               never touches the mesh. It is also exactly the
    //                               transition the game itself performs every time it calls
    //                               PlayAnimation for an interaction, so the character
    //                               demonstrably survives it.
    //
    // Spaced out and attempted a handful of times, because it is still a rebuild.
    if (--m_reinit_wait > 0) {
        return true;
    }
    m_reinit_wait = 90;

    if (++m_reinit_attempts > 6) {
        return true;
    }

    // Two different ways to make the engine run InitializeAnimScriptInstance(true), because
    // the first one demonstrably is not enough on this build.
    //
    // SetAllowRigidBodyAnimNode is the better lever and it is tried first. Reading 4.25:
    //
    //   if (bDisableRigidBodyAnimNode == bInAllow) {
    //       bDisableRigidBodyAnimNode = !bInAllow;
    //       if (bReinitAnim && bRegistered && SkeletalMesh) InitializeAnimScriptInstance(true);
    //   }
    //
    // It flips a flag nothing else in this game touches, never goes near the mesh, and -
    // the part that matters - it tests bRegistered itself. InitializeAnimScriptInstance
    // does nothing at all unless the component is registered, and that is the one condition
    // in the whole chain I have not been able to observe from outside. Calling it twice
    // leaves the flag exactly as it was found and re-initialises on each call.
    const char* lever = "rigidbody";
    bool away = false;
    bool back = false;

    // The one measurement that separates the two remaining explanations. Every condition
    // NeedToSpawnPostPhysicsInstance tests now reads correct, so either the engine ran the
    // initialisation and refused anyway - which the 4.25 source says is impossible - or it
    // never ran it. InitializeAnimScriptInstance always builds a BRAND NEW main instance,
    // so comparing the pointer across the call answers it outright:
    //   pointer changed  -> the engine really did re-initialise, and the refusal is real
    //   pointer the same -> the call never reached the engine, or IsRegistered() is false,
    //                       and no amount of tuning this lever will ever help
    auto* main_before = uc::property_object(mesh, L"AnimScriptInstance");

    // Attempt 3 onwards: stop asking the engine to build the instance and build it here.
    //
    // Every condition NeedToSpawnPostPhysicsInstance tests has now been measured and every
    // one of them is satisfied - the class sits at +752 exactly where the reflection probe
    // said, it differs from AnimClass, and the main instance is provably rebuilt, so
    // IsRegistered() is true and the engine really is executing the surrounding lines. It
    // still does not call NewObject. That cannot be reconciled with the 4.25 source, so it
    // is not worth any more rounds of trying to persuade it.
    //
    // spawn_object is the engine's own NewObject, and the component is the right outer. The
    // instance is then written into PostProcessAnimInstance directly, and the animation-mode
    // toggle - the one lever proven to reach the engine - runs immediately afterwards so the
    // tail of InitializeAnimScriptInstance initialises it for us:
    //
    //   if (PostProcessAnimInstance && !bInitializedPostInstance && bForceReinit)
    //       PostProcessAnimInstance->InitializeAnimation();
    //
    // This is also the last diagnostic worth running. If our instance survives that toggle,
    // the engine sees a non-null class in the slot and the arms should come back. If the
    // engine nulls it, that is proof it reads a different USkeletalMesh than the one this
    // code writes to, and the search moves to finding which.
    if (m_reinit_attempts >= 3) {
        lever = "spawned";
        if (m_spawned == nullptr) {
            m_spawned = API::get()->spawn_object(m_class, mesh);

            // A CONTROL, because "FAILED" on its own proves nothing. spawn_object returning
            // null can mean our class cannot be instantiated - or that this API simply does
            // not work for anim instances on this build, in which case the result says
            // nothing whatever about ABP_VRArms and concluding from it would be the same
            // mistake as every other one today.
            //
            // So the same call is made with the game's own anim Blueprint, which the engine
            // demonstrably instantiates several times a second:
            //   control ok, ours FAILED -> the class really is the problem, and it is a cook
            //                              issue: this exact code path worked with the pak
            //                              built before Combine Rotators was added.
            //   both FAILED             -> spawn_object is unusable here and tells us nothing.
            API::UObject* control = nullptr;
            if (auto** ac = mesh->get_property_data<API::UClass*>(L"AnimClass")) {
                if (*ac != nullptr) {
                    control = API::get()->spawn_object(*ac, mesh);
                }
            }

            // And whether the class default object is actually resident. NewObject needs it;
            // a class loaded without its CDO is exactly the shape of failure we are seeing.
            auto* cdo = API::get()->find_uobject<API::UObject>(
                L"ABP_VRArms_C /Game/TasomachiVR/ABP_VRArms.Default__ABP_VRArms_C");

            API::get()->log_info("[TasomachiVR] ANIMBP | spawn ours=%s | control(%s)=%s | "
                                 "our CDO %s",
                                 m_spawned != nullptr
                                     ? uc::class_name(m_spawned).c_str() : "FAILED",
                                 "game AnimClass",
                                 control != nullptr
                                     ? uc::class_name(control).c_str() : "FAILED",
                                 cdo != nullptr ? "is resident" : "IS MISSING");
        }
        if (m_spawned != nullptr) {
            if (auto** hold = mesh->get_property_data<API::UObject*>(
                    L"PostProcessAnimInstance")) {
                *hold = m_spawned;
            }
        }
        auto* mode2 = mesh->get_property_data<uint8_t>(L"AnimationMode");
        const uint8_t cur2 = mode2 != nullptr ? *mode2 : 0;
        away = uc::call_one(mesh, L"SetAnimationMode", (uint8_t)(cur2 == 0 ? 1 : 0));
        back = uc::call_one(mesh, L"SetAnimationMode", cur2);
    } else if (m_reinit_attempts <= 2) {
        uc::Call a{mesh, L"SetAllowRigidBodyAnimNode"};
        if (a.ok && uc::put(a, 0, false) && uc::put(a, 1, true)) {
            mesh->process_event(a.fn, a.bytes.data());
            away = true;
        }
        uc::Call b{mesh, L"SetAllowRigidBodyAnimNode"};
        if (b.ok && uc::put(b, 0, true) && uc::put(b, 1, true)) {
            mesh->process_event(b.fn, b.bytes.data());
            back = true;
        }
    } else {
        // Fallback: the animation-mode toggle. TEnumAsByte<EAnimationMode::Type> is a whole
        // byte and not a bitfield, so reading it is safe. 0 = AnimationBlueprint,
        // 1 = AnimationSingleNode, 2 = AnimationCustomMode.
        lever = "animmode";
        auto* mode = mesh->get_property_data<uint8_t>(L"AnimationMode");
        const uint8_t current_mode = mode != nullptr ? *mode : 0;
        const uint8_t other_mode = current_mode == 0 ? 1 : 0;
        away = uc::call_one(mesh, L"SetAnimationMode", other_mode);
        back = uc::call_one(mesh, L"SetAnimationMode", current_mode);
    }

    // Reported alongside the toggle because these three are exactly the conditions
    // NeedToSpawnPostPhysicsInstance tests, read straight out of the 4.25 source:
    //   ClassToUse = *SkeletalMesh->PostProcessAnimBlueprint  must be non-null
    //   MainInstanceClass = *AnimClass                        must differ from it
    // If a toggle logs slot=ok and still no instance appears, one of those is the answer
    // and there is no need to guess a third time.
    // Everything InitializeAnimScriptInstance touches, read back straight after the toggle.
    // The slot reads "ok" and no instance appears, so the remaining question is whether the
    // engine ran the initialisation at all:
    //
    //   main rebuilt, post null  -> it ran and NeedToSpawnPostPhysicsInstance said no, which
    //                               given a non-null ClassToUse can only mean AnimClass IS
    //                               our class, or the component reads a different asset
    //   main also null           -> it never ran: IsRegistered() was false, or SetAnimationMode
    //                               did not do what the 4.25 source says it does
    auto** check = asset->get_property_data<API::UClass*>(L"PostProcessAnimBlueprint");
    void* slot_addr = (void*)check;
    auto* main_inst = uc::property_object(mesh, L"AnimScriptInstance");
    auto* post_inst = uc::property_object(mesh, L"PostProcessAnimInstance");

    // The last ambiguity in the read path, and it has been there from the beginning:
    // property_object returns nullptr both when the instance really is null AND when the
    // property cannot be resolved at all. Those two mean opposite things - one says the
    // engine refused to build it, the other says I have been blind the whole time and the
    // arms may well have been created and simply never driven. The raw slot address tells
    // them apart: null = no such property.
    //
    // The offset is checked at the same time. The reflection probe reported
    // PostProcessAnimBlueprint at +752 on /Script/Engine.SkeletalMesh; if what I am writing
    // through does not sit exactly there, then reflection is resolving something else and
    // that alone explains why the engine reads a slot I am not writing to.
    auto** ppai = mesh->get_property_data<API::UObject*>(L"PostProcessAnimInstance");
    const long long slot_offset = (slot_addr != nullptr && asset != nullptr)
        ? (long long)((char*)slot_addr - (char*)asset) : -1;
    auto** anim_class = mesh->get_property_data<API::UClass*>(L"AnimClass");

    API::get()->log_info("[TasomachiVR] ANIMBP | rebuild via %s (attempt %d, calls %d/%d) "
                         "| main %p -> %p %s | asset=%s slot=%s@+%lld ppai_slot=%p "
                         "| AnimClass=%s main=%s post=%s",
                         lever, m_reinit_attempts, (int)away, (int)back,
                         (void*)main_before, (void*)main_inst,
                         main_before != main_inst ? "REBUILT" : "untouched",
                         uc::object_name(asset).c_str(),
                         (check != nullptr && *check == m_class) ? "ok"
                             : (check != nullptr && *check != nullptr) ? "other" : "EMPTY",
                         slot_offset, (void*)ppai,
                         (anim_class != nullptr && *anim_class != nullptr)
                             ? uc::narrow((*anim_class)->get_fname()->to_string()).c_str()
                             : "none",
                         main_inst != nullptr ? uc::class_name(main_inst).c_str() : "null",
                         post_inst != nullptr ? uc::class_name(post_inst).c_str() : "null");

    return true;
#if 0

    // bDisablePostProcessBlueprint would stop InitAnim building the instance at all, so
    // it is worth clearing - but ONLY through the setter.
    //
    // It is declared "uint8 bDisablePostProcessBlueprint : 1", a BITFIELD. Reading it
    // through get_property_data<bool> reads the whole byte, which holds several unrelated
    // flags, and writing a bool back wipes them. That is precisely what broke this: the
    // instance had been created reliably until a previous version of this function
    // "cleared" a flag that was never set, and clobbered its neighbours in the process.
    //
    // The SDK exposes no bit mask, so the rule is simple: never write a bitfield property
    // by hand. Use the reflected setter, and if there is none, leave it alone.
    uc::call_one(mesh, L"SetDisablePostProcessBlueprint", false);

    // Spaced out rather than every tick: this rebuilds the whole animation state.
    if (--m_reinit_wait > 0) {
        return true;
    }
    m_reinit_wait = 90;

    if (++m_reinit_attempts > 6) {
        return true;
    }

    // SetSkeletalMesh(sameMesh) EARLY-OUTS - USkinnedMeshComponent returns immediately
    // when handed the mesh it already has. So the previous version of this, which called
    // it with the current mesh and logged "re-init issued", never did anything at all.
    // The two sessions where the arms worked had simply won a race: the assignment landed
    // before the game's own InitAnim, and the engine picked it up by itself. Twelve
    // repetitions of a no-op could not change that.
    //
    // Clearing the mesh first defeats the early-out and makes the second call rebuild for
    // real. It costs one frame with no mesh, which is why it is attempted a handful of
    // times and no more.
    uc::Call clear{mesh, L"SetSkeletalMesh"};
    if (clear.ok && uc::put(clear, 0, (API::UObject*)nullptr) && uc::put(clear, 1, true)) {
        mesh->process_event(clear.fn, clear.bytes.data());
    }

    uc::Call restore{mesh, L"SetSkeletalMesh"};
    if (restore.ok && uc::put(restore, 0, asset) && uc::put(restore, 1, true)) {
        mesh->process_event(restore.fn, restore.bytes.data());
        API::get()->log_info("[TasomachiVR] ANIMBP | forced re-init %d", m_reinit_attempts);
    }

    return true;
#endif
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
        m_reinit_attempts = 0;
        // Not zero: a fresh pawn gets a second and a half before anything is rebuilt, so a
        // naturally-created instance is found first and the toggle below never fires. Only
        // if none has appeared by then is it worth provoking one.
        m_reinit_wait = 90;
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
                API::get()->log_info("[TasomachiVR] ANIMBP | instance went away - rebuilding");
                m_instance = nullptr;
                m_reinit_attempts = 0;
                m_reinit_wait = 90;
                return;
            }
            API::get()->log_info("[TasomachiVR] ANIMBP | post-process instance is %s",
                                 uc::class_name(current).c_str());
            m_instance = current;
        }
    }
    if (m_instance == nullptr) {
        return;
    }
    {
        // Nothing more to rebuild while it exists.
        m_reinit_attempts = 99;
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

    Vector left_rot = left_raw;
    Vector right_rot = right_raw;
    if (tuning.compose_order == 0) {
        compose_rotators(left_raw, left_off, left_rot);
        compose_rotators(right_raw, right_off, right_rot);
    } else {
        compose_rotators(left_off, left_raw, left_rot);
        compose_rotators(right_off, right_raw, right_rot);
    }

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
