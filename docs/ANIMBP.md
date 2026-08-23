# Articulated arms: the post-process Animation Blueprint

Everything the editor side needs, in the order it has to be done. The runtime side is
already written (`plugin/animbp.cpp`) and compiled.

## Why this route

Four alternatives were measured and closed, in this order:

| Route | Why it is closed |
|---|---|
| Write bones on the game's mesh | No reflected bone setter exists on `USkeletalMeshComponent` |
| Hook the game's AnimBP | `Pc01_AnimBP` is a pure state machine — no IK node, no montage slot |
| A `UPoseableMeshComponent` copy | `AddComponentByClass` arrived in 4.26; `RegisterComponent` is not a UFunction |
| Physics-driven arms | `SK_Pc_01_PhysicsAsset` has **one** body per arm (`LeftArm`, `RightArm`). No forearm, no hand — the arm could only swing rigidly from the shoulder |

A post-process AnimBP is run by the engine on top of whatever the game's own animation
produced, which is exactly the split we want: the game keeps the legs and the torso, we
take the arms.

## Non-negotiable details

These are the ones that fail silently if they are wrong.

**Name the UE project `tasomachi`.** `/Game/` maps to `<project>/Content/`, and the pak's
internal paths must match what the game resolves. With any other project name the pak
mounts and nothing in it is ever found.

**Engine version 4.25.4 exactly.** Not 4.25.3.

**Create the skeleton at the game's own path**: `/Game/chr/PC/SK_Pc_01_Skeleton`. Our
AnimBP will reference it by that path, we exclude it from the pak, and at runtime the
reference resolves to the game's real skeleton. That is what makes the AnimBP compatible
with the game's mesh without shipping any of the game's content.

## 1. The skeleton

The AnimBP has to be compiled against *a* skeleton whose bone names match. Only names
matter — the rest pose affects the editor preview and nothing else, because our asset
never ships.

Two ways to get one:

**Exact (recommended).** Export `SK_Pc_01` out of the game's pak with
[UEViewer/umodel](https://www.gildor.org/en/projects/umodel) — the pak is unencrypted and
4.25 is well supported. That gives a `.psk`; Blender with the PSK importer opens it and
exports an FBX. Import that into the project, into `/Game/chr/PC/`, and rename the
resulting skeleton asset to exactly `SK_Pc_01_Skeleton`.

**Minimal.** Ask me for a generated FBX carrying only the bones the IK touches: `Hips`,
`Spine`, `Spine1`, `Spine2`, `Neck`, `Head`, and both `Shoulder / Arm / ForeArm / Hand`
chains. Enough to compile against, and much less setup — but I cannot validate a
hand-written FBX without the editor, so an import error would cost a round trip. That is
why the export route is recommended.

Bone names are Mixamo convention **without** the `mixamorig:` prefix. `Head`, not `head`.

## 2. The Animation Blueprint

Create an Animation Blueprint, parent class `AnimInstance`, target skeleton
`SK_Pc_01_Skeleton`. Save it as **`/Game/TasomachiVR/ABP_VRArms`**. The plugin loads
`/Game/TasomachiVR/ABP_VRArms.ABP_VRArms_C`, so both the folder and the name matter.

### Variables

Exact spelling. The plugin writes them by property name; a typo shows up as a variable
that never moves, and `plugin/animbp.cpp` logs `variable MISSING:` for each one it cannot
find, so the log will name it.

| Name | Type | Default |
|---|---|---|
| `VR_Enabled` | Boolean | false |
| `VR_Alpha` | Float | 1.0 |
| `VR_DebugTilt` | Float | 0.0 |
| `VR_LeftHandTarget` | Vector | — |
| `VR_RightHandTarget` | Vector | — |
| `VR_LeftHandValid` | Boolean | false |
| `VR_RightHandValid` | Boolean | false |

### AnimGraph

```
[incoming pose] -> Two Bone IK (left) -> Two Bone IK (right) -> Transform (Modify) Bone -> Output Pose
```

**The incoming pose.** A post-process AnimBP receives the pose the game already evaluated.
I am not certain what that node is called in 4.25 — I believe `Input Pose`, but I would
rather say so than have you chase a name I guessed. Look in the AnimGraph context menu; if
there is no such node, tell me what the graph does offer and I will adapt. Do **not** use
`Local Ref Pose` as a substitute: that would throw the game's animation away and the
character would go limp.

**Two Bone IK, left.** The node takes the effector bone and works back through its parent
and grandparent by itself, so naming the hand is enough.

- IK Bone: `LeftHand`
- Effector Location Space: **World Space**
- Effector Location: pin `VR_LeftHandTarget`
- Allow Stretching: off
- Alpha: `VR_Alpha` multiplied by `VR_LeftHandValid` (select 1.0 / 0.0), so the arm falls
  back to the game's animation the moment tracking is lost rather than snapping to zero

Right side identical with `RightHand` and `VR_RightHandTarget` / `VR_RightHandValid`.

World space is deliberate: the hand positions the plugin sends already come from UEVR in
world space, so nothing has to be converted on either side.

**Transform (Modify) Bone — this is the diagnostic.**

- Bone to Modify: `Head`
- Rotation: `(0, VR_DebugTilt, 0)`
- Rotation Mode: Add to Existing
- Rotation Space: Bone Space
- Alpha: 1.0

It exists so that one launch separates three failures. Set `ArmsDebugTilt=30` in the ini:
if the head tilts, the whole chain — pak mounted, class loaded, property assigned,
component re-initialised, instance created, variables written — is proven, and anything
still wrong is in the IK. If the head does not tilt, none of that happened and the IK is
irrelevant.

## 3. Cook and pak

`package_pak.ps1` does both. It cooks the project, then builds a pak containing **only**
`Content/TasomachiVR/` — the skeleton and anything else the cooker pulled in are left out
on purpose, so no game content is redistributed and the references resolve to the game's
own assets.

```powershell
.\package_pak.ps1 -UnrealDir "C:\Program Files\Epic Games\UE_4.25" `
                  -ProjectDir "D:\PROJECT\tasomachi-mod" `
                  -GameDir "H:\Steam\steamapps\common\TASOMACHI"
```

The result is `tasomachi/Content/Paks/TasomachiVR_P.pak`. The `_P` suffix is what makes
the engine mount it with priority over the game's own pak.

Cooked unversioned, to match how the game itself was cooked — its packages have zeroed
version fields, which is what `tools/uasset_names.py` had to work around.

## 4. Switch it on

In `TasomachiVR.ini`:

```ini
Arms=1
ArmsDebugTilt=30      ; prove the chain first, then set back to 0
ArmsAlpha=1.0
```

With `Arms=0` nothing is loaded and the cost is a handful of failed lookups before the
loader gives up.

## What the plugin does at runtime

For the record, so both sides can be checked against one description:

1. `LoadClassAsset_Blocking` on `/Game/TasomachiVR/ABP_VRArms.ABP_VRArms_C`, retried for a
   few seconds because the pak is mounted before the engine is ready to load from it
2. writes the class into `SK_Pc_01`'s `PostProcessAnimBlueprint` — the property is on the
   mesh **asset**, so one write covers the on-foot mesh and the boat's alike
3. `SetSkeletalMesh(sameMesh, bReinitPose=true)` to force `InitAnim` to run again, which
   is the only reflected way to make the engine build the post-process instance
4. reads `PostProcessAnimInstance` off the component and writes the variables every frame

The hand targets come from UEVR's own `UObjectHook`, which drives a borrowed
`ArrowComponent` per hand from the motion controllers and therefore does the
OpenXR-to-world conversion itself. Measured: a hand held out to the side reads 77 cm from
the head bone, and the raw tracking delta confirmed the axis mapping.
