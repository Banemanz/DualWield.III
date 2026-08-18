        RwFrame* root = GetFrameRootSafe(chain.nativeUpper);
        if (!root || GetFrameRootSafe(chain.nativeLower) != root ||
            GetFrameRootSafe(chain.nativeHand) != root ||
            GetFrameRootSafe(chain.mirrorUpper) != root ||
            GetFrameRootSafe(chain.mirrorLower) != root ||
            GetFrameRootSafe(chain.mirrorHand) != root)
            return false;

        chain.valid = true;
        return true;
    }

    // GTA III's hgun animation is partial: it supplies the large weapon-arm pose only
    // on the stock arm, then its gun IK adds the final correction. The working approach
    // mirrors the final shoulder->elbow and elbow->hand geometry. Skin & Bones uses the
    // same operation on the final HAnim matrix array instead of treating its animation
    // data as stock RwFrame pointers.

    static CVector BlendDirection(const CVector& from, const CVector& to, float t) {
        const CVector a = Normalize3(from, Vec3(0.0f, 1.0f, 0.0f));
        const CVector b = Normalize3(to, a);
        const float clamped = ClampFloat(t, 0.0f, 1.0f);
        return Normalize3(Add3(Scale3(a, 1.0f - clamped), Scale3(b, clamped)), b);
    }

    static float UpdateAimBlendOncePerFrame(bool active) {
        if (!active) {
            gAimBlend = 0.0f;
            gAimBlendFrame = CTimer::m_FrameCounter;
            return 0.0f;
        }

        const unsigned int frame = CTimer::m_FrameCounter;
        if (frame != gAimBlendFrame) {
            gAimBlendFrame = frame;
            if (gConfig.aimBlendFrames <= 1)
                gAimBlend = 1.0f;
            else
                gAimBlend = ClampFloat(gAimBlend + 1.0f / static_cast<float>(gConfig.aimBlendFrames), 0.0f, 1.0f);
        }
        return gAimBlend;
    }

    static bool ShouldMirrorAimPose(CPed* ped, bool nativeArmRaised, float& blend) {
        blend = 0.0f;
        if (!ped || !nativeArmRaised) {
            gAimGraceFramesRemaining = 0;
            UpdateAimBlendOncePerFrame(false);
            return false;
        }

        bool active = ped->bIsAimingGun || ped->bIsPointingGunAt ||
            ped->bIsShooting || ped->m_ePedState == PEDSTATE_AIMGUN ||
            ped->m_ePedState == PEDSTATE_ATTACK;
        if (active) {
            gAimGraceFramesRemaining = gConfig.aimGraceFrames;
        }
        else if (gAimGraceFramesRemaining > 0) {
            --gAimGraceFramesRemaining;
            active = true;
        }

        if (!active) {
            UpdateAimBlendOncePerFrame(false);
            return false;
        }

        blend = UpdateAimBlendOncePerFrame(true);
        return blend > 0.0f;
    }

    static void MirrorNativeGunArmPose(CPed* ped, const MirroredArmChain& chain, float blend) {
        if (!ped || !chain.valid || blend <= 0.0f)
            return;

        CVector nativeShoulder, nativeElbow, nativeHand;
        CVector mirrorShoulder, mirrorElbow, mirrorHand;
        if (!GetFrameWorldPosition(chain.nativeUpper, nativeShoulder) ||
            !GetFrameWorldPosition(chain.nativeLower, nativeElbow) ||
            !GetFrameWorldPosition(chain.nativeHand, nativeHand) ||
            !GetFrameWorldPosition(chain.mirrorUpper, mirrorShoulder) ||
            !GetFrameWorldPosition(chain.mirrorLower, mirrorElbow) ||
            !GetFrameWorldPosition(chain.mirrorHand, mirrorHand))
            return;

        const CVector mirrorNormal = Normalize3(ped->GetRight(), Vec3(1.0f, 0.0f, 0.0f));

        const CVector currentUpperDir = Sub3(mirrorElbow, mirrorShoulder);
        const CVector mirroredUpperDir = ReflectDirection(Sub3(nativeElbow, nativeShoulder), mirrorNormal);
        const CVector desiredUpperDir = BlendDirection(currentUpperDir, mirroredUpperDir, blend);

        Basis3 upperWorld = GetFrameWorldBasis(chain.mirrorUpper);
        upperWorld = RotateBasisFromTo(upperWorld, currentUpperDir, desiredUpperDir);
        SetFrameWorldBasis(chain.mirrorUpper, upperWorld);

        if (!GetFrameWorldPosition(chain.mirrorLower, mirrorElbow) ||
            !GetFrameWorldPosition(chain.mirrorHand, mirrorHand))
            return;

        const CVector currentLowerDir = Sub3(mirrorHand, mirrorElbow);
        const CVector mirroredLowerDir = ReflectDirection(Sub3(nativeHand, nativeElbow), mirrorNormal);
        const CVector desiredLowerDir = BlendDirection(currentLowerDir, mirroredLowerDir, blend);

        Basis3 lowerWorld = GetFrameWorldBasis(chain.mirrorLower);
        lowerWorld = RotateBasisFromTo(lowerWorld, currentLowerDir, desiredLowerDir);
        SetFrameWorldBasis(chain.mirrorLower, lowerWorld);
        RwFrameGetLTM(chain.mirrorHand);
    }

    static bool NativeGunArmIsRaised(const MirroredArmChain& chain) {
        if (!chain.valid)
            return false;
        CVector shoulder, hand;
        if (!GetFrameWorldPosition(chain.nativeUpper, shoulder) ||
            !GetFrameWorldPosition(chain.nativeHand, hand))
            return false;
        return hand.z > shoulder.z - 0.25f;
    }

    struct AxisRotation {
        CVector axis;
        float c;
        float s;
        bool identity;
        AxisRotation() : axis(Vec3(0.0f, 0.0f, 1.0f)), c(1.0f), s(0.0f), identity(true) {}
    };

    static AxisRotation MakeAxisRotation(const CVector& fromVector, const CVector& toVector, const Basis3& hint) {
        AxisRotation r;
        const CVector from = Normalize3(fromVector, Vec3(0.0f, 1.0f, 0.0f));
        const CVector to = Normalize3(toVector, from);
        const float d = ClampFloat(Dot3(from, to), -1.0f, 1.0f);
        if (d > 0.99999f)
            return r;

        CVector axis = Cross3(from, to);
        float axisLenSq = Dot3(axis, axis);
        if (axisLenSq <= 0.000001f) {
            axis = Cross3(from, hint.up);
            axisLenSq = Dot3(axis, axis);
            if (axisLenSq <= 0.000001f)
                axis = Cross3(from, hint.right);
            r.axis = Normalize3(axis, Vec3(0.0f, 0.0f, 1.0f));
            r.c = -1.0f;
            r.s = 0.0f;
        }
        else {
            const float axisLen = std::sqrt(axisLenSq);
            r.axis = Scale3(axis, 1.0f / axisLen);
            r.c = d;
            r.s = axisLen;
        }
        r.identity = false;
        return r;
    }

    static CVector ApplyAxisRotation(const CVector& v, const AxisRotation& r) {
        if (r.identity)
            return v;
        return RotateAroundAxis(v, r.axis, r.c, r.s);
    }

    static void ApplyAxisRotationToMatrix(RwMatrix& matrix, const CVector& pivot, const AxisRotation& r) {
        if (r.identity)
            return;

        Basis3 basis = BasisFromRwMatrix(&matrix);
        basis.right = ApplyAxisRotation(basis.right, r);
        basis.up = ApplyAxisRotation(basis.up, r);
        basis.at = ApplyAxisRotation(basis.at, r);
        basis = OrthonormalizeBasis(basis);

        const CVector oldPos = Vec3(matrix.pos.x, matrix.pos.y, matrix.pos.z);
        const CVector newPos = Add3(pivot, ApplyAxisRotation(Sub3(oldPos, pivot), r));

        matrix.right.x = basis.right.x; matrix.right.y = basis.right.y; matrix.right.z = basis.right.z;
        matrix.up.x = basis.up.x; matrix.up.y = basis.up.y; matrix.up.z = basis.up.z;
        matrix.at.x = basis.at.x; matrix.at.y = basis.at.y; matrix.at.z = basis.at.z;
        matrix.pos.x = newPos.x; matrix.pos.y = newPos.y; matrix.pos.z = newPos.z;
    }

    static bool NativeGunArmIsRaisedSkinned(RpHAnimHierarchy* hierarchy) {
        RwMatrix* upper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARMR);
        RwMatrix* hand = GetSkinBoneMatrix(hierarchy, BONE_SRHAND);
        return upper && hand && hand->pos.z > upper->pos.z - 0.25f;
    }

    static bool MirrorSkinnedGunArmPose(CPed* ped, RpHAnimHierarchy* hierarchy, float blend) {
        if (!ped || !hierarchy || blend <= 0.0f)
            return false;

        RwMatrix* nativeUpper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARMR);
        RwMatrix* nativeLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARMR);
        RwMatrix* nativeHand = GetSkinBoneMatrix(hierarchy, BONE_SRHAND);
        RwMatrix* mirrorUpper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARML);
        RwMatrix* mirrorLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARML);
        RwMatrix* mirrorHand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!nativeUpper || !nativeLower || !nativeHand || !mirrorUpper || !mirrorLower || !mirrorHand)
            return false;

        const CVector nativeShoulder = Vec3(nativeUpper->pos.x, nativeUpper->pos.y, nativeUpper->pos.z);
        const CVector nativeElbow = Vec3(nativeLower->pos.x, nativeLower->pos.y, nativeLower->pos.z);
        const CVector nativeHandPos = Vec3(nativeHand->pos.x, nativeHand->pos.y, nativeHand->pos.z);
        const CVector mirrorShoulder = Vec3(mirrorUpper->pos.x, mirrorUpper->pos.y, mirrorUpper->pos.z);
        CVector mirrorElbow = Vec3(mirrorLower->pos.x, mirrorLower->pos.y, mirrorLower->pos.z);
        CVector mirrorHandPos = Vec3(mirrorHand->pos.x, mirrorHand->pos.y, mirrorHand->pos.z);

        const CVector mirrorNormal = Normalize3(ped->GetRight(), Vec3(1.0f, 0.0f, 0.0f));

        const CVector currentUpperDir = Sub3(mirrorElbow, mirrorShoulder);
        const CVector reflectedUpperDir = ReflectDirection(Sub3(nativeElbow, nativeShoulder), mirrorNormal);
        const CVector targetUpperDir = BlendDirection(currentUpperDir, reflectedUpperDir, blend);
        const AxisRotation upperRot = MakeAxisRotation(currentUpperDir, targetUpperDir, BasisFromRwMatrix(mirrorUpper));

        ApplyAxisRotationToMatrix(*mirrorUpper, mirrorShoulder, upperRot);
        ApplyAxisRotationToMatrix(*mirrorLower, mirrorShoulder, upperRot);
        ApplyAxisRotationToMatrix(*mirrorHand, mirrorShoulder, upperRot);

        mirrorElbow = Vec3(mirrorLower->pos.x, mirrorLower->pos.y, mirrorLower->pos.z);
        mirrorHandPos = Vec3(mirrorHand->pos.x, mirrorHand->pos.y, mirrorHand->pos.z);
        const CVector currentLowerDir = Sub3(mirrorHandPos, mirrorElbow);
        const CVector reflectedLowerDir = ReflectDirection(Sub3(nativeHandPos, nativeElbow), mirrorNormal);
        const CVector targetLowerDir = BlendDirection(currentLowerDir, reflectedLowerDir, blend);
        const AxisRotation lowerRot = MakeAxisRotation(currentLowerDir, targetLowerDir, BasisFromRwMatrix(mirrorLower));

        ApplyAxisRotationToMatrix(*mirrorLower, mirrorElbow, lowerRot);
        ApplyAxisRotationToMatrix(*mirrorHand, mirrorElbow, lowerRot);
        return IsFiniteRwMatrix(*mirrorUpper) && IsFiniteRwMatrix(*mirrorLower) && IsFiniteRwMatrix(*mirrorHand);
    }

    static bool ApplyCurrentMirror(CPed* ped, bool refreshSkinnedHierarchy, float* outBlend = 0) {
        if (outBlend)
            *outBlend = 0.0f;
        if (!gConfig.leftArmIK || !IsEligiblePlayer(ped))
            return false;

        RpHAnimHierarchy* hierarchy = 0;
        if (GetSkinnedPedHierarchy(ped, hierarchy)) {
            // Skin & Bones changes interpolation quaternions in its replacement IK. During
            // ProcessControl the matrix array can still contain the previous pose, so the
            // fire path explicitly materializes it before deriving the second muzzle.
            if (refreshSkinnedHierarchy) {
                if (!RpHAnimHierarchyUpdateMatrices(hierarchy))
                    return false;
            }

            float blend = 0.0f;
            if (!ShouldMirrorAimPose(ped, NativeGunArmIsRaisedSkinned(hierarchy), blend))
                return false;
            if (!MirrorSkinnedGunArmPose(ped, hierarchy, blend))
                return false;
            if (outBlend)
                *outBlend = blend;
            return true;
        }

        MirroredArmChain chain;
        if (!CaptureMirroredArmChain(ped, chain))
            return false;
        float blend = 0.0f;
        if (!ShouldMirrorAimPose(ped, NativeGunArmIsRaised(chain), blend))
            return false;
        MirrorNativeGunArmPose(ped, chain, blend);
        if (outBlend)
            *outBlend = blend;
        return true;
    }

    static void ApplyRenderStageMirror(CPed* ped) {
        float blend = 0.0f;
        if (!ApplyCurrentMirror(ped, false, &blend))
            return;
        if (gLeft.owner == ped)
            UpdateLeftWeaponWorldTransform();
    }

    static void PrepareOppositePoseForFire(CPed* ped) {
        if (!ped || !gConfig.leftArmIK)
            return;
        if (ApplyCurrentMirror(ped, true, 0) && gLeft.owner == ped)
            UpdateLeftWeaponWorldTransform();
    }

    static void RenderStandaloneLeftWeapon(CPed* ped) {
        if (!ped || !IsEligiblePlayer(ped) || gLeft.owner != ped || !IsAtomicAliveForOwner(ped))
            return;

        if (gLeft.skinnedSource) {
            RpHAnimHierarchy* hierarchy = 0;
            if (!GetSkinnedPedHierarchy(ped, hierarchy) || !GetSkinBoneMatrix(hierarchy, BONE_SLHAND)) {
                DestroyLeftWeapon();
                return;
            }
        }
        else {
            RwFrame* currentHand = GetPedFrameSafe(ped, 5);
            if (!currentHand || currentHand != gLeft.sourceHandFrame) {
                DestroyLeftWeapon();
                return;
            }
        }

        if (!UpdateLeftWeaponWorldTransform())
            return;

        // Standalone atomic: no RpClumpAddAtomic, no HAnim ownership ambiguity. Its own
        // normal render callback/pipeline still runs, so shader mods can process it as a
        // regular weapon atomic without ped-clump walkers seeing it as a body skin.
        RpAtomicRender(gLeft.atomic);
    }

    static bool ComputeLeftFireSource(CPed* ped, CWeapon* weapon, CVector& out) {
        if (!ped || !weapon || gLeft.owner != ped || !gLeft.helperFrame)
            return false;

        CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType);
        if (!info)
            return false;

        // Make the muzzle source use the exact same corrected transform that is used
