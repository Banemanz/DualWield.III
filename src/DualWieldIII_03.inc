        CPed* ped = CurrentPlayer();
        if (!IsEligiblePlayer(ped)) {
            DestroyLeftWeapon();
            return;
        }

        ResolveSkinBonesApi();

        if (gLeft.owner == ped && gLeft.ownerClump && gLeft.ownerClump != ped->m_pRwClump)
            DestroyLeftWeapon();

        CWeapon* weapon = ped->GetWeapon();
        CWeaponInfo* info = weapon ? CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType) : 0;
        const int modelId = info ? info->m_nModelId : -1;
        RpHAnimHierarchy* hierarchy = 0;
        const bool skinned = GetSkinnedPedHierarchy(ped, hierarchy);

        if (!weapon || !info || modelId < 0 || !NativeWeaponModelReady(ped, info, skinned)) {
            DestroyLeftWeapon();
            return;
        }

        if (IsAtomicAliveForOwner(ped)) {
            if (gLeft.owner != ped || gLeft.weaponType != weapon->m_eWeaponType ||
                gLeft.modelId != modelId || gLeft.skinnedSource != skinned) {
                DestroyLeftWeapon();
            }
            else if (!skinned) {
                RwFrame* currentSourceHand = GetPedFrameSafe(ped, 5);
                if (!currentSourceHand || gLeft.sourceHandFrame != currentSourceHand)
                    DestroyLeftWeapon();
                else {
                    UpdateLeftWeaponWorldTransform();
                    return;
                }
            }
            else {
                // Never interpret Skin & Bones' AnimBlendFrameData union as an RwFrame.
                // HAnim matrix-array ownership is validated fresh each update instead.
                if (!hierarchy || !GetSkinBoneMatrix(hierarchy, BONE_SLHAND))
                    DestroyLeftWeapon();
                else {
                    UpdateLeftWeaponWorldTransform();
                    return;
                }
            }
        }
        else if (gLeft.atomic || gLeft.helperFrame) {
            DestroyLeftWeapon();
        }

        CreateLeftWeapon(ped);
    }

    struct Basis3 {
        CVector right;
        CVector up;
        CVector at;
    };

    struct MirroredArmChain {
        bool valid;
        RwFrame* nativeUpper;
        RwFrame* nativeLower;
        RwFrame* nativeHand;
        RwFrame* mirrorUpper;
        RwFrame* mirrorLower;
        RwFrame* mirrorHand;

        MirroredArmChain()
            : valid(false), nativeUpper(0), nativeLower(0), nativeHand(0),
              mirrorUpper(0), mirrorLower(0), mirrorHand(0) {}
    };

    static CVector Vec3(float x, float y, float z) {
        CVector v;
        v.x = x;
        v.y = y;
        v.z = z;
        return v;
    }

    static float Dot3(const CVector& a, const CVector& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static CVector Add3(const CVector& a, const CVector& b) {
        return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
    }

    static CVector Scale3(const CVector& v, float s) {
        return Vec3(v.x * s, v.y * s, v.z * s);
    }

    static CVector Sub3(const CVector& a, const CVector& b) {
        return Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
    }

    static CVector Cross3(const CVector& a, const CVector& b) {
        return Vec3(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x);
    }

    static CVector Normalize3(const CVector& v, const CVector& fallback) {
        const float lenSq = Dot3(v, v);
        if (!IsFiniteFloat(lenSq) || lenSq <= 0.000001f)
            return fallback;
        const float invLen = 1.0f / std::sqrt(lenSq);
        return Scale3(v, invLen);
    }

    static float ClampFloat(float v, float lo, float hi) {
        if (v < lo)
            return lo;
        if (v > hi)
            return hi;
        return v;
    }

    static Basis3 OrthonormalizeBasis(const Basis3& in) {
        Basis3 out;
        out.right = Normalize3(in.right, Vec3(1.0f, 0.0f, 0.0f));
        CVector up = Sub3(in.up, Scale3(out.right, Dot3(in.up, out.right)));
        out.up = Normalize3(up, Vec3(0.0f, 1.0f, 0.0f));
        out.at = Normalize3(Cross3(out.right, out.up), in.at);
        if (Dot3(out.at, in.at) < 0.0f) {
            out.at = Scale3(out.at, -1.0f);
            out.up = Normalize3(Cross3(out.at, out.right), out.up);
        }
        return out;
    }

    static Basis3 BasisFromRwMatrix(const RwMatrix* m) {
        Basis3 out;
        if (!m) {
            out.right = Vec3(1.0f, 0.0f, 0.0f);
            out.up = Vec3(0.0f, 1.0f, 0.0f);
            out.at = Vec3(0.0f, 0.0f, 1.0f);
            return out;
        }
        out.right = Vec3(m->right.x, m->right.y, m->right.z);
        out.up = Vec3(m->up.x, m->up.y, m->up.z);
        out.at = Vec3(m->at.x, m->at.y, m->at.z);
        return OrthonormalizeBasis(out);
    }

    // A RenderWare frame basis maps local directions into its parent's space:
    // world = right*x + up*y + at*z. Compose the modelling matrices manually so
    // this still works immediately after GTA III writes IK rotations directly.
    static CVector BasisTransformDirection(const Basis3& basis, const CVector& local) {
        return Vec3(
            basis.right.x * local.x + basis.up.x * local.y + basis.at.x * local.z,
            basis.right.y * local.x + basis.up.y * local.y + basis.at.y * local.z,
            basis.right.z * local.x + basis.up.z * local.y + basis.at.z * local.z);
    }

    static CVector BasisInverseTransformDirection(const Basis3& basis, const CVector& parent) {
        return Vec3(
            Dot3(parent, basis.right),
            Dot3(parent, basis.up),
            Dot3(parent, basis.at));
    }

    static Basis3 ComposeBasis(const Basis3& parent, const Basis3& child) {
        Basis3 out;
        out.right = BasisTransformDirection(parent, child.right);
        out.up = BasisTransformDirection(parent, child.up);
        out.at = BasisTransformDirection(parent, child.at);
        return OrthonormalizeBasis(out);
    }

    static Basis3 GetFrameWorldBasis(RwFrame* frame) {
        Basis3 identity = BasisFromRwMatrix(0);
        if (!IsFrameReadable(frame) || !RwFrameGetMatrix(frame))
            return identity;

        Basis3 result = BasisFromRwMatrix(RwFrameGetMatrix(frame));
        RwFrame* current = frame;
        for (int depth = 0; depth < 32; ++depth) {
            RwFrame* parent = RwFrameGetParent(current);
            if (!parent)
                return result;
            if (parent == current || !IsFrameReadable(parent) || !RwFrameGetMatrix(parent))
                return identity;
            result = ComposeBasis(BasisFromRwMatrix(RwFrameGetMatrix(parent)), result);
            current = parent;
        }
        return identity;
    }

    static bool GetFrameWorldPosition(RwFrame* frame, CVector& out) {
        RwMatrix world;
        if (!GetFrameWorldMatrixManual(frame, world))
            return false;
        out.x = world.pos.x;
        out.y = world.pos.y;
        out.z = world.pos.z;
        return IsFiniteVector(out);
    }

    static CVector ReflectDirection(const CVector& v, const CVector& unitPlaneNormal) {
        return Sub3(v, Scale3(unitPlaneNormal, 2.0f * Dot3(v, unitPlaneNormal)));
    }

    static CVector RotateAroundAxis(const CVector& v, const CVector& unitAxis, float c, float s) {
        // Rodrigues' rotation formula, operating entirely in world space.
        return Add3(
            Add3(Scale3(v, c), Scale3(Cross3(unitAxis, v), s)),
            Scale3(unitAxis, Dot3(unitAxis, v) * (1.0f - c)));
    }

    static Basis3 RotateBasisFromTo(const Basis3& basis, const CVector& fromVector, const CVector& toVector) {
        const CVector from = Normalize3(fromVector, Vec3(0.0f, 1.0f, 0.0f));
        const CVector to = Normalize3(toVector, from);
        const float d = ClampFloat(Dot3(from, to), -1.0f, 1.0f);

        if (d > 0.99999f)
            return basis;

        CVector axis = Cross3(from, to);
        float axisLenSq = Dot3(axis, axis);
        float s = 0.0f;
        float c = d;

        if (axisLenSq <= 0.000001f) {
            // 180-degree case. Pick a stable axis perpendicular to the bone and
            // derived from the bone's own orientation so it cannot randomly flip.
            axis = Cross3(from, basis.up);
            axisLenSq = Dot3(axis, axis);
            if (axisLenSq <= 0.000001f) {
                axis = Cross3(from, basis.right);
                axisLenSq = Dot3(axis, axis);
            }
            axis = Normalize3(axis, Vec3(0.0f, 0.0f, 1.0f));
            c = -1.0f;
            s = 0.0f;
        }
        else {
            const float axisLen = std::sqrt(axisLenSq);
            axis = Scale3(axis, 1.0f / axisLen);
            s = axisLen; // |cross(from,to)| == sin(angle) for normalized vectors.
        }

        Basis3 out;
        out.right = RotateAroundAxis(basis.right, axis, c, s);
        out.up = RotateAroundAxis(basis.up, axis, c, s);
        out.at = RotateAroundAxis(basis.at, axis, c, s);
        return OrthonormalizeBasis(out);
    }

    static void SetFrameWorldBasis(RwFrame* frame, const Basis3& desiredWorld) {
        if (!frame || !IsFiniteVector(desiredWorld.right) ||
            !IsFiniteVector(desiredWorld.up) || !IsFiniteVector(desiredWorld.at))
            return;

        if (!IsFrameReadable(frame))
            return;
        RwFrame* parentFrame = RwFrameGetParent(frame);
        if (parentFrame && !IsFrameReadable(parentFrame))
            return;
        const Basis3 parentWorld = parentFrame
            ? GetFrameWorldBasis(parentFrame)
            : BasisFromRwMatrix(0);

        Basis3 local;
        local.right = BasisInverseTransformDirection(parentWorld, desiredWorld.right);
        local.up = BasisInverseTransformDirection(parentWorld, desiredWorld.up);
        local.at = BasisInverseTransformDirection(parentWorld, desiredWorld.at);
        local = OrthonormalizeBasis(local);

        RwMatrix* current = RwFrameGetMatrix(frame);
        if (!current)
            return;

        // IMPORTANT: do not merely write RwFrame::modelling and later dirty only the
        // upper arm. RenderWare tracks dirty state per frame. Direct matrix writes could leave
        // the lower arm/hand using cached LTMs, which makes the visual result look like
        // the untouched fight/locomotion pose. RwFrameTransform marks THIS frame and
        // the hierarchy root dirty exactly like RenderWare's own frame mutators.
        RwMatrix replacement = *current;
        replacement.right.x = local.right.x;
        replacement.right.y = local.right.y;
        replacement.right.z = local.right.z;
        replacement.up.x = local.up.x;
        replacement.up.y = local.up.y;
        replacement.up.z = local.up.z;
        replacement.at.x = local.at.x;
        replacement.at.y = local.at.y;
        replacement.at.z = local.at.z;
        // replacement.pos intentionally remains the model's original local translation.
        RwFrameTransform(frame, &replacement, rwCOMBINEREPLACE);
    }

    static bool CaptureMirroredArmChain(CPed* ped, MirroredArmChain& chain) {
        if (!ped || !ped->m_pRwClump)
            return false;

        // GTA III 1.0 EN IDB:
        //   m_apFrames[4] = Supperarmr   (native weapon upper arm)
        //   m_apFrames[6] = SRhand       (native weapon hand)
        //   m_apFrames[3] = Supperarml   (opposite upper arm)
        //   m_apFrames[5] = SLhand       (opposite hand)
        chain.nativeUpper = GetPedFrameSafe(ped, 4);
        chain.nativeHand = GetPedFrameSafe(ped, 6);
        chain.mirrorUpper = GetPedFrameSafe(ped, 3);
        chain.mirrorHand = GetPedFrameSafe(ped, 5);
        if (!chain.nativeUpper || !chain.nativeHand || !chain.mirrorUpper || !chain.mirrorHand)
            return false;

        chain.nativeLower = RwFrameGetParent(chain.nativeHand);
        chain.mirrorLower = RwFrameGetParent(chain.mirrorHand);
        if (!IsFrameReadable(chain.nativeLower) || !IsFrameReadable(chain.mirrorLower))
            return false;
        if (chain.nativeLower == chain.nativeUpper || chain.mirrorLower == chain.mirrorUpper)
            return false;

        // A ragdoll or skin replacement can temporarily detach/rehome a limb. Only touch
        // the arm when all six frames still resolve to the same skeleton root.
