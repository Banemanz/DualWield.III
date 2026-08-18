# DualWield.III

Dual-wield backport for classic **Grand Theft Auto III 1.0 EN**.

Current baseline: **v7 V3Baseline SkinBonesCompat**.

The current implementation keeps the v3 render-stage mirrored-arm behavior, renders the second weapon as a standalone/private `RpAtomic`, supports both the stock PC `RwFrame` skeleton and the Skin & Bones/Xbox HAnim skeleton backend, and sends the second shot through GTA III's native `CWeapon::Fire` path so muzzle flash/smoke/light/shell effects remain native.

Compatibility hardening in v7 includes conservative vehicle/death/fall/get-up gating, chained existing CALL hooks, model/clump replacement handling, and avoidance of inserting the duplicate weapon atomic into the player's ped clump.

See `V7_NOTES.txt` for the Skin & Bones/ragdoll compatibility notes and `DualWieldIII.ini` for configuration.
