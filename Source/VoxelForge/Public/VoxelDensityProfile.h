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
        // Non-overlapping top-level phases.  The named operation buckets above are intentionally
        // nested diagnostics; these phase buckets are the accounting ledger that must add up to
        // GetDensityAt without double-counting a stack op inside its enclosing phase.
        DensityPrologue,
        DensityCore,
        DensityDisturbances,
        DensityStructuralPosts,
        DensityBoundarySeal,
        DensityDiffLayer,
        DensityTail,
        // Mesher-side accounting.  GenerateMesh is the inclusive total; the following buckets
        // cover its regular-grid work and its non-grid setup/finalisation.
        MesherGenerateMesh,
        MesherDensityGrid,
        MesherCellClassification,
        MesherGradientNormals,
        MesherVertexInterpolation,
        MesherStreamBuilding,
        MesherOther,
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
        TunnelSupportColumnBuilds,
        TunnelSupportColumnCandidates,
        TunnelSupportFloorQueries,
        TunnelSupportFloorChecks,
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

        // Opt-in worker-cache footprint snapshot.  These are current per-thread values (not
        // allocation-event counters), so Snapshot() reports the resident cache footprint at the
        // end of a profiling run rather than multiplying bytes by cache churn.
        uint64 TunnelCacheWorkers = 0;
        uint64 TunnelCacheCapacityEntries = 0;
        uint64 TunnelCacheValidEntries = 0;
        uint64 TunnelCacheStaticBytes = 0;
        uint64 TunnelCacheDynamicBytes = 0;
        uint64 TunnelCacheEntryBytes = 0;
        uint64 TunnelCacheLargestEntryBytes = 0;
        uint64 TunnelCacheLargestWorkerValidEntries = 0;
        uint64 TunnelCacheLargestWorkerBytes = 0;

        uint64 RoomGraphCacheWorkers = 0;
        uint64 RoomGraphCacheCapacityEntries = 0;
        uint64 RoomGraphCacheValidEntries = 0;
        uint64 RoomGraphCacheStaticBytes = 0;
        uint64 RoomGraphCacheDynamicBytes = 0;
        uint64 RoomGraphCacheEntryBytes = 0;
        uint64 RoomGraphCacheLargestEntryBytes = 0;
        uint64 RoomGraphCacheLargestWorkerValidEntries = 0;
        uint64 RoomGraphCacheLargestWorkerBytes = 0;
    };

    VOXELFORGE_API void SetEnabled(bool bEnabled);
    VOXELFORGE_API bool IsEnabled();
    VOXELFORGE_API void Reset();
    VOXELFORGE_API FSnapshot Snapshot();
    VOXELFORGE_API void AddCounter(ECounter Counter, uint64 Amount = 1);
    VOXELFORGE_API void AddMeasurement(EBucket Bucket, uint64 Cycles, uint64 Calls = 1);

    VOXELFORGE_API void SetWorkerTunnelCacheFootprint(
        uint64 CapacityEntries, uint64 ValidEntries,
        uint64 StaticBytes, uint64 DynamicBytes,
        uint64 EntryBytes, uint64 LargestEntryBytes,
        uint64 LargestWorkerValidEntries);
    VOXELFORGE_API void SetWorkerRoomGraphCacheFootprint(
        uint64 CapacityEntries, uint64 ValidEntries,
        uint64 StaticBytes, uint64 DynamicBytes,
        uint64 EntryBytes, uint64 LargestEntryBytes,
        uint64 LargestWorkerValidEntries);
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

        // End a range before its lexical scope ends.  This lets GetDensityAt split its enclosing
        // total into disjoint prologue/core/post phases without moving the large cache block.
        void End();

        FScopedTimer(const FScopedTimer&) = delete;
        FScopedTimer& operator=(const FScopedTimer&) = delete;

    private:
        EBucket Bucket = EBucket::OtherOp;
        bool bActive = false;
        uint64 StartCycles = 0;
    };
}
