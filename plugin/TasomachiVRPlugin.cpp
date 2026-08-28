// TasomachiVR - UEVR C++ plugin.
//
// Owns everything rotational. The Lua script could not: the rotation handed to the
// stereo callback never contains the headset, K2_SetActorRotation is not exposed to
// script, and delegating to UEVR's own aim system fights our view override and kills
// head tracking. From C++ the HMD pose is directly readable, so the world yaw can be
// derived rather than guessed.
//
// Split with the Lua script, which still runs but is down to one job:
//   Lua  -> collapsing the camera boom
//   C++  -> everything else: view position and rotation, snap turn, body yaw, the body
//           itself and the VR settings page
// The eye moved to C++ because two callbacks writing the same struct was a race waiting
// to be noticed, and only this side can see the headset.
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
#include "poses.hpp"
#include "settings.hpp"
#include "ucall.hpp"
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
#include <sstream>
#include <vector>

using API = uevr::API;
namespace uc = tasomachivr::ucall;

using tasomachivr::Body;
using tasomachivr::MenuSettings;
using tasomachivr::Poses;
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
    // Off by default: see MenuRecenter in the ini for why the cure was worse.
    bool  menu_recenter  = true;
    // Fold UEVR's rotation offset into the headset yaw. Off only to compare.
    bool  apply_rotation_offset = true;
    // Degrees of gaze drift before the interface anchor eases back in front.
    float hud_follow_angle = 20.0f;
    // Which way to spin the anchor, and whether to hold the world still while
    // it spins. Both are conventions I could not settle by reading, so they are
    // settings with one obvious right answer to be found in one test.
    // Hard ceiling on one correction, so a wrong axis cannot spin the session.
    float hud_max_turn   = 8.0f;
    float hud_follow_speed = 2.0f;
    float hud_release_angle = 6.0f;
    // Seconds of head-following after a UI event, then it is released again.
    float menu_size      = 1.3f;
    float menu_distance  = 2.0f;
    float body_yaw_damping = 14.0f;   // higher follows faster
    float menu_deadzone = 0.75f;
    float menu_axis_ratio = 1.8f;
    int   menu_repeat_ms = 260;
    bool  write_view_rot = true;
    // Pushes the eye out of the skull. Both are hot-reloaded from the ini.
    float forward_offset = 25.0f;
    float up_offset      = 0.0f;
    // Eye height while piloting the boat, REPLACING up_offset rather than adding to it.
    // The boat seats the view differently from the character, so one number cannot serve
    // both - and on foot the answer turned out to be 0, which leaves nothing to build on.
    float ship_up_offset = -40.0f;
    // Eye forward while piloting, REPLACING forward_offset. The helm is not where her head
    // is, so the number that places the eye on foot has no bearing here.
    float ship_forward_offset = 25.0f;

    // 0 = snap, 1 = smooth. Snap is the default: it is the comfortable choice for most
    // people, and smooth turning is the classic way to make someone sick in VR.
    int   turn_mode      = 0;
    float smooth_speed   = 90.0f; // degrees per second at full stick
    float turn_deadzone  = 0.2f;

    bool  snap_turn      = true;
    // UEVR's roomscale movement, which drags the PAWN about to follow your physical body.
    // Wanted on foot, unwanted on the boat - see the tick for why.
    bool  uevr_roomscale = true;
    bool  ship_roomscale = false;
    // Let the boat have the right stick, so it steers itself and flies along its own nose.
    bool  ship_follow_turn = true;
    // Recentre the view when the possessed pawn changes - boarding, leaving, respawning.
    bool  pawn_recenter = true;
    // The deck turns you with it - see the tick. Off leaves the view pinned to the world
    // while the hull swings underneath.
    // Have the view take the hull's heading. OFF: with the stick reaching the boat AND
    // turning the view, the two already move together, and layering this on top was where
    // the boat started behaving strangely.
    bool  ship_carries_view = false;
    // Take the boat's steering over completely by synthesising its direction vector.
    //
    // OFF, after trying it: it made the boat unpredictable. The measurement it rests on -
    // that the hull points at the angle of the stick vector - held for one sample and does
    // not describe the whole behaviour, since the hull then drifted back off that angle on
    // its own. Left in place, off, because the finding is worth keeping and the switch costs
    // nothing.
    bool  ship_hijack = false;
    // Maps our heading onto the stick angle the boat reads. Flip if it steers the wrong way.
    float ship_stick_sign = 1.0f;
    // Degrees per second the view may follow the hull. 0 removes the limit.
    // Degrees per second the view may follow the hull. Measured: normal steering swings
    // the hull at about 330 deg/s, while the one-frame lurch this guards against is upward of
    // 6000 - so the limit belongs well above the first and far below the second. At 150 it
    // throttled ordinary turning and the view lagged the whole way round.
    float ship_view_follow_max = 720.0f;
    // Withhold the stick's vertical axis from the boat as well. Starts OFF: it is the
    // suspect for the steering having stopped, and a control that works matters more.
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
    bool  head_hide_air_only = true;
    // Seconds the head stays hidden after landing, so it does not reappear mid-recovery.
    float head_hide_linger = 0.30f;

    // The eye follows the head bone, filtered. See eye.hpp for why the filter needs a
    // hard clamp as well as a low-pass.
    bool  swap_sticks    = false;
    // 0 = leave it alone, 1 = X, 2 = Y, 3 = A, 4 = B.
    int   interact_button = 4;
    // Widget names to fade, and how fast. Empty means the feature is off.
    std::string hud_counters;
    float hud_counter_fade = 8.0f;
    bool  hud_always_on  = false;

    float eye_air_lift = 40.0f;
    float eye_air_forward = 0.0f;
    float detail = 4.0f;
    float supersample = 120.0f;
    // Centimetres from the head at which her head is drawn again. 0 = never.
    float head_reveal = 18.0f;
    // Withhold both triggers from the game - they only drive its camera zoom.
    // Substring of a pawn class path that aims its own camera; empty disables this.
    std::string free_camera_pawns{"PhotoMode"};
    // "VRButton:XInputButton" pairs, comma separated. See the ini.
    std::string button_remap{""};
    // A VR button wired straight to a Blueprint event, bypassing the key entirely.
    std::string button_event{
        "Y:InpActEvt_Gamepad_DPad_Down_K2Node_InputKeyEvent_0,"
        "X:InpActEvt_V_K2Node_InputKeyEvent_2"};
    std::string event_actor_class{"BP_CommonSystem_C"};

    // POSING HER IN PHOTO MODE. The button that steps through the loaded animations while
    // the free camera is up, named the way the remap table names buttons: X, Y, A, B, L3,
    // R3, or empty for off.
    //
    // A on the right hand by default. The game has PhotoAgility there - the free camera's
    // speed boost - and the bit is deliberately NOT withheld, so the boost still works:
    // holding A to fly faster and tapping A to change pose do not collide in practice,
    // and taking a control away from photo mode to add one is a poor trade.
    std::string pose_button{"A"};
    // Which animations are offered. Every animation of the heroine is named ANM_Pc01_*,
    // and matching on that is what keeps the birds and the townspeople out of the cycle.
    std::string pose_prefix{"ANM_Pc01_"};
    bool  block_triggers = true;
    int   trigger_threshold = 60;   // of 255
    // What reveals the interface: 1 = left grip, 0 = left trigger.
    int   hud_reveal_source = 1;
    float eye_air_lift_speed = 12.0f;

    int   log_every      = 240;    // frames between diagnostic lines
};

} // namespace

class TasomachiVR final : public uevr::Plugin {
public:
    void on_initialize() override {
        load_config();
        API::get()->log_info(
            // __DATE__ / __TIME__ are stamped at compile time, so the log always says which
            // build is running. Worth one line: a source tree and a deployed DLL can drift
            // apart without anything looking wrong, and that has already cost a whole round
            // of testing where the conclusions were drawn about the wrong binary.
            "[TasomachiVR] plugin up | built " __DATE__ " " __TIME__
            " | yaw_sign=%.0f yaw_offset=%.1f apply_body_yaw=%d "
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
            // menu and the body all sit after this point in the callback and
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

        m_phase = 4;
        if (!m_phase_disabled[4]) {
            drive_vr_page();
        }
        // THE VIEW TAKES THE HULL'S HEADING, ABSOLUTELY.
        //
        // One assignment, not an accumulation. Two earlier shapes of this were wrong in ways
        // worth writing down: adding the per-frame delta to the view yaw drifted and left the
        // tracked space pinned to the world, and then rotating UEVR's room anchor to fix that
        // moved two things at once and became unpredictable.
        //
        // Taking the heading outright cannot drift and needs no anchor arithmetic. The view
        // faces where the character faces - she is standing on the boat, so that is the bow -
        // and your head still turns freely on top of it. Nothing accumulates, so nothing can
        // creep out of alignment over a long flight.
        //
        // The stick is not part of this at all: it reaches TurnRate and turns the hull, and
        // the view follows only because the hull moved.
        if (!m_is_character && m_gameplay.load() && !m_config.ship_hijack &&
            m_config.ship_carries_view &&
            m_pawn != nullptr) {
            uc::Call get{m_pawn, L"K2_GetActorRotation"};
            if (get.ok) {
                UEVR_Rotatorf now{};
                m_pawn->process_event(get.fn, get.bytes.data());
                uc::result(get, now);

                // WHICH ONE TURNS? A flip on the first steer could be the hull swinging to a
                // stale target of its own, or our view taking a heading in the wrong sign
                // convention - this mod runs yaw_sign at -1 for exactly that reason. The two
                // are indistinguishable from the seat, so both are written down: the first
                // few frames aboard, and any single-frame jump big enough to be the flip.
                // RATE LIMITED, because the hull does not always move like a hull.
                //
                // Measured, not supposed: on the first steer after boarding the boat swings
                // 68 degrees in a single frame and then takes half of it back, before
                // settling to about 1.3 degrees a frame. That transient is the game's own
                // code and not something this mod can reach - but there is no reason to pass
                // it on to the eyes. Following at a bounded rate lets the view ignore a
                // one-frame lurch that the hull is about to undo anyway, and costs nothing
                // during normal steering, which is an order of magnitude slower than the cap.
                //
                // It converges on the hull rather than accumulating, so nothing can drift.
                const float previous = m_snap_yaw.load();
                float step = normalize_deg(normalize_deg(now.yaw) - previous);
                const float dt = delta > 0.0f ? delta : 0.016f;
                const float cap = m_config.ship_view_follow_max * dt;
                const bool clamped = cap > 0.0f && std::fabs(step) > cap;
                if (clamped) {
                    step = step > 0.0f ? cap : -cap;
                }
                // PITCH AND ROLL ARE LOGGED TOO, and they are the point of this pass.
                //
                // The stick's vertical axis has to reach the boat for it to steer at all -
                // measured - and it brings a movement artefact with it. The spring arm is
                // collapsed to nothing by the Lua, so an orbit cannot explain it; what is
                // left is the HULL itself pitching or rolling, carrying the camera with it
                // while this mod holds the horizon flat. If these two move with the stick,
                // that is the artefact, and the choice is between following them and damping
                // what they do to the eye - two very different fixes, so the number decides.
                if (m_ship_log < 12 || clamped || std::fabs(step) > 0.5f) {
                    if (m_ship_log < 60) {
                        ++m_ship_log;
                        API::get()->log_info("[TasomachiVR] SHIP | yaw=%.1f pitch=%.1f "
                                             "roll=%.1f view=%.1f step=%.1f%s",
                                             now.yaw, now.pitch, now.roll, previous, step,
                                             clamped ? " (capped)" : "");
                    }
                }
                m_snap_yaw.store(normalize_deg(previous + step));
            }

        }

        m_phase = 5;
        push_render_settings();

        if (m_recenter_wait > 0 && --m_recenter_wait == 0 && m_gameplay.load()) {
            API::VR::recenter_view();
            API::get()->log_info("[TasomachiVR] view recentred on the new pawn (%s)",
                                 m_is_character ? "on foot" : "boat");
        }

        m_phase = 6;
        if (!m_phase_disabled[6]) {
            drive_counters(delta);
        }

        m_phase = 7;
        if (!m_phase_disabled[7]) {
            fire_button_events();
        }

        m_phase = 8;
        if (!m_phase_disabled[8]) {
            // Turning the AnimBP off rebuilds the mesh component underneath the body, which
            // brings the hidden head bone back along with the arm physics. That is exactly
            // what invalidate() is for, and it is the caller's job because the body module
            // has no idea anyone else is driving the animation.
            if (m_poses.tick(m_character, m_free_camera.load(),
                             m_pose_requests.exchange(0), m_config.pose_prefix)) {
                m_body.invalidate();
            }
        }
    }

    // Same structured-exception guard as the pre tick, and for the same reason - it should
    // have been here from the start.
    //
    // The post tick drives the body, the eye and roomscale. A fault anywhere in it is caught
    // by the host, the game carries on, and everything after the fault is skipped silently
    // every frame. That is precisely the failure that took a morning to find on the pre tick,
    // and leaving the other callback unguarded meant the same trap was still armed: a probe
    // placed here logged nothing at all across two builds while sitting on a line that was
    // demonstrably reached.
    void on_post_engine_tick(API::UGameEngine* engine, float delta) override {
        __try {
            post_tick(engine, delta);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            const unsigned long code = GetExceptionCode();
            if (!m_post_faulted) {
                m_post_faulted = true;
                API::get()->log_error("[TasomachiVR] POST-TICK FAULTED, code 0x%08lX - the "
                                      "body, the eye and roomscale all run here", code);
            }
        }
    }

    void post_tick(API::UGameEngine*, float delta) {
        // The body's ORIENTATION is settled before the body is drawn, and that ordering is
        // the whole point: it used to be applied afterwards, so the first frame back from a
        // pause showed her still wearing the yaw she had when the menu opened. That single
        // frame is what read as "the rotation is inverted when I leave the menu".
        if (m_gameplay.load()) {
            update_final_yaw();

            // On foot only. The boat is not a Character and steers itself; see the file
            // header for why writing a body yaw there is wrong rather than merely useless.
            if (m_is_character) {
                apply_body_orientation(delta);
            }
        }

        // HIDDEN WHILE THE PAUSE MENU IS UP.
        //
        // Opening the menu recentres the view, which turns the world without turning her -
        // so she is suddenly seen from an angle she never took, and the body appears to be
        // facing the wrong way. Nothing is actually wrong with her; there is just no reason
        // to look at a body while reading a menu. She is put back, facing correctly, on the
        // way out, through the same cycle the Headless mode always runs on entry.
        const bool menu_visible = m_vrpage.game_menu_visible();

        // UEVR's UI settings, driven LIVE rather than written into its config file at
        // startup. set_mod_value reaches the same settings its own menu edits, so the size
        // slider on the VR page takes effect while you look at it, and the follow mode can
        // depend on where you are - neither of which a startup-only file could do.
        //
        //   follow  glued to your head, which is what makes the panel usable in gameplay
        //           where you turn constantly. On the title screen there is nothing to turn
        //           towards and no character, so it is left anchored in the world - a panel
        //           stuck to your face over a menu you are already looking at is just in the
        //           way.
        //   size    UEVR's default of 2.0 runs past the edge of the headset's field of view.
        // FollowView is PULSED, not held.
        //
        // Held on, the panel is glued to your face and never settles. Held off, it stays
        // wherever it was left and can end up behind you. Switched on for a moment, it
        // snaps round to where you are looking and is then released to sit still there -
        // which is what you actually want from a panel: put in front of you when something
        // happens, and stationary while you read it.
        //
        // set_mod_value reaches the same settings UEVR's own menu edits, so this works
        // live; nothing here needs a restart.
        // LAZY FOLLOW for the whole interface, by moving the ANCHOR.
        //
        // UEVR draws the interface in stage space, oriented by the inverse of its rotation
        // offset and placed at the standing origin:
        //   glm_matrix   = inverse(vr->get_rotation_offset());
        //   glm_matrix[3] += vr->get_standing_origin();
        // so hmd_quat_yaw(), which folds that same offset in, IS the angle between the panel
        // and your gaze. Zero is dead ahead.
        //
        // The correction is expressed in terms of that measured angle rather than derived
        // from the pose, which makes it independent of handedness: whatever convention the
        // yaw extraction uses, reducing the number it reports moves the panel towards you.
        // The previous version derived a direction instead, got it backwards, and - being a
        // feedback loop - did not merely under-correct but ran away. That is what made the
        // camera flip between two orientations.
        //
        // IT WATCHES ITSELF. If the gap grows instead of shrinking, or the total rotation
        // runs past a sane limit, it stops and says so. A feature that disables itself is a
        // far better failure than one that makes the game unplayable while a fix is written.
        // GAMEPLAY ONLY. A cutscene frames the character with the game's own camera, and
        // this mod hands that camera back untouched - so turning the VR anchor underneath it
        // rotates the shot the director composed, in the wrong direction and for no reason.
        // Nothing needs the panel brought to you while you are watching rather than playing.
        if (m_config.hud_follow_angle > 0.0f && m_gameplay.load()) {
            const float drift = hmd_quat_yaw();
            const float away = std::fabs(drift);

            if (!m_hud_easing && away > m_config.hud_follow_angle) {
                m_hud_easing = true;
                m_hud_turned = 0.0f;
                m_hud_elapsed = 0.0f;
                m_hud_start_away = away;
            }
            // Released well before it is perfectly centred. Chasing the last degree doubles
            // the movement for something nobody can see, and leaves the panel drifting for
            // longer than it needs to. Wide to start, early to stop: the gap between the two
            // is the hysteresis, and it is what keeps small head movements from setting the
            // whole thing off again.
            if (m_hud_easing && away < m_config.hud_release_angle) {
                m_hud_easing = false;
                API::get()->log_info("[TasomachiVR] HUD | settled: %.1f deg -> %.1f after "
                                     "%.0f deg of anchor", m_hud_start_away, away,
                                     m_hud_turned);
            }

            if (m_hud_easing) {
                m_hud_last_away = away;
                m_hud_elapsed += delta;

                // The guard judges the MECHANISM, not the player.
                //
                // It used to give up when the gap stopped closing - and then disable itself
                // for the whole session. But the gap is mostly yours: keep turning your head
                // faster than the correction catches up and it grows, which is entirely
                // normal and which I was treating as a fault. That is why it worked twice on
                // the title screen and then went quiet for good.
                //
                // What actually says the mechanism is broken is the correction not doing
                // what it was asked - and that is now measured directly, step by step. A
                // long chase is just a long chase: it ends the attempt and re-arms, rather
                // than condemning the feature.
                if (m_hud_broken > 20) {
                    m_hud_easing = false;
                    m_config.hud_follow_angle = 0.0f;
                    API::get()->log_error("[TasomachiVR] HUD | the anchor is not responding "
                                          "to the correction - disabled for this session.");
                } else if (m_hud_elapsed > 6.0f || std::fabs(m_hud_turned) > m_config.hud_max_turn) {
                    m_hud_easing = false;   // give up on THIS attempt only
                } else {
                    const float dt = delta > 0.0f ? delta : 0.016f;
                    float k = dt * m_config.hud_follow_speed;
                    k = k < 0.0f ? 0.0f : (k > 0.5f ? 0.5f : k);
                    const float step = drift * k;

                    // Reducing the measured angle. The offset gains -step, so the body -
                    // which is snap + yaw_sign * that same measured angle - gains
                    // -yaw_sign * step; adding it back to snap holds her, and the rendered
                    // view with her, perfectly still. Only the anchor moves.
                    const auto before = API::VR::get_rotation_offset();
                    API::VR::set_rotation_offset(quat_mul(yaw_quat(-step), before));

                    // COMPENSATION, and it is not optional - without it the anchor takes the
                    // view and the character round with it, so turning your head one way
                    // sends her the other. It reaches only the view and the body; roomscale
                    // keeps the bare snap yaw, because the trim cancels the offset and the
                    // net mapping from tracking space to the world is unchanged.
                    m_anchor_trim.store(normalize_deg(m_anchor_trim.load() - step));
                    m_hud_turned += step;

                    // Did the spin do what it was asked? Checked every step rather than
                    // assumed: this is what tells a broken mechanism from a player who is
                    // simply turning faster than the correction catches up.
                    const float after = hmd_quat_yaw();
                    const float moved = normalize_deg(after - drift);
                    if (std::fabs(moved + step) > 0.2f) {
                        ++m_hud_broken;
                    } else {
                        m_hud_broken = 0;
                    }
                }
            } else {
                m_hud_last_away = 0.0f;
            }
        }

        // UI_FollowView stays OFF for good: the panel is placed by moving its anchor, not
        // by letting it chase the view. Written once rather than on change, because the
        // loader used to set it too and a cache of "what I last wrote" is only valid when
        // nothing else writes.
        if (!m_ui_follow_forced) {
            m_ui_follow_forced = true;
            API::VR::set_mod_value("UI_FollowView", false);
        }

        if (m_config.menu_size != m_ui_size_applied) {
            m_ui_size_applied = m_config.menu_size;
            API::VR::set_mod_value("UI_Size", m_config.menu_size);
            API::VR::set_mod_value("UI_Distance", m_config.menu_distance);
        }

        // THE HEAD COMES BACK when the view is no longer inside it.
        //
        // The head is hidden for one reason only: at head height you would be looking at the
        // inside of her skull. Step away from it - roomscale, a lean, anything that moves the
        // view - and that reason disappears, leaving a headless character in plain sight.
        //
        // Hysteresis on the distance, because the two states differ by a full re-application
        // of the body and flickering between them would be worse than either.
        // MovementMode is a TEnumAsByte - a whole byte, not a bitfield, so reading it is
        // safe. EMovementMode: 1 = Walking, 3 = Falling.
        bool airborne = false;
        if (m_cmc != nullptr) {
            if (auto* mode = m_cmc->get_property_data<uint8_t>(L"MovementMode")) {
                airborne = (*mode == 3);
            }
        }

        // HELD OVER THE LANDING. Re-armed for as long as she is off the ground, then it
        // runs down - so the head stays away until the recovery animation has played out and
        // does not flash back the instant her feet touch, which is the moment the tuck is
        // still unwinding through the camera.
        {
            const float dt = delta > 0.0f ? delta : 0.016f;
            if (airborne) {
                m_air_linger = m_config.head_hide_linger;
            } else if (m_air_linger > 0.0f) {
                m_air_linger -= dt;
            }
        }

        int body_mode = menu_visible ? 0 : m_config.body_mode;

        // THE HEAD IS ONLY IN THE WAY WHILE AIRBORNE, so that is the only time it is hidden.
        //
        // On the ground the head bone sits behind the eye and never intrudes - which is why
        // the jump needed EyeAirLift and EyeAirForward in the first place: the tuck brings the
        // chest and the head up and forward, into the camera.
        //
        // The point of this is the SHADOW. A hidden bone casts no shadow either, and no
        // arrangement of meshes can separate the two - that was measured three ways. Hiding
        // the head only during a jump means the shadow is whole for as long as you are
        // standing on the ground looking at it, and headless only while you are in the air and
        // not looking. It does not solve the problem; it moves it to where it does not show.
        if (body_mode == 1 && m_config.head_hide_air_only && !airborne &&
            m_air_linger <= 0.0f) {
            body_mode = 2;   // Body::Whole
        }

        // NEAR HIDES, FAR REVEALS - and it now decides in both directions.
        //
        // It used to run only when the mode was already Headless, so it could reveal a hidden
        // head but never hide a shown one. With the head shown by default on the ground, and
        // on the ship where there is no jump to key off, that left nothing to hide it when you
        // lean into the character. The rule owns the choice now: close means hidden, far means
        // whole, with the same hysteresis band as before.
        if ((body_mode == 1 || body_mode == 2) && m_cam_known.load() &&
            m_config.head_reveal > 0.0f) {
            auto* mesh = deref_object(m_pawn, L"Mesh");
            if (mesh == nullptr) {
                mesh = deref_object(m_pawn, L"SK_Pc_01");
            }
            uc::Vec3 head{};
            if (mesh != nullptr && uc::socket_location(mesh, L"Head", head)) {
                const float dx = m_cam[0].load() - head.x;
                const float dy = m_cam[1].load() - head.y;
                const float dz = m_cam[2].load() - head.z;
                const float away = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float on = m_config.head_reveal;
                const float off = on * 0.7f;   // the hysteresis band
                if (away > on) {
                    m_head_shown = true;
                } else if (away < off) {
                    m_head_shown = false;
                }
                body_mode = m_head_shown ? 2 : 1;
            }
        }
        m_body.apply(m_pawn, body_mode, m_gameplay.load());

        if (!m_gameplay.load()) {
            return;
        }

        // A single 0..1 blend for both airborne offsets, so they cannot drift out of step
        // with each other and one speed setting governs the pair.
        {
            const float dt = delta > 0.0f ? delta : 0.016f;
            float k = dt * m_config.eye_air_lift_speed;
            k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
            const float target = airborne ? 1.0f : 0.0f;
            m_air_blend.store(m_air_blend.load() + (target - m_air_blend.load()) * k);
        }

        if (++m_frames >= m_config.log_every) {
            m_frames = 0;
            log_state();
        }
    }

    void on_xinput_get_state(uint32_t*, uint32_t user_index, XINPUT_STATE* state) override {
        if (state == nullptr) {
            return;
        }

        // THE STICKS ARE SWAPPED before anything else looks at them: the right stick moves
        // and the left stick turns.
        //
        // The game binds movement to the left stick and Turn/LookUp to the right, so the
        // swap is done here, once, and everything downstream - the game's own movement
        // bindings included - sees the mapping the player asked for. The physical right
        // stick is copied onto the left, which is what the game reads for MoveForward and
        // MoveRight; the physical left stick is kept aside for our own turning and never
        // reaches the game, so it cannot also walk her sideways.
        const int16_t raw_rx = state->Gamepad.sThumbRX;
        const int16_t raw_ry = state->Gamepad.sThumbRY;
        const int16_t phys_lx = state->Gamepad.sThumbLX;
        const int16_t phys_ly = state->Gamepad.sThumbLY;
        if (m_config.swap_sticks && user_index == 0 && !m_vrpage.is_open()) {
            state->Gamepad.sThumbLX = state->Gamepad.sThumbRX;
            state->Gamepad.sThumbLY = state->Gamepad.sThumbRY;
        }

        // Only pad 0 drives turning. Evaluating every index was what made the Lua
        // version of this spin on Europa: the empty pads read as centred and re-armed
        // the trigger between two real samples.
        // ON THE BOAT THE STICK ONLY STEERS.
        //
        // It does not touch the view there: you are a passenger who can look and walk about
        // freely, and the wheel turns the boat under you rather than swinging you with it.
        // That also settles the mismatch this went through several shapes to solve - if the
        // view never turns from the stick, there is no angle left for the hull to match.
        const bool own_turn = m_on_foot.load() || !m_config.ship_follow_turn;

        // On the boat the stick drives our heading too now - see the hijack below. It is
        // no longer the boat's steering, it is the wheel we hold.
        if (m_config.snap_turn && (own_turn || m_config.ship_hijack) && user_index == 0 &&
            m_gameplay.load()) {
            const float axis = m_config.swap_sticks ? (phys_lx / 32767.0f)
                                                    : (state->Gamepad.sThumbRX / 32767.0f);
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

        // The game must never see the right stick. Both pawns bind it to Turn/TurnRate and
        // LookUp/LookUpRate, so left alone it would fight the head every frame - on foot
        // through ControlRotation, on the boat through the spring arm. With the swap on,
        // its value has already been copied to the left stick above, so zeroing it here
        // costs nothing.
        // Same rule: the right stick is only withheld while this mod is driving the view.
        // A cutscene does not read it, and photo mode aims with it.
        //
        // THE BOAT KEEPS ITS STICK. It steers on the Turn axis and flies along ITS OWN
        // forward, so its heading and its travel are the same thing by construction - stand
        // at the helm and everything lines up. Writing its rotation from our side turned the
        // hull while it went on flying its own heading, which is what stopped making sense.
        //
        // On foot the stick still goes nowhere near the game: there it would fight the head
        // through ControlRotation every frame, which is what it was withheld for.
        // Withheld on foot, where it would fight the head through ControlRotation every
        // frame. On the boat it goes straight through to TurnRate, which is the wheel.
        // THE VERTICAL AXIS IS WITHHELD EVERYWHERE, and that is not a detail. Both pawns
        // bind it to LookUp and LookUpRate, so pushing the stick forward pitched the view -
        // on the boat it was reaching the game untouched, because the block below only ever
        // ran where our own turn does. Pitch belongs to the neck and to nothing else.
        // THE VERTICAL AXIS: withheld on foot, and on the boat only if asked.
        //
        // Measured, and it is the one correlation the logs offer: while this axis still
        // reached the game the hull turned, and from the build that cut it the hull has not
        // moved a degree - with the horizontal axis leaving this hook untouched at full
        // deflection the whole time. So this boat appears to be steered through both axes,
        // not just the one bound to TurnRate.
        //
        // On foot it stays withheld unconditionally: there it pitches the view through
        // LookUp, and pitch belongs to the neck.
        // NULLIFIED OUTRIGHT, and no longer conditional on the turn settings.
        //
        // It used to hang off snap_turn - an unrelated option - so setting SnapTurn=0 would
        // have quietly let this axis through again. Nothing about withholding the pitch has
        // anything to do with how turning is done, so the two are no longer tied together.
        // ON THE BOAT THE RIGHT STICK IS A DIRECTION, AND MUST ARRIVE INTACT.
        //
        // Measured, and it overturns everything that was assumed here: the hull points itself
        // at the ANGLE of the stick vector. Pushed to 69 degrees off forward, the hull went to
        // 68.2 in a single frame. It is an absolute heading control, not a rate.
        //
        // Which explains every symptom at once. Pushing right alone did nothing because its
        // magnitude was 35 per cent, under the boat's own deadzone - not because an axis was
        // missing. A nudge forward only served to clear that deadzone. And pushing fully back
        // asks for a heading of 180 degrees, so the hull obliged: the "flip" was the order
        // being given.
        //
        // Scaling the vertical axis, which is what this code used to do, therefore CORRUPTED
        // the steering - halving y turned a request for 69 degrees into one for 79. Both ends
        // of the stick reach the boat untouched now. On foot the vertical axis is still
        // withheld, where it pitches the view and belongs to the neck alone.
        if (m_gameplay.load() && (m_on_foot.load() || m_config.ship_hijack)) {
            state->Gamepad.sThumbRY = 0;
        }

        // THE BOAT'S CONTROLS, TAKEN OVER ENTIRELY.
        //
        // Measured: the hull points itself at the ANGLE of the right stick vector, absolutely
        // - pushed to 69 degrees off forward it went to -68.2. So the stick is a direction,
        // and every attempt to tame it by touching one axis distorted that direction instead.
        //
        // So the player's stick no longer reaches the boat at all. It turns OUR heading, by
        // snap steps or smoothly, exactly as it does on foot; and a vector pointing at that
        // heading is synthesised for the boat, at full magnitude so it clears the deadzone
        // that made a modest push do nothing. The result is the only thing that was ever
        // wanted here: the stick turns the boat about its vertical axis and does nothing else.
        // No pitch, no camera movement, because the player's vertical push is gone.
        if (!m_on_foot.load() && m_gameplay.load() && m_config.ship_hijack) {
            const float h = m_snap_yaw.load() * m_config.ship_stick_sign * kDegToRad;
            state->Gamepad.sThumbRX = static_cast<int16_t>(-std::sin(h) * 32000.0f);
            state->Gamepad.sThumbRY = static_cast<int16_t>(-std::cos(h) * 32000.0f);
        }
        if ((m_config.snap_turn || m_config.swap_sticks) && own_turn && m_gameplay.load()) {
            state->Gamepad.sThumbRX = 0;
        }

        // WHAT ACTUALLY LEAVES THIS HOOK. Everything above says the stick should reach the
        // boat, and it does not steer - so the next thing to establish is whether the value
        // survives to the game at all. Reported only while the stick is genuinely pushed,
        // and only a handful of times, so it cannot bury the log.
        // BOTH AXES, RAW AND AS THEY LEAVE. The measurements and the description of what the
        // hands are doing disagree: pushing left and right is what steers, yet the hull only
        // moves while the VERTICAL axis is allowed through, with the horizontal one observed
        // leaving this hook untouched at full deflection. The one explanation that fits both
        // is that the two arrive swapped, so this reports them together and settles it.
        if (user_index == 0 && !m_on_foot.load() && m_gameplay.load() &&
            (std::abs(raw_rx) > 8000 || std::abs(raw_ry) > 8000) && m_stick_reports < 14) {
            ++m_stick_reports;
            API::get()->log_info("[TasomachiVR] STICK | in x=%d y=%d | out x=%d y=%d",
                                 (int)raw_rx, (int)raw_ry,
                                 (int)state->Gamepad.sThumbRX,
                                 (int)state->Gamepad.sThumbRY);
        }

        // Unused otherwise; silences the compiler when the swap is off.
        (void)phys_ly;

        // THE TRIGGERS ARE READ, THEN TAKEN AWAY FROM THE GAME.
        //
        // Both of them drive CamZoomIN and CamZoomOUT, which change the camera boom's length.
        // That used to be invisible because this mod overwrote the camera position; now that
        // UEVR owns it, the zoom moves the view - so a pull on either trigger shoves the
        // camera about. They have nothing to do in VR and are simply withheld.
        //
        // The left one is captured on the way past, because it is what reveals the HUD
        // counters. Reading it here rather than through the VR action is deliberate: the
        // runtime's Trigger action is ANALOG, and is_action_active does not mean "pressed"
        // for that kind - it read as held all the time, which is why the counters stopped
        // hiding. This is a plain 0..255 axis with no such ambiguity.
        // WHILE WE OWN THE CAMERA, and not otherwise.
        //
        // Photo mode flies a camera pawn that needs these very inputs: the triggers are its
        // MoveUp axis and the right stick is how it aims. Taking them away left it able to
        // drift forwards and nothing else. Handing the camera back - which is what happens
        // for that pawn - has to mean handing the controls back with it.
        if (user_index == 0) {
            // Captured before this function edits anything: a ButtonEvent may be driven
            // by a pad bit, and the remap below clears the very bits it reads.
            m_raw_buttons.store(state->Gamepad.wButtons);
            m_left_trigger.store(state->Gamepad.bLeftTrigger);
            m_right_trigger.store(state->Gamepad.bRightTrigger);
            // The grip arrives as LeftShoulder - that is what the game binds CamReset to.
            m_left_grip.store((state->Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);

            // ON FOOT ONLY. The triggers are denied to the character because the game
            // puts CamZoomIN and CamZoomOUT on them, and they zoom a third-person camera this
            // mod removed. The BOAT is a different matter entirely: they are its MoveUp axis,
            // right for up and left for down, and taking them away left it unable to climb or
            // descend at all. Photo mode's camera needs them for the same reason, and is
            // already covered by the gameplay test.
            if (m_config.block_triggers && m_gameplay.load() && m_on_foot.load()) {
                state->Gamepad.bLeftTrigger = 0;
                state->Gamepad.bRightTrigger = 0;
            }
            // The grip is ours while we own the view: the game puts CamReset on it, which
            // reaches the very spring arm the script keeps collapsed.
            if (m_config.hud_reveal_source == 1 && m_gameplay.load()) {
                state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_LEFT_SHOULDER;
            }
        }

        if (user_index == 0 && m_vrpage.is_open()) {
            filter_menu_stick(state);
        }

        // INTERACT MOVED TO A BUTTON OF YOUR CHOOSING.
        //
        // The game binds Interact to Gamepad_FaceButton_Right, and whichever physical button
        // the VR runtime happens to map onto that is not something the game or this mod
        // decides - in practice it arrives on X, which is an awkward place for the one
        // button you press constantly.
        //
        // Rather than guess at XInput bits, the four face buttons are read as named VR
        // actions - UEVR exposes AButtonLeft/BButtonLeft/AButtonRight/BButtonRight, which on
        // a Touch are X, Y, A and B. The incoming FaceButton_Right bit is cleared and then
        // re-raised only from the chosen one, so Interact leaves wherever it was and appears
        // exactly where you asked.
        if (user_index == 0) {
            // DENIED FIRST, THEN REMAPPED - and the order is the whole point.
            //
            // A pad control driving a ButtonEvent is withheld from the game, so it stops
            // doing whatever the game had on it. But a remap may legitimately re-raise that
            // very bit from a DIFFERENT control: the right grip carries the stomp, which
            // costs the game its Run on RightShoulder, and the left stick click gives it
            // back. Masking last would have wiped exactly that, silently.
            //
            // The events read m_raw_buttons, captured before any of this, so masking early
            // costs them nothing.
            state->Gamepad.wButtons &= ~m_event_source_mask.load();
            apply_button_remaps(state);
            apply_button_events();
            read_pose_button();
        }

        if (user_index == 0 && m_config.interact_button != 0) {
            state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_B;
            if (interact_held()) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_B;
            }
        }

        if (user_index == 0 && pause_button_held()) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_START;

            // RECENTRE as the menu is opened, once per press - and this is now the RIGHT
            // lever, read out of UEVR's source rather than guessed at.
            //
            // With UI_FollowView off, the interface panel is anchored in the player's own
            // play space:
            //
            //   glm_matrix  = inverse(vr->get_rotation_offset());
            //   glm_matrix[3] += vr->get_standing_origin();
            //   layer.space  = stage_space;
            //
            // So the panel is fixed in the ROOM, oriented by the rotation offset - nothing
            // to do with the game's view rotation, which is what an earlier attempt turned
            // via the snap yaw and which could never have worked. recenter_view sets that
            // offset, which is precisely the panel's orientation.
            //
            // It rotates the rendered view too, and that used to leave the character behind
            // because her facing was computed from the raw pose with the offset left out.
            // That term is folded in now (see hmd_quat_yaw), so the view and the body turn
            // together and this reads as a snap turn rather than a desynchronisation.
            if (m_config.menu_recenter && !m_recentred) {
                m_recentred = true;
                API::VR::recenter_view();
            }
        } else if (user_index == 0) {
            m_recentred = false;
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

        // The axis is chosen ONCE, when the push starts, and held until the stick comes back
        // to centre. Deciding it fresh on every sample is what made this unusable: a push
        // aimed upwards drifts a few degrees sideways on the way, the dominant axis flips
        // mid-gesture, and the row you were only trying to move to has its value changed.
        //
        // A push also has to be clearly one thing or the other - the dominant axis must beat
        // the other by a margin - so a genuinely diagonal shove is ignored rather than
        // guessed at.
        const float mag = ax >= ay ? ax : ay;
        if (mag < m_config.menu_deadzone) {
            m_menu_armed = true;      // back at centre: the next push counts immediately
            m_menu_axis = -1;
            return;
        }

        if (m_menu_axis < 0) {
            // Not named "small": rpcndr.h defines that as a macro for char.
            const float dominant = ax >= ay ? ax : ay;
            const float other = ax >= ay ? ay : ax;
            if (dominant < other * m_config.menu_axis_ratio) {
                return;               // too diagonal to be meant as either
            }
            m_menu_axis = ax >= ay ? 0 : 1;
        }
        const bool horizontal = (m_menu_axis == 0);

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

        // THE CAMERA POSITION IS LEFT TO UEVR, which is what makes its roomscale and its
        // wall collision work. Writing it here - from the character's head bone - is what let
        // the head pass through walls, and no trace could have fixed that: teleporting the
        // camera bypasses collision by construction.
        //
        // This is the shape EuropaVR always had. Everything built to discipline that position
        // - a one-euro anchor filter, an airborne freeze, a sphere trace, our own roomscale -
        // existed only to make a value behave that we should never have been writing, and all
        // of it is gone. The eye is placed by the offsets in the post callback instead.
        (void)position;

        // THE VIEW IS OURS ON THE BOAT TOO, and that is not a detail.
        //
        // Handing the rotation back to the game there was tried and is a mistake: the boat's
        // camera hangs off a spring arm and rides the hull, so the view is dragged around by
        // something the player did not do. On a monitor that reads as weight; in a headset it
        // is a yaw nobody asked for, and it is sickening. A free head is not a preference
        // here, it is the difference between playable and not.
        if (rotation == nullptr || !m_config.write_view_rot) {
            return;
        }

        // Discard the game's camera orientation wholesale. Keeping it as a base would
        // mean the headset only ever added to whatever the boom had chosen that frame.
        // UEVR lays the HMD rotation on top of this.
        rotation->pitch = 0.0f;
        rotation->yaw = normalize_deg(m_snap_yaw.load() + m_anchor_trim.load());
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
        // Airborne, the eye is pushed forward as well as up. Same reason as the lift: the
        // jump animation tucks the character, and the chest comes up and FORWARD into a
        // viewpoint that never moved - height alone does not always clear it.
        const float blend = m_air_blend.load();
        // The boat seats the view somewhere else entirely, so it carries its own pair.
        const float base_forward =
            m_is_character ? m_config.forward_offset : m_config.ship_forward_offset;
        const float forward = base_forward + m_config.eye_air_forward * blend;

        const float r = m_final_yaw.load() * kDegToRad;
        position->x += std::cos(r) * forward;
        position->y += std::sin(r) * forward;

        // Raised while airborne, eased both ways. The jump animation tucks the character up,
        // and knees and chest rise into a viewpoint that is otherwise perfectly placed - this
        // passes over them. It is cosmetic and makes no pretence otherwise.
        //
        // Applied HERE now, on the offset that places the eye, rather than to a camera
        // position of our own: UEVR owns that position, which is what makes its roomscale and
        // its wall collision work.
        //
        // THE BOAT GETS ITS OWN HEIGHT. It is not a Character - it carries a projectile
        // movement component instead - so it seats the view somewhere else entirely, and a
        // single number cannot place the eye correctly in both. m_is_character is the
        // distinction the mod already draws, and it needs no name to match against.
        const float up = m_is_character ? m_config.up_offset : m_config.ship_up_offset;
        position->z += up + m_config.eye_air_lift * blend;

        // Kept so the body module can tell how far the view has drifted from the head. This
        // is the only place the FINAL camera position is known - UEVR owns it, and these
        // offsets are the last thing applied to it.
        m_cam[0].store(position->x);
        m_cam[1].store(position->y);
        m_cam[2].store(position->z);
        m_cam_known.store(true);
    }

private:

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
        s.ship_up_offset = m_config.ship_up_offset;
        s.ship_forward_offset = m_config.ship_forward_offset;
        s.yaw_offset     = m_config.yaw_offset;
        s.pause_button   = m_config.pause_button;
        s.body_mode      = m_config.body_mode;
        s.menu_size      = m_config.menu_size;
        s.hud_always_on  = m_config.hud_always_on;
        s.air_lift       = m_config.eye_air_lift;
        s.air_forward    = m_config.eye_air_forward;
        s.head_hide_linger = m_config.head_hide_linger;
        s.detail         = m_config.detail;
        s.supersample    = m_config.supersample;

        // The page only writes while it is open, so the ini still governs the rest of
        // the time.
        m_vrpage.update(API::get()->get_local_pawn(0), s);

        m_config.turn_mode      = s.turn_mode;
        m_config.snap_angle     = s.snap_angle;
        m_config.smooth_speed   = s.smooth_speed;
        m_config.forward_offset = s.forward_offset;
        m_config.up_offset      = s.up_offset;
        m_config.ship_up_offset = s.ship_up_offset;
        m_config.ship_forward_offset = s.ship_forward_offset;
        m_config.yaw_offset     = s.yaw_offset;
        m_config.body_mode      = s.body_mode;
        m_config.menu_size      = s.menu_size;
        m_config.hud_always_on  = s.hud_always_on;
        m_config.eye_air_lift   = s.air_lift;
        m_config.eye_air_forward = s.air_forward;
        m_config.head_hide_linger = s.head_hide_linger;
        m_config.detail          = s.detail;
        m_config.supersample     = s.supersample;

        // Closing the page is when the player is done choosing, so that is when the choices
        // are written. Saving every tick would rewrite the file while a slider is dragged.
        if (m_vrpage.take_close_event()) {
            save_config();
        }
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
    // The game swaps pawn when boarding or leaving the boat, and rebuilds everything
    // when the player changes zone.
    //
    // IDENTITY IS THE FULL NAME, NOT THE ADDRESS. A pointer compare alone has a hole in it:
    // when a zone unloads, its actors are destroyed and the allocator hands the same address
    // straight back to whatever is built next. The new pawn then tests EQUAL to the old one,
    // this function returns early, and every pointer derived from it - the movement
    // component, the mesh - stays pointing at freed memory. That is a dangling-pointer crash
    // in the post tick, which is exactly what 0xC0000005 was.
    //
    // Unreal appends a unique instance number to every object name, so a recycled address
    // carries a different name and the swap is caught. The Lua half of this mod had already
    // learned to compare names; the C++ half had not.
    void refresh_pawn() {
        auto* pawn = API::get()->get_local_pawn(0);
        const std::wstring id = pawn != nullptr ? pawn->get_full_name() : std::wstring{};
        if (pawn == m_pawn && id == m_pawn_id) {
            return;
        }
        if (pawn == m_pawn && pawn != nullptr) {
            API::get()->log_info("[TasomachiVR] pawn reused an address - %s is now %s",
                                 narrow(m_pawn_id).c_str(), narrow(id).c_str());
        }

        // What we are leaving, captured before the name is thrown away.
        const bool was_free_camera = m_free_camera.load();

        m_pawn = pawn;
        m_pawn_id = id;
        // Everything downstream is rebuilt from the new pawn rather than trusted.
        m_body.invalidate();
        // The interface is rebuilt with the zone, and a new widget starts fully opaque. The
        // rescan was on a ten-second timer, so it came back visible and stayed that way -
        // and worse, the fade was writing into the widgets the old zone had left behind.
        // A pawn swap is the signal that both are stale.
        m_counter_rescan = 0;
        m_cmc = nullptr;
        m_is_character = false;
        m_pawn_playable = false;
        m_free_camera.store(false);
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
        m_on_foot.store(m_is_character);   // read from the input thread

        // FACING THE WAY THE NEW PAWN FACES.
        //
        // The rotation offset carries whatever snap turns were made before boarding, so
        // stepping onto the boat left the view pointing off the bow while the hull flew
        // straight ahead - and since the boat now owns its own heading, nothing was left to
        // reconcile the two. A recentre sets that offset from where you are actually looking,
        // which puts the world's forward back under the game's camera - the bow.
        //
        // Deferred a few frames: the possession has happened but the game's camera has not
        // settled on the new pawn yet, and recentring against the old one would bake in the
        // very error this removes.
        if (m_config.pawn_recenter) {
            m_recenter_wait = 20;
        }
        m_ship_log = 0;   // the first frames aboard are the interesting ones

        // FACING THE BOW ON BOARDING. The view yaw is ours and survives the possession, so
        // stepping onto the boat left it pointing wherever the character had last been turned
        // - as often as not straight back down the deck. The hull's own heading is the only
        // sensible thing to adopt, and taking it here leaves the servo nothing to correct on
        // the first frame either.
        if (!m_is_character && pawn != nullptr) {
            uc::Call get{pawn, L"K2_GetActorRotation"};
            if (get.ok) {
                UEVR_Rotatorf now{};
                pawn->process_event(get.fn, get.bytes.data());
                uc::result(get, now);
                m_snap_yaw.store(normalize_deg(now.yaw));
                API::get()->log_info("[TasomachiVR] boarded: view set to the bow (%.0f)",
                                     now.yaw);
            }
        }

        // ROOMSCALE MOVEMENT IS FOR A CHARACTER, NOT FOR A VEHICLE.
        //
        // UEVR's roomscale moves the POSSESSED PAWN so the world keeps up with your physical
        // body - which is exactly right on foot, and is what gives this mod its wall
        // collision. On the boat the possessed pawn IS the boat, so turning on the spot and
        // then pushing forward drags the hull about, and the boat noses into wherever it has
        // been dragged. That is the 180 that broke the opening: nothing was steering it, it
        // was being carried.
        //
        // Nothing else changes - the snap turn, the stick and the view stay as they are on
        // foot. Only the pawn stops being towed by your body.
        const bool want = m_is_character ? m_config.uevr_roomscale : m_config.ship_roomscale;
        if (want != m_roomscale_on) {
            m_roomscale_on = want;
            API::VR::set_mod_value("VR_RoomscaleMovement", want);
            API::get()->log_info("[TasomachiVR] roomscale movement %s (%s)",
                                 want ? "on" : "off", m_is_character ? "on foot" : "boat");
        }

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

        // PAWNS THE PLAYER AIMS THEMSELVES are handed their camera back.
        //
        // Photo mode possesses PhotoMode_Camera, a free-flying pawn that aims itself with
        // AddControllerPitchInput / YawInput / RollInput. It lives under /Game/, so it passed
        // the test above and was treated as ordinary gameplay - which meant this mod
        // overwrote its rotation every frame with the snap yaw, and the pitch and roll the
        // player was asking for went nowhere. A comment elsewhere claimed photo mode already
        // failed the gameplay test; it did not, and that was worth checking rather than
        // trusting.
        //
        // Treating it as "not gameplay" is exactly the cutscene path: the game keeps its own
        // camera, UEVR lays the headset on top, so you look around with your head and aim
        // with the sticks. The character is drawn whole again too, which is what you want in
        // front of a camera.
        //
        // A substring list rather than one hard-coded name, so a second such pawn needs a
        // setting rather than a build.
        if (m_pawn_playable && !m_config.free_camera_pawns.empty()) {
            const std::string name = narrow(m_pawn_name);
            // A COMMA-SEPARATED LIST now, not one name. Photo mode was the first pawn to want
            // its camera back and the boat is the second, and there was no reason for the
            // setting to hold only one.
            bool hands_off = false;
            std::stringstream want(m_config.free_camera_pawns);
            std::string one;
            while (std::getline(want, one, ',')) {
                while (!one.empty() && (one.front() == ' ' || one.front() == 9)) {
                    one.erase(one.begin());
                }
                while (!one.empty() && (one.back() == ' ' || one.back() == 9)) {
                    one.pop_back();
                }
                if (!one.empty() && name.find(one) != std::string::npos) {
                    hands_off = true;
                }
            }
            if (hands_off) {
                m_pawn_playable = false;
                m_free_camera.store(true);
                API::get()->log_info("[TasomachiVR] %s aims its own camera - handing it back",
                                     name.c_str());
            }
        }

        // THE CHARACTER, KEPT WHILE SOMETHING ELSE IS POSSESSED.
        //
        // Photo mode possesses PhotoMode_Camera, so from that moment get_local_pawn returns
        // the camera and the character is unreachable through it. Everything that asks about
        // the PLAYER - what animation she is in, which flag to suspend - has to keep asking
        // about her, or it goes blind exactly when photo mode is open. That is why the mode
        // could be entered from a bench and never left: the closing press could no longer
        // read the condition it had been allowed in by.
        if (m_is_character && m_pawn_playable) {
            // A DIFFERENT character, not merely a re-possession. Leaving photo mode hands
            // the same object back, and forgetting there would throw away the pose state
            // one phase before the tick that has to undo it - which would leave her frozen
            // in whatever she was posed in, permanently.
            if (pawn != m_character) {
                m_poses.forget();
            }
            m_character = pawn;

            // COMING BACK FROM PHOTO MODE UNDOES A SIT THAT NEVER ENDED.
            //
            // The bench raises IsInAction? when you sit down and only lowers it again when
            // you stand up through its own path. Photo mode takes the character off the
            // bench without going near that path, so the flag stays raised for good and
            // every ability that tests it is dead for the rest of the session - which is
            // exactly the reported symptom.
            //
            // The flags to put back are the ones the bindings already name: whatever a
            // bypass suspends for the length of a call is the same thing left dangling
            // here. Nothing is invented. If photo mode was entered while standing they are
            // already at that value and this does nothing.
            if (was_free_camera) {
                for (const auto& e : m_events) {
                    for (const auto& b : e.bypass) {
                        if (b.on_system || b.name.empty()) {
                            continue;
                        }
                        if (pawn->get_bool_property(b.name) != b.value) {
                            pawn->set_bool_property(b.name, b.value);
                            API::get()->log_info("[TasomachiVR] free camera left - %ls put "
                                                 "back to %d", b.name.c_str(),
                                                 (int)b.value);
                        }
                    }
                }
            }
        }

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
    // The chosen face button, read straight from the VR runtime.
    // Collects the named widgets out of the HUD's tree, once.
    void find_counters(API::UObject* widget, int depth) {
        if (widget == nullptr || depth > 6 || m_counters.size() >= 8) {
            return;
        }
        const auto name = narrow(widget->get_fname()->to_string());
        if (hud_names_include(name)) {
            m_counters.push_back(widget);
            API::get()->log_info("[TasomachiVR] HUD | counter found: %s", name.c_str());
        }

        uc::Call count{widget, L"GetChildrenCount"};
        if (!count.ok) {
            return;
        }
        widget->process_event(count.fn, count.bytes.data());
        int32_t n = 0;
        uc::result(count, n);
        for (int32_t i = 0; i < n && i < 40; ++i) {
            find_counters(uc::child_at(widget, i), depth + 1);
        }
    }

    // The counters are hidden until you ask for them, and FADE rather than pop.
    //
    // Held on the LEFT TRIGGER, which the game binds to CamZoomOUT - a zoom for the third
    // person camera this mod removed, so it is genuinely free. The left grip would have done
    // too, but it carries CamReset, which still reaches the spring arm the script collapses
    // every frame.
    //
    // SetRenderOpacity rather than visibility: visibility can only pop, and it also changes
    // hit testing and layout. Opacity is the one that can be eased.
    // WHOLE NAMES, NOT SUBSTRINGS.
    //
    // This used to ask whether the config string CONTAINED the widget's name, which is true
    // for any name that happens to be a prefix of a configured one: with
    // HudCounters=HorizontalBox_37,HorizontalBox_38, a widget merely called HorizontalBox
    // matched, and so did HorizontalBox_3. The fade then landed on whatever unrelated boxes
    // the game had built - the log shows it settling on three widgets, none of them
    // necessarily the counters - which is why the interface stayed lit after a zone change.
    bool hud_names_include(const std::string& name) const {
        if (name.empty()) {
            return false;
        }
        size_t at = 0;
        while (at <= m_config.hud_counters.size()) {
            const auto end = m_config.hud_counters.find(',', at);
            auto token = m_config.hud_counters.substr(
                at, end == std::string::npos ? std::string::npos : end - at);
            while (!token.empty() && (token.front() == ' ' || token.front() == 9)) {
                token.erase(token.begin());
            }
            while (!token.empty() && (token.back() == ' ' || token.back() == 9)) {
                token.pop_back();
            }
            if (!token.empty() && token == name) {
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            at = end + 1;
        }
        return false;
    }

    void drive_counters(float delta) {
        if (m_config.hud_counters.empty()) {
            return;
        }

        // Re-scanned rather than latched once. Widgets are rebuilt as the game goes - a new
        // area, a reloaded save - and a cached pointer to a dead one writes opacity into
        // nothing while the real interface stays put on screen.
        if (--m_counter_rescan <= 0) {
            m_counter_rescan = m_counters.empty() ? 120 : 180;
            m_counters.clear();

            // Every live UUserWidget, filtered by class name. Naming classes rather than one
            // hard-coded Blueprint is what lets the same mechanism fade a single counter or a
            // whole interface: an entry matching a widget's CLASS fades that widget entirely,
            // an entry matching a child's NAME fades just that child.
            if (auto* klass = API::get()->find_uobject<API::UClass>(
                    L"Class /Script/UMG.UserWidget")) {
                for (auto* w : klass->get_objects_matching<API::UObject>(false)) {
                    if (w == nullptr) {
                        continue;
                    }
                    const auto full = narrow(w->get_full_name());
                    // The Blueprint's template carries the same names as the live widget, so
                    // it looks like a perfectly good answer while being the one object whose
                    // opacity nothing on screen reads.
                    if (full.find("WidgetArchetype") != std::string::npos ||
                        full.find("Default__") != std::string::npos) {
                        continue;
                    }
                    auto* wc = w->get_class();
                    if (wc == nullptr || wc->get_fname() == nullptr) {
                        continue;
                    }
                    if (hud_names_include(narrow(wc->get_fname()->to_string()))) {
                        m_counters.push_back(w);   // the whole widget
                        continue;
                    }
                    if (auto* tree = deref_object(w, L"WidgetTree")) {
                        if (auto* root = deref_object(tree, L"RootWidget")) {
                            find_counters(root, 0);
                        }
                    }
                }
            }

            if (m_counters.size() != m_counters_last) {
                m_counters_last = m_counters.size();
                API::get()->log_info("[TasomachiVR] HUD | fading %d widget(s)",
                                     (int)m_counters.size());
            }
            m_counter_shown = -1.0f;   // force a rewrite onto the new set
        }

        if (m_counters.empty()) {
            return;
        }

        const float target = (m_config.hud_always_on || reveal_held())
                                 ? 1.0f : 0.0f;
        const float dt = delta > 0.0f ? delta : 0.016f;
        float k = dt * m_config.hud_counter_fade;
        k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
        m_counter_alpha += (target - m_counter_alpha) * k;

        // Written only while it is actually changing, so a hidden HUD costs nothing.
        if (std::fabs(m_counter_alpha - m_counter_shown) > 0.002f) {
            m_counter_shown = m_counter_alpha;
            for (auto* w : m_counters) {
                uc::set_opacity(w, m_counter_alpha);
            }
        }
    }

    // A real pull, not a graze: the analog axis is compared against a threshold, so resting
    // a finger on the trigger does not summon the counters.
    // Whatever reveals the interface: 1 = left grip, 0 = left trigger.
    //
    // Both are read from XInput rather than through a VR action. That is deliberate and
    // learned: the runtime's Trigger action is analog, and is_action_active does not mean
    // "pressed" for that kind - read that way it was held permanently, which is why the
    // counters would not hide.
    bool reveal_held() const {
        if (m_config.hud_reveal_source == 1) {
            return m_left_grip.load();
        }
        return m_left_trigger.load() > m_config.trigger_threshold;
    }

    // A GENERAL BUTTON REMAP, driven from the ini.
    //
    // The game listens for things no Touch button reaches - photo mode is on DPad Down, bound
    // as a raw InputKey inside a Blueprint rather than as an action, so there is no mapping to
    // edit. Rather than hard-code each one as it turns up, a VR button is named on the left
    // and the XInput button it should press on the right.
    //
    // The source is read as a named VR ACTION rather than an XInput bit, because which
    // physical button lands on which bit is the runtime's decision and not ours to assume -
    // that is exactly how Interact ended up on X when the game said FaceButton_Right.
    struct Remap { const char* name; const char* action; bool left; };

    static const Remap* vr_buttons(size_t& count) {
        static const Remap table[] = {
            {"X",  "/actions/default/in/AButtonLeft",   true},
            {"Y",  "/actions/default/in/BButtonLeft",   true},
            {"A",  "/actions/default/in/AButtonRight",  false},
            {"B",  "/actions/default/in/BButtonRight",  false},
            {"L3", "/actions/default/in/JoystickClick", true},
            {"R3", "/actions/default/in/JoystickClick", false},
        };
        count = sizeof(table) / sizeof(table[0]);
        return table;
    }

    static WORD xinput_bit(const std::string& n) {
        if (n == "A") return XINPUT_GAMEPAD_A;
        if (n == "B") return XINPUT_GAMEPAD_B;
        if (n == "X") return XINPUT_GAMEPAD_X;
        if (n == "Y") return XINPUT_GAMEPAD_Y;
        if (n == "DPadUp") return XINPUT_GAMEPAD_DPAD_UP;
        if (n == "DPadDown") return XINPUT_GAMEPAD_DPAD_DOWN;
        if (n == "DPadLeft") return XINPUT_GAMEPAD_DPAD_LEFT;
        if (n == "DPadRight") return XINPUT_GAMEPAD_DPAD_RIGHT;
        if (n == "LShoulder") return XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (n == "RShoulder") return XINPUT_GAMEPAD_RIGHT_SHOULDER;
        // The Touch grips arrive as the shoulder buttons - measured, not assumed: that is
        // how the left grip was found when it took over revealing the interface. The aliases
        // exist so the ini can say what the hand does rather than what the pad calls it.
        if (n == "LGrip")     return XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (n == "RGrip")     return XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (n == "Start") return XINPUT_GAMEPAD_START;
        if (n == "Back") return XINPUT_GAMEPAD_BACK;
        if (n == "L3") return XINPUT_GAMEPAD_LEFT_THUMB;
        if (n == "R3") return XINPUT_GAMEPAD_RIGHT_THUMB;
        return 0;
    }

    // The press half of a direct Blueprint call. Nothing is invoked here: this runs on
    // whichever thread XInput is polled from, and calling a UFunction from it would be a
    // crash waiting for a busy frame. Only a bit is set; the game thread does the work.
    void apply_button_events() {
        if (m_config.button_event.empty()) {
            return;
        }
        const auto* vr = API::get()->param()->vr;
        if (vr == nullptr) {
            return;
        }

        if (!m_events_parsed) {
            m_events_parsed = true;
            size_t count = 0;
            const auto* table = vr_buttons(count);
            std::stringstream in(m_config.button_event);
            std::string pair;
            while (std::getline(in, pair, ',') && m_events.size() < 32) {
                const auto colon = pair.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                const auto trim = [](std::string v) {
                    while (!v.empty() && (v.front() == ' ' || v.front() == 9)) v.erase(v.begin());
                    while (!v.empty() && (v.back() == ' ' || v.back() == 9)) v.pop_back();
                    return v;
                };
                const std::string from = trim(pair.substr(0, colon));
                std::string fn = trim(pair.substr(colon + 1));

                // +[system.]Name[=0|1][@CondClass.CondFlag]
                std::vector<Bypass> bypass;
                for (auto plus = fn.find('+'); plus != std::string::npos;
                     plus = fn.find('+')) {
                    std::string spec = fn.substr(plus + 1);
                    fn = fn.substr(0, plus);
                    if (spec.empty()) {
                        continue;
                    }
                    Bypass b{};
                    if (const auto at = spec.find('@'); at != std::string::npos) {
                        const std::string cond = spec.substr(at + 1);
                        spec = spec.substr(0, at);
                        if (cond.rfind("anim:", 0) == 0) {
                            b.cond_anim = cond.substr(5);
                        }
                    }
                    if (const auto eq = spec.find('='); eq != std::string::npos) {
                        b.value = spec.substr(eq + 1) != "0";
                        spec = spec.substr(0, eq);
                    }
                    if (spec.rfind("system.", 0) == 0) {
                        b.on_system = true;
                        spec = spec.substr(7);
                    }
                    b.name.assign(spec.begin(), spec.end());
                    if (!b.name.empty()) {
                        bypass.push_back(b);
                    }
                }

                // "pawn." calls the function on the PLAYER CHARACTER instead of on the
                // event actor. The character carries its own key handlers - the stomp among
                // them - and calling one is the same trick that opened photo mode: it runs
                // what the key would run, without depending on the key arriving.
                bool on_pawn = false;
                if (fn.rfind("pawn.", 0) == 0) {
                    on_pawn = true;
                    fn = fn.substr(5);
                }

                // A source that is not one of the named VR buttons is read as a pad bit,
                // which is how the grips arrive.
                bool matched = false;
                for (size_t i = 0; i < count; ++i) {
                    if (from != table[i].name) {
                        continue;
                    }
                    matched = true;
                    EventBind b{};
                    b.action = vr->get_action_handle(table[i].action);
                    b.left = table[i].left;
                    b.on_pawn = on_pawn;
                    b.bypass = bypass;
                    b.fn.assign(fn.begin(), fn.end());
                    if (b.action == nullptr) {
                        API::get()->log_error("[TasomachiVR] event: %s did not resolve",
                                              table[i].action);
                    } else {
                        m_events.push_back(b);
                        API::get()->log_info("[TasomachiVR] event: %s -> %s(), "
                                             "%d flag(s)", from.c_str(), fn.c_str(),
                                             (int)bypass.size());
                    }
                    break;
                }

                if (!matched && (from == "LTrigger" || from == "RTrigger")) {
                    matched = true;
                    EventBind b{};
                    b.from_trigger = from == "LTrigger" ? 1 : 2;
                    b.on_pawn = on_pawn;
                    b.bypass = bypass;
                    b.fn.assign(fn.begin(), fn.end());
                    m_events.push_back(b);
                    API::get()->log_info("[TasomachiVR] event: %s -> %s%s() (trigger source)",
                                         from.c_str(), on_pawn ? "pawn." : "", fn.c_str());
                }

                if (!matched) {
                    const WORD src = xinput_bit(from);
                    if (src == 0) {
                        API::get()->log_error("[TasomachiVR] event: '%s' is neither a VR "
                                              "button, a pad button nor a trigger",
                                              from.c_str());
                        continue;
                    }
                    EventBind b{};
                    b.from_bit = src;
                    b.on_pawn = on_pawn;
                    b.bypass = bypass;
                    b.fn.assign(fn.begin(), fn.end());
                    m_events.push_back(b);
                    // A MOVE, NOT A COPY - the same rule the remap followed. A pad control
                    // driving one of these stops doing whatever the game had on it, or the
                    // right grip would stomp AND run at the same time.
                    m_event_source_mask.fetch_or(src);
                    API::get()->log_info("[TasomachiVR] event: %s -> %s%s() (pad source)",
                                         from.c_str(), on_pawn ? "pawn." : "", fn.c_str());
                }
            }
        }

        const uint16_t raw = m_raw_buttons.load();
        for (size_t i = 0; i < m_events.size(); ++i) {
            auto& b = m_events[i];
            bool held;
            if (b.from_trigger != 0) {
                const int pull = b.from_trigger == 1 ? m_left_trigger.load()
                                                     : m_right_trigger.load();
                held = pull > m_config.trigger_threshold;
            } else if (b.from_bit != 0) {
                held = (raw & b.from_bit) != 0;
            } else {
                const auto source = b.left ? vr->get_left_joystick_source()
                                           : vr->get_right_joystick_source();
                held = vr->is_action_active(b.action, source);
            }
            if (held == b.was_held) {
                continue;
            }
            b.was_held = held;
            if (held) {
                m_event_pending.fetch_or(1u << i);
            }
        }
    }

    // THE POSE BUTTON. Read as a named VR action for the same reason every other button
    // here is: which physical button arrives on which XInput bit is the runtime's decision,
    // and assuming it is how Interact ended up on X.
    //
    // The edge is tracked whether or not photo mode is up, and only SPENT while it is. A
    // press that began in gameplay - A is Jump - therefore cannot arrive as a pose the
    // instant the camera opens.
    //
    // Nothing is called from here: this runs on whichever thread XInput is polled from, and
    // a UFunction call from it is a crash waiting for a busy frame. The counter is the whole
    // of the work.
    void read_pose_button() {
        if (m_config.pose_button.empty() || m_config.pose_button == "off") {
            return;
        }
        const auto* vr = API::get()->param()->vr;
        if (vr == nullptr) {
            return;
        }

        if (!m_pose_resolved) {
            m_pose_resolved = true;
            size_t count = 0;
            const auto* table = vr_buttons(count);
            for (size_t i = 0; i < count; ++i) {
                if (m_config.pose_button != table[i].name) {
                    continue;
                }
                m_pose_action = vr->get_action_handle(table[i].action);
                m_pose_left = table[i].left;
                break;
            }
            // A source that is not one of the named VR buttons is read as a pad bit, which
            // is how the grips arrive - the same fallback the event binds make.
            if (m_pose_action == nullptr) {
                m_pose_bit = xinput_bit(m_config.pose_button);
            }
            if (m_pose_action == nullptr && m_pose_bit == 0) {
                API::get()->log_error("[TasomachiVR] poses: '%s' is neither a VR button nor "
                                      "a pad button", m_config.pose_button.c_str());
            } else {
                API::get()->log_info("[TasomachiVR] poses: %s cycles the pose in photo mode",
                                     m_config.pose_button.c_str());
            }
        }

        bool held = false;
        if (m_pose_action != nullptr) {
            const auto source = m_pose_left ? vr->get_left_joystick_source()
                                            : vr->get_right_joystick_source();
            held = vr->is_action_active(m_pose_action, source);
        } else if (m_pose_bit != 0) {
            held = (m_raw_buttons.load() & m_pose_bit) != 0;
        }

        if (held == m_pose_was_held) {
            return;
        }
        m_pose_was_held = held;
        if (held && m_free_camera.load()) {
            m_pose_requests.fetch_add(1);
        }
    }

    // WHAT ANIMATION IS THE CHARACTER PLAYING RIGHT NOW?
    //
    // This exists because isSitting? on the bench turned out to be a PULSE, not a state: the
    // log has it going true and back to false inside 0.7s, at the transition, while sitting
    // itself lasts as long as you like. A condition sampled at the instant of a button press
    // could therefore never see it, which is exactly what happened.
    //
    // The bench drives the pose with PlayAnimation and SetAnimationMode, so the character's
    // single-node animation IS the state, and it lasts precisely as long as the sitting does.
    // CurrentAsset is read as a property rather than through GetAnimationAsset, because a
    // property either exists or does not, while a function may simply not be exposed.
    std::string current_anim_name() {
        auto* pawn = m_character != nullptr ? m_character : API::get()->get_local_pawn(0);
        if (pawn == nullptr) {
            return {};
        }
        auto* mesh = deref_object(pawn, L"Mesh");
        if (mesh == nullptr) {
            return {};
        }
        // GetAnimInstance, not GetSingleNodeInstance. The latter is a plain C++ method and
        // is not in the reflection data at all - the probe listed every function on the whole
        // class chain and it is simply absent, which is why this returned an empty string for
        // three test rounds. In SingleNode mode the anim instance IS the single-node
        // instance, so the exposed call reaches the same object.
        uc::Call node{mesh, L"GetAnimInstance"};
        if (!node.ok) {
            if (m_anim_reports < 3) {
                ++m_anim_reports;
                API::get()->log_error("[TasomachiVR] anim: GetAnimInstance missing on %s",
                                      uc::class_name(mesh).c_str());
            }
            return {};
        }
        mesh->process_event(node.fn, node.bytes.data());
        API::UObject* instance = nullptr;
        uc::result(node, instance);
        if (instance == nullptr) {
            if (m_anim_reports < 3) {
                ++m_anim_reports;
                API::get()->log_error("[TasomachiVR] anim: no animation instance");
            }
            return {};
        }
        if (auto* asset = uc::property_object(instance, L"CurrentAsset")) {
            return uc::object_name(asset);
        }
        if (m_anim_reports < 3) {
            ++m_anim_reports;
            API::get()->log_error("[TasomachiVR] anim: instance %s has no CurrentAsset",
                                  uc::class_name(instance).c_str());
        }
        return {};
    }

    // The call half, on the game thread.
    void fire_button_events() {
        const uint32_t pending = m_event_pending.exchange(0);

        // NO FREE-CAMERA GUARD HERE, and that is a correction rather than an omission.
        // A guard used to stand down every event while photo mode was up, on the reasoning
        // that PhotoMode_Camera binds X and Y itself and a press would do two things. What
        // that overlooked is that the bound function is the V handler - a TOGGLE - so the
        // press being suppressed was precisely the one that closes photo mode. Opening
        // worked and closing was impossible.
        if (pending == 0) {
            return;
        }

        API::UObject* actor = nullptr;
        if (auto* array = API::FUObjectArray::get(); array != nullptr) {
            const int32_t count = array->get_object_count();
            for (int32_t i = 0; i < count; ++i) {
                auto* candidate = array->get_object(i);
                if (candidate == nullptr || candidate->get_class() == nullptr) {
                    continue;
                }
                // BY CLASS NAME, not is_a(). The class object find_uobject returns is
                // not the one these instances carry - is_a failed on every object for a
                // whole session while the actor sat in the level answering the keyboard.
                // The name is what agreed with reality, so the name is what is tested.
                auto* c = candidate->get_class();
                if (c->get_fname() == nullptr) {
                    continue;
                }
                if (narrow(c->get_fname()->to_string()) != m_config.event_actor_class) {
                    continue;
                }
                const auto full = narrow(candidate->get_full_name());
                if (full.find("Default__") != std::string::npos) {
                    continue;   // the archetype, which has no level to act on
                }
                if (actor == nullptr) {
                    actor = candidate;
                }
                break;
            }
        }
        if (actor == nullptr) {
            // Bounded: this used to repeat on every press for a whole session.
            if (pending != 0 && m_event_misses < 5) {
                ++m_event_misses;
                API::get()->log_error("[TasomachiVR] event: no live instance of %s",
                                      m_config.event_actor_class.c_str());
            }
            return;
        }

        for (size_t i = 0; i < m_events.size(); ++i) {
            if ((pending & (1u << i)) == 0) {
                continue;
            }
            auto* subject =
                m_character != nullptr ? m_character : API::get()->get_local_pawn(0);
            auto* target = m_events[i].on_pawn ? subject : actor;
            if (target == nullptr) {
                continue;
            }

            // The name may be a PREFIX. A Blueprint key handler compiles to
            // InpActEvt_<key>_K2Node_InputKeyEvent_<n>, and the number is an artifact of
            // compile order that the cooked asset does not show - so an exact name would
            // have to be read out of the running game and would break on a game patch.
            std::wstring resolved = m_events[i].fn;
            uc::Call call{target, resolved.c_str()};
            if (!call.ok) {
                if (auto* klass = target->get_class()) {
                    for (auto* f = klass->get_children(); f != nullptr; f = f->get_next()) {
                        if (f->get_fname() == nullptr) {
                            continue;
                        }
                        const auto name = f->get_fname()->to_string();
                        if (name.rfind(m_events[i].fn, 0) == 0) {
                            resolved = name;
                            break;
                        }
                    }
                }
                call = uc::Call{target, resolved.c_str()};
                if (call.ok && m_events[i].reports < 8) {
                    API::get()->log_info("[TasomachiVR] event: %s resolved to %s",
                                         narrow(m_events[i].fn).c_str(),
                                         narrow(resolved).c_str());
                }
            }
            if (!call.ok) {
                API::get()->log_error("[TasomachiVR] event: %s has no function %s",
                                      m_config.event_actor_class.c_str(),
                                      narrow(m_events[i].fn).c_str());
                continue;
            }
            // HELD, NOT OVERWRITTEN. Each flag is set to its chosen value, the call is
            // made, and the original goes straight back - ProcessEvent is synchronous, so
            // the graph reads its branch inside that window and the game's own state
            // survives it. Leaving IsInAction? cleared would tell the game the character has
            // stood up while she visibly has not.
            auto* pawn_target =
                m_character != nullptr ? m_character : API::get()->get_local_pawn(0);
            std::vector<std::pair<API::UObject*, uint8_t>> saved;
            saved.reserve(m_events[i].bypass.size());
            std::string anim;
            bool anim_read = false;
            for (const auto& b : m_events[i].bypass) {
                bool allowed = b.cond_anim.empty();
                if (!b.cond_anim.empty()) {
                    if (!anim_read) {
                        anim_read = true;
                        anim = current_anim_name();
                    }
                    allowed = anim.find(b.cond_anim) != std::string::npos;
                    if (m_bypass_reports < 6) {
                        ++m_bypass_reports;
                        API::get()->log_info("[TasomachiVR] bypass: animation is '%s', "
                                             "wanted '%s' -> %s", anim.c_str(),
                                             b.cond_anim.c_str(), allowed ? "yes" : "no");
                    }
                }
                auto* flag_target = b.on_system ? actor : pawn_target;
                if (!allowed || flag_target == nullptr) {
                    saved.emplace_back(nullptr, 0);
                    continue;
                }
                saved.emplace_back(flag_target,
                                   flag_target->get_bool_property(b.name) ? 1 : 0);
                flag_target->set_bool_property(b.name, b.value);
                if (m_bypass_reports < 6) {
                    ++m_bypass_reports;
                    API::get()->log_info("[TasomachiVR] bypass: %ls held at %d (was %d)",
                                         b.name.c_str(), (int)b.value,
                                         (int)saved.back().second);
                }
            }

            target->process_event(call.fn, call.bytes.data());

            for (size_t k = 0; k < saved.size(); ++k) {
                if (saved[k].first != nullptr) {
                    saved[k].first->set_bool_property(m_events[i].bypass[k].name,
                                                      saved[k].second != 0);
                }
            }
            // Uncapped while probing: the whole point is to press again in another state.
            if (m_events[i].reports < 8) {
                ++m_events[i].reports;
                API::get()->log_info("[TasomachiVR] event: called %s()",
                                     narrow(m_events[i].fn).c_str());
            }
        }
    }

    void apply_button_remaps(XINPUT_STATE* state) {
        if (m_config.button_remap.empty()) {
            return;
        }
        const auto* vr = API::get()->param()->vr;
        if (vr == nullptr) {
            return;
        }

        if (!m_remaps_parsed) {
            m_remaps_parsed = true;
            size_t count = 0;
            const auto* table = vr_buttons(count);
            std::stringstream in(m_config.button_remap);
            std::string pair;
            while (std::getline(in, pair, ',')) {
                const auto colon = pair.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                const auto trim = [](std::string v) {
                    while (!v.empty() && (v.front() == ' ' || v.front() == 9)) v.erase(v.begin());
                    while (!v.empty() && (v.back() == ' ' || v.back() == 9)) v.pop_back();
                    return v;
                };
                const std::string from = trim(pair.substr(0, colon));
                const std::string to = trim(pair.substr(colon + 1));
                const WORD bit = xinput_bit(to);
                if (bit == 0) {
                    API::get()->log_error("[TasomachiVR] remap: '%s' is not an XInput button",
                                          to.c_str());
                    continue;
                }

                // A source that is not one of the named VR buttons is read as an XInput bit
                // instead. No action handle is involved, so nothing has to be guessed about
                // which physical control the runtime decided to put it on.
                bool matched = false;
                for (size_t i = 0; i < count; ++i) {
                    if (from != table[i].name) {
                        continue;
                    }
                    Bound b{};
                    b.action = vr->get_action_handle(table[i].action);
                    b.left = table[i].left;
                    b.bit = bit;
                    b.clear = xinput_bit(from);   // it stops doing whatever it did before
                    matched = true;
                    if (b.action == nullptr) {
                        API::get()->log_error("[TasomachiVR] remap: %s did not resolve",
                                              table[i].action);
                    } else {
                        m_remaps.push_back(b);
                        API::get()->log_info("[TasomachiVR] remap: %s -> %s", from.c_str(),
                                             to.c_str());
                    }
                    break;
                }

                if (!matched && (from == "LTrigger" || from == "RTrigger")) {
                    matched = true;
                    Bound b{};
                    b.from_trigger = from == "LTrigger" ? 1 : 2;
                    b.bit = bit;
                    m_remaps.push_back(b);
                    API::get()->log_info("[TasomachiVR] remap: %s -> %s (trigger source)",
                                         from.c_str(), to.c_str());
                }

                if (!matched) {
                    const WORD src = xinput_bit(from);
                    if (src == 0) {
                        API::get()->log_error("[TasomachiVR] remap: '%s' is neither a VR "
                                              "button, an XInput button nor a trigger",
                                              from.c_str());
                        continue;
                    }
                    Bound b{};
                    b.from_bit = src;
                    b.clear = src;
                    b.bit = bit;
                    m_remaps.push_back(b);
                    API::get()->log_info("[TasomachiVR] remap: %s -> %s (xinput source)",
                                         from.c_str(), to.c_str());
                }
            }
        }

        // Read before anything is rewritten: an XInput-sourced remap looks at the bit it
        // came in on, and clearing as we go would erase the very thing being tested.
        const WORD raw = state->Gamepad.wButtons;

        // BOTH ENDS OF EVERY ENTRY ARE CLEARED BEFORE ANY IS SET.
        //
        // Clearing and setting entry by entry breaks as soon as two sources share one
        // destination - and two of them do here, since both triggers stand in for Run so that
        // one can hold it and the other tap it in mid-air. The second entry's clear would
        // erase the first one's contribution, and only one trigger would ever work.
        //
        // The source is cleared so it stops doing its old job; the destination so that it is
        // driven only from here, which is what makes this a move rather than a copy.
        for (const auto& b : m_remaps) {
            state->Gamepad.wButtons &= ~b.clear;
            state->Gamepad.wButtons &= ~b.bit;
        }

        for (auto& b : m_remaps) {
            bool held;
            if (b.from_trigger != 0) {
                const int pull = b.from_trigger == 1 ? m_left_trigger.load()
                                                     : m_right_trigger.load();
                held = pull > m_config.trigger_threshold;
            } else if (b.from_bit != 0) {
                held = (raw & b.from_bit) != 0;
            } else {
                const auto source = b.left ? vr->get_left_joystick_source()
                                           : vr->get_right_joystick_source();
                held = vr->is_action_active(b.action, source);
            }
            if (held) {
                state->Gamepad.wButtons |= b.bit;
            }

            // Every transition, for the first few. A button that never reports a release
            // looks exactly like this from the game's side: it fires once, the game sees the
            // press still held, and nothing works afterwards. That is precisely what the HUD
            // counters did when an analog action was read as "active", so it is worth
            // establishing rather than assuming a second time.
            if (held != b.was_held) {
                b.was_held = held;
                if (b.reports < 8) {
                    ++b.reports;
                    API::get()->log_info("[TasomachiVR] remap: source %s",
                                         held ? "PRESSED" : "released");
                }
            }
        }
    }

    bool interact_held() {
        const auto* vr = API::get()->param()->vr;
        if (vr == nullptr) {
            return false;
        }

        if (!m_interact_resolved) {
            m_interact_resolved = true;
            const char* path = nullptr;
            switch (m_config.interact_button) {
            case 1: path = "/actions/default/in/AButtonLeft";  m_interact_left = true;  break;
            case 2: path = "/actions/default/in/BButtonLeft";  m_interact_left = true;  break;
            case 3: path = "/actions/default/in/AButtonRight"; m_interact_left = false; break;
            case 4: path = "/actions/default/in/BButtonRight"; m_interact_left = false; break;
            default: return false;
            }
            m_interact_action = vr->get_action_handle(path);
            if (m_interact_action == nullptr) {
                API::get()->log_info("[TasomachiVR] interact action %s did not resolve", path);
            } else {
                API::get()->log_info("[TasomachiVR] Interact bound to %s", path);
            }
        }
        if (m_interact_action == nullptr) {
            return false;
        }

        const auto source = m_interact_left ? vr->get_left_joystick_source()
                                            : vr->get_right_joystick_source();
        return vr->is_action_active(m_interact_action, source);
    }

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
    // Heading from the headset's FORWARD VECTOR, not from an Euler decomposition.
    //
    // The obvious atan2(2(wy+xz), 1-2(y*y+z*z)) is the textbook yaw, and it is unusable
    // here: its denominator collapses towards zero as the pitch approaches vertical, so the
    // result is dominated by noise exactly when you tilt your head down to look at your
    // feet. The body then jitters wildly for no reason a player could guess at.
    //
    // Rotating (0,0,-1) and taking the heading of its horizontal part has no such weakness
    // - until the forward vector itself points straight up or down, where the horizontal
    // part vanishes. There the head's UP vector is the one lying flat, and it carries the
    // same heading, so it takes over. Between them there is always one good answer.
    static float quat_yaw(const UEVR_Quaternionf& q) {
        // Third column of the rotation matrix, negated: where the headset looks.
        const float fx = -2.0f * (q.x * q.z + q.w * q.y);
        const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));

        if (fx * fx + fz * fz > 0.05f) {
            return std::atan2(-fx, -fz) * kRadToDeg;
        }

        // Looking almost straight up or down: use the up vector instead.
        const float ux = 2.0f * (q.x * q.y - q.w * q.z);
        const float uz = 2.0f * (q.y * q.z + q.w * q.x);
        const float sign = fz > 0.0f ? -1.0f : 1.0f;   // flipped when looking upwards
        return std::atan2(-ux * sign, -uz * sign) * kRadToDeg;
    }

    // Every field NAMED, because UEVR_Quaternionf is declared { w, x, y, z } and not the
    // { x, y, z, w } that almost every other quaternion type uses.
    //
    // This was written with brace initialisation in x,y,z,w order, so w received the
    // computed x, x received y, and so on - the result was scrambled, and it feeds
    // hmd_quat_yaw and therefore the character's whole orientation. The same mistake in the
    // spin below turned a small correction into a constant 180 degree flip, which the axis
    // probe caught: "asked -1.18, moved -178.82".
    //
    // Positional initialisation of a struct whose field order is an assumption is the whole
    // bug. Naming them costs nothing and cannot be got wrong.
    static UEVR_Quaternionf quat_mul(const UEVR_Quaternionf& a, const UEVR_Quaternionf& b) {
        return UEVR_Quaternionf{
            .w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
    }

    // A rotation of `degrees` about the vertical, which is Y in this convention - the same
    // axis quat_yaw treats as up.
    static UEVR_Quaternionf yaw_quat(float degrees) {
        const float half = degrees * 0.5f * kDegToRad;
        return UEVR_Quaternionf{.w = std::cos(half), .x = 0.0f,
                                .y = std::sin(half), .z = 0.0f};
    }

    // The headset yaw WITH UEVR's rotation offset folded in.
    //
    // get_pose returns the raw stage-space pose - UEVR's own source multiplies the offset in
    // at the point of use rather than baking it into the pose. Reading the pose alone
    // therefore misses every recentre, and that is not a detail: the rendered view is built
    // WITH the offset while this value was computed without it, so the two drifted apart by
    // exactly the recentre angle. That is the whole of the "body ends up rotated ninety
    // degrees" bug, which I had put down to a side effect rather than a missing term.
    //
    // Everything downstream - the body's facing, the direction she walks, the interface
    // logic - is derived from this one number, so folding it in here fixes all of them at
    // once and makes recentring a coherent operation instead of a desynchronising one.
    float hmd_quat_yaw() {
        const auto* vr = API::get()->param()->vr;
        if (vr == nullptr) {
            return 0.0f;
        }

        UEVR_Vector3f pos{};
        UEVR_Quaternionf q{};
        vr->get_pose(vr->get_hmd_index(), &pos, &q);

        if (m_config.apply_rotation_offset) {
            const auto offset = API::VR::get_rotation_offset();
            q = quat_mul(offset, q);
        }
        return quat_yaw(q);
    }

    void update_final_yaw() {
        m_quat_yaw.store(hmd_quat_yaw());
        const float head = m_quat_yaw.load() * m_config.yaw_sign + m_snap_yaw.load()
                           + m_anchor_trim.load();
        m_final_yaw.store(normalize_deg(head + m_config.yaw_offset));
    }

    // Makes the body behave like an FPS: face where the player is looking, and strafe
    // or walk backwards rather than pivoting to face the direction of travel. Runs
    // after the game's tick, because applying it before means the Blueprint's own
    // camera handling (CamReset, SysCamAutoAdjust) overwrites it the same frame.
    void apply_body_orientation(float delta) {
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

            // Pitch stays flat: tipping the character because the player looked up is
            // exactly what breaks VR comfort. It also keeps the template's
            // camera-relative MoveForward from walking into the ground.
            // Smoothed, and it can afford to be: this yaw never reaches the view. The
            // stereo callback writes the view from the snap yaw and UEVR lays the headset
            // rotation on top, so filtering here settles the character's mesh and the
            // direction she walks in without adding any head latency at all.
            const float wanted = m_final_yaw.load();
            if (!m_have_applied_yaw) {
                m_applied_yaw = wanted;
                m_have_applied_yaw = true;
            } else {
                const float dt = delta > 0.0f ? delta : 0.016f;
                float a = dt * m_config.body_yaw_damping;
                a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
                // Along the shortest arc, so crossing 180 degrees does not send her the
                // long way round.
                m_applied_yaw = normalize_deg(
                    m_applied_yaw + normalize_deg(wanted - m_applied_yaw) * a);
            }

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
    // Writes the settings the page owns back into TasomachiVR.ini, in place.
    //
    // The file is REWRITTEN LINE BY LINE rather than regenerated: it is mostly comments
    // explaining why each number is what it is, and those are worth more than the numbers.
    // Only the keys listed here are touched; one that is missing is appended.
    void save_config() {
        const std::pair<const char*, std::string> owned[] = {
            {"TurnMode",        std::to_string(m_config.turn_mode)},
            {"SnapAngle",       format_number(m_config.snap_angle)},
            {"SmoothTurnSpeed", format_number(m_config.smooth_speed)},
            {"ForwardOffset",   format_number(m_config.forward_offset)},
            {"UpOffset",        format_number(m_config.up_offset)},
            {"ShipUpOffset",    format_number(m_config.ship_up_offset)},
            {"ShipForwardOffset", format_number(m_config.ship_forward_offset)},
            {"YawOffset",       format_number(m_config.yaw_offset)},
            {"BodyMode",        std::to_string(m_config.body_mode)},
            {"MenuSize",        format_number(m_config.menu_size)},
            {"HudAlwaysOn",     std::to_string(m_config.hud_always_on ? 1 : 0)},
            {"EyeAirLift",      format_number(m_config.eye_air_lift)},
            {"EyeAirForward",   format_number(m_config.eye_air_forward)},
            {"HeadHideLinger",  format_number(m_config.head_hide_linger)},
            {"Detail",          format_number(m_config.detail)},
            {"Supersample",     format_number(m_config.supersample)},
        };

        const auto path = settings_path();
        std::vector<std::string> lines;
        {
            std::ifstream in(path);
            if (!in) {
                return;
            }
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == 0x0D) {
                    line.pop_back();
                }
                lines.push_back(line);
            }
        }

        for (const auto& entry : owned) {
            const std::string prefix = std::string{entry.first} + "=";
            bool replaced = false;
            for (auto& line : lines) {
                if (line.rfind(prefix, 0) == 0) {
                    line = prefix + entry.second;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                lines.push_back(prefix + entry.second);
            }
        }

        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            API::get()->log_error("[TasomachiVR] could not write the settings file");
            return;
        }
        for (const auto& line : lines) {
            out << line << char(0x0A);
        }
        out.close();

        // Our own write must not look like someone editing the file, or the next reload
        // would re-parse what we just produced.
        std::error_code ec;
        m_config_stamp = std::filesystem::last_write_time(path, ec);
        API::get()->log_info("[TasomachiVR] settings saved");
    }

    // Rendering cvars, pushed only when a value actually changes.
    //
    // Console commands rather than the game's Engine.ini: they take effect immediately, so
    // both of these can be judged in the headset instead of over a restart, and nothing is
    // left behind in the game's own configuration when the mod is removed.
    //
    // r.StaticMeshLODDistanceScale is INVERTED - smaller means LODs are swapped later, so
    // more detail. The page offers "detail", the sane direction, and the reciprocal goes to
    // the cvar. foliage.LODDistanceScale runs the other way round, and gets the value as is.
    void push_render_settings() {
        if (std::fabs(m_config.detail - m_detail_applied) > 0.01f) {
            m_detail_applied = m_config.detail;
            const float scale = m_config.detail > 0.1f ? 1.0f / m_config.detail : 1.0f;
            wchar_t cmd[128]{};
            _snwprintf_s(cmd, _TRUNCATE, L"r.StaticMeshLODDistanceScale %.3f", scale);
            API::get()->execute_command(cmd);
            _snwprintf_s(cmd, _TRUNCATE, L"foliage.LODDistanceScale %.3f", m_config.detail);
            API::get()->execute_command(cmd);
            _snwprintf_s(cmd, _TRUNCATE, L"r.ViewDistanceScale %.3f", m_config.detail);
            API::get()->execute_command(cmd);
            API::get()->log_info("[TasomachiVR] render: detail %.1f (LOD scale %.2f)",
                                 m_config.detail, scale);
        }

        if (std::fabs(m_config.supersample - m_supersample_applied) > 0.5f) {
            m_supersample_applied = m_config.supersample;
            wchar_t cmd[128]{};
            _snwprintf_s(cmd, _TRUNCATE, L"r.ScreenPercentage %.0f", m_config.supersample);
            API::get()->execute_command(cmd);
            API::get()->log_info("[TasomachiVR] render: screen percentage %.0f",
                                 m_config.supersample);
        }

    }

    // Trimmed of trailing zeroes, so the file stays as readable as it was written.
    static std::string format_number(float v) {
        char buf[32]{};
        // Negative zero reaches here from a slider dragged through the middle, and
        // "-0" in a settings file reads like a mistake. Zero has one spelling.
        if (v == 0.0f) {
            return "0";
        }
        std::snprintf(buf, sizeof(buf), "%.4g", v);
        return buf;
    }

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

        // Said out loud, because load_config() overwrites EVERY setting from the file - so
        // any reload that fires after the player has changed something on the page silently
        // undoes it. The page writes on close; if a reload lands between the change and the
        // save, the change is gone and the slider "resets by itself".
        API::get()->log_info("[TasomachiVR] config file changed - reloading (air lift was "
                             "%.0f)", m_config.eye_air_lift);

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
            else if (key == "BodyYawDamping")
                m_config.body_yaw_damping = (float)std::atof(value.c_str());
            else if (key == "MenuSize")     m_config.menu_size = (float)std::atof(value.c_str());
            else if (key == "MenuDistance") m_config.menu_distance = (float)std::atof(value.c_str());
            else if (key == "HudFollowAngle")
                m_config.hud_follow_angle = (float)std::atof(value.c_str());
            else if (key == "HudFollowSpeed")
                m_config.hud_follow_speed = (float)std::atof(value.c_str());
            else if (key == "HudReleaseAngle")
                m_config.hud_release_angle = (float)std::atof(value.c_str());
            else if (key == "HudMaxTurn")
                m_config.hud_max_turn = (float)std::atof(value.c_str());
            else if (key == "ApplyRotationOffset")
                m_config.apply_rotation_offset = std::atoi(value.c_str()) != 0;
            else if (key == "MenuRecenter") m_config.menu_recenter = std::atoi(value.c_str()) != 0;
            else if (key == "MenuDeadzone")  m_config.menu_deadzone = (float)std::atof(value.c_str());
            else if (key == "MenuAxisRatio") m_config.menu_axis_ratio = (float)std::atof(value.c_str());
            else if (key == "MenuRepeatMs")  m_config.menu_repeat_ms = std::atoi(value.c_str());
            else if (key == "WriteViewRot")  m_config.write_view_rot = std::atoi(value.c_str()) != 0;
            else if (key == "ForwardOffset") m_config.forward_offset = (float)std::atof(value.c_str());
            else if (key == "UpOffset")      m_config.up_offset = (float)std::atof(value.c_str());
            else if (key == "ShipUpOffset")  m_config.ship_up_offset = (float)std::atof(value.c_str());
            else if (key == "ShipForwardOffset")
                m_config.ship_forward_offset = (float)std::atof(value.c_str());
            else if (key == "SnapTurn")      m_config.snap_turn = std::atoi(value.c_str()) != 0;
            else if (key == "ShipRoomscale") m_config.ship_roomscale = std::atoi(value.c_str()) != 0;
            else if (key == "ShipFollowTurn") m_config.ship_follow_turn = std::atoi(value.c_str()) != 0;
            else if (key == "PawnRecenter")  m_config.pawn_recenter = std::atoi(value.c_str()) != 0;
            else if (key == "ShipViewFollowMax")
                m_config.ship_view_follow_max = (float)std::atof(value.c_str());
            else if (key == "ShipHijack")    m_config.ship_hijack = std::atoi(value.c_str()) != 0;
            else if (key == "ShipStickSign")
                m_config.ship_stick_sign = (float)std::atof(value.c_str());
            else if (key == "ShipCarriesView")
                m_config.ship_carries_view = std::atoi(value.c_str()) != 0;
            else if (key == "UevrRoomscale") m_config.uevr_roomscale = std::atoi(value.c_str()) != 0;
            else if (key == "SnapAngle")     m_config.snap_angle = (float)std::atof(value.c_str());
            else if (key == "SnapThreshold") m_config.snap_threshold = (float)std::atof(value.c_str());
            else if (key == "SnapRelease")   m_config.snap_release = (float)std::atof(value.c_str());
            else if (key == "SnapCooldown")  m_config.snap_cooldown = (float)std::atof(value.c_str());
            else if (key == "TurnMode")      m_config.turn_mode = std::atoi(value.c_str());
            else if (key == "SmoothTurnSpeed") m_config.smooth_speed = (float)std::atof(value.c_str());
            else if (key == "TurnDeadzone")  m_config.turn_deadzone = (float)std::atof(value.c_str());
            else if (key == "PauseButton")   m_config.pause_button = std::atoi(value.c_str());
            else if (key == "GraftPauseMenu")
                m_config.graft_pause_menu = std::atoi(value.c_str()) != 0;
            else if (key == "BodyMode")      m_config.body_mode = std::atoi(value.c_str());
            else if (key == "HeadHideAirOnly")
                m_config.head_hide_air_only = std::atoi(value.c_str()) != 0;
            else if (key == "HeadHideLinger")
                m_config.head_hide_linger = (float)std::atof(value.c_str());
            else if (key == "SwapSticks")    m_config.swap_sticks = std::atoi(value.c_str()) != 0;
            else if (key == "InteractButton") m_config.interact_button = std::atoi(value.c_str());
            else if (key == "HudCounters")   m_config.hud_counters = value;
            else if (key == "HudCounterFade")
                m_config.hud_counter_fade = (float)std::atof(value.c_str());
            else if (key == "HudAlwaysOn")   m_config.hud_always_on = std::atoi(value.c_str()) != 0;
            else if (key == "EyeAirLift")
                m_config.eye_air_lift = (float)std::atof(value.c_str());
            else if (key == "FreeCameraPawns") m_config.free_camera_pawns = value;
            else if (key == "ButtonRemap")   m_config.button_remap = value;
            else if (key == "ButtonEvent")    m_config.button_event = value;
            else if (key == "EventActorClass") m_config.event_actor_class = value;
            else if (key == "PoseButton")    m_config.pose_button = value;
            else if (key == "PosePrefix")    m_config.pose_prefix = value;
            else if (key == "BlockTriggers") m_config.block_triggers = std::atoi(value.c_str()) != 0;
            else if (key == "HudRevealSource")
                m_config.hud_reveal_source = std::atoi(value.c_str());
            else if (key == "TriggerThreshold")
                m_config.trigger_threshold = std::atoi(value.c_str());
            else if (key == "HeadReveal")    m_config.head_reveal = (float)std::atof(value.c_str());
            else if (key == "Detail")        m_config.detail = (float)std::atof(value.c_str());
            else if (key == "Supersample")   m_config.supersample = (float)std::atof(value.c_str());
            else if (key == "EyeAirForward")
                m_config.eye_air_forward = (float)std::atof(value.c_str());
            else if (key == "EyeAirLiftSpeed")
                m_config.eye_air_lift_speed = (float)std::atof(value.c_str());
            else if (key == "LogEvery")      m_config.log_every = std::atoi(value.c_str());
        }
    }

    Config m_config{};

    Body      m_body{};
    Poses     m_poses{};
    VrPage m_vrpage{};
    float  m_widget_time{0.0f};

    std::atomic<bool>  m_gameplay{false};
    // Photo mode, as far as everything outside refresh_pawn is concerned: a pawn that aims
    // its own camera is up. Atomic because the input thread reads it to decide whether the
    // pose button is ours this frame.
    std::atomic<bool>  m_free_camera{false};
    // Mirrors m_is_character for the XInput callback, which runs on another thread.
    std::atomic<bool>  m_on_foot{false};
    int  m_stick_reports{0};
    bool m_roomscale_on{true};
    int  m_recenter_wait{0};
    int  m_ship_log{0};
    // Presses counted on the input thread, spent on the game thread. A counter rather than
    // a flag so a quick double tap moves two poses instead of one.
    std::atomic<unsigned> m_pose_requests{0};
    UEVR_ActionHandle m_pose_action{};
    bool m_pose_resolved{false};
    bool m_pose_left{false};
    WORD m_pose_bit{0};
    bool m_pose_was_held{false};
    std::atomic<float> m_snap_yaw{0.0f};
    std::atomic<float> m_turn_axis{0.0f};
    std::atomic<float> m_quat_yaw{0.0f};
    // Cancels the anchor rotation for the view and the body, and for nothing else.
    std::atomic<float> m_cam[3]{};
    std::atomic<bool> m_cam_known{false};
    // A remap source is either a VR action (a face button, whose physical bit is the
    // runtime's business) or an XInput bit of its own - which is what the grips are, since
    // they reach us already translated to the shoulder buttons.
    struct Bound { UEVR_ActionHandle action; bool left; WORD bit; WORD clear;
                   WORD from_bit; int from_trigger; bool was_held; int reports; };
    std::vector<Bound> m_remaps{};
    bool m_remaps_parsed{false};
    // A button that calls a Blueprint event directly. The press is noticed on the input
    // thread and the call is made on the game thread, which is the only one allowed to
    // touch a UObject - hence the bitmask handing the edge across.
    // One boolean held at a chosen value for the length of one call, then put back.
    //
    //   name        the property, on the player character or on the event actor
    //   value       what to hold it at - false to lift a "player is busy" guard, true to
    //               satisfy one the game has not granted yet
    //   cond_*      the ONLY circumstance this is allowed in. Empty means always, which is
    //               a decision to make deliberately: these flags exist for a reason and
    //               suspending one everywhere re-enables things the game meant to withhold.
    struct Bypass {
        bool on_system{false};
        std::wstring name;
        bool value{false};
        // Substring the character's current animation must contain. Lasts as long as the
        // pose does, unlike the transition flags on the interactable itself.
        std::string cond_anim;
    };

    // from_trigger: 0 none, 1 left, 2 right. A trigger is an analogue byte rather than a
    // bit, so it cannot go through xinput_bit and needs a threshold of its own.
    struct EventBind { UEVR_ActionHandle action; bool left; WORD from_bit; int from_trigger;
                       bool on_pawn; std::wstring fn;
                       bool was_held; int reports;
                       std::vector<Bypass> bypass; };
    std::vector<EventBind> m_events{};
    bool m_events_parsed{false};
    std::atomic<uint32_t> m_event_pending{0};
    int m_event_misses{0};
    int m_bypass_reports{0};
    int m_anim_reports{0};
    std::atomic<uint16_t> m_raw_buttons{0};
    std::atomic<uint16_t> m_event_source_mask{0};
    std::atomic<int> m_left_trigger{0};
    std::atomic<int> m_right_trigger{0};
    std::atomic<bool> m_left_grip{false};
    bool m_head_shown{false};
    float m_detail_applied{-1.0f};
    float m_supersample_applied{-1.0f};
    std::atomic<float> m_air_blend{0.0f};
    float m_air_linger{0.0f};
    std::atomic<float> m_anchor_trim{0.0f};
    std::atomic<float> m_final_yaw{0.0f};
    // The yaw actually written to the pawn, held still while the headset only jitters.
    // How far the pre-tick got. 9 means it ran to the end; anything less, held across
    // frames, is the phase it dies in.
    static constexpr int kPhases = 9;
    int m_phase{0};
    int m_phase_max{0};
    bool m_phase_disabled[kPhases]{};
    int m_fault_phase{-1};
    unsigned long m_fault_code{0};
    bool m_menu_armed{true};
    int  m_menu_axis{-1};
    bool m_recentred{false};
    bool m_menu_was_visible{false};
    bool  m_post_faulted{false};
    bool  m_ui_follow_applied{false};
    bool  m_ui_follow_forced{false};
    bool  m_hud_easing{false};
    float m_hud_last_away{0.0f};
    float m_hud_turned{0.0f};
    float m_hud_start_away{0.0f};
    int   m_hud_broken{0};
    float m_hud_elapsed{0.0f};
    float m_ui_size_applied{-1.0f};
    std::chrono::steady_clock::time_point m_menu_last{};
    float m_applied_yaw{0.0f};
    bool  m_have_applied_yaw{false};

    UEVR_ActionHandle m_interact_action{};
    std::vector<API::UObject*> m_counters{};
    int   m_counter_rescan{0};
    size_t m_counters_last{999};
    float m_counter_alpha{0.0f};
    float m_counter_shown{-1.0f};
    bool m_interact_resolved{false};
    bool m_interact_left{false};
    UEVR_ActionHandle m_pause_action{};
    bool m_pause_resolved{false};
    bool m_pause_ok{false};
    bool m_pause_seen{false};

    API::UObject* m_pc{nullptr};
    API::UObject* m_pawn{nullptr};
    // The pawn's full name, instance number included - what makes a recycled address visible.
    std::wstring m_pawn_id{};
    API::UObject* m_cmc{nullptr};
    // The last pawn that was actually the player character; see refresh_pawn.
    API::UObject* m_character{nullptr};
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
