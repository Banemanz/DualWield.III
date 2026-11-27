# DualWield.III v22 handoff

## Scope

GTA III 1.0 EN only. Original v21 mod owns only the opposite arm and second weapon; native CPed aim/fire must stay authoritative. User reported awkward IK and autoaim making arms enter torso. They requested a PR and a Plugin-SDK build workflow pinned to October 25 2025. Do not merge without their request.

## Repository layout

- `DualWieldIII.cpp`: existing hook, model, renderer, config, stock frame / skinned HAnim adapters.
- `ArmIK.h`: independent target geometry and safety checks.
- `tests/test_ik.cpp`: portable geometry tests; `--csv` emits sample top-down poses.
- `docs/ik-diagnostics.svg`: visualization of synthetic solved poses, not a gameplay screenshot.
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
2. Free aim reflects the captured native wrist reach; active lock-on chooses the common registered entity target (ped torso matrix or vehicle/object location). If unavailable, fall back to native pose.
3. Bound shoulder-relative horizontal direction to 3–85 degrees outward, retaining elevation (including directly up/down). Rearward targets saturate continuously; impossible inboard aim does not swing through the torso.
4. Measure each bone length and infer the native extension ratio, then bound target distance using elbow flexion (8–130 degrees) and ArmReachScale. Law of cosines supplies elbow circle; choose pole from native mirrored bend, outer shoulder plane and slight downward bias. Fall back to maximally outward pole if needed. Both elbow/wrist endpoints remain outside the protected shoulder plane; the segments connecting them therefore also remain outside.
5. Frame adapters apply bounded axial upper-arm twist followed by the forearm rotation to reach the target. The stock path writes only local modelling bases via RwFrameTransform and rolls back all three frames on failure. Xbox/Skin & Bones edits the three local interpolation quaternions (parent-space PRECONCAT); poses are backed up and restored for render and fire separately. Actual rebuilt endpoints must agree with the solve before accepting.
6. The skin's inverse bind matrices produce one fixed hand-to-gun basis. Weapon-local trim participates in wrist stabilization; roll is about the achieved, not requested, barrel axis. Stock retains the previous native-basis grip option. A missing valid skinned bind correction fails cleanly rather than detaching the gun orientation.

## Lifecycle

- Original `CPed::AimGun` CALL around 0x4CB037 remains the native source. Capture skin native aim in that hook after rebuilding, before later locomotion. Rebase a short-lived cached aim through current waist when native call is skipped; key by owner/clump/weapon and invalidate on model/destroy/restart.
- Stock early solve before CPed::Attack, optional late process compatibility pass. Native shot call 0x4E6EBF; second shot samples actual opposite gun muzzle, skips if active stock IK could not solve this frame or skin transaction fails.
- `PedRenderBridge` at 0x4D0484 uses temporary HAnim solve, renders native ped and standalone gun, restores local quaternions. Render hooks are chained once, never recursively chased.
- Move ClearAimFlag hook at 0x4C5BF3 preserves v15 held-target/movement behavior; no change to native weapons, ammo, or bullet effects.

## Testing and limitations

Headless test grid covers 194,472 combinations (full yaw/pitch, close/far distances, both lock/free aim, three scale ratios and three blend amounts); local Linux release passed 1,557,661 checks. Linux sanitizer run passes with leak detection disabled because this container does not support leak-sanitizer tracing. The synthetic diagram is not an in-game screenshot.

Runtime test matrix (actual GTA III not installed here):
1. Clean 1.0 EN + ASI loader, Colt .45 and Uzi, first mouse aim then controller. Turn aim targets through left/center/right/rear at close and long range. The left elbow and wrist should remain outside torso; native right arm retains normal native behavior. Check gun grip and upper-arm twist at each extreme.
2. Repeat with Skin & Bones, then Proper Shaders if used. Walk forward/back, strafe, sprint, retarget, pitch vertically. Ensure no frame-to-frame flip and both gun models remain attached.
3. Fire and reload, switch weapon, get in car, die/respawn, change skin. Verify no weapon duplication, no leaked quaternions and correct native double-shot ammo/flash/smoke/shells. Set LogHandState=1 and provide DualWieldIII.log if a failure remains.
4. Record a short clip of actual autoaim clipping, including which skeleton, controller, target position and direction. Strong cross-body aim is deliberately limited for left arm; native right-arm clipping, if it exists, would require a separate game-side native IK investigation rather than modifying native ballistics here.
