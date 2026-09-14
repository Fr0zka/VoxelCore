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
        // Low-overhead attribution: keeps the parent and stage ledger trustworthy, samples
        // hot component scopes, and collects classifier detail without the old full-scope storm.
        Attribution,
        Full,
    };

    // Count-based sampling is deterministic with respect to the profiler's own counters, but the
    // report is intentionally diagnostic only.  It never participates in density or mesh output.
    constexpr uint32 DefaultSampleInterval = 128;

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
        // Attribution components.  These are nested diagnostics; the non-overlapping phase
        // buckets below remain the density accounting ledger.
        FusedEvaluator,
        InterpretedOpStack,
        OperatorBlock,
        RoomGraphBuild,
        RoomGraphSdf,
        TunnelCoreWorld,
        FusedDetail,
        FusedWorm,
        FusedStructural,
        ClassifierTotal,
        ClassifierIntervalProof,
        ClassifierExactCore,
        ClassifierExactFinal,
        ClassifierRoomTail,
        RuntimeStreamBuilding,
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
        RoomGraphBuildGeneratorTile,
        RoomGraphBuildGeneratorTunnelCore,
        RoomGraphBuildOpShared,
        RoomGraphBuildOpLocal,
        RoomGraphBuildClassifierShared,
        RoomGraphBuildClassifierLocal,
        RoomGraphBuildUnknown,
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
        TunnelAuthoredFloorSamples,
        TunnelAuthoredFloorProtectionClamps,
        TunnelAuthoredFloorDisturbanceSkips,
        TunnelFloorProfileFallbacks,
        TunnelFloorProfileBuilds,
        TunnelFloorProfileSegments,
        TunnelFloorProfileLedges,
        TunnelFloorProfileLedgesBelow2Voxels,
        TunnelFloorProfileLedges2To4Voxels,
        TunnelFloorProfileLedges4To8Voxels,
        TunnelFloorProfileLedges8PlusVoxels,
        PassageSupportFloorBackstopFires,
        PassageNativeFloorCompositions,
        PassageCandidates,
        PassageEvaluated,
        MesherBlockTests,
        MesherBlockAllSolid,
        MesherBlockAllAir,
        TileClassifyCalls,
        TileClassifyAllSolid,
        TileClassifyAllAir,
        TileClassifyMixed,
        FusedTunnelSamples,
        FusedTunnelDetailSamples,
        FusedTunnelDetailSkipped,
        WormEligibleSamples,
        WormBlockProofs,
        WormBlockSkippedSamples,
        OpBlockBuilds,
        OpBlockSamples,
        OpBlockOperators,
        OpBlockActiveOperators,
        OpBlockPrunedOperators,
        MesherDensityGridBytes,
        MesherSharedGridReadBytes,
        MesherOutputArrayBytes,
        MesherOutputAllocatedBytes,
        OpBlockScratchBytes,
        OpBlockCopyBytes,
        Count
    };

    constexpr int32 CounterCount = static_cast<int32>(ECounter::Count);

    // Export the one fast-path bit so callers in the editor module can skip instrumentation
    // without crossing the DLL boundary for every scoped timer or counter.  It is diagnostic
    // state only; it never participates in density or mesh decisions.
    VOXELFORGE_API extern std::atomic<bool> GEnabled;
    VOXELFORGE_API extern std::atomic<EMode> GMode;
    inline thread_local bool GSampledScopeActive = false;

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
        uint64 SpatialIndexBytes = 0;
        uint64 TotalBytes() const
        {
            return SlotStorageBytes + OpStackBytes
                + RoomsBytes + RoomFloorJoinsBytes + TunnelsBytes + PitsBytes
                + ChimneysBytes + ColumnsBytes + SupportColumnEntriesBytes
                + SupportColumnsBytes + SupportColumnIntervalsBytes + SpatialIndexBytes;
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
            SpatialIndexBytes += Other.SpatialIndexBytes;
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

        // Classifier detail is sampled separately from the timer buckets.  Calls is the exact
        // number of classifier invocations; the remaining fields are worker-local samples
        // expanded at Snapshot() using Calls / SampledCalls.
        struct FClassifierStats
        {
            uint64 Calls = 0;
            uint64 SampledCalls = 0;
            uint64 RefineNodes = 0;
            uint64 StackBoxCalls = 0;
            uint64 WholeMixedNodes = 0;
            uint64 WholeSolidNodes = 0;
            uint64 WholeAirNodes = 0;
            uint64 NeedsFinalFieldNodes = 0;
            uint64 SplitNodes = 0;
            uint64 MaxRefinementDepth = 0;
            uint64 ExactCoreSamples = 0;
            uint64 ExactFinalSamples = 0;
            uint64 ExactCoreCacheHits = 0;
            uint64 ExactFinalCacheHits = 0;
            uint64 ExactCoreLeaves = 0;
            uint64 ExactFinalLeaves = 0;
            uint64 StackBoxCycles = 0;
            uint64 ExactCoreCycles = 0;
            uint64 ExactFinalCycles = 0;
            uint64 RoomTailQueries = 0;
            uint64 RoomTailEvaluated = 0;
            uint64 RoomTailCycles = 0;
            uint64 RoomPropagateCycles = 0;
            uint64 RoomExactPrimitiveCycles = 0;
            uint64 RoomCacheWindowCycles = 0;
            uint64 RoomNumRooms = 0;
            uint64 RoomNumTunnels = 0;
            uint64 RoomNumRoomFloorJoins = 0;
            uint64 RoomNumPits = 0;
            uint64 RoomNumChimneys = 0;
        };
        FClassifierStats Classifier;

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
    // Counters are collected only while a diagnostic mode is enabled.  In the ordinary clean
    // path this is a single relaxed load, so the measurement hooks stay dormant.
    FORCEINLINE bool IsEnabledFast()
    {
        return GEnabled.load(std::memory_order_relaxed);
    }
    FORCEINLINE bool AreCountersEnabled() { return IsEnabledFast(); }
    FORCEINLINE bool ShouldProbeScopeFast(EBucket Bucket)
    {
        const EMode Mode = GMode.load(std::memory_order_relaxed);
        if (Mode == EMode::Full)
        {
            return true;
        }
        if (Mode == EMode::Sampled)
        {
            return Bucket == EBucket::GetDensityAt
                || Bucket == EBucket::MesherGenerateMesh
                || Bucket == EBucket::MesherDensityGrid
                || Bucket == EBucket::MesherOther;
        }
        if (Mode == EMode::Attribution)
        {
            switch (Bucket)
            {
            case EBucket::GetDensityAt:
            case EBucket::MesherGenerateMesh:
            case EBucket::MesherDensityGrid:
            case EBucket::MesherCellClassification:
            case EBucket::MesherGradientNormals:
            case EBucket::MesherVertexInterpolation:
            case EBucket::MesherStreamBuilding:
            case EBucket::MesherOther:
            case EBucket::RoomGraphBuild:
                return true;
            default:
                return GSampledScopeActive;
            }
        }
        return false;
    }
    VOXELFORGE_API bool IsEnabled();
    VOXELFORGE_API bool IsCycleTimingEnabled();
    VOXELFORGE_API EMode GetMode();
    VOXELFORGE_API uint32 GetSampleInterval();
    VOXELFORGE_API uint64 GetTimerPairCycles();
    VOXELFORGE_API void CalibrateTimer();
    VOXELFORGE_API void Reset();
    VOXELFORGE_API FSnapshot Snapshot();
    // Worker-local snapshot for a tile profile.  GenerateTileResult is single-owner on its worker,
    // so this avoids attributing another worker's concurrent tile to the current tile diagnostic.
    VOXELFORGE_API FSnapshot SnapshotCurrentThread();
    VOXELFORGE_API void AddCounter(ECounter Counter, uint64 Amount = 1);
    VOXELFORGE_API void AddMeasurement(EBucket Bucket, uint64 Cycles, uint64 Calls = 1);
    VOXELFORGE_API void RecordCall(EBucket Bucket);
    VOXELFORGE_API void RecordClassifierCall(bool bSampled);
    VOXELFORGE_API void AddClassifierStats(const FSnapshot::FClassifierStats& Stats);
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
        bool bEntered;
        bool bSampled;
        bool bBlockTiming;
        bool bPreviousSampled;
        bool bPreviousSampledScopeActive;
        uint64 StartCycles;
    };

    VOXELFORGE_API FScopeToken BeginScope(EBucket Bucket);
    VOXELFORGE_API void EndScope(EBucket Bucket, const FScopeToken& Token);

    class FScopedTimer final
    {
    public:
        explicit FScopedTimer(EBucket InBucket)
        {
            if (IsEnabledFast() && ShouldProbeScopeFast(InBucket))
            {
                Bucket = InBucket;
                Token = BeginScope(InBucket);
                bActive = Token.bEntered;
            }
        }

        explicit FScopedTimer(const TCHAR* InName)
        {
            if (IsEnabledFast())
            {
                Bucket = BucketFromName(InName);
                if (ShouldProbeScopeFast(Bucket))
                {
                    Token = BeginScope(Bucket);
                    bActive = Token.bEntered;
                }
            }
        }

        FORCEINLINE ~FScopedTimer()
        {
            End();
        }

        // End a range before its lexical scope ends.  This lets GetDensityAt split its enclosing
        // total into disjoint prologue/core/post phases without moving the large cache block.
        FORCEINLINE void End()
        {
            if (!bActive)
            {
                return;
            }
            EndScope(Bucket, Token);
            Token.bEntered = false;
            bActive = false;
        }

        FScopedTimer(const FScopedTimer&) = delete;
        FScopedTimer& operator=(const FScopedTimer&) = delete;

    private:
        EBucket Bucket;
        FScopeToken Token;
        bool bActive = false;
    };
}
