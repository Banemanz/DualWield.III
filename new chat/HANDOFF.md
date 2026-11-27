# DualWield.III v23 handoff

## Scope

GTA III 1.0 EN only. Original v21 mod owns only the opposite arm and second weapon; native CPed aim/fire must stay authoritative. User reported awkward IK and autoaim making arms enter torso. In v22 testing, the added arm still looked like a wide punch; the supplied rear-view screenshot showed a flared, approximately shoulder-height elbow and a folded forearm. v23 specifically fixes that pose. They requested a PR and a Plugin-SDK build workflow pinned to October 25 2025. Do not merge without their request.

## Repository layout

- `DualWieldIII.cpp`: existing hook, model, renderer, config, stock frame / skinned HAnim adapters.
- `ArmIK.h`: independent target geometry and safety checks.
- `tests/test_ik.cpp`: portable geometry tests; `--csv` emits sample top-down poses.
- `docs/ik-diagnostics.svg`: before/after visualization of the synthetic sideways-arm regression, not a gameplay screenshot.
- `DualWieldIII.vcxproj` and `.github/workflows/build.yml`: pinned Win32 build/test/package.
- `DualWieldIII.ini`: optional tuning; all keys have defaults in code.

## Reference study (behavior, not copied engine implementation)

- [SA gta-reversed CTaskSimpleUseGun](https://github.com/gta-reversed/gta-reversed/blob/master/source/game_sa/Tasks/TaskTypes/TaskSimpleUseGun.cpp): AimGun dispatches distinct left and right PointArm requests to the same target for dual-wield weapons. It also allows torso orientation to contribute; do not simply port the SA bone IDs or task controller to III.
- [re3 Ped.cpp](https://github.com/mrxenginner/re3/blob/master/src/peds/Ped.cpp): CPed::AimGun aims at m_pSeekTarget (ped middle or non-ped position) or uses direction/pitch; native point target is registered.
- [re3 PedIK.cpp](https://github.com/mrxenginner/re3/blob/master/src/peds/PedIK.cpp): III's native right-arm solver has limb-specific yaw/pitch limits. This mod leaves it in control. The opposite skeleton chain must use local rotations, not overwrite finished world matrices.
- [re3 PlayerPed.cpp](https://github.com/mrxenginner/re3/blob/master/src/peds/PlayerPed.cpp): player controller updates weapon lock target via m_pPointGunAt.
- Pinned [Plugin-SDK](https://github.com/DK22Pac/plugin-sdk/tree/624a6a49265fd7a6fc63bda1611013ceabeacb8a), III SDK CPed/CEntity/CPlaceable/CPedIK declarations; stock 1.0 addresses already identified by the v21 source.

## Algorithm

1. Read all six world joint positions; use current native and opposite shoulders to establish outward direction. Project model forward/up onto the live shoulder plane.
2. Free aim uses the native weapon world aim cue (native hand basis transformed by weapon-local fire direction); native forearm is a fallback, never reflected lateral shoulder-to-wrist chord. Active lock-on shares registered target intent (ped torso or vehicle/object position), with near-target direction smoothly blended rather than allowing a target inside arm reach to collapse the chain.
3. Bound shoulder-relative wrist horizontal yaw to 0–16 degrees outward, retaining elevation (including directly up/down). Cross-body or rearward targets saturate instead of swinging out to the side. No changes to native aim or firing implementation.
4. Measure both bone lengths, prefer the native extension ratio but use 95–98% of measured length at full aim under the default config (respect explicit older/lower ArmReachScale). Use law of cosines for the elbow circle; choose a down/pitch-plane bend with an analytical lateral coefficient that tucks the elbow close to shoulder width. No more default outward pole. Optional elbow flare remains bounded. Both endpoints are constrained to the shoulder-side half-plane; actual joint positions are revalidated.
5. Frame adapters apply bounded axial upper-arm twist followed by the forearm rotation to reach the target. The stock path writes only local modelling bases via RwFrameTransform and rolls back all three frames on failure. Xbox/Skin & Bones edits the three local interpolation quaternions (parent-space PRECONCAT); poses are backed up and restored for render and fire separately. Actual rebuilt endpoints must agree with the solve before accepting.
6. The skin's inverse bind matrices produce one fixed hand-to-gun basis. Weapon-local trim participates in wrist stabilization; roll is about the achieved, not requested, barrel axis. Stock retains the previous native-basis grip option. A missing valid skinned bind correction fails cleanly rather than detaching the gun orientation.

## Lifecycle

- Original `CPed::AimGun` CALL around 0x4CB037 remains the native source. Capture skin native aim in that hook after rebuilding, before later locomotion. Rebase a short-lived cached aim through current waist when native call is skipped; key by owner/clump/weapon and invalidate on model/destroy/restart.
- Stock early solve before CPed::Attack, optional late process compatibility pass. Native shot call 0x4E6EBF; second shot samples actual opposite gun muzzle, skips if active stock IK could not solve this frame or skin transaction fails.
- `PedRenderBridge` at 0x4D0484 uses temporary HAnim solve, renders native ped and standalone gun, restores local quaternions. Render hooks are chained once, never recursively chased.
- Move ClearAimFlag hook at 0x4C5BF3 preserves v15 held-target/movement behavior; no change to native weapons, ammo, or bullet effects.

## Testing and limitations

Headless test grid covers 290,672 combinations: the previous full yaw/pitch/target/blend grid plus 96,200 asymmetric bone-length and older-INI cases, and specific screenshot-inspired wide-native-elbow and point-blank regressions. Local Linux release and ASan/UBSan passed 2,653,461 assertions (leak detection disabled due to container tracing limitation). The synthetic diagram compares the actual v22 and v23 solver outputs using the same input; not an in-game screenshot. For the synthetic sideways native-arm case, v22 placed the left wrist 28.5 cm outward from its shoulder and its elbow 30.5 cm outward; v23 places the wrist at shoulder width and its elbow only 2.3 cm outward. Both retain the actual segment lengths.

Runtime test matrix (actual GTA III not installed here):
1. Clean 1.0 EN + ASI loader, Colt .45 and Uzi, first mouse aim then controller. Turn aim targets through left/center/right/rear at close and long range. The left elbow and wrist should remain outside torso; native right arm retains normal native behavior. Check gun grip, upper-arm twist and that the left elbow is tucked rather than at shoulder-height sideways; in normal forward aim the second forearm should point roughly along the aim ray.
2. Repeat with Skin & Bones, then Proper Shaders if used. Walk forward/back, strafe, sprint, retarget, pitch vertically. Ensure no frame-to-frame flip and both gun models remain attached.
3. Fire and reload, switch weapon, get in car, die/respawn, change skin. Verify no weapon duplication, no leaked quaternions and correct native double-shot ammo/flash/smoke/shells. Set LogHandState=1 and provide DualWieldIII.log if a failure remains.
4. Record a short clip of actual autoaim clipping, including which skeleton, controller, target position and direction. Strong cross-body aim is deliberately limited for left arm; native right-arm clipping, if it exists, would require a separate game-side native IK investigation rather than modifying native ballistics here.
