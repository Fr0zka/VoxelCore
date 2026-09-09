#include "VoxelDensityProfile.h"

namespace VoxelDensityProfile
{
    namespace
    {
        std::atomic<bool> GEnabled(false);
        std::atomic<EMode> GMode(EMode::Disabled);
        std::atomic<uint32> GSampleInterval(DefaultSampleInterval);
        std::atomic<uint64> GTimerPairCycles(0);

        // The old implementation performed a contended atomic fetch_add for every timer and
        // counter.  A TunnelNetwork sample can close twenty-plus ranges and add several
        // candidate counters, so profiling changed the workload it was supposed to measure.
        // Keep one accumulator per worker and only aggregate at Snapshot().  Commandlet resets
        // happen between completed jobs, so the deliberately light-weight list is sufficient.
        struct FThreadState
        {
            uint64 Calls[BucketCount]{};
            uint64 Cycles[BucketCount]{};
            uint64 Samples[BucketCount]{};
            uint64 Counters[CounterCount]{};
            uint32 SampleSequence[BucketCount]{};
            uint32 BlockCallCount[BucketCount]{};
            uint64 BlockStartCycles[BucketCount]{};
            bool BlockActive[BucketCount]{};

            uint64 TunnelCacheCapacityEntries = 0;
            uint64 TunnelCacheValidEntries = 0;
            uint64 TunnelCacheStaticBytes = 0;
            uint64 TunnelCacheDynamicBytes = 0;
            uint64 TunnelCacheEntryBytes = 0;
            uint64 TunnelCacheLargestEntryBytes = 0;
            uint64 TunnelCacheLargestWorkerValidEntries = 0;
            FCacheMemoryBreakdown TunnelCacheBreakdown;

            uint64 RoomGraphCacheCapacityEntries = 0;
            uint64 RoomGraphCacheValidEntries = 0;
            uint64 RoomGraphCacheStaticBytes = 0;
            uint64 RoomGraphCacheDynamicBytes = 0;
            uint64 RoomGraphCacheEntryBytes = 0;
            uint64 RoomGraphCacheLargestEntryBytes = 0;
            uint64 RoomGraphCacheLargestWorkerValidEntries = 0;
            FCacheMemoryBreakdown RoomGraphCacheBreakdown;

            std::atomic<FThreadState*> Next{nullptr};
        };

        std::atomic<FThreadState*> GThreadStates{nullptr};
        thread_local FThreadState* GThreadState = nullptr;
        thread_local uint32 GScopeDepth = 0;
        thread_local bool GScopeSampled = false;
        thread_local bool GSampledScopeActive = false;

        FThreadState& GetThreadState()
        {
            if (GThreadState == nullptr)
            {
                GThreadState = new FThreadState();
                FThreadState* Head = GThreadStates.load(std::memory_order_relaxed);
                do
                {
                    GThreadState->Next.store(Head, std::memory_order_relaxed);
                }
                while (!GThreadStates.compare_exchange_weak(
                    Head, GThreadState,
                    std::memory_order_release,
                    std::memory_order_relaxed));
            }
            return *GThreadState;
        }

        int32 ToIndex(EBucket Bucket)
        {
            const int32 Index = static_cast<int32>(Bucket);
            return (Index >= 0 && Index < BucketCount)
                ? Index
                : static_cast<int32>(EBucket::OtherOp);
        }

        bool IsSampledScopeEnabled(EBucket Bucket)
        {
            // Sampled mode pays for the trustworthy parent and mesh containers only.  Fine
            // operation scopes remain available in Full mode, or through UE Insights scopes,
            // without putting a Begin/End and thread-local bookkeeping on every density call.
            switch (Bucket)
            {
            case EBucket::GetDensityAt:
            case EBucket::MesherGenerateMesh:
            case EBucket::MesherDensityGrid:
            case EBucket::MesherOther:
                return true;
            default:
                return false;
            }
        }
    }

    void SetEnabled(bool bEnabled)
    {
        SetMode(bEnabled ? EMode::Sampled : EMode::Disabled, DefaultSampleInterval);
    }

    void SetMode(EMode Mode, uint32 SampleInterval)
    {
        if (SampleInterval == 0)
        {
            SampleInterval = DefaultSampleInterval;
        }
        GSampleInterval.store(SampleInterval, std::memory_order_relaxed);
        GMode.store(Mode, std::memory_order_relaxed);
        GEnabled.store(Mode != EMode::Disabled, std::memory_order_relaxed);
        if (Mode != EMode::Disabled && GTimerPairCycles.load(std::memory_order_relaxed) == 0)
        {
            CalibrateTimer();
        }
    }

    bool IsEnabled()
    {
        return GEnabled.load(std::memory_order_relaxed);
    }

    bool IsCycleTimingEnabled()
    {
        return GMode.load(std::memory_order_relaxed) != EMode::Disabled;
    }

    EMode GetMode()
    {
        return GMode.load(std::memory_order_relaxed);
    }

    uint32 GetSampleInterval()
    {
        return GSampleInterval.load(std::memory_order_relaxed);
    }

    uint64 GetTimerPairCycles()
    {
        return GTimerPairCycles.load(std::memory_order_relaxed);
    }

    void CalibrateTimer()
    {
        constexpr uint32 Iterations = 100000;
        volatile uint64 Sink = 0;
        const uint64 Start = FPlatformTime::Cycles64();
        for (uint32 Index = 0; Index < Iterations; ++Index)
        {
            const uint64 A = FPlatformTime::Cycles64();
            const uint64 B = FPlatformTime::Cycles64();
            Sink ^= (B - A);
        }
        const uint64 End = FPlatformTime::Cycles64();
        const uint64 Total = End - Start;
        const uint64 PairCycles = Total > 0
            ? FMath::Max<uint64>(1, Total / Iterations)
            : 1;
        GTimerPairCycles.store(PairCycles, std::memory_order_relaxed);
        (void)Sink;
    }

    void Reset()
    {
        for (FThreadState* State = GThreadStates.load(std::memory_order_acquire);
             State != nullptr;
             State = State->Next.load(std::memory_order_acquire))
        {
            for (int32 Index = 0; Index < BucketCount; ++Index)
            {
                State->Calls[Index] = 0;
                State->Cycles[Index] = 0;
                State->Samples[Index] = 0;
            }
            for (int32 Index = 0; Index < CounterCount; ++Index)
            {
                State->Counters[Index] = 0;
            }
            for (int32 Index = 0; Index < BucketCount; ++Index)
            {
                State->SampleSequence[Index] = 0;
                State->BlockCallCount[Index] = 0;
                State->BlockStartCycles[Index] = 0;
                State->BlockActive[Index] = false;
            }
            State->TunnelCacheCapacityEntries = 0;
            State->TunnelCacheValidEntries = 0;
            State->TunnelCacheStaticBytes = 0;
            State->TunnelCacheDynamicBytes = 0;
            State->TunnelCacheEntryBytes = 0;
            State->TunnelCacheLargestEntryBytes = 0;
            State->TunnelCacheBreakdown = FCacheMemoryBreakdown();
            State->RoomGraphCacheCapacityEntries = 0;
            State->RoomGraphCacheValidEntries = 0;
            State->RoomGraphCacheStaticBytes = 0;
            State->RoomGraphCacheDynamicBytes = 0;
            State->RoomGraphCacheEntryBytes = 0;
            State->RoomGraphCacheLargestEntryBytes = 0;
            State->RoomGraphCacheBreakdown = FCacheMemoryBreakdown();
        }
        GScopeDepth = 0;
        GScopeSampled = false;
        GSampledScopeActive = false;
    }

    FSnapshot Snapshot()
    {
        FSnapshot Result;
        for (FThreadState* State = GThreadStates.load(std::memory_order_acquire);
             State != nullptr;
             State = State->Next.load(std::memory_order_acquire))
        {
            for (int32 Index = 0; Index < BucketCount; ++Index)
            {
                Result.Calls[Index] += State->Calls[Index];
                Result.Samples[Index] += State->Samples[Index];
                if (State->Samples[Index] == 0)
                {
                    Result.Cycles[Index] += State->Cycles[Index];
                }
                else
                {
                    // Store raw sampled cycles in the worker-local state.  Estimate the bucket
                    // from its observed call/sample ratio at aggregation time.  This matters for
                    // small buckets: multiplying every sample by 64 would overstate a bucket with
                    // only eight calls where all eight were sampled.
                    const double Scale = static_cast<double>(State->Calls[Index])
                        / static_cast<double>(State->Samples[Index]);
                    const double EstimatedCycles = static_cast<double>(State->Cycles[Index]) * Scale;
                    Result.Cycles[Index] += EstimatedCycles > static_cast<double>(MAX_uint64)
                        ? MAX_uint64
                        : static_cast<uint64>(EstimatedCycles + 0.5);
                }

                if (State->Calls[Index] > 0)
                {
                    ++Result.ActiveWorkers[Index];
                    const double EstimatedWorkerCycles = State->Samples[Index] == 0
                        ? static_cast<double>(State->Cycles[Index])
                        : static_cast<double>(State->Cycles[Index])
                            * static_cast<double>(State->Calls[Index])
                            / static_cast<double>(State->Samples[Index]);
                    Result.WallCycles[Index] = FMath::Max(
                        Result.WallCycles[Index],
                        EstimatedWorkerCycles > static_cast<double>(MAX_uint64)
                            ? MAX_uint64
                            : static_cast<uint64>(EstimatedWorkerCycles + 0.5));
                }
            }
            for (int32 Index = 0; Index < CounterCount; ++Index)
            {
                Result.Counters[Index] += State->Counters[Index];
            }

            const uint64 TunnelWorkerBytes = State->TunnelCacheStaticBytes
                + State->TunnelCacheDynamicBytes;
            if (State->TunnelCacheCapacityEntries > 0)
            {
                ++Result.TunnelCacheWorkers;
                Result.TunnelCacheCapacityEntries += State->TunnelCacheCapacityEntries;
                Result.TunnelCacheValidEntries += State->TunnelCacheValidEntries;
                Result.TunnelCacheStaticBytes += State->TunnelCacheStaticBytes;
                Result.TunnelCacheDynamicBytes += State->TunnelCacheDynamicBytes;
                Result.TunnelCacheEntryBytes += State->TunnelCacheEntryBytes;
                Result.TunnelCacheLargestEntryBytes = FMath::Max(
                    Result.TunnelCacheLargestEntryBytes,
                    State->TunnelCacheLargestEntryBytes);
                Result.TunnelCacheLargestWorkerValidEntries = FMath::Max(
                    Result.TunnelCacheLargestWorkerValidEntries,
                    State->TunnelCacheValidEntries);
                Result.TunnelCacheLargestWorkerBytes = FMath::Max(
                    Result.TunnelCacheLargestWorkerBytes, TunnelWorkerBytes);
                Result.TunnelCacheBreakdown += State->TunnelCacheBreakdown;
            }

            const uint64 RoomGraphWorkerBytes = State->RoomGraphCacheStaticBytes
                + State->RoomGraphCacheDynamicBytes;
            if (State->RoomGraphCacheCapacityEntries > 0)
            {
                ++Result.RoomGraphCacheWorkers;
                Result.RoomGraphCacheCapacityEntries += State->RoomGraphCacheCapacityEntries;
                Result.RoomGraphCacheValidEntries += State->RoomGraphCacheValidEntries;
                Result.RoomGraphCacheStaticBytes += State->RoomGraphCacheStaticBytes;
                Result.RoomGraphCacheDynamicBytes += State->RoomGraphCacheDynamicBytes;
                Result.RoomGraphCacheEntryBytes += State->RoomGraphCacheEntryBytes;
                Result.RoomGraphCacheLargestEntryBytes = FMath::Max(
                    Result.RoomGraphCacheLargestEntryBytes,
                    State->RoomGraphCacheLargestEntryBytes);
                Result.RoomGraphCacheLargestWorkerValidEntries = FMath::Max(
                    Result.RoomGraphCacheLargestWorkerValidEntries,
                    State->RoomGraphCacheValidEntries);
                Result.RoomGraphCacheLargestWorkerBytes = FMath::Max(
                    Result.RoomGraphCacheLargestWorkerBytes, RoomGraphWorkerBytes);
                Result.RoomGraphCacheBreakdown += State->RoomGraphCacheBreakdown;
            }
        }
        Result.TimerPairCycles = GetTimerPairCycles();
        Result.SampleInterval = GetSampleInterval();
        Result.Mode = GetMode();
        return Result;
    }

    void AddCounter(ECounter Counter, uint64 Amount)
    {
        const int32 Index = static_cast<int32>(Counter);
        if (Index >= 0 && Index < CounterCount)
        {
            GetThreadState().Counters[Index] += Amount;
        }
    }

    void AddMeasurement(EBucket Bucket, uint64 Cycles, uint64 Calls)
    {
        const int32 Index = ToIndex(Bucket);
        FThreadState& State = GetThreadState();
        State.Calls[Index] += Calls;
        State.Cycles[Index] += Cycles;
    }

    void RecordCall(EBucket Bucket)
    {
        const int32 Index = ToIndex(Bucket);
        GetThreadState().Calls[Index] += 1;
    }

    bool ShouldSample(EBucket Bucket)
    {
        if (!IsCycleTimingEnabled())
        {
            return false;
        }
        if (GetMode() == EMode::Full)
        {
            return true;
        }

        FThreadState& State = GetThreadState();
        const int32 Index = ToIndex(Bucket);
        const uint32 Sequence = State.SampleSequence[Index]++;
        const uint32 Interval = GetSampleInterval();
        if ((Interval & (Interval - 1u)) == 0u)
        {
            return (Sequence & (Interval - 1u)) == 0u;
        }
        return (Sequence % Interval) == 0u;
    }

    void AddSampledMeasurement(EBucket Bucket, uint64 RawCycles)
    {
        const int32 Index = ToIndex(Bucket);
        FThreadState& State = GetThreadState();
        const uint64 TimerCost = GetTimerPairCycles();
        const uint64 AdjustedCycles = RawCycles > TimerCost
            ? RawCycles - TimerCost
            : 0;
        State.Cycles[Index] += AdjustedCycles;
        State.Samples[Index] += 1;
    }

    void SetWorkerTunnelCacheFootprint(
        uint64 CapacityEntries, uint64 ValidEntries,
        uint64 StaticBytes, uint64 DynamicBytes,
        uint64 EntryBytes, uint64 LargestEntryBytes,
        uint64 LargestWorkerValidEntries,
        const FCacheMemoryBreakdown& Breakdown)
    {
        if (!IsEnabled()) { return; }
        FThreadState& State = GetThreadState();
        State.TunnelCacheCapacityEntries = CapacityEntries;
        State.TunnelCacheValidEntries = ValidEntries;
        State.TunnelCacheStaticBytes = StaticBytes;
        State.TunnelCacheDynamicBytes = DynamicBytes;
        State.TunnelCacheEntryBytes = EntryBytes;
        State.TunnelCacheLargestEntryBytes = LargestEntryBytes;
        State.TunnelCacheLargestWorkerValidEntries = LargestWorkerValidEntries;
        State.TunnelCacheBreakdown = Breakdown;
    }

    void SetWorkerRoomGraphCacheFootprint(
        uint64 CapacityEntries, uint64 ValidEntries,
        uint64 StaticBytes, uint64 DynamicBytes,
        uint64 EntryBytes, uint64 LargestEntryBytes,
        uint64 LargestWorkerValidEntries,
        const FCacheMemoryBreakdown& Breakdown)
    {
        if (!IsEnabled()) { return; }
        FThreadState& State = GetThreadState();
        State.RoomGraphCacheCapacityEntries = CapacityEntries;
        State.RoomGraphCacheValidEntries = ValidEntries;
        State.RoomGraphCacheStaticBytes = StaticBytes;
        State.RoomGraphCacheDynamicBytes = DynamicBytes;
        State.RoomGraphCacheEntryBytes = EntryBytes;
        State.RoomGraphCacheLargestEntryBytes = LargestEntryBytes;
        State.RoomGraphCacheLargestWorkerValidEntries = LargestWorkerValidEntries;
        State.RoomGraphCacheBreakdown = Breakdown;
    }

    const TCHAR* CounterName(ECounter Counter)
    {
        switch (Counter)
        {
        case ECounter::TunnelCacheLookup: return TEXT("TunnelCacheLookup");
        case ECounter::TunnelCacheHit:    return TEXT("TunnelCacheHit");
        case ECounter::TunnelCacheMiss:   return TEXT("TunnelCacheMiss");
        case ECounter::SdfCacheBuild:     return TEXT("SdfCacheBuild");
        case ECounter::CaveRoomCandidates:return TEXT("CaveRoomCandidates");
        case ECounter::CaveRoomEvaluated: return TEXT("CaveRoomEvaluated");
        case ECounter::CaveTunnelCandidates:return TEXT("CaveTunnelCandidates");
        case ECounter::CaveTunnelEvaluated:return TEXT("CaveTunnelEvaluated");
        case ECounter::TunnelCoreCandidates:return TEXT("TunnelCoreCandidates");
        case ECounter::TunnelCoreEvaluated:return TEXT("TunnelCoreEvaluated");
        case ECounter::TunnelSupportColumnBuilds:return TEXT("TunnelSupportColumnBuilds");
        case ECounter::TunnelSupportColumnCandidates:return TEXT("TunnelSupportColumnCandidates");
        case ECounter::TunnelSupportFloorQueries:return TEXT("TunnelSupportFloorQueries");
        case ECounter::TunnelSupportFloorChecks:return TEXT("TunnelSupportFloorChecks");
        case ECounter::PassageCandidates: return TEXT("PassageCandidates");
        case ECounter::PassageEvaluated:  return TEXT("PassageEvaluated");
        case ECounter::Count:              break;
        }
        return TEXT("Unknown");
    }

    EBucket BucketFromName(const TCHAR* Name)
    {
        if (Name == nullptr) { return EBucket::OtherOp; }

        struct FNameBucket
        {
            const TCHAR* Name;
            EBucket Bucket;
        };
        static const FNameBucket Names[] =
        {
            { TEXT("ApplyDisturbances"), EBucket::ApplyDisturbances },
            { TEXT("PassageModifier"), EBucket::PassageModifier },
            { TEXT("PassageLandingAir"), EBucket::PassageLandingAir },
            { TEXT("PassageLandingFloor"), EBucket::PassageLandingFloor },
            { TEXT("PassageLandingRoomFloor"), EBucket::PassageLandingRoomFloor },
            { TEXT("PassageTunnelAir"), EBucket::PassageTunnelAir },
            { TEXT("PassageStructuralPosts"), EBucket::PassageStructuralPosts },
            { TEXT("TunnelCoreSupport"), EBucket::TunnelCoreSupport },
            { TEXT("TunnelCoreSdf"), EBucket::TunnelCoreSdf },
            { TEXT("TunnelCorePosts"), EBucket::TunnelCorePosts },
            { TEXT("StructuralTail"), EBucket::StructuralTail },
            { TEXT("RoomGraphSource"),  EBucket::RoomGraphSource },
            { TEXT("SdfCarve"),         EBucket::SdfCarve },
            { TEXT("SdfConvertOp"),     EBucket::SdfCarve },
            { TEXT("CaveRoughnessMod"), EBucket::CaveRoughnessMod },
            { TEXT("CaveTerraceMod"),   EBucket::CaveTerraceMod },
            { TEXT("LayerLineMod"),     EBucket::LayerLineMod },
            { TEXT("RibbingMod"),       EBucket::RibbingMod },
            { TEXT("CaveOverhangMod"),  EBucket::CaveOverhangMod },
            { TEXT("CaveCliffMod"),     EBucket::CaveCliffMod },
            { TEXT("ScallopMod"),        EBucket::ScallopMod },
            { TEXT("CaveArchMod"),       EBucket::CaveArchMod },
            { TEXT("RoomColumnMod"),     EBucket::RoomColumnMod },
            { TEXT("DomeMod"),           EBucket::DomeMod },
            { TEXT("PinchMod"),          EBucket::PinchMod },
            { TEXT("FloorBiasMod"),      EBucket::FloorBiasMod },
            { TEXT("WormFieldSource"),   EBucket::WormFieldSource },
            { TEXT("OriginSpineOp"),     EBucket::OriginSpineOp },
            { TEXT("BoundarySealOp"),    EBucket::BoundarySealOp },
            { TEXT("PassageCarveOp"),    EBucket::PassageCarveOp },
            { TEXT("CaveTunnelFloorOp"), EBucket::CaveTunnelFloorOp },
            { TEXT("CaveTunnelAirOp"),   EBucket::CaveTunnelAirOp },
            { TEXT("XYEdgeSealOp"),      EBucket::XYEdgeSealOp },
            { TEXT("DensityPrologue"),   EBucket::DensityPrologue },
            { TEXT("DensityCore"),       EBucket::DensityCore },
            { TEXT("DensityDisturbances"), EBucket::DensityDisturbances },
            { TEXT("DensityStructuralPosts"), EBucket::DensityStructuralPosts },
            { TEXT("DensityBoundarySeal"), EBucket::DensityBoundarySeal },
            { TEXT("DensityDiffLayer"),  EBucket::DensityDiffLayer },
            { TEXT("DensityTail"),       EBucket::DensityTail },
            { TEXT("MesherGenerateMesh"), EBucket::MesherGenerateMesh },
            { TEXT("MesherDensityGrid"), EBucket::MesherDensityGrid },
            { TEXT("MesherCellClassification"), EBucket::MesherCellClassification },
            { TEXT("MesherGradientNormals"), EBucket::MesherGradientNormals },
            { TEXT("MesherVertexInterpolation"), EBucket::MesherVertexInterpolation },
            { TEXT("MesherStreamBuilding"), EBucket::MesherStreamBuilding },
            { TEXT("MesherOther"),       EBucket::MesherOther },
        };
        for (const FNameBucket& Entry : Names)
        {
            if (FCString::Strcmp(Name, Entry.Name) == 0)
            {
                return Entry.Bucket;
            }
        }
        return EBucket::OtherOp;
    }

    const TCHAR* BucketName(EBucket Bucket)
    {
        switch (Bucket)
        {
        case EBucket::GetDensityAt:      return TEXT("GetDensityAt");
        case EBucket::ApplyDisturbances:return TEXT("ApplyDisturbances");
        case EBucket::PassageModifier:  return TEXT("PassageModifier");
        case EBucket::PassageLandingAir:return TEXT("PassageLandingAir");
        case EBucket::PassageLandingFloor:return TEXT("PassageLandingFloor");
        case EBucket::PassageLandingRoomFloor:return TEXT("PassageLandingRoomFloor");
        case EBucket::PassageTunnelAir:return TEXT("PassageTunnelAir");
        case EBucket::PassageStructuralPosts:return TEXT("PassageStructuralPosts");
        case EBucket::TunnelCoreSupport:return TEXT("TunnelCoreSupport");
        case EBucket::TunnelCoreSdf:    return TEXT("TunnelCoreSdf");
        case EBucket::TunnelCorePosts:  return TEXT("TunnelCorePosts");
        case EBucket::StructuralTail:   return TEXT("StructuralTail");
        case EBucket::RoomGraphSource:  return TEXT("RoomGraphSource");
        case EBucket::SdfCarve:         return TEXT("SdfCarve");
        case EBucket::CaveRoughnessMod: return TEXT("CaveRoughnessMod");
        case EBucket::CaveTerraceMod:   return TEXT("CaveTerraceMod");
        case EBucket::LayerLineMod:     return TEXT("LayerLineMod");
        case EBucket::RibbingMod:       return TEXT("RibbingMod");
        case EBucket::CaveOverhangMod:  return TEXT("CaveOverhangMod");
        case EBucket::CaveCliffMod:     return TEXT("CaveCliffMod");
        case EBucket::ScallopMod:       return TEXT("ScallopMod");
        case EBucket::CaveArchMod:      return TEXT("CaveArchMod");
        case EBucket::RoomColumnMod:    return TEXT("RoomColumnMod");
        case EBucket::DomeMod:          return TEXT("DomeMod");
        case EBucket::PinchMod:         return TEXT("PinchMod");
        case EBucket::FloorBiasMod:     return TEXT("FloorBiasMod");
        case EBucket::WormFieldSource:  return TEXT("WormFieldSource");
        case EBucket::OriginSpineOp:    return TEXT("OriginSpineOp");
        case EBucket::BoundarySealOp:   return TEXT("BoundarySealOp");
        case EBucket::PassageCarveOp:   return TEXT("PassageCarveOp");
        case EBucket::CaveTunnelFloorOp:return TEXT("CaveTunnelFloorOp");
        case EBucket::CaveTunnelAirOp:  return TEXT("CaveTunnelAirOp");
        case EBucket::XYEdgeSealOp:     return TEXT("XYEdgeSealOp");
        case EBucket::OtherOp:           return TEXT("OtherOp");
        case EBucket::DensityPrologue:   return TEXT("DensityPrologue");
        case EBucket::DensityCore:       return TEXT("DensityCore");
        case EBucket::DensityDisturbances:return TEXT("DensityDisturbances");
        case EBucket::DensityStructuralPosts:return TEXT("DensityStructuralPosts");
        case EBucket::DensityBoundarySeal:return TEXT("DensityBoundarySeal");
        case EBucket::DensityDiffLayer:  return TEXT("DensityDiffLayer");
        case EBucket::DensityTail:       return TEXT("DensityTail");
        case EBucket::MesherGenerateMesh:return TEXT("MesherGenerateMesh");
        case EBucket::MesherDensityGrid:return TEXT("MesherDensityGrid");
        case EBucket::MesherCellClassification:return TEXT("MesherCellClassification");
        case EBucket::MesherGradientNormals:return TEXT("MesherGradientNormals");
        case EBucket::MesherVertexInterpolation:return TEXT("MesherVertexInterpolation");
        case EBucket::MesherStreamBuilding:return TEXT("MesherStreamBuilding");
        case EBucket::MesherOther:       return TEXT("MesherOther");
        case EBucket::Count:             break;
        }
        return TEXT("Unknown");
    }

    FScopeToken BeginScope(EBucket Bucket)
    {
        FScopeToken Token;
        const EMode Mode = GetMode();
        if (!IsEnabled()
            || (Mode == EMode::Sampled && !IsSampledScopeEnabled(Bucket)))
        {
            return Token;
        }

        Token.bEntered = true;
        Token.bPreviousSampled = GScopeSampled;
        Token.bPreviousSampledScopeActive = GSampledScopeActive;
        ++GScopeDepth;

        // For the hot parent, amortise the timer over a deterministic block of calls.  A timer
        // pair around one ~100 ns operation is mostly the timer; a pair around 64 calls is cheap
        // and estimates the parent from real elapsed work rather than instrumentation latency.
        if (Mode == EMode::Sampled && Bucket == EBucket::GetDensityAt)
        {
            FThreadState& State = GetThreadState();
            const int32 Index = ToIndex(Bucket);
            const uint32 Sequence = State.SampleSequence[Index]++;
            Token.bBlockTiming = true;
            const uint32 Interval = GetSampleInterval();
            const bool bBlockBoundary = (Interval & (Interval - 1u)) == 0u
                ? (Sequence & (Interval - 1u)) == 0u
                : (Sequence % Interval) == 0u;
            if (bBlockBoundary && !State.BlockActive[Index])
            {
                State.BlockActive[Index] = true;
                State.BlockCallCount[Index] = 0;
                State.BlockStartCycles[Index] = FPlatformTime::Cycles64();
            }
            GScopeSampled = false;
            return Token;
        }

        // Every bucket samples independently, but only one nested non-container scope owns a
        // timer at a time.  Otherwise a sampled GetDensityAt would also start timers for every
        // phase and operation inside that same call, measuring instrumentation instead of work.
        // Mesher container ranges deliberately remain transparent so their grid can still expose
        // density samples underneath them.
        const bool bContainerScope = Bucket == EBucket::MesherGenerateMesh
            || Bucket == EBucket::MesherDensityGrid;
        Token.bSampled = Mode == EMode::Full
            ? true
            : (!GSampledScopeActive && ShouldSample(Bucket));
        GScopeSampled = Token.bSampled;
        if (Token.bSampled)
        {
            if (Mode == EMode::Sampled && !bContainerScope)
            {
                GSampledScopeActive = true;
            }
            Token.StartCycles = FPlatformTime::Cycles64();
        }
        return Token;
    }

    void EndScope(EBucket Bucket, const FScopeToken& Token)
    {
        if (!Token.bEntered)
        {
            return;
        }

        RecordCall(Bucket);
        if (Token.bBlockTiming)
        {
            FThreadState& State = GetThreadState();
            const int32 Index = ToIndex(Bucket);
            ++State.BlockCallCount[Index];
            const uint32 Interval = GetSampleInterval();
            if (State.BlockActive[Index] && State.BlockCallCount[Index] >= Interval)
            {
                const uint64 RawCycles = FPlatformTime::Cycles64() - State.BlockStartCycles[Index];
                const uint64 TimerCost = GetTimerPairCycles();
                const uint64 AdjustedCycles = RawCycles > TimerCost
                    ? RawCycles - TimerCost
                    : 0;
                State.Cycles[Index] += AdjustedCycles / FMath::Max<uint32>(1, Interval);
                State.Samples[Index] += 1;
                State.BlockActive[Index] = false;
                State.BlockCallCount[Index] = 0;
                State.BlockStartCycles[Index] = 0;
            }
        }
        else if (Token.bSampled)
        {
            AddSampledMeasurement(Bucket, FPlatformTime::Cycles64() - Token.StartCycles);
        }

        check(GScopeDepth > 0);
        --GScopeDepth;
        GScopeSampled = Token.bPreviousSampled;
        GSampledScopeActive = Token.bPreviousSampledScopeActive;
    }

    FScopedTimer::~FScopedTimer()
    {
        End();
    }

    void FScopedTimer::End()
    {
        if (!Token.bEntered) { return; }
        EndScope(Bucket, Token);
        Token.bEntered = false;
    }
}
