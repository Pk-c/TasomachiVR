#include "vrpage.hpp"

#include "ucall.hpp"

#include <cmath>
#include <cstdio>

namespace tasomachivr {
namespace {

using API = uevr::API;
namespace uc = ucall;

constexpr uint8_t kVisible = 4; // SelfHitTestInvisible, what the game's own pages use
constexpr uint8_t kHidden = 2;

constexpr const wchar_t* kTextBlock = L"Class /Script/UMG.TextBlock";
constexpr const wchar_t* kButton = L"Class /Script/UMG.Button";
constexpr const wchar_t* kSlider = L"Class /Script/UMG.Slider";
constexpr const wchar_t* kCheckBox = L"Class /Script/UMG.CheckBox";
constexpr const wchar_t* kVerticalBox = L"Class /Script/UMG.VerticalBox";
constexpr const wchar_t* kHorizontalBox = L"Class /Script/UMG.HorizontalBox";

// UE layout structs, only ever written into a parameter blob at an offset the engine
// itself reported.
struct Vec2 {
    float x{};
    float y{};
};

struct Anchors {
    Vec2 minimum{};
    Vec2 maximum{};
};

struct Margin {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

// FSlateChildSize is { float Value; TEnumAsByte<ESlateSizeRule> SizeRule; }, padded to 8.
struct ChildSize {
    float value{};
    uint8_t rule{};   // 0 Automatic, 1 Fill
    uint8_t pad[3]{};
};

// Gives a CanvasPanelSlot real geometry. Anchors as a proportional rectangle with zero
// offsets means the page occupies that fraction of the screen whatever the resolution,
// which is what the game's own pages do.
void place_on_canvas(API::UObject* slot, float left, float top, float right, float bottom) {
    if (slot == nullptr) {
        API::get()->log_error("[TasomachiVR] VRPAGE | canvas slot is null");
        return;
    }
    const Anchors anchors{{left, top}, {right, bottom}};
    const bool a = uc::call_one(slot, L"SetAnchors", anchors);
    const bool o = uc::call_one(slot, L"SetOffsets", Margin{});
    const bool g = uc::call_one(slot, L"SetAlignment", Vec2{0.0f, 0.0f});
    const bool z = uc::call_one(slot, L"SetAutoSize", false);
    // Above the game's own pages and its background blur, which sit at the default 0.
    const bool k = uc::call_one(slot, L"SetZOrder", int32_t{50});

    // Reported because a slot call that does not exist under this name would leave the
    // page geometry-less again, and that failure is otherwise invisible.
    API::get()->log_info("[TasomachiVR] VRPAGE | canvas slot %s : anchors=%d offsets=%d "
                         "alignment=%d autosize=%d zorder=%d",
                         uc::class_name(slot).c_str(), (int)a, (int)o, (int)g, (int)z, (int)k);
}

void pad_slot(API::UObject* slot, float horizontal, float vertical) {
    if (slot == nullptr) {
        return;
    }
    uc::call_one(slot, L"SetPadding", Margin{horizontal, vertical, horizontal, vertical});
}

// Fill makes a child take a share of the box instead of collapsing to its desired size,
// which for a freshly spawned Slider is close to nothing.
bool fill_slot(API::UObject* slot, float share, float left_gap = 0.0f) {
    if (slot == nullptr) {
        return false;
    }
    const bool sized = uc::call_one(slot, L"SetSize", ChildSize{share, 1});
    // EVerticalAlignment: 0 Fill, 1 Top, 2 Center, 3 Bottom.
    uc::call_one(slot, L"SetVerticalAlignment", uint8_t{2});
    if (left_gap > 0.0f) {
        uc::call_one(slot, L"SetPadding", Margin{left_gap, 0.0f, 0.0f, 0.0f});
    }
    return sized;
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Slider values live in 0..1; the row carries the real range.
float from_slider(const float normalised, const float lo, const float hi, const float step) {
    const float raw = lo + clampf(normalised, 0.0f, 1.0f) * (hi - lo);
    return step > 0.0f ? std::round(raw / step) * step : raw;
}

float to_slider(const float value, const float lo, const float hi) {
    return hi > lo ? clampf((value - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
}

// Through GetValue(), not through the Value property. Dragging a slider updates the
// underlying Slate widget, and USlider::GetValue is what reads back from it - the
// UPROPERTY keeps whatever it was last set to. Reading the property is why nothing moved
// while the checkbox worked: UCheckBox does write CheckedState back, USlider does not.
float slider_value(API::UObject* slider) {
    if (slider == nullptr) {
        return 0.0f;
    }
    uc::Call call{slider, L"GetValue"};
    if (call.ok) {
        slider->process_event(call.fn, call.bytes.data());
        float value = 0.0f;
        if (uc::result(call, value)) {
            return value;
        }
    }
    auto* fallback = slider->get_property_data<float>(L"Value");
    return fallback != nullptr ? *fallback : 0.0f;
}

void set_slider(API::UObject* slider, float value) {
    uc::Call call{slider, L"SetValue"};
    if (call.ok && uc::put(call, 0, value)) {
        slider->process_event(call.fn, call.bytes.data());
    }
}

// FLinearColor, written into a parameter blob like every other struct here.
struct Colour {
    float r{};
    float g{};
    float b{};
    float a{1.0f};
};

// The engine's default slider is dark grey on a dark menu, which is most of why nothing
// looked selected. A bright handle also makes the drag target obvious in a headset,
// where fine contrast is the first thing lost.
void style_slider(API::UObject* slider) {
    uc::call_one(slider, L"SetSliderBarColor", Colour{0.25f, 0.28f, 0.34f, 1.0f});
    uc::call_one(slider, L"SetSliderHandleColor", Colour{1.0f, 0.78f, 0.25f, 1.0f});
}

// Makes a slider move in real increments instead of sweeping continuously. StepSize is
// in the slider's own 0..1 space, so the row's step has to be normalised into it.
//
// RequiresControllerLock defaults to true, which means the pad has to "lock" the slider
// before left and right do anything - that is most of what felt awkward. These are plain
// bools rather than bitfields on USlider, so writing the property directly is safe, and
// the setters are tried first anyway.
void make_stepped(API::UObject* slider, float step, float lo, float hi) {
    if (slider == nullptr || hi <= lo) {
        return;
    }
    const float normalised = step / (hi - lo);

    if (!uc::call_one(slider, L"SetStepSize", normalised)) {
        if (auto* value = slider->get_property_data<float>(L"StepSize")) {
            *value = normalised;
        }
    }
    if (auto* lock = slider->get_property_data<bool>(L"RequiresControllerLock")) {
        *lock = false;
    }
    if (auto* mouse_step = slider->get_property_data<bool>(L"MouseUsesStep")) {
        *mouse_step = true;
    }
}

bool checkbox_checked(API::UObject* box) {
    if (box == nullptr) {
        return false;
    }
    auto* state = box->get_property_data<uint8_t>(L"CheckedState");
    return state != nullptr && *state == 1;
}

void set_checkbox(API::UObject* box, bool checked) {
    uc::Call call{box, L"SetIsChecked"};
    if (call.ok && uc::put(call, 0, checked)) {
        box->process_event(call.fn, call.bytes.data());
    }
}

// The pause menu is driven by Slate's focus navigation from the gamepad, not by a
// cursor - measured: the controller reaches these widgets, a mouse does nothing. So the
// missing piece was never navigation, it was showing WHICH widget holds the focus, since
// engine-default styles barely distinguish it.
bool has_focus(API::UObject* widget) {
    if (widget == nullptr) {
        return false;
    }
    uc::Call call{widget, L"HasKeyboardFocus"};
    if (!call.ok) {
        return false;
    }
    widget->process_event(call.fn, call.bytes.data());
    bool focused = false;
    uc::result(call, focused);
    return focused;
}

// The game's own rows are the reference: to_continue is a named variable on the pause
// menu, so its Button style and its label's font and colour can be lifted straight off
// it. Borrowing beats inventing - the page then matches whatever the game does, in any
// language, at any UI scale.
struct Reference {
    API::UObject* button{nullptr};
    API::UObject* text{nullptr};
};

Reference reference_style(API::UObject* menu) {
    Reference out{};
    out.button = uc::property_object(menu, L"to_continue");
    out.text = uc::child_at(out.button, 0);
    return out;
}

bool apply_button_style(const Reference& from, API::UObject* button) {
    return uc::copy_struct_arg(from.button, L"WidgetStyle", button, L"SetStyle");
}

bool apply_text_style(const Reference& from, API::UObject* text) {
    const bool font = uc::copy_struct_arg(from.text, L"Font", text, L"SetFont");
    const bool colour =
        uc::copy_struct_arg(from.text, L"ColorAndOpacity", text, L"SetColorAndOpacity");
    return font && colour;
}

void take_focus(API::UObject* widget) {
    if (widget == nullptr) {
        return;
    }
    uc::Call call{widget, L"SetKeyboardFocus"};
    if (call.ok) {
        widget->process_event(call.fn, call.bytes.data());
    }
}

} // namespace

bool VrPage::game_menu_visible() const {
    if (m_menu == nullptr) {
        return false;
    }
    uc::Call call{m_menu, L"IsVisible"};
    if (!call.ok) {
        return false;
    }
    m_menu->process_event(call.fn, call.bytes.data());
    bool visible = false;
    uc::result(call, visible);
    return visible;
}

void VrPage::update(API::UObject* pawn, MenuSettings& live) {
    auto* menu = uc::property_object(pawn, L"PauseMenu");
    if (menu == nullptr) {
        return;
    }

    // A fresh instance means our widgets went with the old one.
    if (menu != m_menu) {
        m_menu = menu;
        m_entry = m_page = m_back = m_main_box = nullptr;
        m_open = false;
        m_entry_was_pressed = m_back_was_pressed = false;
        for (auto& row : m_rows) {
            row.text = row.control = nullptr;
            row.shown = -99999.0f;
        }
        if (!build(menu)) {
            m_menu = nullptr; // try again next tick rather than give up on this session
            return;
        }
        sync_from(live);
        m_needs_sync = false;
    }

    if (m_needs_sync) {
        m_needs_sync = false;
        sync_from(live);
    }

    poll(live);
}

bool VrPage::build(API::UObject* menu) {
    auto* tree = uc::property_object(menu, L"WidgetTree");
    auto* root = uc::property_object(tree, L"RootWidget");
    m_main_box = uc::property_object(menu, L"MainMenuBox");
    if (tree == nullptr || root == nullptr || m_main_box == nullptr) {
        return false;
    }

    // The six existing rows live two levels down: MainMenuBox -> Button -> VerticalBox.
    // Those intermediate names are auto-generated and are not variables on the widget,
    // so the hops go through GetChildAt.
    auto* list = uc::child_at(uc::child_at(m_main_box, 0), 0);
    if (list == nullptr) {
        return false;
    }

    const Reference reference = reference_style(menu);

    // --- the entry in the main list ---------------------------------------------
    m_entry = uc::spawn(kButton, tree);
    auto* entry_label = uc::spawn(kTextBlock, tree);
    if (m_entry == nullptr || entry_label == nullptr) {
        return false;
    }
    uc::add_child(m_entry, entry_label);
    uc::set_text(entry_label, L"VR SETTINGS");
    pad_slot(uc::add_child(list, m_entry), 0.0f, 4.0f);
    const bool styled_button = apply_button_style(reference, m_entry);
    const bool styled_text = apply_text_style(reference, entry_label);

    // --- our page, a sibling of the game's own pages ------------------------------
    m_page = uc::spawn(kVerticalBox, tree);
    if (m_page == nullptr) {
        return false;
    }
    place_on_canvas(uc::add_child(root, m_page), 0.30f, 0.18f, 0.70f, 0.86f);
    uc::set_visibility(m_page, kHidden);

    auto* title = uc::spawn(kTextBlock, tree);
    if (title != nullptr) {
        pad_slot(uc::add_child(m_page, title), 0.0f, 10.0f);
        apply_text_style(reference, title);
        uc::set_text(title, L"VR SETTINGS");
    }

    static const struct {
        RowId id;
        const wchar_t* label;
        const wchar_t* unit;
        float lo;
        float hi;
        float step;
        bool toggle;
    } layout[] = {
        {SnapAngle,   L"Snap angle",   L"deg",   15.0f,  90.0f, 5.0f,  false},
        {SmoothTurn,  L"Smooth turn",  nullptr,   0.0f,   1.0f, 1.0f,  true},
        {SmoothSpeed, L"Turn speed",   L"deg/s", 30.0f, 240.0f, 5.0f,  false},
        {EyeForward,  L"Eye forward",  L"cm",   -10.0f,  40.0f, 1.0f,  false},
        {EyeHeight,   L"Eye height",   L"cm",   -15.0f,  15.0f, 1.0f,  false},
        {ShipHeight,  L"Ship height",  L"cm",   -80.0f,  20.0f, 2.0f,  false},
        {ShowBody,    L"Show body",    nullptr,   0.0f,   1.0f, 1.0f,  true},
        {MenuScale,   L"Menu size",    nullptr,   0.6f,   2.5f, 0.1f,  false},
        {HudAlways,   L"HUD always on", nullptr,  0.0f,   1.0f, 1.0f,  true},
        {AirLift,     L"Jump lift",    L"cm",     0.0f,  80.0f, 2.0f,  false},
        {AirForward,  L"Jump forward", L"cm",   -40.0f,  60.0f, 2.0f,  false},
        {HeadLinger,  L"Head hide after", L"s",   0.0f,   1.5f, 0.1f,  false},
        {Detail,      L"LOD detail",   nullptr,   1.0f,   4.0f, 0.5f,  false},
        {Supersample, L"Supersample",  L"%",     70.0f, 200.0f, 5.0f,  false},
    };

    for (const auto& spec : layout) {
        auto* line = uc::spawn(kHorizontalBox, tree);
        auto* label = uc::spawn(kTextBlock, tree);
        auto* control = uc::spawn(spec.toggle ? kCheckBox : kSlider, tree);
        if (line == nullptr || label == nullptr || control == nullptr) {
            return false;
        }

        // The label takes a third, the control the rest: a slider needs real width to
        // be draggable, and Automatic sizing would give it almost none.
        const bool label_sized = fill_slot(uc::add_child(line, label), 1.0f);
        auto* control_slot = uc::add_child(line, control);
        const bool control_sized = fill_slot(control_slot, 2.0f, 28.0f);
        pad_slot(uc::add_child(m_page, line), 12.0f, 6.0f);

        if (!m_logged) {
            API::get()->log_info("[TasomachiVR] VRPAGE | row %s : slot=%s label_sized=%d "
                                 "control_sized=%d",
                                 uc::narrow(spec.label).c_str(),
                                 uc::class_name(control_slot).c_str(), (int)label_sized,
                                 (int)control_sized);
        }

        if (!spec.toggle) {
            style_slider(control);
            make_stepped(control, spec.step, spec.lo, spec.hi);
        }

        auto& row = m_rows[spec.id];
        row.label = spec.label;
        row.unit = spec.unit;
        row.text = label;
        row.control = control;
        row.lo = spec.lo;
        row.hi = spec.hi;
        row.step = spec.step;
        row.is_toggle = spec.toggle;
        apply_text_style(reference, label);
        uc::set_text(label, spec.label);
    }

    // --- back to the main list ----------------------------------------------------
    m_back = uc::spawn(kButton, tree);
    m_back_label = uc::spawn(kTextBlock, tree);
    if (m_back != nullptr && m_back_label != nullptr) {
        uc::add_child(m_back, m_back_label);
        apply_button_style(reference, m_back);
        apply_text_style(reference, m_back_label);
        uc::set_text(m_back_label, L"   RETURN");
        pad_slot(uc::add_child(m_page, m_back), 12.0f, 14.0f);
    }

    if (!m_logged) {
        m_logged = true;
        API::get()->log_info("[TasomachiVR] VRPAGE | built into %s: entry, %d rows, back=%d "
                             "| style source=%s/%s button=%d text=%d",
                             uc::object_name(menu).c_str(), (int)RowCount,
                             (int)(m_back != nullptr), uc::class_name(reference.button).c_str(),
                             uc::class_name(reference.text).c_str(), (int)styled_button,
                             (int)styled_text);
    }
    return true;
}

// A label only carries a number when it has one, and is only rewritten when that number
// changes: set_text leaks one FText per call, so refreshing every frame would leak
// steadily. Per change it is a handful of small allocations across a session.
void VrPage::refresh_label(Row& row, float value, bool focused) {
    if (row.text == nullptr || (value == row.shown && focused == row.focused)) {
        return;
    }
    const bool first = row.shown < -90000.0f;
    row.shown = value;
    row.focused = focused;

    // A caret rather than a colour: FSlateColor is a struct with a styling-mode enum, and
    // a marker in the text is unmistakable in a headset where fine contrast is the first
    // thing lost. Costs the same single rewrite either way.
    const wchar_t* marker = focused ? L" > " : L"   ";

    wchar_t buffer[128]{};
    if (row.is_toggle) {
        _snwprintf_s(buffer, _TRUNCATE, L"%s%s", marker, row.label);
    } else {
        // Decimals follow the step: a row that moves in tenths has to show tenths, or every
        // second nudge appears to do nothing and the value looks stuck.
        const wchar_t* fmt_unit = row.step < 1.0f ? L"%s%s   %.1f %s" : L"%s%s   %.0f %s";
        const wchar_t* fmt_bare = row.step < 1.0f ? L"%s%s   %.1f" : L"%s%s   %.0f";
        if (row.unit != nullptr) {
            _snwprintf_s(buffer, _TRUNCATE, fmt_unit, marker, row.label, value, row.unit);
        } else {
            _snwprintf_s(buffer, _TRUNCATE, fmt_bare, marker, row.label, value);
        }
    }
    uc::set_text(row.text, buffer);

    if (first) {
        API::get()->log_info("[TasomachiVR] VRPAGE | %s first read = %.1f",
                             uc::narrow(row.label).c_str(), value);
    }
}

// Focus has to be moved deliberately. Hiding MainMenuBox while the focus still sat on
// our entry button left Slate navigating from an invisible widget, which is why RETURN
// could not be reached.
void VrPage::open_page(bool open) {
    m_open = open;
    // Both edge detectors have to be disarmed here. Closing used to leave
    // m_back_was_pressed true, so the tick after a reopen read "released while pressed"
    // and shut the page again in the same frame - which looked exactly like the entry
    // button having stopped working.
    m_entry_was_pressed = false;
    m_back_was_pressed = false;
    uc::set_visibility(m_page, open ? kVisible : kHidden);
    uc::set_visibility(m_main_box, open ? kHidden : kVisible);
    take_focus(open ? m_rows[SnapAngle].control : m_entry);
}

// Pushes the settings onto the widgets, recording what was pushed so poll() can tell "the
// player moved this" from "this widget is new and reads whatever it defaulted to".
//
// Called on a fresh pause-menu instance AND every time the page is opened. The second is
// not redundant: the game builds a new WBP_PauseMenu each time you pause, so our sliders
// are new widgets every time.
void VrPage::sync_from(const MenuSettings& live) {
    const auto push = [this](RowId id, float value) {
        auto& row = m_rows[id];
        const float n = to_slider(value, row.lo, row.hi);
        set_slider(row.control, n);
        row.pushed = n;
    };

    push(SnapAngle, live.snap_angle);
    push(SmoothSpeed, live.smooth_speed);
    push(EyeForward, live.forward_offset);
    push(EyeHeight, live.up_offset);
    push(ShipHeight, live.ship_up_offset);
    push(MenuScale, live.menu_size);
    push(AirLift, live.air_lift);
    push(AirForward, live.air_forward);
    push(HeadLinger, live.head_hide_linger);
    push(Detail, live.detail);
    push(Supersample, live.supersample);

    // Recorded like the sliders. The checkboxes had no such protection: they were read
    // unconditionally, so a freshly rebuilt one - the game builds a new pause menu every
    // time it opens - could report its default and quietly turn a setting off.
    const auto check = [this](RowId id, bool on) {
        set_checkbox(m_rows[id].control, on);
        m_rows[id].pushed = on ? 1.0f : 0.0f;
    };
    check(SmoothTurn, live.turn_mode == 1);
    check(ShowBody, live.body_mode == 1);
    check(HudAlways, live.hud_always_on);
}

void VrPage::poll(MenuSettings& live) {
    // Edge-detected: IsPressed stays true for as long as the button is held.
    const bool entry_pressed = uc::is_pressed(m_entry);
    if (m_entry_was_pressed && !entry_pressed) {
        open_page(true);
        m_needs_sync = true;
        m_entry_was_pressed = entry_pressed;
        // Nothing is read on the tick the page opens. poll() used to carry on past this
        // point and pull values out of widgets that sync_from had not reached yet - one
        // frame of reading whatever they happened to hold, straight into the settings.
        return;
    }
    m_entry_was_pressed = entry_pressed;

    if (!m_open) {
        return;
    }

    const bool back_pressed = uc::is_pressed(m_back);
    if (m_back_was_pressed && !back_pressed) {
        open_page(false);
        m_closed = true;
        return;
    }
    m_back_was_pressed = back_pressed;

    const bool back_focused = has_focus(m_back);
    if (back_focused != m_back_focused) {
        m_back_focused = back_focused;
        uc::set_text(m_back_label, back_focused ? L" > RETURN" : L"   RETURN");
    }

    // Only a widget the player actually moved may change a setting; everything else keeps
    // the value in force and is corrected back on screen. This is the fix for settings that
    // "worked, then came back to default": reading the sliders unconditionally let a freshly
    // rebuilt one overwrite a real choice with a default nobody made.
    const auto read = [this](RowId id, float current) {
        auto& row = m_rows[id];
        const float raw = slider_value(row.control);
        float value = current;
        if (row.pushed < 0.0f || std::fabs(raw - row.pushed) > 0.0005f) {
            value = from_slider(raw, row.lo, row.hi, row.step);
            row.pushed = raw;
        } else {
            const float want = to_slider(current, row.lo, row.hi);
            if (std::fabs(want - raw) > 0.0005f) {
                set_slider(row.control, want);
                row.pushed = want;
            }
        }
        refresh_label(row, value, has_focus(row.control));
        return value;
    };

    live.snap_angle = read(SnapAngle, live.snap_angle);
    live.smooth_speed = read(SmoothSpeed, live.smooth_speed);
    live.forward_offset = read(EyeForward, live.forward_offset);
    live.up_offset = read(EyeHeight, live.up_offset);
    live.ship_up_offset = read(ShipHeight, live.ship_up_offset);
    live.menu_size = read(MenuScale, live.menu_size);
    live.air_lift = read(AirLift, live.air_lift);
    live.air_forward = read(AirForward, live.air_forward);
    live.head_hide_linger = read(HeadLinger, live.head_hide_linger);
    live.detail = read(Detail, live.detail);
    live.supersample = read(Supersample, live.supersample);

    // The toggles carry no number, but they still want the focus marker.
    // Same rule as the sliders: only a box the player actually clicked may change a
    // setting. One still holding what we put there is left alone.
    const auto toggle = [this](RowId id, bool current) {
        auto& row = m_rows[id];
        refresh_label(row, 0.0f, has_focus(row.control));
        const bool raw = checkbox_checked(row.control);
        if (row.pushed < 0.0f || raw != (row.pushed > 0.5f)) {
            row.pushed = raw ? 1.0f : 0.0f;
            return raw;
        }
        return current;
    };

    live.turn_mode = toggle(SmoothTurn, live.turn_mode == 1) ? 1 : 0;
    // Only a real toggle may change it. The old line wrote 1 or 0 unconditionally, so
    // BodyMode=2 - the whole character, head included - silently became 0 the first time the
    // page was opened, without anyone touching the checkbox.
    {
        const bool was = live.body_mode == 1;
        const bool now = toggle(ShowBody, was);
        if (now != was) {
            live.body_mode = now ? 1 : 0;
        }
    }
    live.hud_always_on = toggle(HudAlways, live.hud_always_on);
}

} // namespace tasomachivr
