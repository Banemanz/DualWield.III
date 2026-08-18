        // to render the second weapon. This is refreshed here so firing cannot use a
        // previous-frame pose after IK modified the opposite arm.
        if (!UpdateLeftWeaponWorldTransform())
            return false;
        RwMatrix* world = RwFrameGetMatrix(gLeft.helperFrame);
        if (!world)
            return false;

        RwV3d src = { info->m_vecFireOffset.x, info->m_vecFireOffset.y, info->m_vecFireOffset.z };
        RwV3d dst = {};
        RwV3dTransformPoints(&dst, &src, 1, world);
        out.x = dst.x;
        out.y = dst.y;
        out.z = dst.z;
        if (!IsFiniteVector(out))
            return false;

        // A corrupt/rebuilt frame should never be allowed to send a bullet, point light
        // or particle system to infinity/origin. A one-handed gun muzzle must stay close
        // to the player; 3 metres is intentionally generous for modded player models.
        const CVector& pedPos = ped->GetPosition();
        const float dx = out.x - pedPos.x;
        const float dy = out.y - pedPos.y;
        const float dz = out.z - pedPos.z;
        const float distSq = dx * dx + dy * dy + dz * dz;
        return IsFiniteFloat(distSq) && distSq <= 9.0f;
    }

    // Replaces only the CALL at CPed::Attack+0x31F, not CWeapon::Fire globally.
    static bool __fastcall AttackFireHook(CWeapon* weapon, void*, CEntity* shooter, CVector* rightSource) {
        WeaponFireFn fireFn = reinterpret_cast<WeaponFireFn>(
            gFirePatch.previousTarget ? gFirePatch.previousTarget : ADDR_WEAPON_FIRE);
        if (!fireFn)
            return false;

        const bool firedRight = fireFn(weapon, shooter, rightSource);
        if (!firedRight || !gConfig.doubleShot || !shooter || !weapon)
            return firedRight;

        CPed* ped = reinterpret_cast<CPed*>(shooter);
        if (!IsEligiblePlayer(ped, weapon) || gLeft.owner != ped || !IsAtomicAliveForOwner(ped))
            return firedRight;

        // If the first shot exhausted the clip, CWeapon::Fire has already changed the
        // weapon state to RELOADING/OUT_OF_AMMO. Calling it again would simply fail,
        // but this explicit guard avoids unnecessary work and preserves exact semantics.
        if (weapon->m_eWeaponState != WEAPONSTATE_READY && weapon->m_eWeaponState != WEAPONSTATE_FIRING)
            return firedRight;
        if (weapon->m_nAmmoInClip <= 0)
            return firedRight;

        // Materialize the Skin & Bones hierarchy when needed, mirror the finished native
        // arm pose, then rebuild the private weapon frame before deriving the second muzzle.
        PrepareOppositePoseForFire(ped);

        CVector leftSource;
        if (!ComputeLeftFireSource(ped, weapon, leftSource))
            return firedRight;

        fireFn(weapon, shooter, &leftSource);
        return firedRight;
    }

    static bool DecodeRelativeCall(uintptr_t address, uintptr_t& target, int32_t& rel) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(address);
        if (p[0] != 0xE8)
            return false;
        std::memcpy(&rel, p + 1, sizeof(rel));
        target = address + 5 + rel;
        return true;
    }

    static bool IsExecutableAddress(uintptr_t address) {
        if (!address)
            return false;

        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;

        const DWORD prot = mbi.Protect & 0xFFu;
        return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
               prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    }

    static bool InstallCallPatch(uintptr_t address, uintptr_t expectedTarget, void* hook, CallPatch& patch) {
        uintptr_t currentTarget = 0;
        int32_t originalRel = 0;
        if (!DecodeRelativeCall(address, currentTarget, originalRel))
            return false;

        const uintptr_t hookTarget = reinterpret_cast<uintptr_t>(hook);
        if (!hookTarget || currentTarget == hookTarget)
            return false;

        const bool chained = currentTarget != expectedTarget;
        if (chained) {
            if (!gConfig.chainExistingCallHooks || !IsExecutableAddress(currentTarget))
                return false;
        }

        const intptr_t delta = hookTarget - (address + 5);
        if (delta < INT32_MIN || delta > INT32_MAX)
            return false;
        const int32_t newRel = static_cast<int32_t>(delta);

        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        std::memcpy(reinterpret_cast<void*>(address + 1), &newRel, sizeof(newRel));
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 5);
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(address), 5, oldProtect, &ignored);

        patch.address = address;
        patch.previousTarget = currentTarget;
        patch.hookTarget = hookTarget;
        patch.originalRel = originalRel;
        patch.installed = true;
        patch.chained = chained;
        return true;
    }

    static void RestoreCallPatch(CallPatch& patch) {
        if (!patch.installed)
            return;

        // If another ASI patched this CALL after us, leave its newer hook intact.
        uintptr_t currentTarget = 0;
        int32_t currentRel = 0;
        if (!DecodeRelativeCall(patch.address, currentTarget, currentRel) ||
            currentTarget != patch.hookTarget) {
            patch = CallPatch();
            return;
        }

        DWORD oldProtect = 0;
        if (VirtualProtect(reinterpret_cast<void*>(patch.address), 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(reinterpret_cast<void*>(patch.address + 1), &patch.originalRel, sizeof(patch.originalRel));
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(patch.address), 5);
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(patch.address), 5, oldProtect, &ignored);
        }
        patch = CallPatch();
    }

    static void __fastcall PedRenderBridge(CPed* ped, void*) {
        // The bridge is installed on CPed::Render's final CALL. Apply the mirror
        // first so whichever renderer currently owns the call site (stock, Skin & Bones,
        // Proper Shaders bridge, etc.) sees the completed opposite-arm pose.
        ApplyRenderStageMirror(ped);

        PedRenderCallFn previous = reinterpret_cast<PedRenderCallFn>(
            gRenderPatch.previousTarget ? gRenderPatch.previousTarget : ADDR_ENTITY_RENDER);
        if (previous && reinterpret_cast<uintptr_t>(previous) != reinterpret_cast<uintptr_t>(&PedRenderBridge))
            previous(ped);

        // Render our private weapon only after the existing ped renderer returns. It is
        // never enumerated as part of the body clump, which is the crash-critical rule.
        RenderStandaloneLeftWeapon(ped);
    }

    static bool EnsureRenderBridgeInstalled() {
        const uintptr_t hookTarget = reinterpret_cast<uintptr_t>(&PedRenderBridge);
        uintptr_t currentTarget = 0;
        int32_t currentRel = 0;
        if (!DecodeRelativeCall(ADDR_PED_RENDER_CALL, currentTarget, currentRel))
            return false;

        if (currentTarget == hookTarget)
            return true;

        // All normal ASIs have finished startup before the first gameProcess tick, so the
        // initial install chains the settled owner of this CALL (stock, Skin & Bones,
        // Proper Shaders, Plugin-SDK event bridge, etc.). If somebody replaces the CALL
        // later, do not chase/re-chain it every frame: a late wrapper may have captured
        // our bridge as its predecessor, and hooking that wrapper back can create a cycle.
        if (gRenderPatch.installed) {
            if (!gLoggedRenderRechain) {
                char line[320];
                std::snprintf(line, sizeof(line),
                    "DualWieldIII: render CALL was replaced after our bridge (current=%p); not re-hooking to avoid a hook cycle.",
                    reinterpret_cast<void*>(currentTarget));
                Log(line);
                gLoggedRenderRechain = true;
            }
            return false;
        }

        if (!InstallCallPatch(ADDR_PED_RENDER_CALL, ADDR_ENTITY_RENDER,
            reinterpret_cast<void*>(&PedRenderBridge), gRenderPatch)) {
            return false;
        }

        {
            char line[320];
            std::snprintf(line, sizeof(line),
                "DualWieldIII: render bridge active. previous=%p%s.",
                reinterpret_cast<void*>(gRenderPatch.previousTarget),
                gRenderPatch.chained ? " [CHAINED]" : "");
            Log(line);
        }
        return true;
    }

    static void OnPedDestroy(CPed* ped) {
        if (ped && ped == gLeft.owner)
            DestroyLeftWeapon();
    }

    static void OnPedSetModel(CPed* ped) {
        if (!ped || ped != gLeft.owner)
            return;
        DestroyLeftWeapon();
    }

    class Mod {
    public:
        Mod() {
            BuildSiblingPath(gIniPath, sizeof(gIniPath), "DualWieldIII.ini");
            BuildSiblingPath(gLogPath, sizeof(gLogPath), "DualWieldIII.log");
            std::remove(gLogPath);
            LoadConfig();
            Log("DualWieldIII: initialized.");

            const bool fireOk = InstallCallPatch(
                ADDR_ATTACK_FIRE_CALL, ADDR_WEAPON_FIRE,
                reinterpret_cast<void*>(&AttackFireHook), gFirePatch);
            if (!fireOk) {
                // Visual dual-wield can still run if another mod owns an incompatible
                // attack call; only the second native shot is disabled in that case.
                Log("DualWieldIII: fire CALL hook unavailable; visual/IK path remains enabled but DoubleShot is disabled.");
                gConfig.doubleShot = false;
            }
            else {
                char line[320];
                std::snprintf(line, sizeof(line),
                    "DualWieldIII: fire hook active. previous=%p%s.",
                    reinterpret_cast<void*>(gFirePatch.previousTarget),
                    gFirePatch.chained ? " [CHAINED]" : "");
                Log(line);
            }

            ResolveSkinBonesApi();
            // Do not patch 0x4D0484 from the ASI constructor. Skin & Bones and shader
            // mods may install their own render bridges during startup; gameProcess chains
            // the settled owner instead of participating in a load-order race.
            Events::gameProcessEvent += [] {
                ResolveSkinBonesApi();
                EnsureRenderBridgeInstalled();
                UpdateLeftWeapon();
            };
            Events::pedSetModelEvent += [](CPed* ped, int) {
                OnPedSetModel(ped);
            };
            Events::pedDtorEvent += [](CPed* ped) {
                OnPedDestroy(ped);
            };
            Events::restartGameEvent += [] {
                DestroyLeftWeapon();
            };
            Events::shutdownRwEvent += [] {
                DestroyLeftWeapon();
            };
        }

        ~Mod() {
            // RW teardown is handled by shutdownRwEvent while the engine is alive.
            RestoreCallPatch(gRenderPatch);
            RestoreCallPatch(gFirePatch);
        }
    };

    static Mod gMod;
}
