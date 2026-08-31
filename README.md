# TasomachiVR

<img width="1672" height="941" alt="tasoVR" src="https://github.com/user-attachments/assets/acfbcbdc-9d85-45c9-acba-33384a364362" />

A first-person VR mod for **[Tasomachi](https://store.steampowered.com/app/1015890/TASOMACHI_Behind_the_Twilight/)** (
Orbital Express), built on praydog's [UEVR](https://github.com/praydog/UEVR).

Tasomachi is a third-person game. This mod turns it into a first-person VR experience: the camera sits at the character's head, the body faces wherever you look, and movement works like an FPS with snap & smooth turning.

If you like my work you can follow me on Patreon ( free membership ), I try to make like native mode for beautiful games!

https://patreon.com/ChromaticMod

<a href="https://patreon.com/ChromaticMod">
  <img width="200" height="105" alt="imakevrmodforgames-preview" src="https://github.com/user-attachments/assets/0517352b-e120-47bc-b062-b85fc333f814" />
</a>

OR

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/A0Y524C5N8)


## What it does

- First person camera at head height, the head hidden only while you are inside it
- Roomscale movement and wall collision, both left to UEVR — which is what makes them work
- Snap or smooth turning, with the body following the view
- A **VR SETTINGS** page grafted into the game's own pause menu, saved to the ini on close
- The interface fixed in front of you, revealed on the left grip and faded back out
- Photo mode add-on to change the character pose

Everything adjustable from inside the headset lives on the VR page. The rest lives in
`TasomachiVR\TasomachiVR.ini`, where every setting carries a comment explaining what it does
and why it holds the value it does.

Youtube Demo
[![Demo](https://img.youtube.com/vi/NTxkmfaO3Qc/maxresdefault.jpg)](https://youtu.be/NTxkmfaO3Qc)


## Controls

**On Foot**

| Control | Action |
| --- | --- |
| Left stick | Move |
| Right stick | Turn — snap by default, 45° a step; smooth is a setting |
| Right stick click | Pause menu |
| A | Jump |
| B | Interact | Double Jump (Unlockable) |
| X | Photo mode — opens and closes, and works while sitting on a bench |
| Y | Skip cutscene |
| Left grip | Show the interface, held; it fades back out on release |
| Right grip | Stomp (Unlockable) |
| Either trigger | Air-Dash (Unlockable) |

**In AirShip**

| Control | Action |
| --- | --- |
| Left stick | Move |
| Right stick | Adjust Camera |
| Right stick click | Pause menu |
| A | Missile (Unlockable) |
| X | Bomb (Unlockable) |
|Left/Right trigger | Ascend/Descend |

In photo mode, **A** cycles the character's pose. One press past the last one gives her back
the animation she arrived in, without leaving photo mode to get it.

Bindings are for Touch-style controllers. Everything here is remappable in
`TasomachiVR\TasomachiVR.ini` — the entries are `ButtonEvent`, `ButtonRemap`,
`InteractButton`, `PauseButton` and `HudRevealSource`, each with a comment explaining what
it reaches and why.

## Install

Download the archive from [Releases](../../releases) and unzip its contents into

```
tasomachi\Binaries\Win64
```

That's all

**Uninstall** by double-clicking `Uninstall.bat`, next to `dsound.dll`. It removes the loader,
the mod folder, the VR profile it created and the audio line it added. Deleting the files by
hand would leave the last two behind. Save games are never touched.

## Known issues

-During some animations the head may briefly get in the way, if it disturb you, hide the body through the game opions
-When you change your height, you should use the recenter button to adjust the height
 
## Building

```powershell
.\build.ps1           # dsound.dll   - the loader
.\build_plugin.ps1    # TasomachiVR.dll - the plugin
.\deploy.ps1          # install into the game folder for testing
.\deploy.ps1 -Uninstall
.\package.ps1         # build\release\TasomachiVR-<date>.zip
```

Requires the MSVC toolchain and a UEVR build to link against and redistribute — `H:\UEVR` by
default, override with `-UevrDir`.

Most of what looks arbitrary in this mod was measured rather than chosen, and the reasoning
is in the comments next to the code it explains. The ini is written the same way.

## Licence

Code in this repository: MIT (see LICENSE).

See THIRD-PARTY.txt for the full breakdown.

No game content is included in this repository. Tasomachi is the property of Orbital Express.
