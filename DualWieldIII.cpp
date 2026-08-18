/*
    DualWieldIII v21 - VCStyleSymmetricAim
    GTA III 1.0 EN dual-wield backport for the one-handed firearms.

    The v15 movement/aim continuity fix remains authoritative: the movement-originated
    ClearAimFlag call at 0x4C5BF3 is suppressed only while target/fire intent is held,
    preventing forward locomotion from alternating the native gun arm with vanilla walk.

    v17 full-pitch behavior is also retained: a native pose is accepted because GTA
    actually ran CPed::AimGun, not because the weapon hand passed an arbitrary height test.

    v19's coherent local-HAnim controller and v15 aim-continuity path remain intact.
    v21 changes only the arm target geometry to match the newer Vice City implementation:
    mirror the finished native shoulder->elbow and elbow->hand segment directions across
    Claude's sagittal plane, then solve those directions through the opposite arm's LOCAL
    HAnim quaternions. This removes v20's mistake of copying the native arm direction
    unchanged onto the opposite shoulder, which preserves the wrong lateral component and
    makes the twin arm curl inward/cross-body instead of aiming straight ahead symmetrically.

    The second gun socket remains deterministic. On Skin & Bones peds, its constant
    opposite-hand grip correction is derived from the skin's own inverse bind matrices
    (RpSkinGetSkinToBoneMatrices, GTA III 1.0 EN 0x5B10D0). The correction is therefore
    skeleton-relative and cannot switch among 180-degree candidates while firing.
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
#include "CPad.h"

using namespace plugin;

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace DualWieldIII {

    static const uintptr_t ADDR_ATTACK_FIRE_CALL = 0x4E6EBF;
    static const uintptr_t ADDR_WEAPON_FIRE = 0x55C380;
    static const uintptr_t ADDR_PED_AIMGUN_CALL = 0x4CB037;
    static const uintptr_t ADDR_PED_AIMGUN = 0x4C6AA0;
    static const uintptr_t ADDR_MOVE_CLEAR_AIM_CALL = 0x4C5BF3;
    static const uintptr_t ADDR_PED_CLEAR_AIM = 0x4C6A50;
    static const uintptr_t ADDR_PED_RENDER_CALL = 0x4D0484;
    static const uintptr_t ADDR_ENTITY_RENDER = 0x474BD0;
    static const uintptr_t ADDR_ANIMBLEND_FIND_FRAME = 0x405430;
    static const uintptr_t ADDR_SKIN_GET_SKIN_TO_BONE_MATRICES = 0x5B10D0;

    typedef bool(__thiscall* WeaponFireFn)(CWeapon*, CEntity*, CVector*);
    typedef void(__thiscall* PedAimGunFn)(CPed*);
    typedef void(__thiscall* PedClearAimFn)(CPed*);
    typedef void(__thiscall* PedRenderCallFn)(CPed*);
    typedef void* (__cdecl* AnimBlendFindFrameFn)(RpClump*, const char*);
    typedef RpAtomic* (__cdecl* SkinBonesGetPedWeaponAtomicFn)(CPed*);
    typedef RwMatrix* (__cdecl* SkinGetSkinToBoneMatricesFn)(RpSkin*);

    enum SkinBoneTag {
        BONE_SWAIST = 0,
        BONE_SUPPERARMR = 10,
        BONE_SLOWERARMR = 11,
        BONE_SRHAND = 12,
        BONE_SUPPERARML = 13,
        BONE_SLOWERARML = 14,
        BONE_SLHAND = 15
    };

    struct Config {
        bool enabled;
        bool colt45;
        bool uzi;
        bool doubleShot;
        bool leftArmIK;
        bool chainExistingCallHooks;
        int aimBlendFrames;
        int aimHoldFrames;
        float aimPoseBlend;
        float firePoseBlend;
        bool lateProcessRepair;
        bool stabilizeSecondHand;
        float handAimBlend;
        float handAimLimitDeg;
        bool stabilizeSecondHandRoll;
        float handRollBlend;
        float handRollLimitDeg;
        bool copyNativeGripBasis;
        float armReachScale;
        float elbowOutwardDeg;
        bool logHandState;
        int handStateLogInterval;
        float offsetX;
        float offsetY;
        float offsetZ;
        float rotX;
        float rotY;
        float rotZ;

        Config()
            : enabled(true), colt45(true), uzi(true), doubleShot(true), leftArmIK(true),
            chainExistingCallHooks(true), aimBlendFrames(1), aimHoldFrames(4),
            aimPoseBlend(1.0f), firePoseBlend(1.0f),
            lateProcessRepair(false), stabilizeSecondHand(true),
            handAimBlend(1.0f), handAimLimitDeg(90.0f),
            stabilizeSecondHandRoll(true), handRollBlend(1.0f), handRollLimitDeg(90.0f),
            copyNativeGripBasis(true), armReachScale(0.92f), elbowOutwardDeg(0.0f),
            logHandState(false), handStateLogInterval(60),
            offsetX(0.0f), offsetY(0.0f), offsetZ(0.0f),
            rotX(0.0f), rotY(0.0f), rotZ(0.0f) {
        }
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
            : owner(0), ownerClump(0), atomic(0), helperFrame(0), sourceHandFrame(0), skinnedSource(false), modelId(-1), weaponType(WEAPONTYPE_UNARMED) {
        }
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
            installed(false), chained(false) {
        }
    };


    static const int PED_FRAME_SECOND_UPPER = 3;
    static const int PED_FRAME_NATIVE_UPPER = 4;
    static const int PED_FRAME_SECOND_HAND = 5;
    static const int PED_FRAME_NATIVE_HAND = 6;

    enum DualWieldPose {
        DUALPOSE_IDLE = 0,
        DUALPOSE_AIMING,
        DUALPOSE_FIRING,
        DUALPOSE_RELOADING,
        DUALPOSE_LOWERING
    };

    struct HandSnapshot {
        bool handValid;
        bool weaponValid;
        bool muzzleValid;
        RwMatrix handWorld;
        RwMatrix weaponWorld;
        CVector handPosition;
        CVector muzzle;

        void Reset() {
            handValid = false;
            weaponValid = false;
            muzzleValid = false;
            std::memset(&handWorld, 0, sizeof(handWorld));
            std::memset(&weaponWorld, 0, sizeof(weaponWorld));
            handPosition = CVector(0.0f, 0.0f, 0.0f);
            muzzle = CVector(0.0f, 0.0f, 0.0f);
        }
    };

    struct DualWieldState {
        CPed* owner;
        unsigned int frame;
        eWeaponType weaponType;
        DualWieldPose pose;
        bool active;
        bool skinned;
        bool reloading;
        bool fireRightThisFrame;
        bool fireLeftThisFrame;
        float aimBlend;
        HandSnapshot nativeHand;
        HandSnapshot secondHand;

        void Reset() {
            owner = 0;
            frame = 0;
            weaponType = WEAPONTYPE_UNARMED;
            pose = DUALPOSE_IDLE;
            active = false;
            skinned = false;
            reloading = false;
            fireRightThisFrame = false;
            fireLeftThisFrame = false;
            aimBlend = 0.0f;
            nativeHand.Reset();
            secondHand.Reset();
        }
    };

    static Config gConfig;
    static LeftWeaponRuntime gLeft;
    static CallPatch gFirePatch;
    static CallPatch gAimPatch;
    static CallPatch gMoveClearAimPatch;
    static CallPatch gRenderPatch;
    static DualWieldState gDualState;
    static char gIniPath[MAX_PATH] = {};
    static char gLogPath[MAX_PATH] = {};
    static bool gLoggedFirstPostAimPose = false;
    static bool gLoggedFirstLateRepairPose = false;
    static unsigned int gPostAimPoseHits = 0;
    static unsigned int gLateRepairPoseHits = 0;
    static unsigned int gRenderMirrorHits = 0;
    static unsigned int gLastPostAimPoseFrame = 0xFFFFFFFFu;
    static unsigned int gLastLateRepairPoseFrame = 0xFFFFFFFFu;
    static unsigned int gSecondShotAttempts = 0;
    static unsigned int gSecondShotSuccess = 0;
    static bool gLoggedFirstSecondShot = false;
    static bool gLoggedClumpReplacement = false;
    static bool gLoggedRuntimeSummary = false;
    static HMODULE gSkinBonesModule = 0;
    static SkinBonesGetPedWeaponAtomicFn gSkinBonesGetPedWeaponAtomic = 0;
    static bool gSkinBonesProbeDone = false;
    static bool gLoggedSkinBonesDetected = false;
    static bool gLoggedSkinnedPedMode = false;
    static bool gLoggedAimRechain = false;
    static bool gLoggedAimInstallFailure = false;
    static bool gLoggedRenderRechain = false;
    static bool gLoggedCreateFailure = false;
    static unsigned int gCreateFailureFrame = 0;
    static unsigned int gLastHandStateLogFrame = 0;
    static float gAimBlend = 0.0f;
    static unsigned int gAimBlendFrame = 0xFFFFFFFFu;

    struct StableNativeAimPose {
        bool valid;
        CPed* owner;
        RpClump* clump;
        eWeaponType weaponType;
        unsigned int frame;
        RwMatrix waist;
        RwMatrix upper;
        RwMatrix lower;
        RwMatrix hand;

        StableNativeAimPose()
            : valid(false), owner(0), clump(0), weaponType(WEAPONTYPE_UNARMED), frame(0) {
            std::memset(&waist, 0, sizeof(waist));
            std::memset(&upper, 0, sizeof(upper));
            std::memset(&lower, 0, sizeof(lower));
            std::memset(&hand, 0, sizeof(hand));
        }
    };

    static StableNativeAimPose gStableNativeAimPose;
    static unsigned int gLastNativeAimGunFrame = 0xFFFFFFFFu;
    static unsigned int gAimIntentEvalFrame = 0xFFFFFFFFu;
    static int gAimIntentHoldRemaining = 0;
    static bool gAimIntentLatched = false;
    static bool gPoseNativeHandBasisValid = false;
    static RwMatrix gPoseNativeHandBasis;
    // v16: while the render/fire transaction owns the twin arm, the weapon is
    // attached to this exact solved hand matrix. This prevents the gun basis from
    // being updated independently of the hand and removes the loose/floating look.
    static bool gSolvedSecondHandGripValid = false;
    static RwMatrix gSolvedSecondHandGrip;
    // v19 grip correction is derived from the actual skinned skeleton bind pose.
    // There is no animation-dependent 180-degree socket chooser anymore.
    static unsigned int gMoveAimClearSuppressions = 0;
    static unsigned int gCachedAimPoseUses = 0;

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
        gConfig.enabled = ReadBool("Enabled", true);
        gConfig.colt45 = ReadBool("Colt45", true);
        gConfig.uzi = ReadBool("Uzi", true);
        gConfig.doubleShot = ReadBool("DoubleShot", true);
        gConfig.leftArmIK = ReadBool("LeftArmIK", true);
        gConfig.chainExistingCallHooks = ReadBool("ChainExistingCallHooks", true);
        gConfig.aimBlendFrames = static_cast<int>(GetPrivateProfileIntA("DualWield", "AimBlendFrames", 1, gIniPath));
        if (gConfig.aimBlendFrames < 0) gConfig.aimBlendFrames = 0;
        if (gConfig.aimBlendFrames > 8) gConfig.aimBlendFrames = 8;
        gConfig.aimHoldFrames = static_cast<int>(GetPrivateProfileIntA("DualWield", "AimHoldFrames", 4, gIniPath));
        if (gConfig.aimHoldFrames < 1) gConfig.aimHoldFrames = 1;
        if (gConfig.aimHoldFrames > 12) gConfig.aimHoldFrames = 12;
        gConfig.aimPoseBlend = ReadFloat("AimPoseBlend", 1.0f);
        if (gConfig.aimPoseBlend < 0.0f) gConfig.aimPoseBlend = 0.0f;
        if (gConfig.aimPoseBlend > 1.0f) gConfig.aimPoseBlend = 1.0f;
        gConfig.firePoseBlend = ReadFloat("FirePoseBlend", 1.0f);
        if (gConfig.firePoseBlend < 0.0f) gConfig.firePoseBlend = 0.0f;
        if (gConfig.firePoseBlend > 1.0f) gConfig.firePoseBlend = 1.0f;
        gConfig.lateProcessRepair = ReadBool("LateProcessRepair", false);
        gConfig.stabilizeSecondHand = ReadBool("StabilizeSecondHand", true);
        gConfig.handAimBlend = ReadFloat("HandAimBlend", 1.0f);
        if (gConfig.handAimBlend < 0.0f) gConfig.handAimBlend = 0.0f;
        if (gConfig.handAimBlend > 1.0f) gConfig.handAimBlend = 1.0f;
        gConfig.handAimLimitDeg = ReadFloat("HandAimLimitDeg", 90.0f);
        if (gConfig.handAimLimitDeg < 0.0f) gConfig.handAimLimitDeg = 0.0f;
        if (gConfig.handAimLimitDeg > 180.0f) gConfig.handAimLimitDeg = 180.0f;
        gConfig.stabilizeSecondHandRoll = ReadBool("StabilizeSecondHandRoll", true);
        gConfig.handRollBlend = ReadFloat("HandRollBlend", 1.0f);
        if (gConfig.handRollBlend < 0.0f) gConfig.handRollBlend = 0.0f;
        if (gConfig.handRollBlend > 1.0f) gConfig.handRollBlend = 1.0f;
        gConfig.handRollLimitDeg = ReadFloat("HandRollLimitDeg", 90.0f);
        if (gConfig.handRollLimitDeg < 0.0f) gConfig.handRollLimitDeg = 0.0f;
        if (gConfig.handRollLimitDeg > 180.0f) gConfig.handRollLimitDeg = 180.0f;
        gConfig.copyNativeGripBasis = ReadBool("CopyNativeGripBasis", true);
        gConfig.armReachScale = ReadFloat("ArmReachScale", 0.92f);
        if (gConfig.armReachScale < 0.70f) gConfig.armReachScale = 0.70f;
        if (gConfig.armReachScale > 1.00f) gConfig.armReachScale = 1.00f;
        gConfig.elbowOutwardDeg = ReadFloat("ElbowOutwardDeg", 0.0f);
        if (gConfig.elbowOutwardDeg < 0.0f) gConfig.elbowOutwardDeg = 0.0f;
        if (gConfig.elbowOutwardDeg > 15.0f) gConfig.elbowOutwardDeg = 15.0f;
        gConfig.logHandState = ReadBool("LogHandState", false);
        gConfig.handStateLogInterval = static_cast<int>(GetPrivateProfileIntA("DualWield", "HandStateLogInterval", 60, gIniPath));
        if (gConfig.handStateLogInterval < 1) gConfig.handStateLogInterval = 1;
        if (gConfig.handStateLogInterval > 600) gConfig.handStateLogInterval = 600;
        gConfig.offsetX = ReadFloat("OffsetX", 0.0f);
        gConfig.offsetY = ReadFloat("OffsetY", 0.0f);
        gConfig.offsetZ = ReadFloat("OffsetZ", 0.0f);
        gConfig.rotX = ReadFloat("RotationX", 0.0f);
        gConfig.rotY = ReadFloat("RotationY", 0.0f);
        gConfig.rotZ = ReadFloat("RotationZ", 0.0f);
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

        // Explicit physical/animation ownership flags. Keep our post-AimGun arm correction
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
        // DualWieldIII owns both of these objects independently. The atomic is deliberately NOT
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
        gAimBlend = 0.0f;
        gAimBlendFrame = 0xFFFFFFFFu;
        gDualState.Reset();
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


    static void* FindAnimBlendFrameDataRaw(CPed* ped, const char* name) {
        if (!ped || !ped->m_pRwClump || !name || !name[0])
            return 0;
        if (!IsReadableAddress(reinterpret_cast<const void*>(ADDR_ANIMBLEND_FIND_FRAME), 5))
            return 0;

        // GTA III 1.0 EN: RpAnimBlendClumpFindFrame = 0x405430. Skin & Bones replaces
        // this same entry point with a skinned-aware implementation, so calling the
        // settled game address works for both the stock RwFrame and Xbox/HAnim paths.
        AnimBlendFindFrameFn findFrame = reinterpret_cast<AnimBlendFindFrameFn>(ADDR_ANIMBLEND_FIND_FRAME);
        void* frameData = findFrame(ped->m_pRwClump, name);
        return IsReadableAddress(frameData, 1) ? frameData : 0;
    }

    static unsigned char* FrameRotationFlags(void* frameData) {
        return IsReadableAddress(frameData, 1)
            ? reinterpret_cast<unsigned char*>(frameData) : 0;
    }

    static bool RawAimIntent(CPed* ped) {
        if (!gConfig.leftArmIK || !IsEligiblePlayer(ped))
            return false;
        CWeapon* weapon = ped->GetWeapon();
        if (!weapon || weapon->m_eWeaponState == WEAPONSTATE_RELOADING ||
            weapon->m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO)
            return false;

        bool padAim = false;
        CPad* pad = CPad::GetPad(0);
        if (pad)
            padAim = pad->GetTarget() || pad->GetWeapon();

        return padAim || ped->bIsAimingGun || ped->bIsPointingGunAt || ped->bIsShooting ||
            weapon->m_eWeaponState == WEAPONSTATE_FIRING ||
            ped->m_ePedState == PEDSTATE_AIMGUN || ped->m_ePedState == PEDSTATE_ATTACK;
    }

    static bool ContinuousAimIntent(CPed* ped) {
        if (!ped || !IsEligiblePlayer(ped)) {
            gAimIntentHoldRemaining = 0;
            gAimIntentLatched = false;
            return false;
        }

        CWeapon* weapon = ped->GetWeapon();
        if (!weapon || weapon->m_eWeaponState == WEAPONSTATE_RELOADING ||
            weapon->m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO) {
            gAimIntentHoldRemaining = 0;
            gAimIntentLatched = false;
            return false;
        }

        const unsigned int frame = CTimer::m_FrameCounter;
        if (frame != gAimIntentEvalFrame) {
            gAimIntentEvalFrame = frame;
            if (RawAimIntent(ped)) {
                gAimIntentHoldRemaining = gConfig.aimHoldFrames;
                gAimIntentLatched = true;
            }
            else if (gAimIntentHoldRemaining > 0) {
                --gAimIntentHoldRemaining;
                gAimIntentLatched = true;
            }
            else {
                gAimIntentLatched = false;
            }
        }
        return gAimIntentLatched;
    }

    static bool WantsSecondArmPose(CPed* ped) {
        return ContinuousAimIntent(ped);
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
        RpSkin* skin;
        RpHAnimHierarchy* hierarchy;
        SkinProbe() : atomic(0), skin(0), hierarchy(0) {}
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
        probe->skin = skin;
        probe->hierarchy = hierarchy;
        return 0; // stop once the actual skinned body atomic is found
    }

    static bool GetSkinnedPedSkinHierarchy(CPed* ped, RpSkin*& skin, RpHAnimHierarchy*& hierarchy) {
        skin = 0;
        hierarchy = 0;
        if (!ped || !ped->m_pRwClump)
            return false;

        SkinProbe probe;
        RpClumpForAllAtomics(ped->m_pRwClump, FindSkinnedAtomicCB, &probe);
        if (!probe.atomic || !probe.skin || !probe.hierarchy)
            return false;

        RpHAnimHierarchy* h = probe.hierarchy;
        if (!IsReadableAddress(h, sizeof(RpHAnimHierarchy)))
            return false;
        if (h->numNodes <= 0 || h->numNodes > 128 || !h->pMatrixArray || !h->pNodeInfo)
            return false;
        if (!IsReadableAddress(h->pMatrixArray, sizeof(RwMatrix) * static_cast<size_t>(h->numNodes)) ||
            !IsReadableAddress(h->pNodeInfo, sizeof(RpHAnimNodeInfo) * static_cast<size_t>(h->numNodes)))
            return false;

        skin = probe.skin;
        hierarchy = h;
        return true;
    }

    static bool GetSkinnedPedHierarchy(CPed* ped, RpHAnimHierarchy*& hierarchy) {
        RpSkin* skin = 0;
        return GetSkinnedPedSkinHierarchy(ped, skin, hierarchy);
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

    static float DotRwAxis(const RwV3d& a, const RwV3d& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static void CopyAxisSigned(RwV3d& dst, const RwV3d& src, float sign) {
        dst.x = src.x * sign;
        dst.y = src.y * sign;
        dst.z = src.z * sign;
    }

    static bool BuildOppositeGripWorld(
        CPed* ped,
        RpHAnimHierarchy* hierarchy,
        const RwMatrix& nativeHand,
        const RwMatrix& secondHand,
        RwMatrix& out);

    static bool PutWeaponFrameOnSkinnedHand(CPed* ped, RwFrame* helperFrame) {
        if (!ped || !helperFrame)
            return false;
        RpHAnimHierarchy* hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false;
        RwMatrix* secondHand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!secondHand)
            return false;

        // During the v19 transactional firing/render pose, gSolvedSecondHandGrip is a
        // frozen bind-corrected socket matrix built from the coherently rebuilt SLhand.
        // Use it as the single authority for the visible gun until the transaction ends.
        if (gSolvedSecondHandGripValid && IsFiniteRwMatrix(gSolvedSecondHandGrip))
            return PutWeaponFrameOnWorldMatrix(helperFrame, gSolvedSecondHandGrip);

        if (!gConfig.copyNativeGripBasis)
            return PutWeaponFrameOnWorldMatrix(helperFrame, *secondHand);
        RwMatrix* nativeHand = GetSkinBoneMatrix(hierarchy, BONE_SRHAND);
        const RwMatrix* nativeGripBasis = gPoseNativeHandBasisValid ? &gPoseNativeHandBasis : nativeHand;
        if (!nativeGripBasis)
            return false;
        RwMatrix gripWorld;
        if (!BuildOppositeGripWorld(ped, hierarchy, *nativeGripBasis, *secondHand, gripWorld))
            return false;
        return PutWeaponFrameOnWorldMatrix(helperFrame, gripWorld);
    }

    static bool UpdateLeftWeaponWorldTransform() {
        if (!gLeft.owner || !gLeft.atomic || !gLeft.helperFrame)
            return false;
        if (gLeft.skinnedSource)
            return PutWeaponFrameOnSkinnedHand(gLeft.owner, gLeft.helperFrame);
        if (!gLeft.sourceHandFrame)
            return false;
        if (!gConfig.copyNativeGripBasis)
            return PutWeaponFrameOnHand(gLeft.helperFrame, gLeft.sourceHandFrame);

        RwFrame* nativeFrame = GetPedFrameSafe(gLeft.owner, PED_FRAME_NATIVE_HAND);
        if (!nativeFrame)
            return false;
        RwMatrix nativeHand, secondHand, gripWorld;
        if (!GetFrameWorldMatrixManual(nativeFrame, nativeHand) ||
            !GetFrameWorldMatrixManual(gLeft.sourceHandFrame, secondHand) ||
            !BuildOppositeGripWorld(gLeft.owner, 0, nativeHand, secondHand, gripWorld))
            return false;
        return PutWeaponFrameOnWorldMatrix(gLeft.helperFrame, gripWorld);
    }


    static bool ComputeWeaponWorldFromHand(const RwMatrix& handWorld, RwMatrix& weaponWorld) {
        BuildGripCorrection(weaponWorld);
        RwMatrixTransform(&weaponWorld, &handWorld, rwCOMBINEPOSTCONCAT);
        return IsFiniteRwMatrix(weaponWorld);
    }

    static bool ComputeMuzzleFromWeaponWorld(CWeaponInfo* info, const RwMatrix& weaponWorld, CVector& out) {
        if (!info || !IsFiniteRwMatrix(weaponWorld))
            return false;
        RwV3d src = { info->m_vecFireOffset.x, info->m_vecFireOffset.y, info->m_vecFireOffset.z };
        RwV3d dst = {};
        RwV3dTransformPoints(&dst, &src, 1, const_cast<RwMatrix*>(&weaponWorld));
        out.x = dst.x;
        out.y = dst.y;
        out.z = dst.z;
        return IsFiniteVector(out);
    }

    static void FillHandSnapshotFromMatrix(HandSnapshot& hand, const RwMatrix& handWorld, CWeaponInfo* info, bool makeWeapon) {
        hand.Reset();
        if (!IsFiniteRwMatrix(handWorld))
            return;
        hand.handWorld = handWorld;
        hand.handPosition.x = handWorld.pos.x;
        hand.handPosition.y = handWorld.pos.y;
        hand.handPosition.z = handWorld.pos.z;
        hand.handValid = IsFiniteVector(hand.handPosition);
        if (makeWeapon && info && ComputeWeaponWorldFromHand(handWorld, hand.weaponWorld)) {
            hand.weaponValid = true;
            hand.muzzleValid = ComputeMuzzleFromWeaponWorld(info, hand.weaponWorld, hand.muzzle);
        }
    }

    static bool CaptureStockHandMatrix(CPed* ped, int frameIndex, RwMatrix& out) {
        RwFrame* frame = GetPedFrameSafe(ped, frameIndex);
        return frame && GetFrameWorldMatrixManual(frame, out);
    }

    static bool CaptureSkinnedHandMatrix(CPed* ped, int boneId, RwMatrix& out) {
        RpHAnimHierarchy* hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false;
        RwMatrix* m = GetSkinBoneMatrix(hierarchy, boneId);
        if (!m)
            return false;
        out = *m;
        return IsFiniteRwMatrix(out);
    }

    static bool IsMuzzleSaneForPed(CPed* ped, const CVector& muzzle) {
        if (!ped || !IsFiniteVector(muzzle))
            return false;
        const CVector& pedPos = ped->GetPosition();
        const float dx = muzzle.x - pedPos.x;
        const float dy = muzzle.y - pedPos.y;
        const float dz = muzzle.z - pedPos.z;
        const float distSq = dx * dx + dy * dy + dz * dz;
        return IsFiniteFloat(distSq) && distSq <= 9.0f;
    }

    static bool UpdateDualWieldState(CPed* ped, CWeapon* weapon, const CVector* nativeMuzzle, bool refreshSecondWeapon) {
        gDualState.Reset();
        if (!ped || !weapon || !IsEligiblePlayer(ped, weapon))
            return false;

        CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType);
        if (!info)
            return false;

        gDualState.owner = ped;
        gDualState.frame = CTimer::m_FrameCounter;
        gDualState.weaponType = weapon->m_eWeaponType;
        gDualState.active = true;
        gDualState.reloading = weapon->m_eWeaponState == WEAPONSTATE_RELOADING;
        gDualState.skinned = IsSkinnedPed(ped);
        gDualState.aimBlend = gAimBlend;
        gDualState.pose = gDualState.reloading ? DUALPOSE_RELOADING :
            (ped->bIsShooting || ped->m_ePedState == PEDSTATE_ATTACK ? DUALPOSE_FIRING :
                (gAimBlend > 0.05f ? DUALPOSE_AIMING : DUALPOSE_IDLE));

        RwMatrix nativeHand;
        if (gDualState.skinned) {
            if (CaptureSkinnedHandMatrix(ped, BONE_SRHAND, nativeHand))
                FillHandSnapshotFromMatrix(gDualState.nativeHand, nativeHand, info, false);
        }
        else {
            if (CaptureStockHandMatrix(ped, PED_FRAME_NATIVE_HAND, nativeHand))
                FillHandSnapshotFromMatrix(gDualState.nativeHand, nativeHand, info, false);
        }
        if (nativeMuzzle && IsFiniteVector(*nativeMuzzle)) {
            gDualState.nativeHand.muzzle = *nativeMuzzle;
            gDualState.nativeHand.muzzleValid = true;
        }

        if (refreshSecondWeapon && !UpdateLeftWeaponWorldTransform())
            return false;
        if (gLeft.owner == ped && gLeft.helperFrame) {
            RwMatrix* secondWeapon = RwFrameGetMatrix(gLeft.helperFrame);
            if (secondWeapon && IsFiniteRwMatrix(*secondWeapon)) {
                gDualState.secondHand.weaponWorld = *secondWeapon;
                gDualState.secondHand.weaponValid = true;
                gDualState.secondHand.muzzleValid = ComputeMuzzleFromWeaponWorld(info, *secondWeapon, gDualState.secondHand.muzzle) &&
                    IsMuzzleSaneForPed(ped, gDualState.secondHand.muzzle);
            }

            RwMatrix secondHand;
            bool gotSecondHand = false;
            if (gLeft.skinnedSource)
                gotSecondHand = CaptureSkinnedHandMatrix(ped, BONE_SLHAND, secondHand);
            else
                gotSecondHand = CaptureStockHandMatrix(ped, PED_FRAME_SECOND_HAND, secondHand);
            if (gotSecondHand) {
                gDualState.secondHand.handWorld = secondHand;
                gDualState.secondHand.handPosition.x = secondHand.pos.x;
                gDualState.secondHand.handPosition.y = secondHand.pos.y;
                gDualState.secondHand.handPosition.z = secondHand.pos.z;
                gDualState.secondHand.handValid = IsFiniteVector(gDualState.secondHand.handPosition);
            }
        }
        return gDualState.secondHand.weaponValid || gDualState.secondHand.handValid || gDualState.nativeHand.handValid;
    }

    static void MaybeLogHandState() {
        if (!gConfig.logHandState || !gDualState.active)
            return;
        const unsigned int frame = CTimer::m_FrameCounter;
        if (frame - gLastHandStateLogFrame < static_cast<unsigned int>(gConfig.handStateLogInterval))
            return;
        gLastHandStateLogFrame = frame;

        char line[512];
        std::snprintf(line, sizeof(line),
            "DualWieldIII hand state: pose=%d blend=%.2f nativeHand=(%.3f %.3f %.3f) nativeMuzzle=(%.3f %.3f %.3f valid=%d) secondHand=(%.3f %.3f %.3f) secondMuzzle=(%.3f %.3f %.3f valid=%d).",
            static_cast<int>(gDualState.pose), gDualState.aimBlend,
            gDualState.nativeHand.handPosition.x, gDualState.nativeHand.handPosition.y, gDualState.nativeHand.handPosition.z,
            gDualState.nativeHand.muzzle.x, gDualState.nativeHand.muzzle.y, gDualState.nativeHand.muzzle.z,
            gDualState.nativeHand.muzzleValid ? 1 : 0,
            gDualState.secondHand.handPosition.x, gDualState.secondHand.handPosition.y, gDualState.secondHand.handPosition.z,
            gDualState.secondHand.muzzle.x, gDualState.secondHand.muzzle.y, gDualState.secondHand.muzzle.z,
            gDualState.secondHand.muzzleValid ? 1 : 0);
        Log(line);
    }

    static RwObject* CreateModelInstance(CBaseModelInfo* modelInfo) {
        if (!modelInfo)
            return 0;

        // CPed::AddWeaponModel(0x4CF8F0) calls [modelInfo->vtable + 0x0C].
        // Use that exact virtual slot instead of depending on a Plugin-SDK wrapper.
        typedef RwObject* (__thiscall* CreateInstanceFn)(CBaseModelInfo*);
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
            sourceHand = GetPedFrameSafe(ped, PED_FRAME_SECOND_HAND);
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
            RwMatrix* nativeHand = GetSkinBoneMatrix(skinHierarchy, BONE_SRHAND);
            RwMatrix* leftHand = GetSkinBoneMatrix(skinHierarchy, BONE_SLHAND);
            RwMatrix socketWorld;
            posed = nativeHand && leftHand &&
                BuildOppositeGripWorld(ped, skinHierarchy, *nativeHand, *leftHand, socketWorld) &&
                PutWeaponFrameOnWorldMatrix(helper, socketWorld);
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
                "DualWieldIII: skinned ped path active; HAnim=%p nodes=%d leftHand=%p. m_apFrames[*].m_pFrame is NOT treated as RwFrame.",
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
            "DualWieldIII: create deferred. ped=%p state=%d inVeh=%d weapon=%d modelField=%d infoModel=%d skinned=%d hierarchy=%p S&Bweapon=%p.",
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

        const bool activeAimNow = ped->bIsAimingGun || ped->bIsPointingGunAt ||
            ped->bIsShooting || ped->m_ePedState == PEDSTATE_AIMGUN ||
            ped->m_ePedState == PEDSTATE_ATTACK;
        if (!activeAimNow) {
            gAimBlend = 0.0f;
            gAimBlendFrame = 0xFFFFFFFFu;
        }

        if (gLeft.owner == ped && gLeft.ownerClump && gLeft.ownerClump != ped->m_pRwClump) {
            if (!gLoggedClumpReplacement) {
                Log("DualWieldIII: player clump replaced; destroying standalone second weapon and rebinding cleanly.");
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
                RwFrame* currentSourceHand = GetPedFrameSafe(ped, PED_FRAME_SECOND_HAND);
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
            mirrorUpper(0), mirrorLower(0), mirrorHand(0) {
        }
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

    static Basis3 InverseOrthonormalBasis(const Basis3& basis) {
        // Basis vectors are matrix columns. For an orthonormal rotation the inverse is
        // the transpose, so the inverse columns are the original rows.
        Basis3 out;
        out.right = Vec3(basis.right.x, basis.up.x, basis.at.x);
        out.up = Vec3(basis.right.y, basis.up.y, basis.at.y);
        out.at = Vec3(basis.right.z, basis.up.z, basis.at.z);
        return OrthonormalizeBasis(out);
    }

    static bool BuildOppositeGripWorld(
        CPed* ped,
        RpHAnimHierarchy* hierarchy,
        const RwMatrix& nativeHand,
        const RwMatrix& secondHand,
        RwMatrix& out
    ) {
        if (!IsFiniteRwMatrix(nativeHand) || !IsFiniteRwMatrix(secondHand))
            return false;

        // Skin & Bones places the stock gun directly on SRhand. The same unmirrored
        // weapon model cannot simply inherit SLhand's mirrored bind orientation though.
        // Derive the ONE constant left-hand -> native-hand rotation from the skin's own
        // inverse bind matrices:
        //
        //      C = inverse(BindSecond) * BindNative
        //      weaponWorld = SecondHandWorld * C
        //
        // RpSkinGetSkinToBoneMatrices returns inverse bind matrices, so
        // inverse(BindSecond) is available directly and BindNative is the transpose of
        // the native inverse-bind rotation. This correction is skeleton data, not an
        // animation-dependent best-fit choice; recoil/walking can never make it flip.
        RpSkin* skin = 0;
        RpHAnimHierarchy* skinHierarchy = 0;
        if (ped && GetSkinnedPedSkinHierarchy(ped, skin, skinHierarchy) &&
            skin && skinHierarchy && (!hierarchy || hierarchy == skinHierarchy)) {
            const int nativeHAnimIndex = RpHAnimIDGetIndex(skinHierarchy, BONE_SRHAND);
            const int secondHAnimIndex = RpHAnimIDGetIndex(skinHierarchy, BONE_SLHAND);
            int nativeIndex = nativeHAnimIndex;
            int secondIndex = secondHAnimIndex;
            if (nativeHAnimIndex >= 0 && nativeHAnimIndex < skinHierarchy->numNodes &&
                secondHAnimIndex >= 0 && secondHAnimIndex < skinHierarchy->numNodes &&
                skinHierarchy->pNodeInfo) {
                const int n = skinHierarchy->pNodeInfo[nativeHAnimIndex].nodeIndex;
                const int s = skinHierarchy->pNodeInfo[secondHAnimIndex].nodeIndex;
                if (n >= 0 && n < skinHierarchy->numNodes) nativeIndex = n;
                if (s >= 0 && s < skinHierarchy->numNodes) secondIndex = s;
            }
            const int maxIndex = nativeIndex > secondIndex ? nativeIndex : secondIndex;
            SkinGetSkinToBoneMatricesFn getSkinToBone =
                reinterpret_cast<SkinGetSkinToBoneMatricesFn>(ADDR_SKIN_GET_SKIN_TO_BONE_MATRICES);
            RwMatrix* inverseBind = getSkinToBone ? getSkinToBone(skin) : 0;

            if (nativeIndex >= 0 && secondIndex >= 0 && maxIndex < skinHierarchy->numNodes &&
                inverseBind && IsReadableAddress(inverseBind,
                    sizeof(RwMatrix) * static_cast<size_t>(maxIndex + 1)) &&
                IsFiniteRwMatrix(inverseBind[nativeIndex]) &&
                IsFiniteRwMatrix(inverseBind[secondIndex])) {
                const Basis3 nativeInvBind = BasisFromRwMatrix(&inverseBind[nativeIndex]);
                const Basis3 secondInvBind = BasisFromRwMatrix(&inverseBind[secondIndex]);
                const Basis3 nativeBind = InverseOrthonormalBasis(nativeInvBind);
                const Basis3 correction = ComposeBasis(secondInvBind, nativeBind);
                const Basis3 secondWorld = BasisFromRwMatrix(&secondHand);
                const Basis3 weaponWorld = ComposeBasis(secondWorld, correction);

                out = secondHand;
                out.right.x = weaponWorld.right.x; out.right.y = weaponWorld.right.y; out.right.z = weaponWorld.right.z;
                out.up.x = weaponWorld.up.x;    out.up.y = weaponWorld.up.y;    out.up.z = weaponWorld.up.z;
                out.at.x = weaponWorld.at.x;    out.at.y = weaponWorld.at.y;    out.at.z = weaponWorld.at.z;
                // The weapon origin remains the actual SLhand bone origin. Optional
                // Offset*/Rotation* INI trim is applied afterwards in weapon-local space.
                return IsFiniteRwMatrix(out);
            }
        }

        // Stock/non-skinned fallback: GTA's native weapon basis is already known-good.
        // Borrow only that orientation while anchoring translation to the real opposite
        // hand. This is stable and avoids the v18 identity/180X/180Y/180Z chooser.
        out = nativeHand;
        out.pos = secondHand.pos;
        return IsFiniteRwMatrix(out);
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
        chain.nativeUpper = GetPedFrameSafe(ped, PED_FRAME_NATIVE_UPPER);
        chain.nativeHand = GetPedFrameSafe(ped, PED_FRAME_NATIVE_HAND);
        chain.mirrorUpper = GetPedFrameSafe(ped, PED_FRAME_SECOND_UPPER);
        chain.mirrorHand = GetPedFrameSafe(ped, PED_FRAME_SECOND_HAND);
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

    // GTA III's hgun animation supplies the native weapon-arm base pose, then AimGun
    // applies the final gun-IK correction. We sample that finished intent at the AimGun
    // stage and rotate the opposite arm locally. Skin & Bones uses HAnim interpolation
    // quaternions; final matrix-array entries are treated strictly as solver output.

    static CVector BlendDirection(const CVector& from, const CVector& to, float t) {
        const CVector a = Normalize3(from, Vec3(0.0f, 1.0f, 0.0f));
        const CVector b = Normalize3(to, a);
        const float clamped = ClampFloat(t, 0.0f, 1.0f);
        return Normalize3(Add3(Scale3(a, 1.0f - clamped), Scale3(b, clamped)), b);
    }

    static float UpdateAimBlendOncePerFrame(float targetBlend) {
        targetBlend = ClampFloat(targetBlend, 0.0f, 1.0f);
        const unsigned int frame = CTimer::m_FrameCounter;
        if (frame != gAimBlendFrame) {
            gAimBlendFrame = frame;
            if (gConfig.aimBlendFrames <= 1)
                gAimBlend = targetBlend;
            else if (targetBlend > gAimBlend)
                gAimBlend = ClampFloat(gAimBlend + 1.0f / static_cast<float>(gConfig.aimBlendFrames), 0.0f, targetBlend);
            else
                gAimBlend = targetBlend;
        }
        return gAimBlend;
    }

    static bool ShouldMirrorAimPose(CPed* ped, float& blend) {
        blend = 0.0f;
        if (!ped || !ContinuousAimIntent(ped))
            return false;

        CWeapon* weapon = ped->GetWeapon();
        if (!weapon)
            return false;

        const bool firing = ped->bIsShooting || weapon->m_eWeaponState == WEAPONSTATE_FIRING ||
            ped->m_ePedState == PEDSTATE_ATTACK;
        const float targetBlend = firing ? gConfig.firePoseBlend : gConfig.aimPoseBlend;
        if (firing) {
            gAimBlend = ClampFloat(targetBlend, 0.0f, 1.0f);
            gAimBlendFrame = CTimer::m_FrameCounter;
            blend = gAimBlend;
        }
        else {
            blend = UpdateAimBlendOncePerFrame(targetBlend);
        }
        return blend > 0.001f;
    }


    static bool GetActiveWeaponLocalFireDirection(CPed* ped, CVector& localDir) {
        if (!ped)
            return false;
        CWeapon* weapon = ped->GetWeapon();
        if (!weapon)
            return false;
        CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType);
        if (!info)
            return false;
        localDir = Normalize3(
            Vec3(info->m_vecFireOffset.x, info->m_vecFireOffset.y, info->m_vecFireOffset.z),
            Vec3(0.0f, 1.0f, 0.0f));
        return IsFiniteVector(localDir);
    }

    static bool ComputeNativeAndSecondWeaponDirections(
        CPed* ped,
        const RwMatrix& nativeHandWorld,
        const RwMatrix& secondHandWorld,
        CVector& nativeWeaponDir,
        CVector& secondWeaponDir
    ) {
        CVector localFireDir;
        if (!GetActiveWeaponLocalFireDirection(ped, localFireDir))
            return false;

        // GTA III's native weapon is attached directly to SRhand. The duplicate uses
        // our grip correction before SLhand, so measure the duplicate through the same
        // corrected weapon matrix that is actually rendered/fired.
        nativeWeaponDir = Normalize3(
            BasisTransformDirection(BasisFromRwMatrix(&nativeHandWorld), localFireDir),
            Vec3(0.0f, 1.0f, 0.0f));

        RwMatrix secondWeaponWorld;
        if (!ComputeWeaponWorldFromHand(secondHandWorld, secondWeaponWorld))
            return false;
        secondWeaponDir = Normalize3(
            BasisTransformDirection(BasisFromRwMatrix(&secondWeaponWorld), localFireDir),
            nativeWeaponDir);

        return IsFiniteVector(nativeWeaponDir) && IsFiniteVector(secondWeaponDir);
    }

    static CVector ProjectDirectionOntoPlane(const CVector& v, const CVector& normal, const CVector& fallback) {
        const CVector n = Normalize3(normal, Vec3(0.0f, 1.0f, 0.0f));
        return Normalize3(Sub3(v, Scale3(n, Dot3(v, n))), fallback);
    }

    static bool ComputeWeaponRollCorrection(
        CPed* ped,
        const RwMatrix& nativeHandWorld,
        const RwMatrix& secondHandWorld,
        float blend,
        CVector& worldAxis,
        float& radians
    ) {
        radians = 0.0f;
        if (!gConfig.stabilizeSecondHandRoll || !ped || blend <= 0.001f)
            return true;

        CVector nativeWeaponDir, secondWeaponDir;
        if (!ComputeNativeAndSecondWeaponDirections(
            ped, nativeHandWorld, secondHandWorld, nativeWeaponDir, secondWeaponDir))
            return false;

        RwMatrix secondWeaponWorld;
        if (!ComputeWeaponWorldFromHand(secondHandWorld, secondWeaponWorld))
            return false;

        // Native GTA III attaches the weapon atomic directly to SRhand, so the native
        // hand basis is the native weapon basis. After our 180-degree grip correction,
        // the duplicate weapon should carry the same roll around the shared fire axis.
        const Basis3 nativeWeaponBasis = BasisFromRwMatrix(&nativeHandWorld);
        const Basis3 secondWeaponBasis = BasisFromRwMatrix(&secondWeaponWorld);
        const CVector axis = Normalize3(nativeWeaponDir, Vec3(0.0f, 1.0f, 0.0f));

        CVector nativeReference = ProjectDirectionOntoPlane(nativeWeaponBasis.up, axis,
            ProjectDirectionOntoPlane(nativeWeaponBasis.right, axis, Vec3(1.0f, 0.0f, 0.0f)));
        CVector secondReference = ProjectDirectionOntoPlane(secondWeaponBasis.up, axis,
            ProjectDirectionOntoPlane(secondWeaponBasis.right, axis, nativeReference));

        const float c = ClampFloat(Dot3(secondReference, nativeReference), -1.0f, 1.0f);
        const float sgn = Dot3(Cross3(secondReference, nativeReference), axis);
        float angle = std::atan2(sgn, c) * ClampFloat(blend * gConfig.handRollBlend, 0.0f, 1.0f);
        const float limit = gConfig.handRollLimitDeg * 0.01745329251994329577f;
        if (limit > 0.0f) {
            if (angle > limit) angle = limit;
            if (angle < -limit) angle = -limit;
        }

        worldAxis = axis;
        radians = angle;
        return IsFiniteFloat(radians) && IsFiniteVector(worldAxis);
    }

    static bool StabilizeStockSecondHandWeaponRoll(CPed* ped, const MirroredArmChain& chain, float blend) {
        if (!gConfig.stabilizeSecondHandRoll || !ped || !chain.valid || blend <= 0.001f)
            return true;

        RwMatrix nativeHandWorld;
        RwMatrix secondHandWorld;
        if (!GetFrameWorldMatrixManual(chain.nativeHand, nativeHandWorld) ||
            !GetFrameWorldMatrixManual(chain.mirrorHand, secondHandWorld))
            return false;

        CVector axisWorld;
        float radians = 0.0f;
        if (!ComputeWeaponRollCorrection(ped, nativeHandWorld, secondHandWorld, blend, axisWorld, radians))
            return false;
        if (std::fabs(radians) < 1.0e-5f)
            return true;

        Basis3 handWorld = GetFrameWorldBasis(chain.mirrorHand);
        const float c = std::cos(radians);
        const float sn = std::sin(radians);
        handWorld.right = RotateAroundAxis(handWorld.right, axisWorld, c, sn);
        handWorld.up = RotateAroundAxis(handWorld.up, axisWorld, c, sn);
        handWorld.at = RotateAroundAxis(handWorld.at, axisWorld, c, sn);
        SetFrameWorldBasis(chain.mirrorHand, handWorld);
        RwFrameGetLTM(chain.mirrorHand);
        return true;
    }

    static bool StabilizeStockSecondHandWeaponForward(
        CPed* ped,
        const MirroredArmChain& chain,
        float blend
    ) {
        if (!gConfig.stabilizeSecondHand || !ped || !chain.valid || blend <= 0.001f)
            return true;

        RwMatrix nativeHandWorld;
        RwMatrix secondHandWorld;
        if (!GetFrameWorldMatrixManual(chain.nativeHand, nativeHandWorld) ||
            !GetFrameWorldMatrixManual(chain.mirrorHand, secondHandWorld))
            return false;

        CVector nativeWeaponDir, secondWeaponDir;
        if (!ComputeNativeAndSecondWeaponDirections(
            ped, nativeHandWorld, secondHandWorld, nativeWeaponDir, secondWeaponDir))
            return false;

        const float dot = ClampFloat(Dot3(secondWeaponDir, nativeWeaponDir), -1.0f, 1.0f);
        const float fullRadians = std::acos(dot);
        float targetRadians = fullRadians * ClampFloat(blend * gConfig.handAimBlend, 0.0f, 1.0f);
        const float limitRadians = gConfig.handAimLimitDeg * 0.01745329251994329577f;
        if (limitRadians > 0.0f && targetRadians > limitRadians)
            targetRadians = limitRadians;
        const float directionBlend = fullRadians > 1.0e-5f
            ? ClampFloat(targetRadians / fullRadians, 0.0f, 1.0f)
            : 0.0f;
        const CVector desiredDir = BlendDirection(secondWeaponDir, nativeWeaponDir, directionBlend);

        Basis3 handWorld = GetFrameWorldBasis(chain.mirrorHand);
        handWorld = RotateBasisFromTo(handWorld, secondWeaponDir, desiredDir);
        SetFrameWorldBasis(chain.mirrorHand, handWorld);
        RwFrameGetLTM(chain.mirrorHand);
        return true;
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

        // Use the actual shoulder-to-shoulder axis as the sagittal-plane normal.
        // Unlike ped->GetRight(), this follows torso twist already contributed by the
        // current locomotion/weapon animation.
        const CVector mirrorNormal = Normalize3(
            Sub3(nativeShoulder, mirrorShoulder),
            Normalize3(ped->GetRight(), Vec3(1.0f, 0.0f, 0.0f)));

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

        // Forward locomotion contributes a strong wrist/hand swing to the opposite arm.
        // Segment-direction correction alone leaves that twist intact, so the clone can
        // visibly wag even though shoulder/elbow/hand positions are correct. Stabilize
        // the actual corrected weapon-forward cue at the HAND, not a guessed world matrix.
        StabilizeStockSecondHandWeaponForward(ped, chain, blend);
        StabilizeStockSecondHandWeaponRoll(ped, chain, blend);
    }

    struct RawHAnimInterpFrame {
        uint32_t keyFrame1;
        uint32_t keyFrame2;
        RtQuat q;
        RwV3d t;
    };
    static_assert(sizeof(RawHAnimInterpFrame) == 0x24, "Unexpected standard HAnim interpolation-frame size");

    static RawHAnimInterpFrame* GetHAnimInterpFrame(RpHAnimHierarchy* hierarchy, int boneId) {
        if (!hierarchy || hierarchy->numNodes <= 0 || hierarchy->numNodes > 128)
            return 0;

        const int index = RpHAnimIDGetIndex(hierarchy, boneId);
        if (index < 0 || index >= hierarchy->numNodes)
            return 0;

        // RenderWare 3.4/3.5 HAnim stores the current interpolated keyframes directly
        // after RpHAnimHierarchy. The SDK's rpHANIMHIERARCHYGETINTERPFRAME macro is
        // exactly this calculation. Standard frames are 0x24 bytes; reject anything
        // smaller or absurd rather than guessing into another interpolation scheme.
        const int keyFrameSize = hierarchy->currentKeyFrameSize;
        if (keyFrameSize != static_cast<int>(sizeof(RawHAnimInterpFrame)))
            return 0;

        unsigned char* base = reinterpret_cast<unsigned char*>(hierarchy + 1);
        const size_t totalBytes = static_cast<size_t>(hierarchy->numNodes) * static_cast<size_t>(keyFrameSize);
        if (totalBytes / static_cast<size_t>(keyFrameSize) != static_cast<size_t>(hierarchy->numNodes) ||
            !IsReadableAddress(base, totalBytes))
            return 0;

        RawHAnimInterpFrame* frame = reinterpret_cast<RawHAnimInterpFrame*>(
            base + static_cast<size_t>(index) * static_cast<size_t>(keyFrameSize));
        return IsReadableAddress(frame, sizeof(RawHAnimInterpFrame)) ? frame : 0;
    }

    static CVector WorldAxisToParentLocal(const CVector& axisWorld, const RwMatrix& parentWorld) {
        return Normalize3(Vec3(
            Dot3(axisWorld, Vec3(parentWorld.right.x, parentWorld.right.y, parentWorld.right.z)),
            Dot3(axisWorld, Vec3(parentWorld.up.x, parentWorld.up.y, parentWorld.up.z)),
            Dot3(axisWorld, Vec3(parentWorld.at.x, parentWorld.at.y, parentWorld.at.z))),
            Vec3(0.0f, 0.0f, 1.0f));
    }

    static bool RotateSkinnedBoneTowardDirection(
        RpHAnimHierarchy* hierarchy,
        int boneId,
        int parentBoneId,
        int childBoneId,
        const CVector& desiredWorldDirection,
        float blend,
        float maxRadians
    ) {
        if (!hierarchy || blend <= 0.001f)
            return false;

        RawHAnimInterpFrame* interp = GetHAnimInterpFrame(hierarchy, boneId);
        RwMatrix* bone = GetSkinBoneMatrix(hierarchy, boneId);
        RwMatrix* parent = GetSkinBoneMatrix(hierarchy, parentBoneId);
        RwMatrix* child = GetSkinBoneMatrix(hierarchy, childBoneId);
        if (!interp || !bone || !parent || !child)
            return false;

        const CVector bonePos = Vec3(bone->pos.x, bone->pos.y, bone->pos.z);
        const CVector childPos = Vec3(child->pos.x, child->pos.y, child->pos.z);
        const CVector currentDirection = Normalize3(Sub3(childPos, bonePos), desiredWorldDirection);
        const CVector desiredDirection = Normalize3(desiredWorldDirection, currentDirection);
        const float dot = ClampFloat(Dot3(currentDirection, desiredDirection), -1.0f, 1.0f);
        if (dot > 0.99995f)
            return true;

        CVector axisWorld = Cross3(currentDirection, desiredDirection);
        float axisLenSq = Dot3(axisWorld, axisWorld);
        if (!IsFiniteFloat(axisLenSq) || axisLenSq < 1.0e-8f) {
            // Nearly 180 degrees: use a stable axis from the parent's current basis.
            axisWorld = Cross3(currentDirection,
                Vec3(parent->up.x, parent->up.y, parent->up.z));
            axisLenSq = Dot3(axisWorld, axisWorld);
            if (axisLenSq < 1.0e-8f)
                axisWorld = Cross3(currentDirection,
                    Vec3(parent->right.x, parent->right.y, parent->right.z));
            axisLenSq = Dot3(axisWorld, axisWorld);
            if (axisLenSq < 1.0e-8f)
                return false;
        }
        axisWorld = Scale3(axisWorld, 1.0f / std::sqrt(axisLenSq));
        const CVector axisLocal = WorldAxisToParentLocal(axisWorld, *parent);

        float radians = std::acos(dot) * ClampFloat(blend, 0.0f, 1.0f);
        if (maxRadians > 0.0f && radians > maxRadians)
            radians = maxRadians;
        if (radians < 1.0e-5f)
            return true;

        RwV3d rwAxis = { axisLocal.x, axisLocal.y, axisLocal.z };
        RtQuatRotate(&interp->q, &rwAxis, radians * 57.29577951308232f, rwCOMBINEPRECONCAT);

        // axisLocal is expressed in the PARENT bone's coordinate system. For a child
        // local quaternion that correction must be PRE-concatenated (R * q), not
        // POST-concatenated (q * R). The latter interprets the same axis as child-local
        // and is exactly what made the correction wander as the walking parent rotated.
        // Rebuild the hierarchy from the corrected local quaternion.
        return RpHAnimHierarchyUpdateMatrices(hierarchy) != 0;
    }


    static bool StabilizeSkinnedSecondHandWeaponForward(
        CPed* ped,
        RpHAnimHierarchy* hierarchy,
        float blend
    ) {
        if (!gConfig.stabilizeSecondHand || !ped || !hierarchy || blend <= 0.001f)
            return true;

        RawHAnimInterpFrame* handInterp = GetHAnimInterpFrame(hierarchy, BONE_SLHAND);
        RwMatrix* nativeHand = GetSkinBoneMatrix(hierarchy, BONE_SRHAND);
        RwMatrix* secondLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARML);
        RwMatrix* secondHand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!handInterp || !nativeHand || !secondLower || !secondHand)
            return false;

        CVector nativeWeaponDir, secondWeaponDir;
        if (!ComputeNativeAndSecondWeaponDirections(
            ped, *nativeHand, *secondHand, nativeWeaponDir, secondWeaponDir))
            return false;

        const float dot = ClampFloat(Dot3(secondWeaponDir, nativeWeaponDir), -1.0f, 1.0f);
        if (dot > 0.99995f)
            return true;

        CVector axisWorld = Cross3(secondWeaponDir, nativeWeaponDir);
        float axisLenSq = Dot3(axisWorld, axisWorld);
        if (!IsFiniteFloat(axisLenSq) || axisLenSq < 1.0e-8f) {
            // Stable 180-degree fallback from the forearm basis.
            axisWorld = Cross3(secondWeaponDir,
                Vec3(secondLower->up.x, secondLower->up.y, secondLower->up.z));
            axisLenSq = Dot3(axisWorld, axisWorld);
            if (axisLenSq < 1.0e-8f) {
                axisWorld = Cross3(secondWeaponDir,
                    Vec3(secondLower->right.x, secondLower->right.y, secondLower->right.z));
                axisLenSq = Dot3(axisWorld, axisWorld);
            }
            if (axisLenSq < 1.0e-8f)
                return false;
        }
        axisWorld = Scale3(axisWorld, 1.0f / std::sqrt(axisLenSq));

        const CVector axisLocal = WorldAxisToParentLocal(axisWorld, *secondLower);
        float radians = std::acos(dot) * ClampFloat(blend * gConfig.handAimBlend, 0.0f, 1.0f);
        const float limitRadians = gConfig.handAimLimitDeg * 0.01745329251994329577f;
        if (limitRadians > 0.0f && radians > limitRadians)
            radians = limitRadians;
        if (radians < 1.0e-5f)
            return true;

        RwV3d rwAxis = { axisLocal.x, axisLocal.y, axisLocal.z };
        RtQuatRotate(&handInterp->q, &rwAxis,
            radians * 57.29577951308232f, rwCOMBINEPRECONCAT);

        // Local quaternion is the procedural input; matrix array is output. The
        // animation system will refresh this local pose on the next animation update.
        return RpHAnimHierarchyUpdateMatrices(hierarchy) != 0;
    }


    static bool StabilizeSkinnedSecondHandWeaponRoll(CPed* ped, RpHAnimHierarchy* hierarchy, float blend) {
        if (!gConfig.stabilizeSecondHandRoll || !ped || !hierarchy || blend <= 0.001f)
            return true;

        RawHAnimInterpFrame* handInterp = GetHAnimInterpFrame(hierarchy, BONE_SLHAND);
        RwMatrix* nativeHand = GetSkinBoneMatrix(hierarchy, BONE_SRHAND);
        RwMatrix* secondLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARML);
        RwMatrix* secondHand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!handInterp || !nativeHand || !secondLower || !secondHand)
            return false;

        CVector axisWorld;
        float radians = 0.0f;
        if (!ComputeWeaponRollCorrection(ped, *nativeHand, *secondHand, blend, axisWorld, radians))
            return false;
        if (std::fabs(radians) < 1.0e-5f)
            return true;

        const CVector axisLocal = WorldAxisToParentLocal(axisWorld, *secondLower);
        RwV3d rwAxis = { axisLocal.x, axisLocal.y, axisLocal.z };
        RtQuatRotate(&handInterp->q, &rwAxis,
            radians * 57.29577951308232f, rwCOMBINEPRECONCAT);
        return RpHAnimHierarchyUpdateMatrices(hierarchy) != 0;
    }

    static bool MirrorSkinnedGunArmPose(CPed* ped, RpHAnimHierarchy* hierarchy, float blend) {
        if (!ped || !hierarchy || blend <= 0.0f)
            return false;

        // CPed::AimGun has just changed the native arm's local HAnim quaternion.
        // Materialize that finished native pose before measuring segment directions.
        if (!RpHAnimHierarchyUpdateMatrices(hierarchy))
            return false;

        RwMatrix* nativeUpper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARMR);
        RwMatrix* nativeLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARMR);
        RwMatrix* nativeHand = GetSkinBoneMatrix(hierarchy, BONE_SRHAND);
        RwMatrix* mirrorUpper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARML);
        RwMatrix* mirrorLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARML);
        RwMatrix* mirrorHand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!nativeUpper || !nativeLower || !nativeHand ||
            !mirrorUpper || !mirrorLower || !mirrorHand)
            return false;

        const CVector nativeShoulder = Vec3(nativeUpper->pos.x, nativeUpper->pos.y, nativeUpper->pos.z);
        const CVector nativeElbow = Vec3(nativeLower->pos.x, nativeLower->pos.y, nativeLower->pos.z);
        const CVector nativeHandPos = Vec3(nativeHand->pos.x, nativeHand->pos.y, nativeHand->pos.z);
        const CVector mirrorShoulder = Vec3(mirrorUpper->pos.x, mirrorUpper->pos.y, mirrorUpper->pos.z);

        const CVector mirrorNormal = Normalize3(
            Sub3(nativeShoulder, mirrorShoulder),
            Normalize3(ped->GetRight(), Vec3(1.0f, 0.0f, 0.0f)));

        // Xbox/Skin & Bones hierarchy IDs follow GTA III's 16-bone HAnim order:
        // torso=8, native upper/lower/hand=10/11/12, opposite=13/14/15.
        const CVector desiredUpperDir =
            ReflectDirection(Sub3(nativeElbow, nativeShoulder), mirrorNormal);
        if (!RotateSkinnedBoneTowardDirection(
            hierarchy, BONE_SUPPERARML, 8, BONE_SLOWERARML,
            desiredUpperDir, blend, 1.35f))
            return false;

        // Upper-arm correction has rebuilt the hierarchy, so measure the lower arm from
        // its new current pose before applying the forearm correction.
        mirrorLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARML);
        mirrorHand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!mirrorLower || !mirrorHand)
            return false;

        const CVector desiredLowerDir =
            ReflectDirection(Sub3(nativeHandPos, nativeElbow), mirrorNormal);
        if (!RotateSkinnedBoneTowardDirection(
            hierarchy, BONE_SLOWERARML, BONE_SUPPERARML, BONE_SLHAND,
            desiredLowerDir, blend, 1.55f))
            return false;

        // Correct the local hand quaternion enough to make the duplicate weapon's real
        // fire-offset vector follow the native gun direction; do not write a final world matrix.
        if (!StabilizeSkinnedSecondHandWeaponForward(ped, hierarchy, blend))
            return false;
        if (!StabilizeSkinnedSecondHandWeaponRoll(ped, hierarchy, blend))
            return false;
        return GetSkinBoneMatrix(hierarchy, BONE_SLHAND) != 0;
    }


    struct SkinnedSecondArmBackup {
        bool valid;
        RtQuat upperQ;
        RtQuat lowerQ;
        RtQuat handQ;
        SkinnedSecondArmBackup() : valid(false) {
            std::memset(&upperQ, 0, sizeof(upperQ));
            std::memset(&lowerQ, 0, sizeof(lowerQ));
            std::memset(&handQ, 0, sizeof(handQ));
        }
    };

    struct MatrixAxisRotation {
        CVector axis;
        float c;
        float s;
        bool identity;
        MatrixAxisRotation()
            : axis(Vec3(0.0f, 0.0f, 1.0f)), c(1.0f), s(0.0f), identity(true) {
        }
    };

    static float Length3(const CVector& v) {
        const float lenSq = Dot3(v, v);
        return (!IsFiniteFloat(lenSq) || lenSq <= 0.0f) ? 0.0f : std::sqrt(lenSq);
    }

    static CVector ReflectPointAcrossPlane(const CVector& p, const CVector& planePoint, const CVector& unitNormal) {
        return Sub3(p, Scale3(unitNormal, 2.0f * Dot3(Sub3(p, planePoint), unitNormal)));
    }

    static MatrixAxisRotation MakeMatrixAxisRotation(
        const CVector& fromVector,
        const CVector& toVector,
        const Basis3& hint
    ) {
        MatrixAxisRotation r;
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
            if (Dot3(axis, axis) <= 0.000001f)
                return r;
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

    static CVector ApplyMatrixAxisRotation(const CVector& v, const MatrixAxisRotation& r) {
        return r.identity ? v : RotateAroundAxis(v, r.axis, r.c, r.s);
    }

    static void ApplyMatrixAxisRotationToWorldMatrix(
        RwMatrix& matrix,
        const CVector& pivot,
        const MatrixAxisRotation& r
    ) {
        if (r.identity)
            return;

        Basis3 basis = BasisFromRwMatrix(&matrix);
        basis.right = ApplyMatrixAxisRotation(basis.right, r);
        basis.up = ApplyMatrixAxisRotation(basis.up, r);
        basis.at = ApplyMatrixAxisRotation(basis.at, r);
        basis = OrthonormalizeBasis(basis);

        const CVector oldPos = Vec3(matrix.pos.x, matrix.pos.y, matrix.pos.z);
        const CVector newPos = Add3(pivot, ApplyMatrixAxisRotation(Sub3(oldPos, pivot), r));

        matrix.right.x = basis.right.x; matrix.right.y = basis.right.y; matrix.right.z = basis.right.z;
        matrix.up.x = basis.up.x; matrix.up.y = basis.up.y; matrix.up.z = basis.up.z;
        matrix.at.x = basis.at.x; matrix.at.y = basis.at.y; matrix.at.z = basis.at.z;
        matrix.pos.x = newPos.x; matrix.pos.y = newPos.y; matrix.pos.z = newPos.z;
    }

    static bool BackupSkinnedSecondArm(RpHAnimHierarchy* hierarchy, SkinnedSecondArmBackup& backup) {
        backup.valid = false;
        if (!hierarchy)
            return false;

        RawHAnimInterpFrame* upper = GetHAnimInterpFrame(hierarchy, BONE_SUPPERARML);
        RawHAnimInterpFrame* lower = GetHAnimInterpFrame(hierarchy, BONE_SLOWERARML);
        RawHAnimInterpFrame* hand = GetHAnimInterpFrame(hierarchy, BONE_SLHAND);
        if (!upper || !lower || !hand)
            return false;

        backup.upperQ = upper->q;
        backup.lowerQ = lower->q;
        backup.handQ = hand->q;
        backup.valid = true;
        return true;
    }

    static void RestoreSkinnedSecondArm(RpHAnimHierarchy* hierarchy, const SkinnedSecondArmBackup& backup) {
        if (!hierarchy || !backup.valid)
            return;

        RawHAnimInterpFrame* upper = GetHAnimInterpFrame(hierarchy, BONE_SUPPERARML);
        RawHAnimInterpFrame* lower = GetHAnimInterpFrame(hierarchy, BONE_SLOWERARML);
        RawHAnimInterpFrame* hand = GetHAnimInterpFrame(hierarchy, BONE_SLHAND);
        if (!upper || !lower || !hand)
            return;

        upper->q = backup.upperQ;
        lower->q = backup.lowerQ;
        hand->q = backup.handQ;
        // Final HAnim matrices are solver output. Rebuild them from the restored local
        // rotations so no render/fire transaction leaks into later systems or next frame.
        RpHAnimHierarchyUpdateMatrices(hierarchy);
    }

    // v14 Skin & Bones visual controller. Do not solve the independently animated
    // opposite arm. Mirror the COMPLETE final native weapon-arm pose instead, so
    // forward-walk arm swing cannot participate in the result at all.
    static float BasisSimilarity(const Basis3& a, const Basis3& b) {
        return Dot3(a.right, b.right) + Dot3(a.up, b.up) + Dot3(a.at, b.at);
    }

    static Basis3 BuildProperMirroredBasis(
        const RwMatrix& nativeMatrix,
        const RwMatrix& oppositeReference,
        const CVector& mirrorNormal
    ) {
        // Reflecting all three world axes produces an improper (det=-1) basis. A
        // left/right skeleton also changes local handedness, so flip exactly one
        // reflected local axis to restore a proper rotation. Pick the axis whose
        // resulting bind orientation is closest to the real opposite bone. This
        // avoids hardcoding an Xbox bone-axis convention.
        const Basis3 nativeBasis = BasisFromRwMatrix(&nativeMatrix);
        const Basis3 reference = BasisFromRwMatrix(&oppositeReference);

        const CVector rr = ReflectDirection(nativeBasis.right, mirrorNormal);
        const CVector ru = ReflectDirection(nativeBasis.up, mirrorNormal);
        const CVector ra = ReflectDirection(nativeBasis.at, mirrorNormal);

        Basis3 candidates[3];
        candidates[0].right = Scale3(rr, -1.0f); candidates[0].up = ru;                  candidates[0].at = ra;
        candidates[1].right = rr;                  candidates[1].up = Scale3(ru, -1.0f); candidates[1].at = ra;
        candidates[2].right = rr;                  candidates[2].up = ru;                  candidates[2].at = Scale3(ra, -1.0f);

        int best = 0;
        float bestScore = BasisSimilarity(candidates[0], reference);
        for (int i = 1; i < 3; ++i) {
            const float score = BasisSimilarity(candidates[i], reference);
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        return OrthonormalizeBasis(candidates[best]);
    }

    static bool MirrorNativeBoneMatrixExact(
        const RwMatrix& nativeMatrix,
        const RwMatrix& oppositeReference,
        const CVector& planePoint,
        const CVector& mirrorNormal,
        RwMatrix& out
    ) {
        out = oppositeReference; // preserve RenderWare flags/padding from the real bone.
        const Basis3 basis = BuildProperMirroredBasis(nativeMatrix, oppositeReference, mirrorNormal);
        const CVector nativePos = Vec3(nativeMatrix.pos.x, nativeMatrix.pos.y, nativeMatrix.pos.z);
        const CVector mirroredPos = ReflectPointAcrossPlane(nativePos, planePoint, mirrorNormal);

        out.right.x = basis.right.x; out.right.y = basis.right.y; out.right.z = basis.right.z;
        out.up.x = basis.up.x;       out.up.y = basis.up.y;       out.up.z = basis.up.z;
        out.at.x = basis.at.x;       out.at.y = basis.at.y;       out.at.z = basis.at.z;
        out.pos.x = mirroredPos.x;   out.pos.y = mirroredPos.y;   out.pos.z = mirroredPos.z;
        return IsFiniteRwMatrix(out);
    }

    static bool RebaseWorldMatrix(const RwMatrix& oldRoot, const RwMatrix& newRoot,
        const RwMatrix& source, RwMatrix& out) {
        if (!IsFiniteRwMatrix(oldRoot) || !IsFiniteRwMatrix(newRoot) || !IsFiniteRwMatrix(source))
            return false;

        const Basis3 oldBasis = BasisFromRwMatrix(&oldRoot);
        const Basis3 newBasis = BasisFromRwMatrix(&newRoot);
        const Basis3 sourceBasis = BasisFromRwMatrix(&source);

        Basis3 local;
        local.right = BasisInverseTransformDirection(oldBasis, sourceBasis.right);
        local.up = BasisInverseTransformDirection(oldBasis, sourceBasis.up);
        local.at = BasisInverseTransformDirection(oldBasis, sourceBasis.at);
        const Basis3 rebased = ComposeBasis(newBasis, OrthonormalizeBasis(local));

        const CVector oldRootPos = Vec3(oldRoot.pos.x, oldRoot.pos.y, oldRoot.pos.z);
        const CVector newRootPos = Vec3(newRoot.pos.x, newRoot.pos.y, newRoot.pos.z);
        const CVector sourcePos = Vec3(source.pos.x, source.pos.y, source.pos.z);
        const CVector localPos = BasisInverseTransformDirection(oldBasis, Sub3(sourcePos, oldRootPos));
        const CVector rebasedPos = Add3(newRootPos, BasisTransformDirection(newBasis, localPos));

        out = source;
        out.right.x = rebased.right.x; out.right.y = rebased.right.y; out.right.z = rebased.right.z;
        out.up.x = rebased.up.x; out.up.y = rebased.up.y; out.up.z = rebased.up.z;
        out.at.x = rebased.at.x; out.at.y = rebased.at.y; out.at.z = rebased.at.z;
        out.pos.x = rebasedPos.x; out.pos.y = rebasedPos.y; out.pos.z = rebasedPos.z;
        return IsFiniteRwMatrix(out);
    }

    static void InvalidateStableNativeAimPose() {
        gStableNativeAimPose = StableNativeAimPose();
        gPoseNativeHandBasisValid = false;
        gSolvedSecondHandGripValid = false;
    }

    static bool CaptureStableNativeAimPose(CPed* ped, RpHAnimHierarchy* hierarchy) {
        if (!ped || !hierarchy)
            return false;
        RwMatrix* waist = GetSkinBoneMatrix(hierarchy, BONE_SWAIST);
        RwMatrix* upper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARMR);
        RwMatrix* lower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARMR);
        RwMatrix* hand = GetSkinBoneMatrix(hierarchy, BONE_SRHAND);
        CWeapon* weapon = ped->GetWeapon();
        if (!waist || !upper || !lower || !hand || !weapon ||
            !IsFiniteRwMatrix(*waist) || !IsFiniteRwMatrix(*upper) ||
            !IsFiniteRwMatrix(*lower) || !IsFiniteRwMatrix(*hand))
            return false;

        gStableNativeAimPose.valid = true;
        gStableNativeAimPose.owner = ped;
        gStableNativeAimPose.clump = ped->m_pRwClump;
        gStableNativeAimPose.weaponType = weapon->m_eWeaponType;
        gStableNativeAimPose.frame = CTimer::m_FrameCounter;
        gStableNativeAimPose.waist = *waist;
        gStableNativeAimPose.upper = *upper;
        gStableNativeAimPose.lower = *lower;
        gStableNativeAimPose.hand = *hand;
        return true;
    }

    static bool GetStableNativeAimSource(CPed* ped, RpHAnimHierarchy* hierarchy,
        RwMatrix& upper, RwMatrix& lower, RwMatrix& hand) {
        if (!ped || !hierarchy)
            return false;

        RwMatrix* currentWaist = GetSkinBoneMatrix(hierarchy, BONE_SWAIST);
        RwMatrix* currentUpper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARMR);
        RwMatrix* currentLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARMR);
        RwMatrix* currentHand = GetSkinBoneMatrix(hierarchy, BONE_SRHAND);
        CWeapon* weapon = ped->GetWeapon();
        if (!currentWaist || !currentUpper || !currentLower || !currentHand || !weapon)
            return false;

        const unsigned int frame = CTimer::m_FrameCounter;
        const bool verifiedNativeAimThisFrame = gLastNativeAimGunFrame == frame;

        // A pose is only admitted to the cache on a frame where GTA actually executed
        // CPed::AimGun. This is the critical difference from v14: a forward-walk frame
        // whose SetMoveAnim cleared the aim flag can never become the twin-arm source.
        // Do NOT reject low aim by testing hand.z against shoulder.z. GTA III's native
        // gun IK legitimately moves the weapon hand below the shoulder when pitching
        // down. gLastNativeAimGunFrame is the authoritative proof that this is a real
        // AimGun result, regardless of elevation.
        if (verifiedNativeAimThisFrame) {
            if (CaptureStableNativeAimPose(ped, hierarchy)) {
                upper = *currentUpper;
                lower = *currentLower;
                hand = *currentHand;
                gPoseNativeHandBasis = hand;
                gPoseNativeHandBasisValid = true;
                return true;
            }
        }

        const unsigned int maxAge = static_cast<unsigned int>(gConfig.aimHoldFrames + 3);
        if (!gStableNativeAimPose.valid || gStableNativeAimPose.owner != ped ||
            gStableNativeAimPose.clump != ped->m_pRwClump ||
            gStableNativeAimPose.weaponType != weapon->m_eWeaponType ||
            frame - gStableNativeAimPose.frame > maxAge || !IsFiniteRwMatrix(*currentWaist))
            return false;

        // Rebase the last VERIFIED aiming pose through the current waist transform.
        // Claude can translate/turn with locomotion, but the walk animation is never
        // allowed to supply the opposite weapon-arm bend on a skipped AimGun frame.
        if (!RebaseWorldMatrix(gStableNativeAimPose.waist, *currentWaist, gStableNativeAimPose.upper, upper) ||
            !RebaseWorldMatrix(gStableNativeAimPose.waist, *currentWaist, gStableNativeAimPose.lower, lower) ||
            !RebaseWorldMatrix(gStableNativeAimPose.waist, *currentWaist, gStableNativeAimPose.hand, hand))
            return false;

        ++gCachedAimPoseUses;
        gPoseNativeHandBasis = hand;
        gPoseNativeHandBasisValid = true;
        return true;
    }

    static void CopyRwBasis(const RwMatrix& source, RwMatrix& destination) {
        destination.right.x = source.right.x; destination.right.y = source.right.y; destination.right.z = source.right.z;
        destination.up.x = source.up.x;       destination.up.y = source.up.y;       destination.up.z = source.up.z;
        destination.at.x = source.at.x;       destination.at.y = source.at.y;       destination.at.z = source.at.z;
    }

    static void WriteBasisToRwMatrix(const Basis3& basis, RwMatrix& destination) {
        destination.right.x = basis.right.x; destination.right.y = basis.right.y; destination.right.z = basis.right.z;
        destination.up.x = basis.up.x;       destination.up.y = basis.up.y;       destination.up.z = basis.up.z;
        destination.at.x = basis.at.x;       destination.at.y = basis.at.y;       destination.at.z = basis.at.z;
    }

    static bool StabilizeSkinnedSecondHandToNativeWeapon(
        CPed* ped,
        RpHAnimHierarchy* hierarchy,
        const RwMatrix& nativeHandSource,
        float blend
    ) {
        if (!ped || !hierarchy || blend <= 0.001f)
            return false;

        RawHAnimInterpFrame* handInterp = GetHAnimInterpFrame(hierarchy, BONE_SLHAND);
        RwMatrix* secondLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARML);
        RwMatrix* secondHand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!handInterp || !secondLower || !secondHand || !IsFiniteRwMatrix(nativeHandSource))
            return false;

        CVector localFireDir;
        if (!GetActiveWeaponLocalFireDirection(ped, localFireDir))
            return false;

        // First align the actual duplicate weapon's barrel direction. The duplicate
        // weapon transform includes the bind-pose grip correction; do not align raw
        // SLhand axes and then hope the asymmetric pistol model follows them.
        RwMatrix gripWorld;
        if (!BuildOppositeGripWorld(ped, hierarchy, nativeHandSource, *secondHand, gripWorld))
            return false;

        const CVector desiredForward = Normalize3(
            BasisTransformDirection(BasisFromRwMatrix(&nativeHandSource), localFireDir),
            Vec3(0.0f, 1.0f, 0.0f));
        const CVector currentForward = Normalize3(
            BasisTransformDirection(BasisFromRwMatrix(&gripWorld), localFireDir),
            desiredForward);

        float forwardDot = ClampFloat(Dot3(currentForward, desiredForward), -1.0f, 1.0f);
        if (forwardDot < 0.99995f) {
            CVector axisWorld = Cross3(currentForward, desiredForward);
            float axisLenSq = Dot3(axisWorld, axisWorld);
            if (axisLenSq < 1.0e-8f) {
                axisWorld = Cross3(currentForward,
                    Vec3(secondLower->up.x, secondLower->up.y, secondLower->up.z));
                axisLenSq = Dot3(axisWorld, axisWorld);
                if (axisLenSq < 1.0e-8f) {
                    axisWorld = Cross3(currentForward,
                        Vec3(secondLower->right.x, secondLower->right.y, secondLower->right.z));
                    axisLenSq = Dot3(axisWorld, axisWorld);
                }
            }
            if (axisLenSq > 1.0e-8f) {
                axisWorld = Scale3(axisWorld, 1.0f / std::sqrt(axisLenSq));
                const CVector axisLocal = WorldAxisToParentLocal(axisWorld, *secondLower);
                float radians = std::acos(forwardDot) * ClampFloat(blend * gConfig.handAimBlend, 0.0f, 1.0f);
                const float limit = gConfig.handAimLimitDeg * 0.01745329251994329577f;
                if (limit > 0.0f && radians > limit)
                    radians = limit;
                RwV3d rwAxis = { axisLocal.x, axisLocal.y, axisLocal.z };
                RtQuatRotate(&handInterp->q, &rwAxis,
                    radians * 57.29577951308232f, rwCOMBINEPRECONCAT);
                if (!RpHAnimHierarchyUpdateMatrices(hierarchy))
                    return false;
            }
        }

        // Then match roll around the common barrel axis. Re-read the solved hand and
        // grip after the forward correction so this is one coherent local-quaternion
        // solve, not a world-matrix overwrite of the wrist after the fact.
        handInterp = GetHAnimInterpFrame(hierarchy, BONE_SLHAND);
        secondLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARML);
        secondHand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!handInterp || !secondLower || !secondHand ||
            !BuildOppositeGripWorld(ped, hierarchy, nativeHandSource, *secondHand, gripWorld))
            return false;

        if (gConfig.stabilizeSecondHandRoll) {
            const CVector axis = desiredForward;
            const Basis3 nativeBasis = BasisFromRwMatrix(&nativeHandSource);
            const Basis3 gripBasis = BasisFromRwMatrix(&gripWorld);
            const CVector nativeReference = ProjectDirectionOntoPlane(nativeBasis.up, axis,
                ProjectDirectionOntoPlane(nativeBasis.right, axis, Vec3(1.0f, 0.0f, 0.0f)));
            const CVector gripReference = ProjectDirectionOntoPlane(gripBasis.up, axis,
                ProjectDirectionOntoPlane(gripBasis.right, axis, nativeReference));

            const float c = ClampFloat(Dot3(gripReference, nativeReference), -1.0f, 1.0f);
            const float sign = Dot3(Cross3(gripReference, nativeReference), axis);
            float radians = std::atan2(sign, c) * ClampFloat(blend * gConfig.handRollBlend, 0.0f, 1.0f);
            const float limit = gConfig.handRollLimitDeg * 0.01745329251994329577f;
            if (limit > 0.0f) {
                if (radians > limit) radians = limit;
                if (radians < -limit) radians = -limit;
            }

            if (std::fabs(radians) > 1.0e-5f) {
                const CVector axisLocal = WorldAxisToParentLocal(axis, *secondLower);
                RwV3d rwAxis = { axisLocal.x, axisLocal.y, axisLocal.z };
                RtQuatRotate(&handInterp->q, &rwAxis,
                    radians * 57.29577951308232f, rwCOMBINEPRECONCAT);
                if (!RpHAnimHierarchyUpdateMatrices(hierarchy))
                    return false;
            }
        }

        return true;
    }

    // v21 Skin & Bones controller: copy the successful VC geometry exactly at the
    // segment level, but keep the GTA III-specific continuity and bind-socket fixes.
    //
    // Important: the opposite shoulder must NOT inherit the native upper-arm direction
    // unchanged. The native direction contains a lateral component appropriate to the
    // native shoulder. Reusing it on the opposite shoulder makes the arm curl inward.
    // Reflecting that direction across the ped sagittal plane flips only the lateral
    // component, producing the Max-Payne/VC-style symmetric forward firing pose.
    static bool SolveSkinnedSecondArmLocalHAnim(
        CPed* ped,
        RpHAnimHierarchy* hierarchy,
        const RwMatrix& nativeUpperSource,
        const RwMatrix& nativeLowerSource,
        const RwMatrix& nativeHandSource,
        float blend
    ) {
        if (!ped || !hierarchy || blend <= 0.001f ||
            !IsFiniteRwMatrix(nativeUpperSource) || !IsFiniteRwMatrix(nativeLowerSource) ||
            !IsFiniteRwMatrix(nativeHandSource))
            return false;

        if (!RpHAnimHierarchyUpdateMatrices(hierarchy))
            return false;

        const CVector nativeShoulder = Vec3(
            nativeUpperSource.pos.x, nativeUpperSource.pos.y, nativeUpperSource.pos.z);
        const CVector nativeElbow = Vec3(
            nativeLowerSource.pos.x, nativeLowerSource.pos.y, nativeLowerSource.pos.z);
        const CVector nativeHandPos = Vec3(
            nativeHandSource.pos.x, nativeHandSource.pos.y, nativeHandSource.pos.z);

        RwMatrix* secondUpper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARML);
        if (!secondUpper)
            return false;

        const CVector secondShoulder = Vec3(
            secondUpper->pos.x, secondUpper->pos.y, secondUpper->pos.z);

        // Same mirror plane convention used by the VC revision and the original v3 III
        // controller: plane normal is the ped's right axis. Fall back to the live
        // shoulder-to-shoulder axis if a mod has left the entity basis degenerate.
        const CVector shoulderAxis = Normalize3(
            Sub3(nativeShoulder, secondShoulder),
            Vec3(1.0f, 0.0f, 0.0f));
        const CVector mirrorNormal = Normalize3(
            ped->GetRight(),
            shoulderAxis);

        const CVector nativeUpperDir = Normalize3(
            Sub3(nativeElbow, nativeShoulder),
            Vec3(0.0f, 1.0f, 0.0f));
        const CVector nativeLowerDir = Normalize3(
            Sub3(nativeHandPos, nativeElbow),
            nativeUpperDir);

        CVector desiredUpperDir = Normalize3(
            ReflectDirection(nativeUpperDir, mirrorNormal),
            nativeUpperDir);
        const CVector desiredLowerDir = Normalize3(
            ReflectDirection(nativeLowerDir, mirrorNormal),
            nativeLowerDir);

        // Optional EXTRA flare after the true mirror. Default is zero because the mirror
        // already supplies the outward elbow arch that VC gets naturally. This remains
        // available only as a small taste adjustment.
        if (gConfig.elbowOutwardDeg > 0.001f) {
            const CVector outward = Normalize3(
                Sub3(secondShoulder, nativeShoulder),
                Scale3(mirrorNormal, -1.0f));
            const float maxRadians = gConfig.elbowOutwardDeg * 0.01745329251994329577f;
            const float d = ClampFloat(Dot3(desiredUpperDir, outward), -1.0f, 1.0f);
            const float angle = std::acos(d);
            if (angle > 1.0e-5f) {
                CVector axis = Cross3(desiredUpperDir, outward);
                const float axisLen = Length3(axis);
                if (axisLen > 1.0e-5f) {
                    axis = Scale3(axis, 1.0f / axisLen);
                    const float radians = (angle < maxRadians) ? angle : maxRadians;
                    desiredUpperDir = Normalize3(
                        RotateAroundAxis(desiredUpperDir, axis,
                            std::cos(radians), std::sin(radians)),
                        desiredUpperDir);
                }
            }
        }

        // Match the proven VC limits rather than v20's much looser 1.50/1.70 rad caps.
        // These are caps only; at blend=1 the solver still reaches the reflected target
        // whenever the required correction lies inside the normal firing range.
        if (!RotateSkinnedBoneTowardDirection(
            hierarchy, BONE_SUPPERARML, 8, BONE_SLOWERARML,
            desiredUpperDir, blend, 0.95f))
            return false;

        if (!RotateSkinnedBoneTowardDirection(
            hierarchy, BONE_SLOWERARML, BONE_SUPPERARML, BONE_SLHAND,
            desiredLowerDir, blend, 1.10f))
            return false;

        // Keep v19/v20's GTA III-specific grip solution. The VC lesson being copied here
        // is the arm geometry; the bind-derived socket is still safer for Skin & Bones.
        if (!StabilizeSkinnedSecondHandToNativeWeapon(
            ped, hierarchy, nativeHandSource, blend))
            return false;

        RwMatrix* secondHand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!secondHand ||
            !BuildOppositeGripWorld(ped, hierarchy, nativeHandSource, *secondHand,
                gSolvedSecondHandGrip))
            return false;

        gSolvedSecondHandGripValid = true;
        return true;
    }

    static bool PrepareTemporarySkinnedSecondArmPose(
        CPed* ped,
        RpHAnimHierarchy*& hierarchy,
        SkinnedSecondArmBackup& backup,
        float& blend
    ) {
        hierarchy = 0;
        blend = 0.0f;
        gPoseNativeHandBasisValid = false;
        gSolvedSecondHandGripValid = false;
        if (!gConfig.leftArmIK || !IsEligiblePlayer(ped) || !GetSkinnedPedHierarchy(ped, hierarchy))
            return false;

        if (!RpHAnimHierarchyUpdateMatrices(hierarchy))
            return false;
        if (!BackupSkinnedSecondArm(hierarchy, backup))
            return false;
        if (!ShouldMirrorAimPose(ped, blend)) {
            RestoreSkinnedSecondArm(hierarchy, backup);
            backup.valid = false;
            return false;
        }

        RwMatrix nativeUpperSource, nativeLowerSource, nativeHandSource;
        if (!GetStableNativeAimSource(ped, hierarchy, nativeUpperSource, nativeLowerSource, nativeHandSource) ||
            !SolveSkinnedSecondArmLocalHAnim(ped, hierarchy, nativeUpperSource, nativeLowerSource, nativeHandSource, blend)) {
            RestoreSkinnedSecondArm(hierarchy, backup);
            backup.valid = false;
            gPoseNativeHandBasisValid = false;
            gSolvedSecondHandGripValid = false;
            return false;
        }
        return true;
    }

    static bool ApplyCurrentMirror(CPed* ped, float* outBlend = 0) {
        if (outBlend)
            *outBlend = 0.0f;
        if (!gConfig.leftArmIK || !IsEligiblePlayer(ped))
            return false;

        RpHAnimHierarchy* hierarchy = 0;
        if (GetSkinnedPedHierarchy(ped, hierarchy)) {
            float blend = 0.0f;
            if (!ShouldMirrorAimPose(ped, blend))
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
        // Aim elevation is not a validity test. In particular, downward aim can put the
        // native hand well below its shoulder while the pose is still a valid gun pose.
        if (!ShouldMirrorAimPose(ped, blend))
            return false;
        MirrorNativeGunArmPose(ped, chain, blend);
        if (outBlend)
            *outBlend = blend;
        return true;
    }

    static void ApplySecondArmPosePass(CPed* ped, bool lateRepair) {
        if (!ped)
            return;

        if (!WantsSecondArmPose(ped))
            return;

        const unsigned int frame = CTimer::m_FrameCounter;
        unsigned int& lastFrame = lateRepair ? gLastLateRepairPoseFrame : gLastPostAimPoseFrame;
        if (lastFrame == frame)
            return;

        float blend = 0.0f;
        if (!ApplyCurrentMirror(ped, &blend))
            return;

        lastFrame = frame;
        if (lateRepair)
            ++gLateRepairPoseHits;
        else
            ++gPostAimPoseHits;

        if (gLeft.owner == ped)
            UpdateLeftWeaponWorldTransform();
        UpdateDualWieldState(ped, ped ? ped->GetWeapon() : 0, 0, false);
        MaybeLogHandState();

        bool& logged = lateRepair ? gLoggedFirstLateRepairPose : gLoggedFirstPostAimPose;
        if (!logged) {
            RpHAnimHierarchy* hierarchy = 0;
            const bool skinned = GetSkinnedPedHierarchy(ped, hierarchy);
            char line[448];
            std::snprintf(line, sizeof(line),
                "DualWieldIII: %s local arm correction active mode=%s blend=%.2f hierarchy=%p handStabilize=%d.",
                lateRepair ? "post-CGame repair" : "post-AimGun early",
                skinned ? "Skin&Bones/HAnim-render-space" : "stock/RwFrame-local",
                blend, hierarchy, gConfig.stabilizeSecondHand ? 1 : 0);
            Log(line);
            logged = true;
        }
    }

    static void ApplyPostAimSecondArmPose(CPed* ped) {
        // Early pass is still required: CPed::Attack happens later in the same ped
        // control path and must sample a corrected current-frame opposite-hand muzzle.
        ApplySecondArmPosePass(ped, false);
    }

    static void ApplyLateProcessSecondArmPose(CPed* ped) {
        if (!gConfig.lateProcessRepair || IsSkinnedPed(ped))
            return;

        // GTA III 1.0 EN IDB:
        //   CPlayerPed::ProcessControl 0x4EFD90
        //     0x4EFE50 -> CPed::ProcessControl
        //     0x4F03A2 -> SetRealMoveAnim
        //     0x4F048D -> ProcessAnimGroups (can ReApplyMoveAnims)
        // Plugin-SDK's gameProcessEvent is AFTER the CGame::Process call at 0x48E49B.
        // Optional compatibility pass for a third-party controller that changes the
        // skeleton again after the normal player process. Off by default to avoid applying
        // a second delta to the same fresh animation pose.
        ApplySecondArmPosePass(ped, true);
    }

    static void PrepareOppositePoseForFire(CPed* ped) {
        if (!ped || gLeft.owner != ped)
            return;

        // Skin & Bones is handled transactionally inside ComputeLeftFireSource: backup
        // clean local HAnim quaternions -> solve/rebuild -> sample helper/muzzle ->
        // restore/rebuild. Nothing procedural survives the fire transaction.
        if (gLeft.skinnedSource)
            return;

        if (gLastPostAimPoseFrame != CTimer::m_FrameCounter && WantsSecondArmPose(ped))
            ApplySecondArmPosePass(ped, false);
        UpdateLeftWeaponWorldTransform();
    }

    static void RenderStandaloneLeftWeapon(CPed* ped, bool transformAlreadyPrepared = false) {
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
            RwFrame* currentHand = GetPedFrameSafe(ped, PED_FRAME_SECOND_HAND);
            if (!currentHand || currentHand != gLeft.sourceHandFrame) {
                DestroyLeftWeapon();
                return;
            }
        }

        if (!transformAlreadyPrepared && !UpdateLeftWeaponWorldTransform())
            return;

        // Standalone atomic: no RpClumpAddAtomic, no HAnim ownership ambiguity. Its own
        // normal render callback/pipeline still runs, so shader mods can process it as a
        // regular weapon atomic without ped-clump walkers seeing it as a body skin.
        RpAtomicRender(gLeft.atomic);
    }

    static bool ComputeLeftFireSource(CPed* ped, CWeapon* weapon, const CVector* nativeMuzzle, CVector& out) {
        if (!ped || !weapon || gLeft.owner != ped || !gLeft.helperFrame)
            return false;

        if (gLeft.skinnedSource) {
            RpHAnimHierarchy* hierarchy = 0;
            SkinnedSecondArmBackup backup;
            float blend = 0.0f;
            const bool posed = PrepareTemporarySkinnedSecondArmPose(ped, hierarchy, backup, blend);
            if (!posed)
                return false;

            const bool weaponOk = UpdateLeftWeaponWorldTransform();
            RwMatrix weaponWorld;
            bool muzzleOk = false;
            if (weaponOk) {
                RwMatrix* helper = RwFrameGetMatrix(gLeft.helperFrame);
                CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType);
                if (helper && info && IsFiniteRwMatrix(*helper)) {
                    weaponWorld = *helper;
                    muzzleOk = ComputeMuzzleFromWeaponWorld(info, weaponWorld, out) && IsMuzzleSaneForPed(ped, out);
                }
            }

            // Fire sampling must not leave a procedural body pose behind. The helper
            // keeps the solved weapon matrix/muzzle, while the backed-up local HAnim
            // quaternions are restored and the normal matrix array is rebuilt.
            RestoreSkinnedSecondArm(hierarchy, backup);
            gPoseNativeHandBasisValid = false;
            gSolvedSecondHandGripValid = false;
            return muzzleOk;
        }

        if (!UpdateDualWieldState(ped, weapon, nativeMuzzle, true))
            return false;
        if (!gDualState.secondHand.muzzleValid)
            return false;

        out = gDualState.secondHand.muzzle;
        return true;
    }

    // Replaces only the CALL at CPed::Attack+0x31F, not CWeapon::Fire globally.
    static bool __fastcall AttackFireHook(CWeapon* weapon, void*, CEntity* shooter, CVector* rightSource) {
        WeaponFireFn fireFn = reinterpret_cast<WeaponFireFn>(
            gFirePatch.previousTarget ? gFirePatch.previousTarget : ADDR_WEAPON_FIRE);
        if (!fireFn)
            return false;

        const bool firedRight = fireFn(weapon, shooter, rightSource);
        if (firedRight) {
            gDualState.fireRightThisFrame = true;
        }
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

        // The opposite arm is normally authored beside native AimGun. The fire hook only
        // invokes the guarded fallback if that stage did not run this frame, then derives
        // the muzzle from the actual controlled second-hand weapon transform.
        PrepareOppositePoseForFire(ped);

        CVector leftSource;
        if (!ComputeLeftFireSource(ped, weapon, rightSource, leftSource))
            return firedRight;

        gDualState.fireRightThisFrame = true;
        gDualState.fireLeftThisFrame = true;
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
                    "DualWieldIII: second native shot succeeded at muzzle %.3f %.3f %.3f; native flash/smoke/light/shell path active.",
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

    static bool ShouldPreserveMoveAimFlag(CPed* ped) {
        if (!ped || !IsEligiblePlayer(ped))
            return false;
        CWeapon* weapon = ped->GetWeapon();
        if (!weapon || weapon->m_eWeaponState == WEAPONSTATE_RELOADING ||
            weapon->m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO)
            return false;

        CPad* pad = CPad::GetPad(0);
        const bool heldInput = pad && (pad->GetTarget() || pad->GetWeapon());
        return heldInput || ped->bIsAimingGun || ped->bIsPointingGunAt || ped->bIsShooting ||
            weapon->m_eWeaponState == WEAPONSTATE_FIRING || ped->m_ePedState == PEDSTATE_ATTACK;
    }

    static void __fastcall MoveAnimClearAimHook(CPed* ped, void*) {
        PedClearAimFn previous = reinterpret_cast<PedClearAimFn>(
            gMoveClearAimPatch.previousTarget ? gMoveClearAimPatch.previousTarget : ADDR_PED_CLEAR_AIM);

        // GTA III CPed::SetMoveAnim calls ClearAimFlag at 0x4C5BF3 specifically for
        // move states 2/3/4. With target/fire held that can make CPed::ProcessControl's
        // later 0x4CB037 AimGun gate miss alternating forward-walk frames. Preserve the
        // flag only for an eligible dual-wield player who is actively targeting/firing.
        if (ShouldPreserveMoveAimFlag(ped)) {
            ++gMoveAimClearSuppressions;
            gAimIntentHoldRemaining = gConfig.aimHoldFrames;
            gAimIntentLatched = true;
            return;
        }

        if (previous && reinterpret_cast<uintptr_t>(previous) != reinterpret_cast<uintptr_t>(&MoveAnimClearAimHook))
            previous(ped);
    }

    static bool EnsureMoveAimContinuityBridgeInstalled() {
        const uintptr_t hookTarget = reinterpret_cast<uintptr_t>(&MoveAnimClearAimHook);
        uintptr_t currentTarget = 0;
        int32_t currentRel = 0;
        if (!DecodeRelativeCall(ADDR_MOVE_CLEAR_AIM_CALL, currentTarget, currentRel))
            return false;
        if (currentTarget == hookTarget)
            return true;
        if (gMoveClearAimPatch.installed)
            return false;
        if (!InstallCallPatch(ADDR_MOVE_CLEAR_AIM_CALL, ADDR_PED_CLEAR_AIM,
            reinterpret_cast<void*>(&MoveAnimClearAimHook), gMoveClearAimPatch))
            return false;

        char line[320];
        std::snprintf(line, sizeof(line),
            "DualWieldIII: forward-move aim continuity bridge active at 0x4C5BF3. previous=%p%s.",
            reinterpret_cast<void*>(gMoveClearAimPatch.previousTarget),
            gMoveClearAimPatch.chained ? " [CHAINED]" : "");
        Log(line);
        return true;
    }

    static void __fastcall PedAimGunHook(CPed* ped, void*) {
        // Let GTA/Skin & Bones finish the normal animation and native gun-arm IK first.
        // The opposite arm is corrected once from that fresh current-frame pose below.
        PedAimGunFn previous = reinterpret_cast<PedAimGunFn>(
            gAimPatch.previousTarget ? gAimPatch.previousTarget : ADDR_PED_AIMGUN);
        if (previous && reinterpret_cast<uintptr_t>(previous) != reinterpret_cast<uintptr_t>(&PedAimGunHook))
            previous(ped);

        if (ped && IsEligiblePlayer(ped))
            gLastNativeAimGunFrame = CTimer::m_FrameCounter;

        // Stock PC skeleton keeps the old local-frame path. Skin & Bones/Xbox does NOT
        // receive a quaternion correction here anymore; v13 solves its final matrix pose
        // transactionally at fire sampling and immediately before rendering.
        if (!IsSkinnedPed(ped))
            ApplyPostAimSecondArmPose(ped);
    }

    static bool EnsureAimGunBridgeInstalled() {
        const uintptr_t hookTarget = reinterpret_cast<uintptr_t>(&PedAimGunHook);
        uintptr_t currentTarget = 0;
        int32_t currentRel = 0;
        if (!DecodeRelativeCall(ADDR_PED_AIMGUN_CALL, currentTarget, currentRel))
            return false;

        if (currentTarget == hookTarget)
            return true;

        if (gAimPatch.installed) {
            if (!gLoggedAimRechain) {
                char line[320];
                std::snprintf(line, sizeof(line),
                    "DualWieldIII: AimGun CALL replaced after our bridge (current=%p); not re-hooking to avoid a hook cycle.",
                    reinterpret_cast<void*>(currentTarget));
                Log(line);
                gLoggedAimRechain = true;
            }
            return false;
        }

        if (!InstallCallPatch(ADDR_PED_AIMGUN_CALL, ADDR_PED_AIMGUN,
            reinterpret_cast<void*>(&PedAimGunHook), gAimPatch)) {
            if (!gLoggedAimInstallFailure) {
                Log("DualWieldIII: AimGun CALL hook unavailable; stock-skeleton early IK disabled. Skin & Bones transactional local-HAnim solver remains available.");
                gLoggedAimInstallFailure = true;
            }
            return false;
        }

        {
            char line[320];
            std::snprintf(line, sizeof(line),
                "DualWieldIII: AimGun bridge active at 0x4CB037. previous=%p%s.",
                reinterpret_cast<void*>(gAimPatch.previousTarget),
                gAimPatch.chained ? " [CHAINED]" : "");
            Log(line);
        }
        return true;
    }

    static void __fastcall PedRenderBridge(CPed* ped, void*) {
        PedRenderCallFn previous = reinterpret_cast<PedRenderCallFn>(
            gRenderPatch.previousTarget ? gRenderPatch.previousTarget : ADDR_ENTITY_RENDER);

        RpHAnimHierarchy* hierarchy = 0;
        SkinnedSecondArmBackup backup;
        float blend = 0.0f;
        bool posed = false;

        // Skin & Bones calls CEntity::UpdateRpHAnim from CPed::PreRender before this
        // render site. Rebuild from the clean animation/native-IK state, then apply one
        // TEMPORARY local-HAnim opposite-arm correction and let RenderWare rebuild the
        // complete chain. No independent final bone matrices are authored here.
        bool weaponTransformPrepared = false;
        if (ped && IsEligiblePlayer(ped) && IsSkinnedPed(ped)) {
            posed = PrepareTemporarySkinnedSecondArmPose(ped, hierarchy, backup, blend);
            if (posed) {
                ++gRenderMirrorHits;
                if (gLeft.owner == ped && gLeft.skinnedSource && IsAtomicAliveForOwner(ped))
                    weaponTransformPrepared = UpdateLeftWeaponWorldTransform();
            }
        }

        if (previous && reinterpret_cast<uintptr_t>(previous) != reinterpret_cast<uintptr_t>(&PedRenderBridge))
            previous(ped);

        // The body and Skin & Bones' separate hand have just rendered from the same
        // coherently rebuilt HAnim snapshot. If we prepared the second weapon before
        // that draw, do NOT sample the hierarchy again: render the frozen helper matrix
        // verbatim so the gun and the fist come from the exact same solved hand.
        RenderStandaloneLeftWeapon(ped, weaponTransformPrepared);

        // Do not leak visual matrix edits into later systems or the next frame.
        if (posed)
            RestoreSkinnedSecondArm(hierarchy, backup);
        gPoseNativeHandBasisValid = false;
        gSolvedSecondHandGripValid = false;
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

    static void LogRuntimeSummary() {
        if (gLoggedRuntimeSummary)
            return;
        char line[320];
        std::snprintf(line, sizeof(line),
            "DualWieldIII summary: postAim=%u renderPose=%u moveAimClearSuppressed=%u cachedAimPose=%u shots=%u/%u skinned=%d aimPrev=%p renderPrev=%p.",
            gPostAimPoseHits, gRenderMirrorHits, gMoveAimClearSuppressions, gCachedAimPoseUses,
            gSecondShotSuccess, gSecondShotAttempts, gLoggedSkinnedPedMode ? 1 : 0,
            reinterpret_cast<void*>(gAimPatch.previousTarget),
            reinterpret_cast<void*>(gRenderPatch.previousTarget));
        Log(line);
        gLoggedRuntimeSummary = true;
    }

    static void OnPedDestroy(CPed* ped) {
        if (ped && (ped == gLeft.owner || ped == gStableNativeAimPose.owner)) {
            InvalidateStableNativeAimPose();
            gAimIntentHoldRemaining = 0;
            gAimIntentLatched = false;
        }
        if (ped && ped == gLeft.owner)
            DestroyLeftWeapon();
    }

    static void OnPedSetModel(CPed* ped) {
        if (!ped || ped != gLeft.owner)
            return;
        Log("DualWieldIII: pedSetModelEvent observed; destroying standalone second weapon before rebinding frames.");
        InvalidateStableNativeAimPose();
        gAimIntentHoldRemaining = 0;
        gAimIntentLatched = false;
        DestroyLeftWeapon();
    }

    class Mod {
    public:
        Mod() {
            BuildSiblingPath(gIniPath, sizeof(gIniPath), "DualWieldIII.ini");
            BuildSiblingPath(gLogPath, sizeof(gLogPath), "DualWieldIII.log");
            std::remove(gLogPath);
            LoadConfig();
            Log("DualWieldIII: initialized aim-continuity dual-wield controller.");

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
            // Deliberately do NOT patch 0x4D0484 from the ASI constructor. Skin & Bones
            // and shader mods may install their own render bridges during startup. The
            // first gameProcess tick occurs after ASI initialization and chains the final
            // owner instead of participating in a load-order race.
            Events::gameProcessEvent += [] {
                ResolveSkinBonesApi();
                EnsureMoveAimContinuityBridgeInstalled();
                EnsureAimGunBridgeInstalled();
                EnsureRenderBridgeInstalled();
                UpdateLeftWeapon();

                CPed* player = CurrentPlayer();
                ApplyLateProcessSecondArmPose(player);
                };
            Events::pedSetModelEvent += [](CPed* ped, int) {
                OnPedSetModel(ped);
                };
            Events::pedDtorEvent += [](CPed* ped) {
                OnPedDestroy(ped);
                };
            Events::restartGameEvent += [] {
                InvalidateStableNativeAimPose();
                gAimIntentHoldRemaining = 0;
                gAimIntentLatched = false;
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
            RestoreCallPatch(gAimPatch);
            RestoreCallPatch(gMoveClearAimPatch);
            RestoreCallPatch(gFirePatch);
        }
    };

    static Mod gMod;
}
