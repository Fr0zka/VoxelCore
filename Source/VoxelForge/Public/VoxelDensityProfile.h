// Opt-in density-path profiling used by the editor commandlet and targeted PIE diagnostics.
// Disabled by default so the shipping density path pays only one predictable branch in the
// scoped timers below.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

#include <atomic>

namespace VoxelDensityProfile
{
    enum class EBucket : uint8
    {
        GetDensityAt,
        ApplyDisturbances,
        PassageModifier,
        PassageLandingAir,
        PassageLandingFloor,
        PassageLandingRoomFloor,
        PassageTunnelAir,
        PassageStructuralPosts,
        TunnelCoreSupport,
        TunnelCoreSdf,
        TunnelCorePosts,
        StructuralTail,
        RoomGraphSource,
        SdfCarve,
        CaveRoughnessMod,
        CaveTerraceMod,
        LayerLineMod,
        RibbingMod,
        CaveOverhangMod,
        CaveCliffMod,
        ScallopMod,
        CaveArchMod,
        RoomColumnMod,
        DomeMod,
        PinchMod,
        FloorBiasMod,
        WormFieldSource,
        OriginSpineOp,
        BoundarySealOp,
        PassageCarveOp,
        CaveTunnelFloorOp,
        CaveTunnelAirOp,
        XYEdgeSealOp,
        OtherOp,
        Count
    };

    constexpr int32 BucketCount = static_cast<int32>(EBucket::Count);

    enum class ECounter : uint8
    {
        TunnelCacheLookup,
        TunnelCacheHit,
        TunnelCacheMiss,
        SdfCacheBuild,
        CaveRoomCandidates,
        CaveRoomEvaluated,
        CaveTunnelCandidates,
        CaveTunnelEvaluated,
        TunnelCoreCandidates,
        TunnelCoreEvaluated,
        PassageCandidates,
        PassageEvaluated,
        Count
    };

    constexpr int32 CounterCount = static_cast<int32>(ECounter::Count);

    struct FSnapshot
    {
        uint64 Calls[BucketCount]{};
        uint64 Cycles[BucketCount]{};
        uint64 Counters[CounterCount]{};
    };

    VOXELFORGE_API void SetEnabled(bool bEnabled);
    VOXELFORGE_API bool IsEnabled();
    VOXELFORGE_API void Reset();
    VOXELFORGE_API FSnapshot Snapshot();
    VOXELFORGE_API void AddCounter(ECounter Counter, uint64 Amount = 1);
    VOXELFORGE_API const TCHAR* CounterName(ECounter Counter);
    VOXELFORGE_API EBucket BucketFromName(const TCHAR* Name);
    VOXELFORGE_API const TCHAR* BucketName(EBucket Bucket);

    class FScopedTimer final
    {
    public:
        explicit FScopedTimer(EBucket InBucket)
            : Bucket(InBucket)
            , bActive(IsEnabled())
            , StartCycles(bActive ? FPlatformTime::Cycles64() : 0)
        {
        }

        explicit FScopedTimer(const TCHAR* InName)
            : Bucket(EBucket::OtherOp)
            , bActive(IsEnabled())
            , StartCycles(0)
        {
            if (bActive)
            {
                Bucket = BucketFromName(InName);
                StartCycles = FPlatformTime::Cycles64();
            }
        }

        ~FScopedTimer();

        FScopedTimer(const FScopedTimer&) = delete;
        FScopedTimer& operator=(const FScopedTimer&) = delete;

    private:
        EBucket Bucket = EBucket::OtherOp;
        bool bActive = false;
        uint64 StartCycles = 0;
    };
}
