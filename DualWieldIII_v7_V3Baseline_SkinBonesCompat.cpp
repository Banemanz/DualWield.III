/*
    DualWieldIII v7 - V3Baseline SkinBonesCompat
    GTA III 1.0 EN dual-wield backport for the one-handed firearms.

    Reverse-engineering anchors (GTA III 1.0 EN):
      CPed::Attack()                          0x4E6BA0
        -> CWeapon::Fire call                0x4E6EBF
      CWeapon::Fire(CEntity*, CVector*)      0x55C380
      CWeapon::FireInstantHit(...)           0x55D2E0
      CPedIK::PointGunInDirection(...)       0x4ED9B0
      CPedIK::PointGunInDirectionUsingArm    0x4EDB20
      CPed::AddWeaponModel(int)              0x4CF8F0
      CPed::Render -> CEntity::Render CALL   0x4D0484

    Ped frame table from GTA III's CPedModelInfo::m_pPedIds:
      m_apFrames[3] = Supperarml
      m_apFrames[4] = Supperarmr
      m_apFrames[5] = SLhand
      m_apFrames[6] = SRhand

    Design:
      - The original/right weapon model remains 100% native.
      - A second weapon atomic is created from the same GTA III model and attached
        to a private ROOT RwFrame owned by this ASI. Neither the helper nor the atomic
        is inserted into Claude's skeleton/ped clump; every frame the helper is rebuilt
        from the opposite hand's world matrix plus a configurable local grip correction.
      - CPed::Attack's single native CWeapon::Fire call is intercepted. The native
        shot is performed first. If it succeeded and enough ammo/state remains, the
        SAME native CWeapon::Fire is invoked again using the left weapon's muzzle.
      - GTA III has only one gun-arm IK state and its hgun partial animation poses
        only the stock firing arm. v7 deliberately keeps the proven v3 render-stage mirror: the FINAL
        stock arm pose is mirrored immediately before the CPed::Render -> CEntity::Render call.
        Stock PC skeletons use RwFrame transforms; Skin & Bones/Xbox skeletons use the
        RpHAnim hierarchy matrix array and are never interpreted as RwFrame pointers.
      - The second shot still goes through a real CWeapon::Fire path. For Colt/Uzi,
        CWeapon::FireInstantHit uses the supplied fireSource for the native point light,
        gunflash, gunsmoke and gunshell, so no duplicate/fake muzzle particle is added.
      - Compatibility hardening: safe on-foot ped-state gating, model/frame replacement
        handling, finite muzzle validation, chainable pre-existing CALL hooks, and hook
        restore logic that never overwrites a later mod's patch.
      - Ragdoll/shader compatibility: unlike v3, the duplicate weapon atomic is NEVER
        inserted into the player's RpClump. It remains a standalone non-skinned atomic.
        A chainable bridge on CPed::Render's final CALL mirrors the arm before the existing
        render target (including Skin & Bones) and renders the private weapon afterward.
        This prevents external ped-clump HAnim walkers from mistaking it for body skin.

    Supported game executable: classic GTA III 1.0 EN.
    The hooks validate their original CALL opcodes/targets before patching; if an
    incompatible executable is detected, the mod leaves code untouched.
*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>

#include "plugin.h"
#include "RenderWare.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CPedIK.h"
#include "CWeapon.h"
#include "CWeaponInfo.h"
#include "CModelInfo.h"
#include "CTimer.h"

using namespace plugin;

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace DualWieldIII {

    static const uintptr_t ADDR_ATTACK_FIRE_CALL = 0x4E6EBF;
    static const uintptr_t ADDR_WEAPON_FIRE       = 0x55C380;
    static const uintptr_t ADDR_PED_RENDER_CALL  = 0x4D0484;
    static const uintptr_t ADDR_ENTITY_RENDER      = 0x474BD0;

    typedef bool (__thiscall *WeaponFireFn)(CWeapon*, CEntity*, CVector*);
    typedef void (__thiscall *PedRenderCallFn)(CPed*);
    typedef RpAtomic* (__cdecl *SkinBonesGetPedWeaponAtomicFn)(CPed*);

    enum SkinBoneTag {
        BONE_SWAIST      = 0,
        BONE_SUPPERARMR  = 10,
        BONE_SLOWERARMR  = 11,
        BONE_SRHAND      = 12,
        BONE_SUPPERARML  = 13,
        BONE_SLOWERARML  = 14,
        BONE_SLHAND      = 15
    };

    struct Config {
        bool enabled;
        bool colt45;
        bool uzi;
        bool doubleShot;
        bool leftArmIK;
        bool chainExistingCallHooks;
        int aimGraceFrames;
        int aimBlendFrames;
        float offsetX;
        float offsetY;
        float offsetZ;
        float rotX;
        float rotY;
        float rotZ;

        Config()
            : enabled(true), colt45(true), uzi(true), doubleShot(true), leftArmIK(true),
              chainExistingCallHooks(true), aimGraceFrames(2), aimBlendFrames(3),
              offsetX(0.04f), offsetY(-0.05f), offsetZ(0.0f),
              rotX(180.0f), rotY(0.0f), rotZ(0.0f) {}
    };

    struct LeftWeaponRuntime {
        CPed* owner;
        RpClump* ownerClump;
        RpAtomic* atomic;
        RwFrame* helperFrame;
        RwFrame* sourceHandFrame;
        bool skinnedSource;
        int modelId;
        eWeaponType weaponType;

        LeftWeaponRuntime()
            : owner(0), ownerClump(0), atomic(0), helperFrame(0), sourceHandFrame(0), skinnedSource(false), modelId(-1), weaponType(WEAPONTYPE_UNARMED) {}
    };

    struct CallPatch {
        uintptr_t address;
        uintptr_t previousTarget;
        uintptr_t hookTarget;
        int32_t originalRel;
        bool installed;
        bool chained;

        CallPatch()
            : address(0), previousTarget(0), hookTarget(0), originalRel(0),
              installed(false), chained(false) {}
    };

    static Config gConfig;
    static LeftWeaponRuntime gLeft;
    static CallPatch gFirePatch;
    static CallPatch gRenderPatch;
    static char gIniPath[MAX_PATH] = {};
    static char gLogPath[MAX_PATH] = {};
    static bool gLoggedFirstRenderMirror = false;
    static unsigned int gRenderMirrorHits = 0;
    static unsigned int gSecondShotAttempts = 0;
    static unsigned int gSecondShotSuccess = 0;
    static bool gLoggedFirstSecondShot = false;
    static bool gLoggedClumpReplacement = false;
    static bool gLoggedRuntimeSummary = false;
    static int gAimGraceFramesRemaining = 0;
    static HMODULE gSkinBonesModule = 0;
    static SkinBonesGetPedWeaponAtomicFn gSkinBonesGetPedWeaponAtomic = 0;
    static bool gSkinBonesProbeDone = false;
    static bool gLoggedSkinBonesDetected = false;
    static bool gLoggedSkinnedPedMode = false;
    static bool gLoggedRenderRechain = false;
    static bool gLoggedCreateFailure = false;
    static unsigned int gCreateFailureFrame = 0;
    static float gAimBlend = 0.0f;
    static unsigned int gAimBlendFrame = 0xFFFFFFFFu;

    static void BuildSiblingPath(char* out, size_t outSize, const char* filename) {
        if (!out || outSize == 0)
            return;
        out[0] = '\0';

        char modulePath[MAX_PATH] = {};
        GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), modulePath, MAX_PATH);
        char* slashA = std::strrchr(modulePath, '\\');
        char* slashB = std::strrchr(modulePath, '/');
        char* slash = slashA;
        if (!slash || (slashB && slashB > slash))
            slash = slashB;
        if (slash)
            *(slash + 1) = '\0';
        else
            modulePath[0] = '\0';

        std::snprintf(out, outSize, "%s%s", modulePath, filename);
        out[outSize - 1] = '\0';
    }

    static void Log(const char* text) {
        if (!text || !gLogPath[0])
            return;
        FILE* f = std::fopen(gLogPath, "a");
        if (!f)
            return;
        std::fprintf(f, "%s\n", text);
        std::fclose(f);
    }

    static bool ReadBool(const char* key, bool fallback) {
        return GetPrivateProfileIntA("DualWield", key, fallback ? 1 : 0, gIniPath) != 0;
    }

    static float ReadFloat(const char* key, float fallback) {
        char fallbackText[64] = {};
        char value[64] = {};
        std::snprintf(fallbackText, sizeof(fallbackText), "%.6f", fallback);
        GetPrivateProfileStringA("DualWield", key, fallbackText, value, sizeof(value), gIniPath);
        float parsed = fallback;
        if (std::sscanf(value, "%f", &parsed) != 1)
            return fallback;
        return parsed;
    }

    static void LoadConfig() {
        gConfig.enabled    = ReadBool("Enabled", true);
        gConfig.colt45     = ReadBool("Colt45", true);
        gConfig.uzi        = ReadBool("Uzi", true);
        gConfig.doubleShot = ReadBool("DoubleShot", true);
        gConfig.leftArmIK  = ReadBool("LeftArmIK", true);
        gConfig.chainExistingCallHooks = ReadBool("ChainExistingCallHooks", true);
        gConfig.aimGraceFrames = static_cast<int>(GetPrivateProfileIntA("DualWield", "AimGraceFrames", 2, gIniPath));
        if (gConfig.aimGraceFrames < 0) gConfig.aimGraceFrames = 0;
        if (gConfig.aimGraceFrames > 8) gConfig.aimGraceFrames = 8;
        gConfig.aimBlendFrames = static_cast<int>(GetPrivateProfileIntA("DualWield", "AimBlendFrames", 3, gIniPath));
        if (gConfig.aimBlendFrames < 0) gConfig.aimBlendFrames = 0;
        if (gConfig.aimBlendFrames > 8) gConfig.aimBlendFrames = 8;
        gConfig.offsetX    = ReadFloat("OffsetX", 0.04f);
        gConfig.offsetY    = ReadFloat("OffsetY", -0.05f);
        gConfig.offsetZ    = ReadFloat("OffsetZ", 0.0f);
        gConfig.rotX       = ReadFloat("RotationX", 180.0f);
        gConfig.rotY       = ReadFloat("RotationY", 0.0f);
        gConfig.rotZ       = ReadFloat("RotationZ", 0.0f);
    }

    static bool IsDualWeapon(eWeaponType type) {
        switch (type) {
        case WEAPONTYPE_COLT45:
            return gConfig.colt45;
        case WEAPONTYPE_UZI:
            return gConfig.uzi;
        default:
            return false;
        }
    }

    static CPed* CurrentPlayer() {
        return FindPlayerPed();
    }

    static bool IsUnsafeDualWieldState(CPed* ped) {
        if (!ped || ped->m_bInVehicle)
            return true;

        // Explicit physical/animation ownership flags. Keep our render-stage arm edit
        // away from falling/get-up/death transitions where a ragdoll system is likely
        // to own the same frames. Do not use bDontAcceptIKLookAts here: that flag is
        // about look-at IK and is not a reliable generic ragdoll signal.
        if (ped->bIsPedDieAnimPlaying || ped->bFallenDown ||
            ped->bGetUpAnimStarted || ped->bIsInTheAir)
            return true;

        // Never impose the mirrored gun arm while a vehicle/death/ragdoll/special-state
        // animation owns the body. This is intentionally conservative: missing one frame
        // of the second gun is preferable to fighting another skeleton controller.
        switch (ped->m_ePedState) {
        case PEDSTATE_DUMMY:
        case PEDSTATE_STATES_NO_AI:
        case PEDSTATE_ON_FIRE:
        case PEDSTATE_JUMP:
        case PEDSTATE_FALL:
        case PEDSTATE_GETUP:
        case PEDSTATE_STAGGER:
        case PEDSTATE_DIVE_AWAY:
        case PEDSTATE_ENTER_TRAIN:
        case PEDSTATE_EXIT_TRAIN:
        case PEDSTATE_DRIVING:
        case PEDSTATE_PASSENGER:
        case PEDSTATE_TAXI_PASSENGER:
        case PEDSTATE_OPEN_DOOR:
        case PEDSTATE_DIE:
        case PEDSTATE_DEAD:
        case PEDSTATE_CARJACK:
        case PEDSTATE_DRAG_FROM_CAR:
        case PEDSTATE_ENTER_CAR:
        case PEDSTATE_STEAL_CAR:
        case PEDSTATE_EXIT_CAR:
        case PEDSTATE_HANDS_UP:
        case PEDSTATE_ARRESTED:
            return true;
        default:
            return false;
        }
    }

    static bool IsEligiblePlayer(CPed* ped, CWeapon* weapon = 0) {
        if (!gConfig.enabled || !ped || ped != CurrentPlayer() || IsUnsafeDualWieldState(ped))
            return false;
        if (!ped->m_pRwClump)
            return false;

        CWeapon* active = ped->GetWeapon();
        if (!active)
            return false;
        if (weapon && active != weapon)
            return false;
        return IsDualWeapon(active->m_eWeaponType);
    }

    static bool IsAtomicAliveForOwner(CPed* ped) {
        // A skinned Skin & Bones ped intentionally has no usable RwFrame in m_apFrames[]:
        // that union member is an HAnim interpolation frame pointer instead. Only require
        // sourceHandFrame for the stock non-skinned path.
        return ped && gLeft.owner == ped && gLeft.ownerClump && gLeft.atomic &&
               gLeft.helperFrame && (gLeft.skinnedSource || gLeft.sourceHandFrame) &&
               ped->m_pRwClump == gLeft.ownerClump;
    }

    static void DestroyLeftWeapon() {
        // v7 owns both of these objects independently. The atomic is deliberately NOT
        // in the player's clump, so cleanup never calls RpClumpRemoveAtomic and remains
        // safe even if a skin/ragdoll mod replaced the player's clump this frame.
        if (gLeft.atomic) {
            RpAtomicDestroy(gLeft.atomic);
            gLeft.atomic = 0;
        }
        if (gLeft.helperFrame) {
            RwFrameDestroy(gLeft.helperFrame);
            gLeft.helperFrame = 0;
        }
        gLeft = LeftWeaponRuntime();
        gAimGraceFramesRemaining = 0;
        gAimBlend = 0.0f;
        gAimBlendFrame = 0xFFFFFFFFu;
    }

    static bool IsFiniteFloat(float v) {
        return std::isfinite(v) != 0;
    }

    static bool IsFiniteVector(const CVector& v) {
        return IsFiniteFloat(v.x) && IsFiniteFloat(v.y) && IsFiniteFloat(v.z);
    }

    static bool IsFiniteRwVector(const RwV3d& v) {
        return IsFiniteFloat(v.x) && IsFiniteFloat(v.y) && IsFiniteFloat(v.z);
    }

    static bool IsFiniteRwMatrix(const RwMatrix& m) {
        return IsFiniteRwVector(m.right) && IsFiniteRwVector(m.up) &&
               IsFiniteRwVector(m.at) && IsFiniteRwVector(m.pos);
    }

    static bool IsReadableAddress(const void* ptr, size_t bytes = sizeof(void*)) {
        if (!ptr || bytes == 0)
            return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t finish = begin + bytes;
        const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBegin + mbi.RegionSize;
        return finish >= begin && begin >= regionBegin && finish <= regionEnd;
    }

    static bool IsFrameReadable(RwFrame* frame) {
        // We only need enough of RwFrame to safely use its object/parent/modelling fields.
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
                "DualWieldIII v7: Skin & Bones iii_anim detected at %p; weapon export=%p.",
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
        // used by v3; only the source of handWorld differs for stock vs skinned peds.
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

        if (skinned && !gLoggedSkinnedPedMode) {
            char line[320];
            std::snprintf(line, sizeof(line),
                "DualWieldIII v7: skinned ped path active; HAnim=%p nodes=%d leftHand=%p. m_apFrames[*].m_pFrame is NOT treated as RwFrame.",
                skinHierarchy, skinHierarchy ? skinHierarchy->numNodes : 0,
                skinHierarchy ? GetSkinBoneMatrix(skinHierarchy, BONE_SLHAND) : 0);
            Log(line);
            gLoggedSkinnedPedMode = true;
        }
        return true;
    }

    static void LogCreateFailureOccasionally(CPed* ped, CWeapon* weapon, CWeaponInfo* info, bool skinned) {
        const unsigned int frame = CTimer::m_FrameCounter;
        if (gLoggedCreateFailure && frame - gCreateFailureFrame < 300)
            return;
        gLoggedCreateFailure = true;
        gCreateFailureFrame = frame;

        char line[512];
        RpHAnimHierarchy* hierarchy = 0;
        const bool hasHierarchy = GetSkinnedPedHierarchy(ped, hierarchy);
        const void* nativeWeapon = (gSkinBonesGetPedWeaponAtomic && ped)
            ? reinterpret_cast<void*>(gSkinBonesGetPedWeaponAtomic(ped)) : 0;
        std::snprintf(line, sizeof(line),
            "DualWieldIII v7: create deferred. ped=%p state=%d inVeh=%d weapon=%d modelField=%d infoModel=%d skinned=%d hierarchy=%p S&Bweapon=%p.",
            ped, ped ? static_cast<int>(ped->m_ePedState) : -1,
            ped ? static_cast<int>(ped->m_bInVehicle) : -1,
            weapon ? static_cast<int>(weapon->m_eWeaponType) : -1,
            ped ? ped->m_nWepModelID : -999,
            info ? info->m_nModelId : -999,
            skinned ? 1 : 0, hasHierarchy ? hierarchy : 0, nativeWeapon);
        Log(line);
    }

    static void UpdateLeftWeapon() {
        CPed* ped = CurrentPlayer();
        if (!IsEligiblePlayer(ped)) {
            DestroyLeftWeapon();
            return;
        }

        ResolveSkinBonesApi();

        if (gLeft.owner == ped && gLeft.ownerClump && gLeft.ownerClump != ped->m_pRwClump) {
            if (!gLoggedClumpReplacement) {
                Log("DualWieldIII v7: player clump replaced; destroying standalone second weapon and rebinding cleanly.");
                gLoggedClumpReplacement = true;
            }
            DestroyLeftWeapon();
        }

        CWeapon* weapon = ped->GetWeapon();
        CWeaponInfo* info = weapon ? CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType) : 0;
        const int modelId = info ? info->m_nModelId : -1;
        RpHAnimHierarchy* hierarchy = 0;
        const bool skinned = GetSkinnedPedHierarchy(ped, hierarchy);

        if (!weapon || !info || modelId < 0 || !NativeWeaponModelReady(ped, info, skinned)) {
            DestroyLeftWeapon();
            LogCreateFailureOccasionally(ped, weapon, info, skinned);
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

        if (!CreateLeftWeapon(ped))
            LogCreateFailureOccasionally(ped, weapon, info, skinned);
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
        // upper arm. RenderWare tracks dirty state per frame. The v2 code could leave
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
    // on the stock arm, then its gun IK adds the final correction. v3's successful look
    // came from mirroring the FINAL shoulder->elbow and elbow->hand geometry. v7 keeps
    // that exact idea. For Skin & Bones we perform the same operation on the final HAnim
    // matrix array instead of pretending AnimBlendFrameData::m_pFrame is an RwFrame.

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

    static bool ApplyCurrentV3Mirror(CPed* ped, bool refreshSkinnedHierarchy, float* outBlend = 0) {
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
        if (!ApplyCurrentV3Mirror(ped, false, &blend))
            return;

        ++gRenderMirrorHits;
        if (gLeft.owner == ped)
            UpdateLeftWeaponWorldTransform();

        if (!gLoggedFirstRenderMirror) {
            RpHAnimHierarchy* hierarchy = 0;
            const bool skinned = GetSkinnedPedHierarchy(ped, hierarchy);
            char line[320];
            std::snprintf(line, sizeof(line),
                "DualWieldIII v7: v3 render-stage mirror active mode=%s blend=%.2f hierarchy=%p.",
                skinned ? "Skin&Bones/HAnim" : "stock/RwFrame", blend, hierarchy);
            Log(line);
            gLoggedFirstRenderMirror = true;
        }
    }

    static void PrepareOppositePoseForFire(CPed* ped) {
        if (!ped || !gConfig.leftArmIK)
            return;
        if (ApplyCurrentV3Mirror(ped, true, 0) && gLeft.owner == ped)
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

        // v3 behavior, but with an explicit Skin & Bones backend: materialize the
        // skinned hierarchy if needed, mirror the finished native arm pose, then
        // rebuild the private weapon frame BEFORE deriving the second muzzle.
        PrepareOppositePoseForFire(ped);

        CVector leftSource;
        if (!ComputeLeftFireSource(ped, weapon, leftSource))
            return firedRight;

        ++gSecondShotAttempts;
        const bool firedLeft = fireFn(weapon, shooter, &leftSource);
        if (firedLeft) {
            ++gSecondShotSuccess;

            // Do NOT add a manual flash here. GTA III FireInstantHit uses fireSource for
            // CPointLights::AddLight, GUNFLASH_NOANIM, GUNSMOKE2 and AddGunshell for
            // both Colt .45 and Uzi. A successful second native Fire therefore already
            // produces the complete stock muzzle package at this leftSource.
            if (!gLoggedFirstSecondShot) {
                char line[320];
                std::snprintf(line, sizeof(line),
                    "DualWieldIII v7: second native shot succeeded at muzzle %.3f %.3f %.3f; native flash/smoke/light/shell path active.",
                    leftSource.x, leftSource.y, leftSource.z);
                Log(line);
                gLoggedFirstSecondShot = true;
            }
        }

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
        // The bridge is installed on CPed::Render's final CALL. Apply the v3 mirror
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
        // later, DO NOT chase/re-chain it every frame: a late wrapper may have captured
        // our bridge as its predecessor, and hooking that wrapper back can create a cycle.
        // Leave the newer owner intact and log once instead of risking recursive rendering.
        if (gRenderPatch.installed) {
            if (!gLoggedRenderRechain) {
                char line[320];
                std::snprintf(line, sizeof(line),
                    "DualWieldIII v7: render CALL was replaced after our bridge (current=%p); not re-hooking to avoid a hook cycle.",
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
                "DualWieldIII v7: render bridge active. previous=%p%s.",
                reinterpret_cast<void*>(gRenderPatch.previousTarget),
                gRenderPatch.chained ? " [CHAINED]" : "");
            Log(line);
        }
        return true;
    }

    static void LogRuntimeSummary() {
        if (gLoggedRuntimeSummary)
            return;
        char line[320];
        std::snprintf(line, sizeof(line),
            "DualWieldIII v7 summary: render mirrors=%u second-shot attempts=%u success=%u skinnedMode=%d renderPrev=%p.",
            gRenderMirrorHits, gSecondShotAttempts, gSecondShotSuccess,
            gLoggedSkinnedPedMode ? 1 : 0, reinterpret_cast<void*>(gRenderPatch.previousTarget));
        Log(line);
        gLoggedRuntimeSummary = true;
    }

    static void OnPedDestroy(CPed* ped) {
        if (ped && ped == gLeft.owner)
            DestroyLeftWeapon();
    }

    static void OnPedSetModel(CPed* ped) {
        if (!ped || ped != gLeft.owner)
            return;
        Log("DualWieldIII v7: pedSetModelEvent observed; destroying standalone second weapon before rebinding frames.");
        DestroyLeftWeapon();
    }

    class Mod {
    public:
        Mod() {
            BuildSiblingPath(gIniPath, sizeof(gIniPath), "DualWieldIII.ini");
            BuildSiblingPath(gLogPath, sizeof(gLogPath), "DualWieldIII_v7_V3Baseline_SkinBonesCompat.log");
            std::remove(gLogPath);
            LoadConfig();

            const bool fireOk = InstallCallPatch(
                ADDR_ATTACK_FIRE_CALL, ADDR_WEAPON_FIRE,
                reinterpret_cast<void*>(&AttackFireHook), gFirePatch);
            if (!fireOk) {
                // Visual dual-wield can still run if another mod owns an incompatible
                // attack call; only the second native shot is disabled in that case.
                Log("DualWieldIII v7: fire CALL hook unavailable; visual/IK path remains enabled but DoubleShot is disabled.");
                gConfig.doubleShot = false;
            }
            else {
                char line[320];
                std::snprintf(line, sizeof(line),
                    "DualWieldIII v7: fire hook active. previous=%p%s.",
                    reinterpret_cast<void*>(gFirePatch.previousTarget),
                    gFirePatch.chained ? " [CHAINED]" : "");
                Log(line);
            }

            ResolveSkinBonesApi();
            // Deliberately do NOT patch 0x4D0484 from the ASI constructor. Skin & Bones
            // and shader mods may install their own render bridges during startup. The
            // first gameProcess tick occurs after ASI initialization and chains the final
            // owner instead of participating in a load-order race.
            Log("DualWieldIII v7: render bridge installation deferred until gameProcess so Skin & Bones / shader startup hooks settle first.");

            Log("DualWieldIII v7: second weapon uses standalone RpAtomic rendering; it is never inserted into the player ped clump.");
            Log("DualWieldIII v7: stock RwFrame and Skin & Bones/Xbox HAnim skeletons use separate pose backends; skinned m_apFrames[*].m_pFrame is never treated as RwFrame.");
            Log("DualWieldIII v7: no CPedIK call-site patch is installed; Skin & Bones may keep ownership of its replacement IK functions.");
            Log("DualWieldIII v7: crash 0x5B1791 is RpHAnimHierarchyUpdateMatrices(NULL); standalone rendering avoids exposing the weapon atomic to external ped HAnim walkers.");
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
                LogRuntimeSummary();
                DestroyLeftWeapon();
            };
        }

        ~Mod() {
            // RW teardown is handled by shutdownRwEvent while the engine is alive.
            LogRuntimeSummary();
            RestoreCallPatch(gRenderPatch);
            RestoreCallPatch(gFirePatch);
        }
    };

    static Mod gMod;
}
