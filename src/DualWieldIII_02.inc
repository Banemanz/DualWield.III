        return IsReadableAddress(frame, 0x50);
    }

    static void ResolveSkinBonesApi() {
        if (gSkinBonesProbeDone && gSkinBonesModule)
            return;

        HMODULE module = GetModuleHandleA("iii_anim.asi");
        if (!module)
            module = GetModuleHandleA("iii_anim.dll");
        if (!module) {
            // ASI load order is not guaranteed. Keep probing until the game has reached
            // normal processing so a late-loaded Skin & Bones can still be detected.
            gSkinBonesProbeDone = false;
            return;
        }

        gSkinBonesModule = module;
        gSkinBonesGetPedWeaponAtomic = reinterpret_cast<SkinBonesGetPedWeaponAtomicFn>(
            GetProcAddress(module, "IIIAnimGetPedWeaponAtomic"));
        gSkinBonesProbeDone = true;

        if (!gLoggedSkinBonesDetected) {
            char line[256];
            std::snprintf(line, sizeof(line),
                "DualWieldIII: Skin & Bones iii_anim detected at %p; weapon export=%p.",
                module, reinterpret_cast<void*>(gSkinBonesGetPedWeaponAtomic));
            Log(line);
            gLoggedSkinBonesDetected = true;
        }
    }

    struct SkinProbe {
        RpAtomic* atomic;
        RpHAnimHierarchy* hierarchy;
        SkinProbe() : atomic(0), hierarchy(0) {}
    };

    static RpAtomic* FindSkinnedAtomicCB(RpAtomic* atomic, void* data) {
        SkinProbe* probe = reinterpret_cast<SkinProbe*>(data);
        if (!probe || !atomic)
            return atomic;
        RpGeometry* geometry = RpAtomicGetGeometry(atomic);
        if (!geometry)
            return atomic;
        RpSkin* skin = RpSkinGeometryGetSkin(geometry);
        if (!skin)
            return atomic;

        RpHAnimHierarchy* hierarchy = RpSkinAtomicGetHAnimHierarchy(atomic);
        if (!hierarchy)
            return atomic;

        probe->atomic = atomic;
        probe->hierarchy = hierarchy;
        return 0; // stop once the actual skinned body atomic is found
    }

    static bool GetSkinnedPedHierarchy(CPed* ped, RpHAnimHierarchy*& hierarchy) {
        hierarchy = 0;
        if (!ped || !ped->m_pRwClump)
            return false;

        SkinProbe probe;
        RpClumpForAllAtomics(ped->m_pRwClump, FindSkinnedAtomicCB, &probe);
        if (!probe.atomic || !probe.hierarchy)
            return false;

        RpHAnimHierarchy* h = probe.hierarchy;
        if (!IsReadableAddress(h, sizeof(RpHAnimHierarchy)))
            return false;
        if (h->numNodes <= 0 || h->numNodes > 128 || !h->pMatrixArray || !h->pNodeInfo)
            return false;
        if (!IsReadableAddress(h->pMatrixArray, sizeof(RwMatrix) * static_cast<size_t>(h->numNodes)) ||
            !IsReadableAddress(h->pNodeInfo, sizeof(RpHAnimNodeInfo) * static_cast<size_t>(h->numNodes)))
            return false;

        hierarchy = h;
        return true;
    }

    static bool IsSkinnedPed(CPed* ped) {
        RpHAnimHierarchy* hierarchy = 0;
        return GetSkinnedPedHierarchy(ped, hierarchy);
    }

    static RwMatrix* GetSkinBoneMatrix(RpHAnimHierarchy* hierarchy, int boneId) {
        if (!hierarchy || !hierarchy->pMatrixArray || !hierarchy->pNodeInfo)
            return 0;
        const int index = RpHAnimIDGetIndex(hierarchy, boneId);
        if (index < 0 || index >= hierarchy->numNodes)
            return 0;
        RwMatrix* matrix = &hierarchy->pMatrixArray[index];
        return IsReadableAddress(matrix, sizeof(RwMatrix)) && IsFiniteRwMatrix(*matrix) ? matrix : 0;
    }

    static bool NativeWeaponModelReady(CPed* ped, CWeaponInfo* info, bool skinned) {
        if (!ped || !info || info->m_nModelId < 0)
            return false;
        if (ped->m_nWepModelID != info->m_nModelId)
            return false;

        if (skinned) {
            ResolveSkinBonesApi();
            // Skin & Bones replaces CPed::AddWeaponModel for skinned clumps and stores
            // the native weapon atomic outside the ped clump. Its exported accessor is
            // the best available confirmation that the right/native weapon actually exists.
            if (gSkinBonesGetPedWeaponAtomic && !gSkinBonesGetPedWeaponAtomic(ped))
                return false;
        }
        return true;
    }

    static RwFrame* GetPedFrameSafe(CPed* ped, int index) {
        if (!ped || index < 0 || index >= 12)
            return 0;
        AnimBlendFrameData* data = ped->m_apFrames[index];
        if (!IsReadableAddress(data, sizeof(AnimBlendFrameData)))
            return 0;
        RwFrame* frame = data->m_pFrame;
        if (!IsFrameReadable(frame))
            return 0;
        return frame;
    }

    static RwFrame* GetFrameRootSafe(RwFrame* frame) {
        if (!IsFrameReadable(frame))
            return 0;
        RwFrame* current = frame;
        for (int depth = 0; depth < 32; ++depth) {
            RwFrame* parent = RwFrameGetParent(current);
            if (!parent)
                return current;
            if (parent == current || !IsFrameReadable(parent))
                return 0;
            current = parent;
        }
        return 0; // corrupt/cyclic hierarchy
    }

    static void BuildGripCorrection(RwMatrix& correction) {
        static RwV3d axisX = { 1.0f, 0.0f, 0.0f };
        static RwV3d axisY = { 0.0f, 1.0f, 0.0f };
        static RwV3d axisZ = { 0.0f, 0.0f, 1.0f };
        RwV3d offset = { gConfig.offsetX, gConfig.offsetY, gConfig.offsetZ };

        RwMatrixSetIdentity(&correction);
        if (gConfig.rotX != 0.0f)
            RwMatrixRotate(&correction, &axisX, gConfig.rotX, rwCOMBINEPRECONCAT);
        if (gConfig.rotY != 0.0f)
            RwMatrixRotate(&correction, &axisY, gConfig.rotY, rwCOMBINEPRECONCAT);
        if (gConfig.rotZ != 0.0f)
            RwMatrixRotate(&correction, &axisZ, gConfig.rotZ, rwCOMBINEPRECONCAT);
        RwMatrixTranslate(&correction, &offset, rwCOMBINEPRECONCAT);
    }

    // Same matrix-walk used by GTA III CPedIK::GetWorldMatrix: modelling matrix
    // followed by every parent with POSTCONCAT. This deliberately does not depend
    // on a cached LTM being dirty/clean after the mirrored IK has just edited bones.
    static bool GetFrameWorldMatrixManual(RwFrame* frame, RwMatrix& out) {
        if (!IsFrameReadable(frame) || !RwFrameGetMatrix(frame))
            return false;

        out = *RwFrameGetMatrix(frame);
        RwFrame* current = frame;
        for (int depth = 0; depth < 32; ++depth) {
            RwFrame* parent = RwFrameGetParent(current);
            if (!parent)
                return IsFiniteRwMatrix(out);
            if (parent == current || !IsFrameReadable(parent))
                return false;
            RwMatrix* parentMatrix = RwFrameGetMatrix(parent);
            if (!parentMatrix || !IsFiniteRwMatrix(*parentMatrix))
                return false;
            RwMatrixTransform(&out, parentMatrix, rwCOMBINEPOSTCONCAT);
            current = parent;
        }
        return false; // corrupt/cyclic hierarchy
    }

    static bool PutWeaponFrameOnWorldMatrix(RwFrame* helperFrame, const RwMatrix& handWorld) {
        if (!helperFrame || !IsFiniteRwMatrix(handWorld))
            return false;

        // Weapon-local -> grip correction -> hand world. The helper is a private root,
        // so its modelling matrix is already world-space. This is the same composition
        // Only the source of handWorld differs for stock vs skinned peds.
        RwMatrix weaponWorld;
        BuildGripCorrection(weaponWorld);
        RwMatrixTransform(&weaponWorld, &handWorld, rwCOMBINEPOSTCONCAT);
        if (!IsFiniteRwMatrix(weaponWorld))
            return false;

        RwMatrix* helperMatrix = RwFrameGetMatrix(helperFrame);
        if (!helperMatrix)
            return false;
        *helperMatrix = weaponWorld;
        RwFrameUpdateObjects(helperFrame);
        return true;
    }

    static bool PutWeaponFrameOnHand(RwFrame* helperFrame, RwFrame* handFrame) {
        if (!helperFrame || !handFrame)
            return false;
        RwMatrix handWorld;
        if (!GetFrameWorldMatrixManual(handFrame, handWorld))
            return false;
        return PutWeaponFrameOnWorldMatrix(helperFrame, handWorld);
    }

    static bool PutWeaponFrameOnSkinnedHand(CPed* ped, RwFrame* helperFrame) {
        if (!ped || !helperFrame)
            return false;
        RpHAnimHierarchy* hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false;
        RwMatrix* hand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!hand)
            return false;
        return PutWeaponFrameOnWorldMatrix(helperFrame, *hand);
    }

    static bool UpdateLeftWeaponWorldTransform() {
        if (!gLeft.owner || !gLeft.atomic || !gLeft.helperFrame)
            return false;
        if (gLeft.skinnedSource)
            return PutWeaponFrameOnSkinnedHand(gLeft.owner, gLeft.helperFrame);
        if (!gLeft.sourceHandFrame)
            return false;
        return PutWeaponFrameOnHand(gLeft.helperFrame, gLeft.sourceHandFrame);
    }

    static RwObject* CreateModelInstance(CBaseModelInfo* modelInfo) {
        if (!modelInfo)
            return 0;

        // CPed::AddWeaponModel(0x4CF8F0) calls [modelInfo->vtable + 0x0C].
        // Use that exact virtual slot instead of depending on a Plugin-SDK wrapper.
        typedef RwObject* (__thiscall *CreateInstanceFn)(CBaseModelInfo*);
        void** vtable = *reinterpret_cast<void***>(modelInfo);
        if (!vtable || !vtable[3])
            return 0;
        return reinterpret_cast<CreateInstanceFn>(vtable[3])(modelInfo);
    }

    static bool CreateLeftWeapon(CPed* ped) {
        if (!ped || !ped->m_pRwClump)
            return false;

        CWeapon* weapon = ped->GetWeapon();
        if (!weapon || !IsDualWeapon(weapon->m_eWeaponType))
            return false;

        CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType);
        if (!info || info->m_nModelId < 0)
            return false;

        RpHAnimHierarchy* skinHierarchy = 0;
        const bool skinned = GetSkinnedPedHierarchy(ped, skinHierarchy);
        if (!NativeWeaponModelReady(ped, info, skinned))
            return false;

        RwFrame* sourceHand = 0;
        if (!skinned) {
            sourceHand = GetPedFrameSafe(ped, 5);
            if (!sourceHand)
                return false;
        }

        CBaseModelInfo* modelInfo = CModelInfo::GetModelInfo(info->m_nModelId);
        if (!modelInfo)
            return false;

        RwObject* rwObject = CreateModelInstance(modelInfo);
        if (!rwObject)
            return false;

        RpAtomic* atomic = reinterpret_cast<RpAtomic*>(rwObject);
        RwFrame* oldFrame = RpAtomicGetFrame(atomic);
        RwFrame* helper = RwFrameCreate();
        if (!helper) {
            RpAtomicDestroy(atomic);
            if (oldFrame)
                RwFrameDestroy(oldFrame);
            return false;
        }

        if (oldFrame)
            RwFrameDestroy(oldFrame);
        RpAtomicSetFrame(atomic, helper);

        bool posed = false;
        if (skinned) {
            RwMatrix* leftHand = GetSkinBoneMatrix(skinHierarchy, BONE_SLHAND);
            posed = leftHand && PutWeaponFrameOnWorldMatrix(helper, *leftHand);
        }
        else {
            posed = PutWeaponFrameOnHand(helper, sourceHand);
        }
        if (!posed) {
            RpAtomicDestroy(atomic);
            RwFrameDestroy(helper);
            return false;
        }

        // Keep the duplicate entirely outside the player clump. Skin & Bones itself
        // stores its native weapon atomic outside a skinned ped clump for the same kind
        // of reason: a skinned body clump is treated as HAnim-owned data by its render path.
        gLeft.owner = ped;
        gLeft.ownerClump = ped->m_pRwClump;
        gLeft.atomic = atomic;
        gLeft.helperFrame = helper;
        gLeft.sourceHandFrame = sourceHand;
        gLeft.skinnedSource = skinned;
        gLeft.modelId = info->m_nModelId;
        gLeft.weaponType = weapon->m_eWeaponType;
        return true;
    }

    static void UpdateLeftWeapon() {
