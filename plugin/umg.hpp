// TasomachiVR - grafting onto the game's own UMG.
//
// Why this direction: every other surface has been ruled out by measurement.
//   * UEVR's plugin render target draws fine but its quad only reaches the headset
//     while UEVR's own overlay is open.
//   * The viewport draw callback hands an FCanvas*, which has no UClass, so there is
//     nothing to call Blueprint drawing on.
//   * AHUD::Canvas does not exist here: the game has no HUD actor at all (pc=1 hud=0),
//     because everything it draws is UMG.
//
// That last fact is also the opening. The game's UI is UMG added to the viewport, and
// UEVR projects that layer - it is why the HUD and the pause menu are visible in VR. So
// instead of inventing a surface, the menu can live inside the one the game already
// puts on screen.
//
// This file starts as a probe: it finds the live pause-menu widget and writes its tree
// to the log, because everything that follows - which panel to attach to, what a slot
// looks like, whether a TextBlock can be spawned into it - depends on the real shape
// rather than on what a cooked .uasset name table suggests.
#pragma once

#include <uevr/API.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace tasomachivr {

class Umg {
public:
    // Walks and logs the widget tree of every live instance of the named widget class.
    // Done once per class, since the tree does not change shape while the game runs.
    void probe(const wchar_t* class_path);

    // Lists every live UUserWidget with its real class name. Guesses nothing: it is how
    // the class paths above get confirmed rather than assumed, and it reports even when
    // it finds nothing, so silence can never again be mistaken for "not called".
    void discover();

private:
    // Calls a UFunction on an object with a parameter blob built from offsets read out
    // of the function itself, and reads a named return value back out.
    struct Blob {
        std::vector<uint8_t> bytes;
        uevr::API::UFunction* fn{nullptr};
        bool ok{false};
    };

    static Blob make_blob(uevr::API::UObject* object, const wchar_t* function);
    template <typename T>
    static bool put(Blob& blob, const wchar_t* param, const T& value);
    template <typename T>
    static bool get(const Blob& blob, const wchar_t* param, T& out);

    void walk(uevr::API::UObject* widget, int depth);

    std::vector<std::wstring> m_probed;
    std::vector<std::wstring> m_reported;
    int m_nodes{0};
    int m_discover_attempts{0};
};

} // namespace tasomachivr
