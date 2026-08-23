// TasomachiVR - UEVR C++ plugin.
//
// Owns everything rotational. The Lua script could not: the rotation handed to the
// stereo callback never contains the headset, K2_SetActorRotation is not exposed to
// script, and delegating to UEVR's own aim system fights our view override and kills
// head tracking. From C++ the HMD pose is directly readable, so the world yaw can be
// derived rather than guessed.
//
// Split with the Lua script, which still runs:
//   Lua  -> view POSITION (head bone, anchoring), mesh hiding, spring arm collapse
//   C++  -> view ROTATION, snap turn, HMD yaw, body yaw, forward eye offset
// They write different fields of different structs, so callback ordering cannot make
// them collide.
//
// The game has two playable pawns and they must not be treated alike:
//
//   ThirdPersonCharacter_C  on foot. A Character: capsule, CharacterMovement, and a
//                           camera on a boom driven by ControlRotation. Here the body
//                           yaw follows the head, which is what makes it an FPS.
//   BP_pawn_Plane_C         the flying boat that opens the game. Not a Character: it
//                           steers itself with K2_SetActorRotation and orbits its own
//                           spring arm, and never reads ControlRotation. Forcing a
//                           body yaw here would either do nothing or steer the boat
//                           with the player's neck, so it is deliberately skipped -
//                           the left stick keeps steering and the head only looks.
//
// Every engine property goes through get_property_data + a null check: API.hpp warns
// that get_property dereferences blindly.

#include <uevr/Plugin.hpp>

#include "body.hpp"
#include "eye.hpp"
#include "animbp.hpp"
#include "hands.hpp"
#include "reflect.hpp"
#include "roomscale.hpp"
#include "settings.hpp"
#include "umg.hpp"
#include "vrpage.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using API = uevr::API;
using tasomachivr::AnimBp;
using tasomachivr::Body;
using tasomachivr::Eye;
using tasomachivr::Hands;
using tasomachivr::Reflect;
using tasomachivr::Roomscale;
using tasomachivr::MenuSettings;
using tasomachivr::Umg;
using tasomachivr::VrPage;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kDegToRad = kPi / 180.0f;

float normalize_deg(float deg) {
    while (deg > 180.0f) {
        deg -= 360.0f;
    }
    while (deg < -180.0f) {
        deg += 360.0f;
    }
    return deg;
}

struct Config {
    // OpenXR is right-handed with +Y up; UE is left-handed with +Z up, so a head turn
    // maps to the opposite sign of yaw. Measured on Europa, same convention here.
    float yaw_sign       = -1.0f;
    float yaw_offset     = 0.0f;
    bool  apply_body_yaw = true;
    float body_yaw_deadzone = 0.35f;   // degrees
    float menu_deadzone = 0.62f;
    int   menu_repeat_ms = 260;
    bool  write_view_rot = true;
    // Pushes the eye out of the skull. Both are hot-reloaded from the ini.
    float forward_offset = 10.0f;
    float up_offset      = 6.0f;

    // 0 = snap, 1 = smooth. Snap is the default: it is the comfortable choice for most
    // people, and smooth turning is the classic way to make someone sick in VR.
    int   turn_mode      = 0;
    float smooth_speed   = 90.0f; // degrees per second at full stick
    float turn_deadzone  = 0.2f;

    bool  snap_turn      = true;
    float snap_angle     = 45.0f;
    float snap_threshold = 0.5f;
    float snap_release   = 0.3f;
    // Seconds, not frames. A frame count turns into a much longer wait the moment the
    // framerate drops, which is exactly when snap turning already feels worst.
    float snap_cooldown  = 0.3f;

    // The game binds Pose (its pause menu) to Gamepad_Special_Right, which is XInput
    // START, and nothing on a Touch controller reaches it. Since we already rewrite
    // the XInput state, we can press START ourselves when a VR button is held.
    // 0 = off, 1 = left menu button, 2 = left stick click, 3 = right stick click.
    // Defaults to the right stick click: the menu button resolves but SteamVR keeps it
    // for its own dashboard, so a press never reaches the game.
    int   pause_button   = 3;

    // Writes the live widget tree of the game's own UI to the log. UMG is the layer
    // UEVR projects into the headset - it is why the HUD and the pause menu are visible
    // - so it is where a menu can actually live. Retried until the widgets exist,
    // because the pause menu class is only created once the player opens it.
    bool  widget_probe   = false;

    // Logs what this build actually exposes for the articulated-arms work: whether
    // UPoseableMeshComponent and its pose functions are reachable, whether a spawned
    // component can be registered, and whether the physics fallback is available.
    // One run answers all three; see reflect.hpp.
    bool  reflect_probe  = false;

    // Borrows a spare ArrowComponent per hand, asks UEVR's UObjectHook to drive it from
    // the motion controller, and reports where the two end up relative to the head bone.
    // That is the target the physics-driven arms will chase, and reading it back from
    // UEVR is what avoids deriving the OpenXR-to-world mapping by hand.
    bool  hand_probe     = false;

    // Articulated arms through our own post-process AnimBP. Off until the patch pak that
    // carries it is installed: with no class to load, this only costs a handful of failed
    // lookups before it gives up.
    bool  arms           = false;
    float arms_alpha     = 1.0f;
    // Tilts the head by this many degrees through the Blueprint. The one number that says
    // whether the whole chain - pak, load, assign, re-init, instance - is alive, without
    // depending on the IK being right.
    float arms_debug_tilt = 0.0f;
    float arms_reach_scale = 1.0f;
    // Constant correction on the wrist, in degrees. The hand bone's axes follow the rig's
    // convention, not the controller's, so some multiple of 90 is almost always needed.
    // Re-read about once a second like everything else, so it can be dialled in live in
    // the headset instead of guessed at.
    // One per side: the two hand bones carry mirrored local axes, so a correction that
    // squares up one wrist puts the other exactly wrong.
    float arms_left_hand_offset[3] = {-90.0f, 90.0f, 0.0f};    // pitch, yaw, roll
    float arms_right_hand_offset[3] = {90.0f, 90.0f, 0.0f};
    // Elbow hint, along her own shoulder axis. Negate the first to fold the elbow the
    // other way.
    float arms_elbow_out = 25.0f;
    float arms_elbow_down = 10.0f;
    // Wrist relative to the controller, in centimetres along the controller's own axes.
    float arms_wrist_offset[3] = {0.0f, 0.0f, 0.0f};   // forward, right, up
    // 0 = controller then offset, 1 = offset then controller.
    int   arms_compose_order = 0;

    // The body follows the player's own steps. On foot only - the flying boat is a pawn
    // too, and displacing it would sail the boat with the player's footsteps. See
    // roomscale.hpp for why this needs the standing origin shifted as well.
    bool  roomscale      = false;
    // Centimetres in a single frame. A tracking glitch or a recentre can produce a metre
    // at once, and teleporting the character across the room is worse than losing a step.
    float roomscale_max_step = 25.0f;
    int   roomscale_yaw_source = 0;
    int   roomscale_compensate = 1;

    // Reuse the game's own options page as the VR menu. Five of its rows are camera
    // settings that VR makes meaningless - we own the camera, so nothing reads them any
    // more - and they are already styled, already navigable, already saved by the game.
    // The slider becomes the snap angle, the X-invert checkbox the turn mode.
    // Note: the game persists these in its own settings save, so they survive without
    // TasomachiVR.ini having anything to say about it.
    bool  graft_pause_menu = false;

    // What the player sees of themselves. 0 = whole mesh hidden, 1 = body visible with
    // the head bone collapsed. See body.hpp for what each costs.
    int   body_mode      = 1;

    // The eye. This used to live in the Lua script; it moved to C++ because the wall test
    // has to happen where the eye is computed. See eye.hpp.
    bool  eye_stabilise  = true;
    float eye_bob_damping = 9.0f;
    float eye_sway_damping = 22.0f;
    float eye_sway_limit = 4.0f;
    // Holds the view at a wall instead of letting the player lean their head through a
    // partition. The roomscale move already sweeps the capsule, so the body cannot walk
    // through geometry - but nothing stopped the head.
    bool  eye_collide    = true;
    float eye_probe_radius = 12.0f;
    float eye_wall_margin = 3.0f;
    int   eye_trace_channel = 2;

    int   log_every      = 240;    // frames between diagnostic lines
};

} // namespace

class TasomachiVR final : public uevr::Plugin {
public:
    void on_initialize() override {
        load_config();
        API::get()->log_info(
            "[TasomachiVR] plugin up | yaw_sign=%.0f yaw_offset=%.1f apply_body_yaw=%d "
            "write_view_rot=%d forward=%.1f up=%.1f turn=%d snap=%.0fdeg pause=%d",
            m_config.yaw_sign, m_config.yaw_offset, (int)m_config.apply_body_yaw,
            (int)m_config.write_view_rot, m_config.forward_offset, m_config.up_offset,
            m_config.turn_mode, m_config.snap_angle, m_config.pause_button);
    }

    // Structured-exception wrapper, and the reason it exists: three modules driven from
    // this callback - the pause-menu graft, the hands and the anim Blueprint - all went
    // silent at once while everything driven from the POST tick kept working. The only
    // reading consistent with that is the callback dying partway through, every frame,
    // with nothing written down. A fault here is caught by the host and the game carries on
    // regardless, so it costs nothing to notice and everything to miss.
    //
    // The body lives in its own function because __try cannot be used in a function that
    // needs C++ object unwinding, and because m_phase then records exactly how far it got.
    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {
        __try {
            pre_tick(engine, delta);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            const unsigned long code = GetExceptionCode();
            // Disarm the module that faulted, permanently. Letting it fault again next
            // frame is what turned one dangling pointer into "nothing works": the pause
            // menu, the hands and the arms all sit after this point in the callback and
            // were skipped for an entire session without a word in the log. One broken
            // module should cost its own feature and no others.
            if (m_phase >= 0 && m_phase < kPhases) {
                m_phase_disabled[m_phase] = true;
            }
            if (m_fault_phase != m_phase || m_fault_code != code) {
                m_fault_phase = m_phase;
                m_fault_code = code;
                API::get()->log_error("[TasomachiVR] PRE-TICK FAULTED at phase %d, code 0x%08lX"
                                      " - that phase is now disabled; the rest of the tick"
                                      " keeps running", m_phase, code);
            }
        }
        if (m_phase > m_phase_max) {
            m_phase_max = m_phase;
        }
    }

    void pre_tick(API::UGameEngine*, float delta) {
        m_phase = 0;
        if (m_snap_wait > 0.0f) {
            m_snap_wait -= delta;
        }

        if (m_config.turn_mode == 1 && m_gameplay.load()) {
            const float axis = m_turn_axis.load();
            if (axis != 0.0f) {
                m_snap_yaw.store(
                    normalize_deg(m_snap_yaw.load() + axis * m_config.smooth_speed * delta));
            }
        }

        // Re-read the ini roughly once a second so the camera can be dialled in
        // without restarting the game.
        if (++m_config_age >= 60) {
            m_config_age = 0;
            reload_config_if_changed();
        }

        m_phase = 1;
        refresh_pawn();
        m_phase = 2;
        m_gameplay.store(compute_gameplay());

        // Before the gameplay gate: assigning the Blueprint early enough that the engine's
        // own InitAnim picks it up is far cheaper than rebuilding the animation afterwards.
        m_phase = 3;
        if (m_config.arms && !m_phase_disabled[3]) {
            m_animbp.prepare();
        }

        m_phase = 4;
        if (!m_phase_disabled[4]) {
            drive_vr_page();
        }
        m_phase = 5;

        // Gated on gameplay: the one-shot fired at the main menu on the engine's
        // SpectatorPawn, which has no mesh, so the live half reported nothing.
        if (m_config.reflect_probe && m_gameplay.load()) {
            m_reflect.run(m_pawn);
        }

        // The hands are tracked whenever the arms need them, and the probe only decides
        // whether it also gets logged.
        m_phase = 6;
        if ((m_config.arms || m_config.hand_probe) && m_gameplay.load()
            && !m_phase_disabled[6]) {
            m_hands.set_wrist_offset(m_config.arms_wrist_offset[0],
                                     m_config.arms_wrist_offset[1],
                                     m_config.arms_wrist_offset[2]);
            m_hands.update(m_pawn, m_config.hand_probe, m_final_yaw.load());
        }

        m_phase = 7;
        if (m_config.arms && m_gameplay.load() && !m_phase_disabled[7]) {
            AnimBp::Targets targets{};
            if (m_hands.tracked()) {
                const auto& l = m_hands.left_position();
                const auto& r = m_hands.right_position();
                const auto& lr = m_hands.left_rotation();
                const auto& rr = m_hands.right_rotation();
                targets.left[0] = l.x;  targets.left[1] = l.y;  targets.left[2] = l.z;
                targets.right[0] = r.x; targets.right[1] = r.y; targets.right[2] = r.z;
                targets.left_rotation[0] = lr.x;
                targets.left_rotation[1] = lr.y;
                targets.left_rotation[2] = lr.z;
                targets.right_rotation[0] = rr.x;
                targets.right_rotation[1] = rr.y;
                targets.right_rotation[2] = rr.z;
                targets.have_left = targets.have_right = true;
            }

            AnimBp::Tuning tuning{};
            tuning.alpha = m_config.arms_alpha;
            tuning.debug_tilt = m_config.arms_debug_tilt;
            tuning.reach_scale = m_config.arms_reach_scale;
            for (int i = 0; i < 3; ++i) {
                tuning.left_hand_offset[i] = m_config.arms_left_hand_offset[i];
                tuning.right_hand_offset[i] = m_config.arms_right_hand_offset[i];
            }
            tuning.elbow_out = m_config.arms_elbow_out;
            tuning.elbow_down = m_config.arms_elbow_down;
            tuning.compose_order = m_config.arms_compose_order;
            m_animbp.update(m_pawn, targets, tuning);
        }

        m_phase = 8;
        if (m_config.widget_probe && !m_phase_disabled[8]) {
            m_widget_time += delta;
            if (m_widget_time >= 0.5f) {
                m_widget_time = 0.0f;
                m_umg.discover();
                m_umg.probe(L"WidgetBlueprintGeneratedClass /Game/ThirdPersonBP/Blueprints/"
                            L"WBP_PauseMenu.WBP_PauseMenu_C");
                m_umg.probe(L"WidgetBlueprintGeneratedClass /Game/ThirdPersonBP/Blueprints/"
                            L"WBP_HUD.WBP_HUD_C");
            }
        }

    }

    void on_post_engine_tick(API::UGameEngine*, float delta) override {
        // Before the early return: handing the character back for a cutscene is what
        // needs to happen precisely when gameplay is false.
        m_body.apply(m_pawn, m_config.body_mode, m_gameplay.load());

        if (!m_gameplay.load()) {
            return;
        }

        update_final_yaw();

        // On foot only. The boat is not a Character and steers itself; see the file
        // header for why writing a body yaw there is wrong rather than merely useless.
        if (m_is_character) {
            apply_body_orientation();
        }

        drive_eye(delta);

        // Only on foot, and only while the character is the one being framed: the boat
        // is a pawn as well, and stepping sideways must not sail it.
        Roomscale::Settings roomscale{};
        roomscale.max_step = m_config.roomscale_max_step;
        roomscale.yaw_source = m_config.roomscale_yaw_source;
        roomscale.compensate = m_config.roomscale_compensate;
        m_roomscale.update(m_pawn, m_config.roomscale && m_is_character, m_snap_yaw.load(),
                           m_final_yaw.load(), roomscale);

        if (++m_frames >= m_config.log_every) {
            m_frames = 0;
            log_state();
        }
    }

    void on_xinput_get_state(uint32_t*, uint32_t user_index, XINPUT_STATE* state) override {
        if (state == nullptr) {
            return;
        }

        // Only pad 0 drives turning. Evaluating every index was what made the Lua
        // version of this spin on Europa: the empty pads read as centred and re-armed
        // the trigger between two real samples.
        if (m_config.snap_turn && user_index == 0 && m_gameplay.load()) {
            const float axis = state->Gamepad.sThumbRX / 32767.0f;
            const float mag = std::fabs(axis);

            if (m_config.turn_mode == 1) {
                // Smooth turning is integrated on the game thread, which is the only
                // place with a delta time. XInput is polled several times per frame, so
                // integrating here would turn faster the more often the game asks.
                m_turn_axis.store(mag < m_config.turn_deadzone ? 0.0f : axis);
            } else if (mag < m_config.snap_release) {
                m_snap_armed = true;
            } else if (m_snap_armed && m_snap_wait <= 0.0f && mag >= m_config.snap_threshold) {
                const float step = axis > 0.0f ? m_config.snap_angle : -m_config.snap_angle;
                m_snap_yaw.store(normalize_deg(m_snap_yaw.load() + step));
                m_snap_armed = false;
                m_snap_wait = m_config.snap_cooldown;
                ++m_snap_count;
            }
        }

        // The game must never see the right stick. Both pawns bind it to Turn/TurnRate
        // and LookUp/LookUpRate, so left alone it would fight the head every frame -
        // on foot through ControlRotation, on the boat through the spring arm.
        if (m_config.snap_turn) {
            state->Gamepad.sThumbRX = 0;
            state->Gamepad.sThumbRY = 0;
        }

        if (user_index == 0 && m_vrpage.is_open()) {
            filter_menu_stick(state);
        }

        if (user_index == 0 && pause_button_held()) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_START;
        }
    }

    // Slate navigates the page from the left stick, and raw it is unusable: a thumbstick
    // rests off-centre, crosses Slate's own 0.5 threshold on the way past, and repeats as
    // fast as XInput is polled - which is several times per frame, not per tick. Three
    // things fix it, and all three matter:
    //
    //   a real deadzone     so resting drift and the travel through centre emit nothing
    //   one dominant axis   so a diagonal push cannot change the row and the value at once,
    //                       which is most of what makes it feel uncontrollable
    //   a repeat interval   so holding the stick steps at a readable rate instead of
    //                       flying through the list
    //
    // Only while our own page is open: the game's menus keep the stick they were built for.
    void filter_menu_stick(XINPUT_STATE* state) {
        const float x = state->Gamepad.sThumbLX / 32767.0f;
        const float y = state->Gamepad.sThumbLY / 32767.0f;
        const float ax = std::fabs(x);
        const float ay = std::fabs(y);

        state->Gamepad.sThumbLX = 0;
        state->Gamepad.sThumbLY = 0;

        const bool horizontal = ax >= ay;
        const float mag = horizontal ? ax : ay;

        if (mag < m_config.menu_deadzone) {
            m_menu_armed = true;      // back at centre: the next push counts immediately
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!m_menu_armed) {
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - m_menu_last).count();
            if (since < m_config.menu_repeat_ms) {
                return;               // held: swallow until the interval has passed
            }
        }
        m_menu_armed = false;
        m_menu_last = now;

        // Emitted at full deflection so Slate is never left in the band where it has to
        // decide for itself how fast to repeat.
        const int16_t full = 32767;
        if (horizontal) {
            state->Gamepad.sThumbLX = x > 0.0f ? full : -full;
        } else {
            state->Gamepad.sThumbLY = y > 0.0f ? full : -full;
        }
    }

    void on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int, float,
                                             UEVR_Vector3f* position,
                                             UEVR_Rotatorf* rotation, bool) override {
        if (!m_gameplay.load()) {
            return;
        }

        // Position and rotation are written in the same place now. The Lua script used to
        // own the position, which meant two callbacks racing to write the same struct.
        if (position != nullptr) {
            float eye[3]{};
            if (m_eye.apply(eye)) {
                position->x = eye[0];
                position->y = eye[1];
                position->z = eye[2];
            }
        }

        if (rotation == nullptr || !m_config.write_view_rot) {
            return;
        }

        // Discard the game's camera orientation wholesale. Keeping it as a base would
        // mean the headset only ever added to whatever the boom had chosen that frame.
        // UEVR lays the HMD rotation on top of this.
        rotation->pitch = 0.0f;
        rotation->yaw = m_snap_yaw.load();
        rotation->roll = 0.0f;
    }

    void on_post_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int, float,
                                              UEVR_Vector3f* position, UEVR_Rotatorf*,
                                              bool) override {
        if (position == nullptr || !m_gameplay.load()) {
            return;
        }

        // Horizontal push out of the skull, along the head yaw. This follows yaw only,
        // deliberately: UEVR's own camera offset follows the full view direction, and
        // having one of the two stay level is what keeps looking down from sliding the
        // eye into the chest.
        const float r = m_final_yaw.load() * kDegToRad;
        position->x += std::cos(r) * m_config.forward_offset;
        position->y += std::sin(r) * m_config.forward_offset;
        position->z += m_config.up_offset;
    }

private:
    void drive_eye(float delta) {
        Eye::Settings settings{};
        settings.stabilise = m_config.eye_stabilise;
        settings.bob_damping = m_config.eye_bob_damping;
        settings.sway_damping = m_config.eye_sway_damping;
        settings.sway_limit = m_config.eye_sway_limit;
        settings.collide = m_config.eye_collide;
        settings.probe_radius = m_config.eye_probe_radius;
        settings.wall_margin = m_config.eye_wall_margin;
        settings.trace_channel = m_config.eye_trace_channel;

        auto* mesh = deref_object(m_pawn, L"Mesh");
        if (mesh == nullptr) {
            mesh = deref_object(m_pawn, L"SK_Pc_01");
        }
        m_eye.update(m_pawn, mesh, delta, m_gameplay.load(), settings);
    }

    // Copies the page-editable settings out of the live config, lets the page work on
    // them, and copies back. Going through a separate struct keeps the loader-side
    // settings out of reach of the page by construction.
    void drive_vr_page() {
        if (!m_config.graft_pause_menu) {
            return;
        }

        MenuSettings s{};
        s.turn_mode      = m_config.turn_mode;
        s.snap_angle     = m_config.snap_angle;
        s.snap_cooldown  = m_config.snap_cooldown;
        s.smooth_speed   = m_config.smooth_speed;
        s.turn_deadzone  = m_config.turn_deadzone;
        s.forward_offset = m_config.forward_offset;
        s.up_offset      = m_config.up_offset;
        s.yaw_offset     = m_config.yaw_offset;
        s.pause_button   = m_config.pause_button;
        s.body_mode      = m_config.body_mode;

        // The page only writes while it is open, so the ini still governs the rest of
        // the time.
        m_vrpage.update(API::get()->get_local_pawn(0), s);

        m_config.turn_mode      = s.turn_mode;
        m_config.snap_angle     = s.snap_angle;
        m_config.smooth_speed   = s.smooth_speed;
        m_config.forward_offset = s.forward_offset;
        m_config.up_offset      = s.up_offset;
        m_config.yaw_offset     = s.yaw_offset;
        m_config.body_mode      = s.body_mode;
    }

    static API::UObject* deref_object(API::UObject* owner, const wchar_t* name) {
        if (owner == nullptr) {
            return nullptr;
        }
        auto** slot = owner->get_property_data<API::UObject*>(name);
        return slot != nullptr ? *slot : nullptr;
    }

    // Property lookups go by name and walk the class chain, so everything derived from
    // the pawn identity is resolved once per pawn rather than sixty times a second.
    // The game swaps pawn when boarding or leaving the boat, which is the only time
    // this needs to run again.
    void refresh_pawn() {
        auto* pawn = API::get()->get_local_pawn(0);
        if (pawn == m_pawn) {
            return;
        }

        m_pawn = pawn;
        m_cmc = nullptr;
        m_is_character = false;
        m_pawn_playable = false;
        m_pawn_name.clear();

        if (pawn == nullptr) {
            return;
        }

        // A Character has a CharacterMovementComponent; the boat has a
        // ProjectileMovementComponent and no ControlRotation handling at all. That
        // distinction is the whole reason this function exists, so it is what gets
        // tested rather than the class name - a name match would silently stop working
        // on any pawn the game adds later.
        m_cmc = deref_object(pawn, L"CharacterMovement");
        m_is_character = m_cmc != nullptr;

        if (auto* klass = pawn->get_class(); klass != nullptr) {
            m_pawn_name = klass->get_full_name();
        }

        // On the menu maps the engine hands us its fallback ASpectatorPawn, which has
        // no body and no head bone - and taking over the view rotation there broke the
        // menu camera for no gain. Every pawn this game actually plays is a Blueprint
        // living under /Game/, while the spectator is "Class /Script/Engine.
        // SpectatorPawn", so that is the line: pawns made of game content, not engine
        // fallbacks. Measured rather than assumed - both names came out of the log.
        m_pawn_playable = m_pawn_name.find(L"/Game/") != std::wstring::npos;

        m_flag_age = 0; // re-assert the movement flags immediately on a new pawn
        // A new pawn faces wherever it spawned; carrying the old held yaw over would fight
        // it for one deadzone's worth before catching up.
        m_have_applied_yaw = false;

        API::get()->log_info("[TasomachiVR] pawn changed: %s | character=%d playable=%d",
                             narrow(m_pawn_name).c_str(), (int)m_is_character,
                             (int)m_pawn_playable);
    }

    // Gameplay means the camera is framing the pawn we control. On foot and on the
    // boat alike the view target IS the possessed pawn, so a pointer compare covers
    // both without naming either. Everything else - the CineCameraActor a
    // LevelSequence spawns for a cutscene, PhotoMode_Camera, the menu maps - fails it,
    // and we hand the shot straight back to the game with only UEVR's stereo on top.
    bool compute_gameplay() {
        if (m_pawn == nullptr || !m_pawn_playable) {
            return false;
        }

        auto* pc = API::get()->get_player_controller(0);
        if (pc == nullptr) {
            return false;
        }

        auto* pcm = deref_object(pc, L"PlayerCameraManager");
        if (pcm == nullptr) {
            return false;
        }

        // FTViewTarget begins with AActor* Target.
        auto* view_target = pcm->get_property_data<API::UObject*>(L"ViewTarget");
        auto* target = view_target != nullptr ? *view_target : nullptr;

        const bool gameplay = target == m_pawn;
        if (gameplay != m_was_gameplay) {
            m_was_gameplay = gameplay;
            std::wstring name;
            if (target != nullptr) {
                if (auto* klass = target->get_class(); klass != nullptr) {
                    name = klass->get_full_name();
                }
            }
            API::get()->log_info("[TasomachiVR] gameplay=%d | view target=%s", (int)gameplay,
                                 target == nullptr ? "<none>" : narrow(name).c_str());
        }
        return gameplay;
    }

    // True while the chosen VR button is held. Action paths come from UEVR's own
    // manifest; the handle is resolved once, since a bad path would otherwise be
    // retried on every poll and stay invisible.
    bool pause_button_held() {
        if (m_config.pause_button == 0) {
            return false;
        }

        const auto* vr = API::get()->param()->vr;
        if (vr == nullptr) {
            return false;
        }

        // Stick click rather than a grip: grips are held constantly while playing, so
        // pausing would fire by accident. Both pawns bind L3 to something (RunPad on
        // foot), so the right stick click is the free one - and we already zero the
        // right stick, so nothing else wants it.
        const char* path = nullptr;
        bool left = true;
        switch (m_config.pause_button) {
        case 1: path = "/actions/default/in/SystemButton";   left = true;  break;
        case 2: path = "/actions/default/in/JoystickClick";  left = true;  break;
        case 3: path = "/actions/default/in/JoystickClick";  left = false; break;
        default: return false;
        }

        if (!m_pause_resolved) {
            m_pause_resolved = true;
            m_pause_action = vr->get_action_handle(path);
            m_pause_ok = m_pause_action != nullptr;
            if (!m_pause_ok) {
                API::get()->log_info("[TasomachiVR] pause action %s did not resolve", path);
            }
        }
        if (!m_pause_ok) {
            return false;
        }

        const auto source = left ? vr->get_left_joystick_source() : vr->get_right_joystick_source();
        const bool held = vr->is_action_active(m_pause_action, source);
        if (held) {
            m_pause_seen = true;
        }
        return held;
    }

    // Yaw about the OpenXR up axis (Y).
    static float quat_yaw(const UEVR_Quaternionf& q) {
        const float siny = 2.0f * (q.w * q.y + q.x * q.z);
        const float cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
        return std::atan2(siny, cosy) * kRadToDeg;
    }

    float hmd_quat_yaw() {
        const auto* vr = API::get()->param()->vr;
        if (vr == nullptr) {
            return 0.0f;
        }

        UEVR_Vector3f pos{};
        UEVR_Quaternionf q{};
        vr->get_pose(vr->get_hmd_index(), &pos, &q);
        return quat_yaw(q);
    }

    void update_final_yaw() {
        m_quat_yaw.store(hmd_quat_yaw());
        const float head = m_quat_yaw.load() * m_config.yaw_sign + m_snap_yaw.load();
        m_final_yaw.store(normalize_deg(head + m_config.yaw_offset));
    }

    // Makes the body behave like an FPS: face where the player is looking, and strafe
    // or walk backwards rather than pivoting to face the direction of travel. Runs
    // after the game's tick, because applying it before means the Blueprint's own
    // camera handling (CamReset, SysCamAutoAdjust) overwrites it the same frame.
    void apply_body_orientation() {
        if (!m_config.apply_body_yaw) {
            return;
        }

        auto* pc = API::get()->get_player_controller(0);
        if (pc != m_pc) {
            m_pc = pc;
            m_control_rotation = pc != nullptr
                ? pc->get_property_data<UEVR_Rotatorf>(L"ControlRotation")
                : nullptr;
            m_control_ok = m_control_rotation != nullptr;
        }

        if (m_control_rotation != nullptr) {
            // A deadzone, because bUseControllerRotationYaw snaps the whole pawn onto this
            // value every single tick. A headset is never perfectly still - it reports a
            // fraction of a degree of noise even on a tripod - and feeding that straight in
            // rotates the body every frame, which is what the trembling was. The camera sits
            // on the Head bone of that body, so it inherited the shake rather than causing it.
            //
            // Same shape of fix as the roomscale deadzone: a continuous noisy signal driving
            // a rigid body needs a resting state. Holding the last value below the threshold
            // gives it one. Above the threshold it tracks continuously, so a deliberate turn
            // is never stepped - the worst-case lag is the deadzone itself, well under what
            // anyone can see.
            const float wanted = m_final_yaw.load();
            if (!m_have_applied_yaw ||
                std::fabs(normalize_deg(wanted - m_applied_yaw)) > m_config.body_yaw_deadzone) {
                m_applied_yaw = wanted;
                m_have_applied_yaw = true;
            }

            // Pitch stays flat: tipping the character because the player looked up is
            // exactly what breaks VR comfort. It also keeps the template's
            // camera-relative MoveForward from walking into the ground.
            m_control_rotation->pitch = 0.0f;
            m_control_rotation->yaw = m_applied_yaw;
            m_control_rotation->roll = 0.0f;
        }

        if (m_pawn == nullptr) {
            return;
        }

        // These are mode flags, not per-frame state. Six times a second is responsive
        // enough, and set_bool_property has to find the property by name every call.
        if (--m_flag_age > 0) {
            return;
        }
        m_flag_age = 10;

        m_pawn->set_bool_property(L"bUseControllerRotationYaw", true);

        if (m_cmc != nullptr) {
            m_cmc->set_bool_property(L"bOrientRotationToMovement", false);
            // Deliberately FALSE, and it used to be true. The two flags do the same job by
            // different means and they were fighting: bUseControllerRotationYaw snaps the
            // pawn to the control rotation every tick, while bUseControllerDesiredRotation
            // makes the movement component interpolate towards it at RotationRate. With
            // both on, the interpolation won and the body trailed the head - which is
            // exactly the lag felt when turning on the spot.
            m_cmc->set_bool_property(L"bUseControllerDesiredRotation", false);
        }
    }

    static std::string narrow(const std::wstring& w) {
        if (w.empty()) {
            return {};
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) {
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
        }
        return out;
    }

    void log_state() {
        API::get()->log_info(
            "[TasomachiVR] quat_yaw=%.1f snap_yaw=%.1f final_yaw=%.1f snaps=%d | "
            "character=%d playable=%d control=%d cmc=%d pause=%d/%d | phase=%d/%d | pawn=%s",
            m_quat_yaw.load(), m_snap_yaw.load(), m_final_yaw.load(), m_snap_count,
            (int)m_is_character, (int)m_pawn_playable, (int)m_control_ok,
            (int)(m_cmc != nullptr), (int)m_pause_ok, (int)m_pause_seen,
            m_phase, m_phase_max, narrow(m_pawn_name).c_str());
    }

    // One settings file, in the game folder, next to everything else the player
    // unpacked. The plugin lives in the UEVR profile but that is an implementation
    // detail; nobody should have to know it to change a number.
    static std::filesystem::path settings_path() {
        wchar_t buf[MAX_PATH * 2]{};
        if (GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf))) == 0) {
            return {};
        }
        return std::filesystem::path{buf}.parent_path() / L"TasomachiVR" / L"TasomachiVR.ini";
    }

    // Reloads the ini and reports only when something actually moved, so tuning the
    // camera from the file shows up in the log without spamming it.
    void reload_config_if_changed() {
        // Only reopen the file when it has actually been written to. Parsing it once a
        // second regardless is a pointless disk hit for a value that changes twice in a
        // session, if at all.
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(settings_path(), ec);
        if (ec || stamp == m_config_stamp) {
            return;
        }
        m_config_stamp = stamp;

        const Config before = m_config;
        load_config();

        if (before.forward_offset != m_config.forward_offset ||
            before.up_offset != m_config.up_offset ||
            before.yaw_sign != m_config.yaw_sign ||
            before.yaw_offset != m_config.yaw_offset ||
            before.snap_angle != m_config.snap_angle ||
            before.turn_mode != m_config.turn_mode ||
            before.smooth_speed != m_config.smooth_speed) {
            API::get()->log_info(
                "[TasomachiVR] config reloaded | forward=%.1f up=%.1f yaw_sign=%.0f "
                "yaw_offset=%.1f turn=%d snap_angle=%.0f",
                m_config.forward_offset, m_config.up_offset, m_config.yaw_sign,
                m_config.yaw_offset, m_config.turn_mode, m_config.snap_angle);
        }
    }

    void load_config() {
        std::ifstream in{settings_path()};
        if (!in) {
            return;
        }

        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos || line.empty() || line[0] == ';') {
                continue;
            }
            const auto key = line.substr(0, eq);
            const auto value = line.substr(eq + 1);

            if (key == "YawSign")            m_config.yaw_sign = (float)std::atof(value.c_str());
            else if (key == "YawOffset")     m_config.yaw_offset = (float)std::atof(value.c_str());
            else if (key == "ApplyBodyYaw")  m_config.apply_body_yaw = std::atoi(value.c_str()) != 0;
            else if (key == "BodyYawDeadzone") m_config.body_yaw_deadzone = (float)std::atof(value.c_str());
            else if (key == "MenuDeadzone")  m_config.menu_deadzone = (float)std::atof(value.c_str());
            else if (key == "MenuRepeatMs")  m_config.menu_repeat_ms = std::atoi(value.c_str());
            else if (key == "WriteViewRot")  m_config.write_view_rot = std::atoi(value.c_str()) != 0;
            else if (key == "ForwardOffset") m_config.forward_offset = (float)std::atof(value.c_str());
            else if (key == "UpOffset")      m_config.up_offset = (float)std::atof(value.c_str());
            else if (key == "SnapTurn")      m_config.snap_turn = std::atoi(value.c_str()) != 0;
            else if (key == "SnapAngle")     m_config.snap_angle = (float)std::atof(value.c_str());
            else if (key == "SnapThreshold") m_config.snap_threshold = (float)std::atof(value.c_str());
            else if (key == "SnapRelease")   m_config.snap_release = (float)std::atof(value.c_str());
            else if (key == "SnapCooldown")  m_config.snap_cooldown = (float)std::atof(value.c_str());
            else if (key == "TurnMode")      m_config.turn_mode = std::atoi(value.c_str());
            else if (key == "SmoothTurnSpeed") m_config.smooth_speed = (float)std::atof(value.c_str());
            else if (key == "TurnDeadzone")  m_config.turn_deadzone = (float)std::atof(value.c_str());
            else if (key == "PauseButton")   m_config.pause_button = std::atoi(value.c_str());
            else if (key == "WidgetProbe")   m_config.widget_probe = std::atoi(value.c_str()) != 0;
            else if (key == "ReflectProbe")  m_config.reflect_probe = std::atoi(value.c_str()) != 0;
            else if (key == "HandProbe")     m_config.hand_probe = std::atoi(value.c_str()) != 0;
            else if (key == "Arms")          m_config.arms = std::atoi(value.c_str()) != 0;
            else if (key == "ArmsAlpha")     m_config.arms_alpha = (float)std::atof(value.c_str());
            else if (key == "ArmsDebugTilt")
                m_config.arms_debug_tilt = (float)std::atof(value.c_str());
            else if (key == "ArmsReachScale")
                m_config.arms_reach_scale = (float)std::atof(value.c_str());
            else if (key == "ArmsLeftHandOffsetPitch")
                m_config.arms_left_hand_offset[0] = (float)std::atof(value.c_str());
            else if (key == "ArmsLeftHandOffsetYaw")
                m_config.arms_left_hand_offset[1] = (float)std::atof(value.c_str());
            else if (key == "ArmsLeftHandOffsetRoll")
                m_config.arms_left_hand_offset[2] = (float)std::atof(value.c_str());
            else if (key == "ArmsRightHandOffsetPitch")
                m_config.arms_right_hand_offset[0] = (float)std::atof(value.c_str());
            else if (key == "ArmsRightHandOffsetYaw")
                m_config.arms_right_hand_offset[1] = (float)std::atof(value.c_str());
            else if (key == "ArmsRightHandOffsetRoll")
                m_config.arms_right_hand_offset[2] = (float)std::atof(value.c_str());
            else if (key == "ArmsElbowOut")
                m_config.arms_elbow_out = (float)std::atof(value.c_str());
            else if (key == "ArmsElbowDown")
                m_config.arms_elbow_down = (float)std::atof(value.c_str());
            else if (key == "ArmsWristForward")
                m_config.arms_wrist_offset[0] = (float)std::atof(value.c_str());
            else if (key == "ArmsWristRight")
                m_config.arms_wrist_offset[1] = (float)std::atof(value.c_str());
            else if (key == "ArmsWristUp")
                m_config.arms_wrist_offset[2] = (float)std::atof(value.c_str());
            else if (key == "Roomscale")     m_config.roomscale = std::atoi(value.c_str()) != 0;
            else if (key == "RoomscaleMaxStep")
                m_config.roomscale_max_step = (float)std::atof(value.c_str());
            else if (key == "RoomscaleYawSource")
                m_config.roomscale_yaw_source = std::atoi(value.c_str());
            else if (key == "RoomscaleCompensate")
                m_config.roomscale_compensate = std::atoi(value.c_str());
            else if (key == "ArmsComposeOrder")
                m_config.arms_compose_order = std::atoi(value.c_str());
            else if (key == "GraftPauseMenu")
                m_config.graft_pause_menu = std::atoi(value.c_str()) != 0;
            else if (key == "BodyMode")      m_config.body_mode = std::atoi(value.c_str());
            else if (key == "EyeStabilise")
                m_config.eye_stabilise = std::atoi(value.c_str()) != 0;
            else if (key == "EyeBobDamping")
                m_config.eye_bob_damping = (float)std::atof(value.c_str());
            else if (key == "EyeSwayDamping")
                m_config.eye_sway_damping = (float)std::atof(value.c_str());
            else if (key == "EyeSwayLimit")
                m_config.eye_sway_limit = (float)std::atof(value.c_str());
            else if (key == "EyeCollide")    m_config.eye_collide = std::atoi(value.c_str()) != 0;
            else if (key == "EyeProbeRadius")
                m_config.eye_probe_radius = (float)std::atof(value.c_str());
            else if (key == "EyeWallMargin")
                m_config.eye_wall_margin = (float)std::atof(value.c_str());
            else if (key == "EyeTraceChannel")
                m_config.eye_trace_channel = std::atoi(value.c_str());
            else if (key == "LogEvery")      m_config.log_every = std::atoi(value.c_str());
        }
    }

    Config m_config{};

    Body      m_body{};
    Eye       m_eye{};
    Roomscale m_roomscale{};
    Hands   m_hands{};
    AnimBp  m_animbp{};
    Reflect m_reflect{};
    Umg    m_umg{};
    VrPage m_vrpage{};
    float  m_widget_time{0.0f};

    std::atomic<bool>  m_gameplay{false};
    std::atomic<float> m_snap_yaw{0.0f};
    std::atomic<float> m_turn_axis{0.0f};
    std::atomic<float> m_quat_yaw{0.0f};
    std::atomic<float> m_final_yaw{0.0f};
    // The yaw actually written to the pawn, held still while the headset only jitters.
    // How far the pre-tick got. 8 means it ran to the end; anything less, held across
    // frames, is the phase it dies in.
    static constexpr int kPhases = 9;
    int m_phase{0};
    int m_phase_max{0};
    bool m_phase_disabled[kPhases]{};
    int m_fault_phase{-1};
    unsigned long m_fault_code{0};
    bool m_menu_armed{true};
    std::chrono::steady_clock::time_point m_menu_last{};
    float m_applied_yaw{0.0f};
    bool  m_have_applied_yaw{false};

    UEVR_ActionHandle m_pause_action{};
    bool m_pause_resolved{false};
    bool m_pause_ok{false};
    bool m_pause_seen{false};

    API::UObject* m_pc{nullptr};
    API::UObject* m_pawn{nullptr};
    API::UObject* m_cmc{nullptr};
    UEVR_Rotatorf* m_control_rotation{nullptr};
    std::wstring m_pawn_name{};
    bool m_is_character{false};
    bool m_pawn_playable{false};
    bool m_was_gameplay{false};
    int  m_flag_age{0};

    bool  m_snap_armed{true};
    float m_snap_wait{0.0f};
    int   m_snap_count{0};

    int m_frames{0};
    int m_config_age{0};
    std::filesystem::file_time_type m_config_stamp{};
    bool m_control_ok{false};
};

// Plugin.hpp picks this up through uevr::detail::g_plugin.
static TasomachiVR g_tasomachivr_plugin{};
