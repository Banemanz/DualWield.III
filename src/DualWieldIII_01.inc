/*
    DualWieldIII
    Dual-wield backport for classic GTA III 1.0 EN.

    The second weapon is a standalone RpAtomic kept outside the player ped clump.
    Stock PC RwFrame and Skin & Bones/Xbox HAnim skeletons use separate pose paths.
    Colt .45/Uzi second shots use GTA III's native CWeapon::Fire path.
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
    static int gAimGraceFramesRemaining = 0;
    static HMODULE gSkinBonesModule = 0;
    static SkinBonesGetPedWeaponAtomicFn gSkinBonesGetPedWeaponAtomic = 0;
    static bool gSkinBonesProbeDone = false;
    static bool gLoggedSkinBonesDetected = false;
    static bool gLoggedRenderRechain = false;
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
        // The atomic and helper frame are owned independently by this ASI. The atomic is deliberately NOT
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
