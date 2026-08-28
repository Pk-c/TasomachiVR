// TasomachiVR - the "VR SETTINGS" page, built inside the game's own pause menu.
//
// The pause menu is UMG, which is the layer UEVR projects into the headset - the one
// surface the game itself proves is visible. So rather than drawing a menu, this builds
// one out of the same widget classes the game uses, in the same widget tree, and it
// inherits the game's styling, UEVR's UI distance and size, and the existing cursor.
//
// Two things are done by polling rather than by binding:
//
//   * Clicks. UButton::OnClicked is a BlueprintAssignable delegate, and binding it from
//     a plugin would need a UFunction of our own, which we have no way to create.
//     UButton::IsPressed() is BlueprintCallable, so an edge detector on it gives the
//     same answer with nothing invented.
//   * Values. The sliders and checkboxes are read every tick while the page is up,
//     exactly as the repurposed camera rows already are.
//
// The widget is rebuilt every time the player opens the pause menu, because the game
// constructs a fresh instance each time - the log showed WBP_PauseMenu_C_2147482381.
#pragma once

#include <uevr/API.hpp>

#include "settings.hpp"

#include <cstdint>

namespace tasomachivr {

class VrPage {
public:
    // Builds the entry and the page into whatever pause menu is currently live, then
    // polls it. Call once per engine tick; does nothing while no menu exists.
    void update(uevr::API::UObject* pawn, MenuSettings& live);

    // The stick has to be filtered differently while this page has the focus, so the
    // caller needs to know when that is.
    bool is_open() const { return m_open; }

    // Whether the GAME's pause menu is on screen - not just our page inside it.
    bool game_menu_visible() const;

    // True once, after the page is closed. The caller uses it to persist the settings.
    bool take_close_event() {
        const bool v = m_closed;
        m_closed = false;
        return v;
    }

private:
    // One row: a label and a control, in a HorizontalBox. Sliders are always 0..1 and
    // mapped, so the engine side needs no range configuration.
    struct Row {
        const wchar_t* label{nullptr};
        const wchar_t* unit{nullptr};
        uevr::API::UObject* text{nullptr};
        uevr::API::UObject* control{nullptr};
        float lo{0.0f};
        float hi{1.0f};
        float step{1.0f};
        bool  is_toggle{false};
        // The normalised value last pushed onto this widget. A widget still holding it has
        // not been touched by the player and must NOT write back - that is how a freshly
        // rebuilt slider used to overwrite a real setting with its own default.
        float pushed{-1.0f};
        float shown{-99999.0f}; // last value written into the label
        bool  focused{false};   // last focus state written into the label
    };

    enum RowId {
        SnapAngle,
        SmoothTurn,
        SmoothSpeed,
        EyeForward,
        EyeHeight,
        ShipHeight,
        ShipForward,
        // YawTrim is still a setting in the ini; it just does not earn a row on a page you
        // read in a headset - it is set once, if ever.
        ShowBody,
        MenuScale,
        HudAlways,
        AirLift,
        AirForward,
        HeadLinger,
        Detail,
        Supersample,
        RowCount,
    };

    bool build(uevr::API::UObject* menu);
    void poll(MenuSettings& live);
    // Rewrites a row's label when its value or its focus changed - and only then, since
    // each rewrite costs one leaked FText.
    void refresh_label(Row& row, float value, bool focused);
    void open_page(bool open);
    void sync_from(const MenuSettings& live);

    uevr::API::UObject* m_menu{nullptr};      // the instance this UI belongs to
    uevr::API::UObject* m_main_box{nullptr};  // the game's main list, hidden while we show
    uevr::API::UObject* m_entry{nullptr};     // our button in the main list
    uevr::API::UObject* m_page{nullptr};      // our VerticalBox
    uevr::API::UObject* m_back{nullptr};
    uevr::API::UObject* m_back_label{nullptr};
    bool m_back_focused{false};

    Row m_rows[RowCount]{};

    bool m_open{false};
    bool m_closed{false};
    bool m_needs_sync{false};
    bool m_entry_was_pressed{false};
    bool m_back_was_pressed{false};
    bool m_logged{false};
};

} // namespace tasomachivr
