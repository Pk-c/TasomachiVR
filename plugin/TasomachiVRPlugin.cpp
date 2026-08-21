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

#include "settings.hpp"
#include "umg.hpp"
#include "vrpage.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using API = uevr::API;
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
    bool  write_view_rot = true;
    // Pushes the eye out of the skull. Both are hot-reloaded from the ini.
    float forward_offset = 10.0f;
    float up_offset      = 0.0f;

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

    // Reuse the game's own options page as the VR menu. Five of its rows are camera
    // settings that VR makes meaningless - we own the camera, so nothing reads them any
    // more - and they are already styled, already navigable, already saved by the game.
    // The slider becomes the snap angle, the X-invert checkbox the turn mode.
    // Note: the game persists these in its own settings save, so they survive without
    // TasomachiVR.ini having anything to say about it.
    bool  graft_pause_menu = false;

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

    void on_pre_engine_tick(API::UGameEngine*, float delta) override {
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

        refresh_pawn();
        m_gameplay.store(compute_gameplay());

        drive_vr_page();

        if (m_config.widget_probe) {
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

    void on_post_engine_tick(API::UGameEngine*, float) override {
        if (!m_gameplay.load()) {
            return;
        }

        update_final_yaw();

        // On foot only. The boat is not a Character and steers itself; see the file
        // header for why writing a body yaw there is wrong rather than merely useless.
        if (m_is_character) {
            apply_body_orientation();
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

        if (user_index == 0 && pause_button_held()) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_START;
        }
    }

    void on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int, float,
                                             UEVR_Vector3f*, UEVR_Rotatorf* rotation,
                                             bool) override {
        if (rotation == nullptr || !m_config.write_view_rot || !m_gameplay.load()) {
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

        // The page only writes while it is open, so the ini still governs the rest of
        // the time.
        m_vrpage.update(API::get()->get_local_pawn(0), s);

        m_config.turn_mode      = s.turn_mode;
        m_config.snap_angle     = s.snap_angle;
        m_config.smooth_speed   = s.smooth_speed;
        m_config.forward_offset = s.forward_offset;
        m_config.up_offset      = s.up_offset;
        m_config.yaw_offset     = s.yaw_offset;
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
            // Pitch stays flat: tipping the character because the player looked up is
            // exactly what breaks VR comfort. It also keeps the template's
            // camera-relative MoveForward from walking into the ground.
            m_control_rotation->pitch = 0.0f;
            m_control_rotation->yaw = m_final_yaw.load();
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
            m_cmc->set_bool_property(L"bUseControllerDesiredRotation", true);
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
            "character=%d playable=%d control=%d cmc=%d pause=%d/%d | pawn=%s",
            m_quat_yaw.load(), m_snap_yaw.load(), m_final_yaw.load(), m_snap_count,
            (int)m_is_character, (int)m_pawn_playable, (int)m_control_ok,
            (int)(m_cmc != nullptr), (int)m_pause_ok, (int)m_pause_seen,
            narrow(m_pawn_name).c_str());
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
            else if (key == "GraftPauseMenu")
                m_config.graft_pause_menu = std::atoi(value.c_str()) != 0;
            else if (key == "LogEvery")      m_config.log_every = std::atoi(value.c_str());
        }
    }

    Config m_config{};

    Umg    m_umg{};
    VrPage m_vrpage{};
    float  m_widget_time{0.0f};

    std::atomic<bool>  m_gameplay{false};
    std::atomic<float> m_snap_yaw{0.0f};
    std::atomic<float> m_turn_axis{0.0f};
    std::atomic<float> m_quat_yaw{0.0f};
    std::atomic<float> m_final_yaw{0.0f};

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
