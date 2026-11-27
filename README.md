# DualWield.III

Dual-wield backport for classic **Grand Theft Auto III 1.0 EN**.

Supports Colt .45 and Uzi with a second visible weapon, dual firing and native muzzle effects. The second weapon is a standalone `RpAtomic` outside the player ped clump. Supports the stock PC `RwFrame` skeleton and the Skin & Bones/Xbox HAnim skeleton with guarded compatibility hooks.

## v23: compact opposite-arm inverse kinematics

- Separate analytic two-link IK for the opposite arm, using live shoulder positions and actual upper/forearm lengths.
- Free aim follows the real native gun direction, not the potentially sideways shoulder-to-wrist chord. Controller lock-on shares target intent; the second arm stays extended even when the target is very close. Both routes keep the elbow and wrist on their own side of the torso.
- Compact aiming envelope: wrist yaw limited to 0–16° outward, elbow bend mostly below the aiming ray rather than sideways, and default full-aim extension at 95–98% of limb length. The elbow remains tucked; upper-arm axial twist, actual bone lengths, and up/down aiming are preserved.
- Wrist forward and roll stabilization use exact angular caps; roll is around the actual current weapon axis. A zero cap really disables that correction.
- A failed solve restores local frames/quaternions; the native right arm and first shot are never changed. Skin & Bones render/fire poses remain temporary and use the same bind-relative grip correction. Capture the native skinned aim immediately after `CPed::AimGun`, rather than later in animation/render.
- Native aim continuity, existing call chaining, weapon model ownership and native double-shot effects are retained.

Cross-body targets outside left-arm reach are intentionally saturated: the opposite arm cannot always point exactly at a very close or rearward target without clipping. The game continues to control native right-arm aim and firing.

![Synthetic v22 versus v23 compact aim comparison](docs/ik-diagnostics.svg)

## Install / build

Download the `DualWieldIII-Win32` artifact from the successful GitHub Actions run. Extract its zip and place `DualWieldIII.asi` and `DualWieldIII.ini` together where your GTA III ASI loader loads plugins. No FusionFix dependency. Skin & Bones is optional. Use GTA III 1.0 EN; other executables are not supported by these fixed hook addresses.

The project is Win32 Release, Visual Studio 2022 (v143). The workflow checks out Plugin-SDK at the October 25, 2025 commit [624a6a49265fd7a6fc63bda1611013ceabeacb8a](https://github.com/DK22Pac/plugin-sdk/commit/624a6a49265fd7a6fc63bda1611013ceabeacb8a), generates its Visual Studio project, builds `plugin_iii`, runs the IK tests on x86 and Linux, builds the ASI, checks its PE machine type, and packages the ASI, INI and README.

To build locally, set `PLUGIN_SDK_DIR` to that checkout (including submodules), generate/build the SDK as in `.github/workflows/build.yml`, then:

```bat
msbuild DualWieldIII.vcxproj /m /p:Configuration="Release GTA3" /p:Platform=Win32
```

## Configuration

The mod reads `DualWieldIII.ini` next to the ASI. All keys are optional and have compiled defaults; the supplied INI documents those defaults. It does not write an INI automatically.

| Key | Default | Meaning |
|---|---:|---|
| LeftArmIK | 1 | Enable constrained second-arm posing |
| AimBlendFrames | 1 | Frames to ramp in aim |
| AimHoldFrames | 4 | Short aim-continuity hold |
| ArmReachScale | 0.980 | Maximum fraction of measured arm length (0.70–1.00); now actually used |
| ElbowOutwardDeg | 0 | Optional extra bounded elbow flare (0–15) |
| HandAimLimitDeg | 90 | Max forward wrist correction per solve |
| HandRollLimitDeg | 60 | Max barrel-axis wrist roll per solve |
| CopyNativeGripBasis | 1 | Stable native-basis stock socket / bind-relative skinned socket |
| LateProcessRepair | 0 | Optional compatibility repair for stock frame mods |

## Verification

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror tests/test_ik.cpp -o test_ik
./test_ik
```

Headless tests exercise 290,672 poses, including the sideways-punch regression, lock-on/free aim, close targets, full pitch, all yaw quadrants, asymmetric arm lengths, older reach settings, partial blend, rigid transformation, degenerate data and validation of rebuilt joints. Tests also enforce compact wrist/elbow spread and forward forearm alignment. They cannot establish in-game skin weighting, controller timing, or the native arm's visual behavior; see [handoff notes](new%20chat/HANDOFF.md) for the gameplay test checklist.

## TODO

- Gameplay integration (stats system or cheat to activate).
- In-game visual regression testing and compatibility with additional skeleton replacements.
