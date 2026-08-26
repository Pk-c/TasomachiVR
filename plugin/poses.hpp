// TasomachiVR - posing the heroine from the free camera.
//
// The game's photo mode flies a camera around a character who stands in whatever idle the
// AnimBP happens to be in, and nothing in its interface changes that. But the game already
// poses her itself: the bench drives the sitting pose with PlayAnimation and
// SetAnimationMode, so the technique is not a guess - it is the game's own, called from
// outside.
//
// WHERE THE POSES COME FROM. UEVR can only reach objects that are already loaded, and there
// is no reflected StaticLoadObject to pull an asset in with. So the list is not hard-coded:
// the loaded object array is swept for AnimSequences whose name carries the character
// prefix, and whatever is resident becomes the cycle. Fifteen or so are always there -
// ThirdPersonCharacter and Pc01_AnimBP hard-reference them, so they load with her - and the
// rest come and go with the zone whose event Blueprint references them. A sweep per press
// rather than a cached list is what makes that safe as well as generous: an asset the zone
// dropped is gone from the list before it can be played through a dangling pointer.
#pragma once

#include <string>
#include <vector>

#include <uevr/API.hpp>

namespace tasomachivr {

class Poses {
public:
    // Every game-thread tick. `requests` is how many times the cycle button was pressed
    // since the last call - counted on the XInput thread, acted on here, because calling a
    // UFunction from that thread is a crash waiting for a busy frame.
    //
    // Returns true when the animation mode was changed, which the caller must answer with
    // Body::invalidate(): re-initialising the animation rebuilds the component underneath
    // us, so the hidden head bone comes back and the arm physics with it.
    bool tick(uevr::API::UObject* character, bool photo_mode, unsigned requests,
              const std::string& prefix);

    // A pose is only ever restored onto the mesh it was applied to. Forget everything.
    void forget();

    // What is showing, for the log and for anything that wants to say it out loud.
    const std::string& current() const { return m_current; }

private:
    void rescan(const std::string& prefix);
    uevr::API::UObject* find(const std::string& name) const;
    bool apply(uevr::API::UObject* mesh, uevr::API::UObject* asset);
    bool restore(uevr::API::UObject* mesh);
    void remember(uevr::API::UObject* mesh);

    std::vector<uevr::API::UObject*> m_assets{};
    std::vector<std::string> m_names{};

    // -1 is "the game's own animation", which is a real entry in the cycle rather than the
    // absence of one: pressing through the end of the list has to be able to give her back
    // whatever she was doing without leaving photo mode to do it.
    int m_index{-1};
    std::string m_current{};

    bool m_posed{false};
    bool m_was_photo{false};

    // WHAT SHE WAS DOING BEFORE WE TOUCHED HER, and why this is not just "AnimationBlueprint".
    // Photo mode can be opened from a bench - this mod lifts the flag that used to forbid it -
    // and the bench pose IS a single-node animation. Restoring to the AnimBP there would end
    // the sitting the player deliberately posed her in, so the mode she arrived in is what
    // she is put back into, asset included.
    uint8_t m_prior_mode{0};
    uevr::API::UObject* m_prior_asset{nullptr};
    // Its name too: the pointer stops being safe the moment something else is played, since
    // the mesh was the only thing holding the asset up.
    std::string m_prior_name{};
    uevr::API::UObject* m_mesh{nullptr};
};

} // namespace tasomachivr
