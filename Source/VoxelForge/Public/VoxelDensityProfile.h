// Opt-in density-path profiling used by the editor commandlet and targeted PIE diagnostics.
// Disabled by default so the shipping density path pays only one predictable branch in the
// scoped timers below.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

#include <atomic>

namespace VoxelDensityProfile
{
    enum class EMode : uint8
    {
        Disabled,
        Sampled,
        Full,
    };

    // Count-based sampling is deterministic with respect to the profiler's own counters, but the
    // report is intentionally diagnostic only.  It never participates in density or mesh output.
    constexpr uint32 DefaultSampleInterval = 64;

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
        TunnelSupportFloorBackstopFires,
        TunnelRoomFloorBackstopFires,
        TunnelFloorProfileBuilds,
        TunnelFloorProfileSegments,
        TunnelFloorProfileLedges,
        PassageCandidates,
        PassageEvaluated,
        MesherBlockTests,
        MesherBlockAllSolid,
        MesherBlockAllAir,
        Count
    };

    constexpr int32 CounterCount = static_cast<int32>(ECounter::Count);

    // Resident allocator-backed bytes for one worker's generation cache family.  The profiler
    // aggregates this by worker; the fields intentionally describe allocations owned by the
    // cache, not the inline size of the enclosing C++ object.  SlotStorageBytes is the one
    // exception: it is the inline/static storage reserved for the worker's fixed cache table.
    struct FCacheMemoryBreakdown
    {
        uint64 SlotStorageBytes = 0;
        uint64 OpStackBytes = 0;
        uint64 RoomsBytes = 0;
        uint64 RoomFloorJoinsBytes = 0;
        uint64 TunnelsBytes = 0;
        uint64 PitsBytes = 0;
        uint64 ChimneysBytes = 0;
        uint64 ColumnsBytes = 0;
        uint64 SupportColumnEntriesBytes = 0;
        uint64 SupportColumnsBytes = 0;
        uint64 SupportColumnIntervalsBytes = 0;
        uint64 TotalBytes() const
        {
            return SlotStorageBytes + OpStackBytes
                + RoomsBytes + RoomFloorJoinsBytes + TunnelsBytes + PitsBytes
                + ChimneysBytes + ColumnsBytes + SupportColumnEntriesBytes
                + SupportColumnsBytes + SupportColumnIntervalsBytes;
        }

        uint64 DynamicBytes() const
        {
            return TotalBytes() - SlotStorageBytes;
        }

        FCacheMemoryBreakdown& operator+=(const FCacheMemoryBreakdown& Other)
        {
            SlotStorageBytes += Other.SlotStorageBytes;
            OpStackBytes += Other.OpStackBytes;
            RoomsBytes += Other.RoomsBytes;
            RoomFloorJoinsBytes += Other.RoomFloorJoinsBytes;
            TunnelsBytes += Other.TunnelsBytes;
            PitsBytes += Other.PitsBytes;
            ChimneysBytes += Other.ChimneysBytes;
            ColumnsBytes += Other.ColumnsBytes;
            SupportColumnEntriesBytes += Other.SupportColumnEntriesBytes;
            SupportColumnsBytes += Other.SupportColumnsBytes;
            SupportColumnIntervalsBytes += Other.SupportColumnIntervalsBytes;
            return *this;
        }
    };

    struct FSnapshot
    {
        uint64 Calls[BucketCount]{};
        // Sum of sampled worker time. Useful for CPU-work accounting, but not directly comparable
        // with an export wall clock when the mesher runs in parallel.
        uint64 Cycles[BucketCount]{};
        uint64 Samples[BucketCount]{};
        // Largest estimated worker contribution. This is the report-facing parallel wall
        // estimate; it avoids mistaking several parallel workers for several exports.
        uint64 WallCycles[BucketCount]{};
        uint32 ActiveWorkers[BucketCount]{};
        uint64 Counters[CounterCount]{};

        uint64 TimerPairCycles = 0;
        uint32 SampleInterval = DefaultSampleInterval;
        EMode Mode = EMode::Disabled;

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
        FCacheMemoryBreakdown TunnelCacheBreakdown;

        uint64 RoomGraphCacheWorkers = 0;
        uint64 RoomGraphCacheCapacityEntries = 0;
        uint64 RoomGraphCacheValidEntries = 0;
        uint64 RoomGraphCacheStaticBytes = 0;
        uint64 RoomGraphCacheDynamicBytes = 0;
        uint64 RoomGraphCacheEntryBytes = 0;
        uint64 RoomGraphCacheLargestEntryBytes = 0;
        uint64 RoomGraphCacheLargestWorkerValidEntries = 0;
        uint64 RoomGraphCacheLargestWorkerBytes = 0;
        FCacheMemoryBreakdown RoomGraphCacheBreakdown;
    };

    VOXELFORGE_API void SetEnabled(bool bEnabled);
    VOXELFORGE_API void SetMode(EMode Mode, uint32 SampleInterval = DefaultSampleInterval);
    // Coarse counters are intentionally always collected, even when cycle timing is disabled.
    // They are the low-cost diagnostic path; only cycle timing is opt-in.
    FORCEINLINE bool AreCountersEnabled() { return true; }
    VOXELFORGE_API bool IsEnabled();
    VOXELFORGE_API bool IsCycleTimingEnabled();
    VOXELFORGE_API EMode GetMode();
    VOXELFORGE_API uint32 GetSampleInterval();
    VOXELFORGE_API uint64 GetTimerPairCycles();
    VOXELFORGE_API void CalibrateTimer();
    VOXELFORGE_API void Reset();
    VOXELFORGE_API FSnapshot Snapshot();
    VOXELFORGE_API void AddCounter(ECounter Counter, uint64 Amount = 1);
    VOXELFORGE_API void AddMeasurement(EBucket Bucket, uint64 Cycles, uint64 Calls = 1);
    VOXELFORGE_API void RecordCall(EBucket Bucket);
    VOXELFORGE_API bool ShouldSample(EBucket Bucket);
    VOXELFORGE_API void AddSampledMeasurement(EBucket Bucket, uint64 RawCycles);

    VOXELFORGE_API void SetWorkerTunnelCacheFootprint(
        uint64 CapacityEntries, uint64 ValidEntries,
        uint64 StaticBytes, uint64 DynamicBytes,
        uint64 EntryBytes, uint64 LargestEntryBytes,
        uint64 LargestWorkerValidEntries,
        const FCacheMemoryBreakdown& Breakdown);
    VOXELFORGE_API void SetWorkerRoomGraphCacheFootprint(
        uint64 CapacityEntries, uint64 ValidEntries,
        uint64 StaticBytes, uint64 DynamicBytes,
        uint64 EntryBytes, uint64 LargestEntryBytes,
        uint64 LargestWorkerValidEntries,
        const FCacheMemoryBreakdown& Breakdown);
    VOXELFORGE_API const TCHAR* CounterName(ECounter Counter);
    VOXELFORGE_API EBucket BucketFromName(const TCHAR* Name);
    VOXELFORGE_API const TCHAR* BucketName(EBucket Bucket);

    struct FScopeToken
    {
        bool bEntered = false;
        bool bSampled = false;
        bool bBlockTiming = false;
        bool bPreviousSampled = false;
        bool bPreviousSampledScopeActive = false;
        uint64 StartCycles = 0;
    };

    VOXELFORGE_API FScopeToken BeginScope(EBucket Bucket);
    VOXELFORGE_API void EndScope(EBucket Bucket, const FScopeToken& Token);

    class FScopedTimer final
    {
    public:
        explicit FScopedTimer(EBucket InBucket)
            : Bucket(InBucket)
            , Token(BeginScope(InBucket))
        {
        }

        explicit FScopedTimer(const TCHAR* InName)
            : Bucket(EBucket::OtherOp)
        {
            Bucket = BucketFromName(InName);
            Token = BeginScope(Bucket);
        }

        ~FScopedTimer();

        // End a range before its lexical scope ends.  This lets GetDensityAt split its enclosing
        // total into disjoint prologue/core/post phases without moving the large cache block.
        void End();

        FScopedTimer(const FScopedTimer&) = delete;
        FScopedTimer& operator=(const FScopedTimer&) = delete;

    private:
        EBucket Bucket = EBucket::OtherOp;
        FScopeToken Token;
    };
}
