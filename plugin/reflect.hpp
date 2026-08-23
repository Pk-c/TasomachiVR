// TasomachiVR - what the engine actually exposes, measured rather than remembered.
//
// The whole articulated-arms plan rests on reflection: nothing can be called that is not
// a UFunction, because the only entry point a plugin has is ProcessEvent. Three questions
// decide the architecture, and none of them can be answered by reading engine headers -
// only by asking this build:
//
//   1. Is UPoseableMeshComponent present, with CopyPoseFromSkeletalComponent and
//      SetBoneTransformByName? That is the design: keep the game's animated mesh hidden
//      and driving the pose, and override the arms on a visible poseable copy.
//   2. Can a freshly spawned component be made live? spawn_object constructs the object,
//      but a component only renders once it is registered, and RegisterComponent may not
//      be a UFunction at all. AddComponentByClass would solve it - it arrived in 4.26,
//      and this is 4.25.
//   3. If not: are the physics functions reachable? Simulating the arms and steering the
//      hand bones towards the controllers is cruder, but it needs no new component.
//
// Absence is as informative as presence here, so everything is logged either way.
#pragma once

#include <uevr/API.hpp>

namespace tasomachivr {

class Reflect {
public:
    // Once, and only when asked. Writes one block per class to the UEVR log, then walks
    // the live pawn to report what its mesh asset actually carries.
    void run(uevr::API::UObject* pawn);

private:
    bool m_done{false};
};

} // namespace tasomachivr
