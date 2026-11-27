# DualWield.III v24 handoff

## Scope

GTA III 1.0 EN only. Original v21 mod owns only the opposite (left) arm and second weapon; native right arm, CPed aim/fire must stay authoritative. User reported autoaim clipping, v22 wide sideways elbow, and a **worse** v23 screenshot: rear view, left elbow and raised forearm/gun pointing screen-left while the reticle and tracers are up and screen-right. v23's synthetic test did not model that actual cross-body aiming case. Do not interpret synthetic geometry as in-game proof. v24 addresses the incorrect direction source and infinite shoulder-side limit and adds logging needed to distinguish real skeleton/render integration failures. User requested a PR and Plugin-SDK CI pinned to October 25, 2025. Do not merge without their request.

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
- [re3 Weapon.cpp](https://github.com/mrxenginner/re3/blob/master/src/weapons/Weapon.cpp): FireInstantHit uses a camera-derived source/target ray for mouse aim; fallback transforms (range,0,0) through PED_HANDR. WeaponInfo::m_vecFireOffset is a muzzle point, not a barrel direction.
- [re3 Camera.cpp](https://github.com/mrxenginner/re3/blob/master/src/core/Camera.cpp): Find3rdPersonCamTargetVector computes the reticle ray. The SDK entry point is used rather than reimplementing camera projection.
- [re3 PedIK.cpp](https://github.com/mrxenginner/re3/blob/master/src/peds/PedIK.cpp): III's native right-arm solver has limb-specific yaw/pitch limits. This mod leaves it in control. The opposite skeleton chain must use local rotations, not overwrite finished world matrices.
- [re3 PlayerPed.cpp](https://github.com/mrxenginner/re3/blob/master/src/peds/PlayerPed.cpp): player controller updates weapon lock target via m_pPointGunAt.
- Pinned [Plugin-SDK](https://github.com/DK22Pac/plugin-sdk/tree/624a6a49265fd7a6fc63bda1611013ceabeacb8a), III SDK CPed/CEntity/CPlaceable/CPedIK declarations; stock 1.0 addresses already identified by the v21 source.

## Algorithm

1. Read all six world joint positions; use current native/opposite shoulder span with model up/forward to establish a torso-relative frame. Derive actual bone lengths from the live skeleton.
2. Use the native game aim cue: for mouse third-person follow-ped aim, call Find3rdPersonCamTargetVector with the native muzzle point and use target minus source. For non-mouse free aim, use native hand local +X (the stock firing fallback). For registered controller lock-on aim toward the ped torso/entity position, blend near-body target direction back toward native aim. Do not normalize the muzzle offset as the aim vector.
3. Control forearm direction separately from upper arm. Forearm may yaw inboard toward the reticle across the front of the chest; upper arm has the much narrower -17/+25 degree yaw range (negative is inboard) and uses native vertical bend information. At full aim preserve elevation. Preferred extension comes from ArmReachScale, but safe aim alignment takes precedence. Extreme yaw and pitch saturate instead of folding the arm through the torso.
4. The exclusion region extends only to a finite chest-front depth proportional to shoulder span. Check entire upper and lower segments within that depth against a sloped shoulder boundary. For an elbow behind the chest front plane use a tangent yaw limit; beyond the plane the hand can cross inboard. Preserve the actual lengths of both segments.
5. Frame adapters apply bounded axial upper-arm twist followed by forearm and wrist rotation. Stock writes local modelling bases, HAnim edits local interpolation quaternions. Verify actual rebuilt joints, restore on failure. Both use render-time temporary poses; HAnim also solves transactionally during firing.
6. Preserve the fixed bind-relative skin grip. Stock's copied native basis is rotated toward the solved forearm aim, stored only for the matching ped/clump/frame, without changing the right-hand bone. Exact capped forward and axial roll corrections remain; geometry safety is not a claim about skin deformation.
7. Log the actual pose once per 90 aiming/render frames, by default. IK24 includes rig, camera-ray/native-hand-X/lock-target cue, requested and solved/actual vectors, held gun X axes, right/left elbow and hand positions in torso-relative coordinates, lengths and endpoint error. IK24 post-draw reports residuals after chained rendering; pose unavailable flags failed integration. Disable LogIKDiagnostics in the INI if unwanted.

## Lifecycle

- Original `CPed::AimGun` CALL around 0x4CB037 remains the native source. Capture skin native aim in that hook after rebuilding, before later locomotion. Use live verified native matrices for that frame; rebase a short-lived cached aim through current waist only when native call is skipped; key by owner/clump/weapon and invalidate on model/destroy/restart.
- Stock early solve before CPed::Attack, optional late process compatibility pass. Native shot call 0x4E6EBF; second shot samples actual opposite gun muzzle, skips if active stock IK could not solve this frame or skin transaction fails.
- `PedRenderBridge` at 0x4D0484 uses temporary stock or HAnim solve, renders native ped and standalone gun from one snapshot, then restores local matrices/quaternions and stock grip state. Render hooks are chained once, never recursively chased.
- Move ClearAimFlag hook at 0x4C5BF3 preserves v15 held-target/movement behavior; no change to native weapons, ammo, or bullet effects.

## Testing and limitations

Headless test grid covers 290,672 combinations plus targeted regressions. Local Linux release and ASan/UBSan are geometry checks only; no installed GTA III executable or real skin/deformation render is available here. The synthetic diagram compares actual v23 and v24 solver output using identical camera-right/pitched-up input: v23 leaves its left wrist at shoulder width while v24 moves it inboard in front of the chest and aligns its forearm with the aim ray. This deliberately models the reported direction mismatch, not a screenshot recreation.

Runtime test matrix (actual GTA III not installed here):
1. Clean 1.0 EN + ASI loader, Colt .45 and Uzi, first mouse aim then controller. Turn aim targets through left/center/right/rear at close and long range. The left elbow and wrist should not pass through torso; native right arm retains normal native behavior. Check gun grip, upper-arm twist and that the left elbow is tucked rather than at shoulder-height sideways; in normal forward aim the second forearm should point roughly along the aim ray.
2. Repeat with Skin & Bones, then Proper Shaders if used. Walk forward/back, strafe, sprint, retarget, pitch vertically. Ensure no frame-to-frame flip and both gun models remain attached.
3. Fire and reload, switch weapon, get in car, die/respawn, change skin. Verify no weapon duplication, no leaked quaternions and correct native double-shot ammo/flash/smoke/shells. Provide DualWieldIII.log (IK24 logging is enabled by default) if a failure remains.
4. Record a short clip of actual autoaim clipping, including which skeleton, controller, target position and direction. Cross-body movement is allowed in front of the chest, and only extreme/rearward directions saturate; native right-arm clipping, if it exists, would require a separate game-side native IK investigation rather than modifying native ballistics here.
