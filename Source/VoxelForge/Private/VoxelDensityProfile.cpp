#include "VoxelDensityProfile.h"

namespace VoxelDensityProfile
{
    std::atomic<bool> GEnabled(false);
    std::atomic<EMode> GMode(EMode::Disabled);

    namespace
    {
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
            FSnapshot::FClassifierStats Classifier;

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
            // Sampled mode pays for the trustworthy parent and mesh containers only. Attribution
            // additionally exposes the component scopes, but still samples them; Full remains the
            // deliberately invasive mode for per-call scope timing.
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

        bool IsAttributionScope(EBucket Bucket)
        {
            switch (Bucket)
            {
            case EBucket::GetDensityAt:
            case EBucket::ApplyDisturbances:
            case EBucket::PassageModifier:
            case EBucket::PassageLandingAir:
            case EBucket::PassageLandingFloor:
            case EBucket::PassageLandingRoomFloor:
            case EBucket::PassageTunnelAir:
            case EBucket::PassageStructuralPosts:
            case EBucket::TunnelCoreSupport:
            case EBucket::TunnelCoreSdf:
            case EBucket::TunnelCorePosts:
            case EBucket::StructuralTail:
            case EBucket::RoomGraphSource:
            case EBucket::SdfCarve:
            case EBucket::FusedEvaluator:
            case EBucket::InterpretedOpStack:
            case EBucket::OperatorBlock:
            case EBucket::RoomGraphBuild:
            case EBucket::RoomGraphSdf:
            case EBucket::TunnelCoreWorld:
            case EBucket::FusedDetail:
            case EBucket::FusedWorm:
            case EBucket::FusedStructural:
            case EBucket::ClassifierTotal:
            case EBucket::ClassifierIntervalProof:
            case EBucket::ClassifierExactCore:
            case EBucket::ClassifierExactFinal:
            case EBucket::ClassifierRoomTail:
            case EBucket::RuntimeStreamBuilding:
            case EBucket::DensityPrologue:
            case EBucket::DensityCore:
            case EBucket::DensityDisturbances:
            case EBucket::DensityStructuralPosts:
            case EBucket::DensityBoundarySeal:
            case EBucket::DensityDiffLayer:
            case EBucket::DensityTail:
            case EBucket::MesherGenerateMesh:
            case EBucket::MesherDensityGrid:
            case EBucket::MesherCellClassification:
            case EBucket::MesherGradientNormals:
            case EBucket::MesherVertexInterpolation:
            case EBucket::MesherStreamBuilding:
            case EBucket::MesherOther:
                return true;
            default:
                return false;
            }
        }

        bool IsAttributionAlwaysTimed(EBucket Bucket)
        {
            switch (Bucket)
            {
            case EBucket::MesherGenerateMesh:
            case EBucket::MesherDensityGrid:
            case EBucket::MesherOther:
            case EBucket::RoomGraphBuild:
                return true;
            default:
                return false;
            }
        }

        uint64 ScaleSampledValue(uint64 Value, uint64 Calls, uint64 Samples)
        {
            if (Samples == 0 || Calls <= Samples)
            {
                return Value;
            }
            const double Scaled = static_cast<double>(Value)
                * static_cast<double>(Calls) / static_cast<double>(Samples);
            return Scaled > static_cast<double>(MAX_uint64)
                ? MAX_uint64
                : static_cast<uint64>(Scaled + 0.5);
        }

        void AddClassifierStatsScaled(
            FSnapshot::FClassifierStats& Destination,
            const FSnapshot::FClassifierStats& Source,
            uint64 Calls,
            uint64 Samples)
        {
            Destination.RefineNodes += ScaleSampledValue(Source.RefineNodes, Calls, Samples);
            Destination.StackBoxCalls += ScaleSampledValue(Source.StackBoxCalls, Calls, Samples);
            Destination.WholeMixedNodes += ScaleSampledValue(Source.WholeMixedNodes, Calls, Samples);
            Destination.WholeSolidNodes += ScaleSampledValue(Source.WholeSolidNodes, Calls, Samples);
            Destination.WholeAirNodes += ScaleSampledValue(Source.WholeAirNodes, Calls, Samples);
            Destination.NeedsFinalFieldNodes += ScaleSampledValue(Source.NeedsFinalFieldNodes, Calls, Samples);
            Destination.SplitNodes += ScaleSampledValue(Source.SplitNodes, Calls, Samples);
            Destination.MaxRefinementDepth = FMath::Max(
                Destination.MaxRefinementDepth, Source.MaxRefinementDepth);
            Destination.ExactCoreSamples += ScaleSampledValue(Source.ExactCoreSamples, Calls, Samples);
            Destination.ExactFinalSamples += ScaleSampledValue(Source.ExactFinalSamples, Calls, Samples);
            Destination.ExactCoreCacheHits += ScaleSampledValue(Source.ExactCoreCacheHits, Calls, Samples);
            Destination.ExactFinalCacheHits += ScaleSampledValue(Source.ExactFinalCacheHits, Calls, Samples);
            Destination.ExactCoreLeaves += ScaleSampledValue(Source.ExactCoreLeaves, Calls, Samples);
            Destination.ExactFinalLeaves += ScaleSampledValue(Source.ExactFinalLeaves, Calls, Samples);
            Destination.StackBoxCycles += ScaleSampledValue(Source.StackBoxCycles, Calls, Samples);
            Destination.ExactCoreCycles += ScaleSampledValue(Source.ExactCoreCycles, Calls, Samples);
            Destination.ExactFinalCycles += ScaleSampledValue(Source.ExactFinalCycles, Calls, Samples);
            Destination.RoomTailQueries += ScaleSampledValue(Source.RoomTailQueries, Calls, Samples);
            Destination.RoomTailEvaluated += ScaleSampledValue(Source.RoomTailEvaluated, Calls, Samples);
            Destination.RoomTailCycles += ScaleSampledValue(Source.RoomTailCycles, Calls, Samples);
            Destination.RoomPropagateCycles += ScaleSampledValue(Source.RoomPropagateCycles, Calls, Samples);
            Destination.RoomExactPrimitiveCycles += ScaleSampledValue(Source.RoomExactPrimitiveCycles, Calls, Samples);
            Destination.RoomCacheWindowCycles += ScaleSampledValue(Source.RoomCacheWindowCycles, Calls, Samples);
            // These are topology descriptors, not per-call work counts.  Keep the largest observed
            // descriptor instead of multiplying one sampled graph description by the classifier
            // call expansion factor.
            Destination.RoomNumRooms = FMath::Max(Destination.RoomNumRooms, Source.RoomNumRooms);
            Destination.RoomNumTunnels = FMath::Max(Destination.RoomNumTunnels, Source.RoomNumTunnels);
            Destination.RoomNumRoomFloorJoins = FMath::Max(
                Destination.RoomNumRoomFloorJoins, Source.RoomNumRoomFloorJoins);
            Destination.RoomNumPits = FMath::Max(Destination.RoomNumPits, Source.RoomNumPits);
            Destination.RoomNumChimneys = FMath::Max(Destination.RoomNumChimneys, Source.RoomNumChimneys);
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
            State->Classifier = FSnapshot::FClassifierStats();
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

            Result.Classifier.Calls += State->Classifier.Calls;
            Result.Classifier.SampledCalls += State->Classifier.SampledCalls;
            AddClassifierStatsScaled(
                Result.Classifier,
                State->Classifier,
                State->Classifier.Calls,
                State->Classifier.SampledCalls);

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

    FSnapshot SnapshotCurrentThread()
    {
        FSnapshot Result;
        FThreadState* State = GThreadState;
        if (State == nullptr)
        {
            Result.TimerPairCycles = GetTimerPairCycles();
            Result.SampleInterval = GetSampleInterval();
            Result.Mode = GetMode();
            return Result;
        }

        for (int32 Index = 0; Index < BucketCount; ++Index)
        {
            Result.Calls[Index] = State->Calls[Index];
            Result.Samples[Index] = State->Samples[Index];
            if (State->Samples[Index] == 0)
            {
                Result.Cycles[Index] = State->Cycles[Index];
            }
            else
            {
                const double Scale = static_cast<double>(State->Calls[Index])
                    / static_cast<double>(State->Samples[Index]);
                const double EstimatedCycles = static_cast<double>(State->Cycles[Index]) * Scale;
                Result.Cycles[Index] = EstimatedCycles > static_cast<double>(MAX_uint64)
                    ? MAX_uint64
                    : static_cast<uint64>(EstimatedCycles + 0.5);
            }
            Result.WallCycles[Index] = Result.Cycles[Index];
            Result.ActiveWorkers[Index] = State->Calls[Index] > 0 ? 1 : 0;
        }
        for (int32 Index = 0; Index < CounterCount; ++Index)
        {
            Result.Counters[Index] = State->Counters[Index];
        }
        Result.Classifier = State->Classifier;
        if (State->Classifier.SampledCalls > 0)
        {
            FSnapshot::FClassifierStats Raw = State->Classifier;
            Result.Classifier = FSnapshot::FClassifierStats();
            Result.Classifier.Calls = State->Classifier.Calls;
            Result.Classifier.SampledCalls = State->Classifier.SampledCalls;
            AddClassifierStatsScaled(
                Result.Classifier,
                Raw,
                State->Classifier.Calls,
                State->Classifier.SampledCalls);
        }
        Result.TimerPairCycles = GetTimerPairCycles();
        Result.SampleInterval = GetSampleInterval();
        Result.Mode = GetMode();
        return Result;
    }

    void AddCounter(ECounter Counter, uint64 Amount)
    {
        if (!IsEnabledFast())
        {
            return;
        }
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

    void RecordClassifierCall(bool bSampled)
    {
        FThreadState& State = GetThreadState();
        ++State.Classifier.Calls;
        if (bSampled)
        {
            ++State.Classifier.SampledCalls;
        }
        ++State.Calls[ToIndex(EBucket::ClassifierTotal)];
    }

    void AddClassifierStats(const FSnapshot::FClassifierStats& Stats)
    {
        FThreadState& State = GetThreadState();
        State.Classifier.RefineNodes += Stats.RefineNodes;
        State.Classifier.StackBoxCalls += Stats.StackBoxCalls;
        State.Classifier.WholeMixedNodes += Stats.WholeMixedNodes;
        State.Classifier.WholeSolidNodes += Stats.WholeSolidNodes;
        State.Classifier.WholeAirNodes += Stats.WholeAirNodes;
        State.Classifier.NeedsFinalFieldNodes += Stats.NeedsFinalFieldNodes;
        State.Classifier.SplitNodes += Stats.SplitNodes;
        State.Classifier.MaxRefinementDepth = FMath::Max(
            State.Classifier.MaxRefinementDepth, Stats.MaxRefinementDepth);
        State.Classifier.ExactCoreSamples += Stats.ExactCoreSamples;
        State.Classifier.ExactFinalSamples += Stats.ExactFinalSamples;
        State.Classifier.ExactCoreCacheHits += Stats.ExactCoreCacheHits;
        State.Classifier.ExactFinalCacheHits += Stats.ExactFinalCacheHits;
        State.Classifier.ExactCoreLeaves += Stats.ExactCoreLeaves;
        State.Classifier.ExactFinalLeaves += Stats.ExactFinalLeaves;
        State.Classifier.StackBoxCycles += Stats.StackBoxCycles;
        State.Classifier.ExactCoreCycles += Stats.ExactCoreCycles;
        State.Classifier.ExactFinalCycles += Stats.ExactFinalCycles;
        State.Classifier.RoomTailQueries += Stats.RoomTailQueries;
        State.Classifier.RoomTailEvaluated += Stats.RoomTailEvaluated;
        State.Classifier.RoomTailCycles += Stats.RoomTailCycles;
        State.Classifier.RoomPropagateCycles += Stats.RoomPropagateCycles;
        State.Classifier.RoomExactPrimitiveCycles += Stats.RoomExactPrimitiveCycles;
        State.Classifier.RoomCacheWindowCycles += Stats.RoomCacheWindowCycles;
        State.Classifier.RoomNumRooms = FMath::Max(
            State.Classifier.RoomNumRooms, Stats.RoomNumRooms);
        State.Classifier.RoomNumTunnels = FMath::Max(
            State.Classifier.RoomNumTunnels, Stats.RoomNumTunnels);
        State.Classifier.RoomNumRoomFloorJoins = FMath::Max(
            State.Classifier.RoomNumRoomFloorJoins, Stats.RoomNumRoomFloorJoins);
        State.Classifier.RoomNumPits = FMath::Max(
            State.Classifier.RoomNumPits, Stats.RoomNumPits);
        State.Classifier.RoomNumChimneys = FMath::Max(
            State.Classifier.RoomNumChimneys, Stats.RoomNumChimneys);
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
        case ECounter::RoomGraphBuildGeneratorTile: return TEXT("RoomGraphBuildGeneratorTile");
        case ECounter::RoomGraphBuildGeneratorTunnelCore: return TEXT("RoomGraphBuildGeneratorTunnelCore");
        case ECounter::RoomGraphBuildOpShared: return TEXT("RoomGraphBuildOpShared");
        case ECounter::RoomGraphBuildOpLocal: return TEXT("RoomGraphBuildOpLocal");
        case ECounter::RoomGraphBuildClassifierShared: return TEXT("RoomGraphBuildClassifierShared");
        case ECounter::RoomGraphBuildClassifierLocal: return TEXT("RoomGraphBuildClassifierLocal");
        case ECounter::RoomGraphBuildUnknown: return TEXT("RoomGraphBuildUnknown");
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
        case ECounter::TunnelSupportFloorBackstopFires:return TEXT("TunnelSupportFloorBackstopFires");
        case ECounter::TunnelRoomFloorBackstopFires:return TEXT("TunnelRoomFloorBackstopFires");
        case ECounter::TunnelAuthoredFloorSamples:return TEXT("TunnelAuthoredFloorSamples");
        case ECounter::TunnelAuthoredFloorProtectionClamps:return TEXT("TunnelAuthoredFloorProtectionClamps");
        case ECounter::TunnelAuthoredFloorDisturbanceSkips:return TEXT("TunnelAuthoredFloorDisturbanceSkips");
        case ECounter::TunnelFloorProfileFallbacks:return TEXT("TunnelFloorProfileFallbacks");
        case ECounter::TunnelFloorProfileBuilds:return TEXT("TunnelFloorProfileBuilds");
        case ECounter::TunnelFloorProfileSegments:return TEXT("TunnelFloorProfileSegments");
        case ECounter::TunnelFloorProfileLedges:return TEXT("TunnelFloorProfileLedges");
        case ECounter::TunnelFloorProfileLedgesBelow2Voxels:return TEXT("TunnelFloorProfileLedgesBelow2Voxels");
        case ECounter::TunnelFloorProfileLedges2To4Voxels:return TEXT("TunnelFloorProfileLedges2To4Voxels");
        case ECounter::TunnelFloorProfileLedges4To8Voxels:return TEXT("TunnelFloorProfileLedges4To8Voxels");
        case ECounter::TunnelFloorProfileLedges8PlusVoxels:return TEXT("TunnelFloorProfileLedges8PlusVoxels");
        case ECounter::TunnelFloorSteepEdges:return TEXT("TunnelFloorSteepEdges");
        case ECounter::TunnelFloorWindingEdges:return TEXT("TunnelFloorWindingEdges");
        case ECounter::TunnelFloorDropEdges:return TEXT("TunnelFloorDropEdges");
        case ECounter::TunnelFloorMultiStepEdges:return TEXT("TunnelFloorMultiStepEdges");
        case ECounter::PassageSupportFloorBackstopFires:return TEXT("PassageSupportFloorBackstopFires");
        case ECounter::PassageNativeFloorCompositions:return TEXT("PassageNativeFloorCompositions");
        case ECounter::PassageCandidates: return TEXT("PassageCandidates");
        case ECounter::PassageEvaluated:  return TEXT("PassageEvaluated");
        case ECounter::MesherBlockTests:  return TEXT("MesherBlockTests");
        case ECounter::MesherBlockAllSolid:return TEXT("MesherBlockAllSolid");
        case ECounter::MesherBlockAllAir: return TEXT("MesherBlockAllAir");
        case ECounter::TileClassifyCalls: return TEXT("TileClassifyCalls");
        case ECounter::TileClassifyAllSolid: return TEXT("TileClassifyAllSolid");
        case ECounter::TileClassifyAllAir: return TEXT("TileClassifyAllAir");
        case ECounter::TileClassifyMixed: return TEXT("TileClassifyMixed");
        case ECounter::FusedTunnelSamples: return TEXT("FusedTunnelSamples");
        case ECounter::FusedTunnelDetailSamples: return TEXT("FusedTunnelDetailSamples");
        case ECounter::FusedTunnelDetailSkipped: return TEXT("FusedTunnelDetailSkipped");
        case ECounter::WormEligibleSamples: return TEXT("WormEligibleSamples");
        case ECounter::WormBlockProofs: return TEXT("WormBlockProofs");
        case ECounter::WormBlockSkippedSamples: return TEXT("WormBlockSkippedSamples");
        case ECounter::OpBlockBuilds: return TEXT("OpBlockBuilds");
        case ECounter::OpBlockSamples: return TEXT("OpBlockSamples");
        case ECounter::OpBlockOperators: return TEXT("OpBlockOperators");
        case ECounter::OpBlockActiveOperators: return TEXT("OpBlockActiveOperators");
        case ECounter::OpBlockPrunedOperators: return TEXT("OpBlockPrunedOperators");
        case ECounter::MesherDensityGridBytes: return TEXT("MesherDensityGridBytes");
        case ECounter::MesherSharedGridReadBytes: return TEXT("MesherSharedGridReadBytes");
        case ECounter::MesherOutputArrayBytes: return TEXT("MesherOutputArrayBytes");
        case ECounter::MesherOutputAllocatedBytes: return TEXT("MesherOutputAllocatedBytes");
        case ECounter::OpBlockScratchBytes: return TEXT("OpBlockScratchBytes");
        case ECounter::OpBlockCopyBytes: return TEXT("OpBlockCopyBytes");
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
            { TEXT("XYEdgeSealOp"),      EBucket::XYEdgeSealOp },
            { TEXT("FusedEvaluator"),    EBucket::FusedEvaluator },
            { TEXT("InterpretedOpStack"),EBucket::InterpretedOpStack },
            { TEXT("OperatorBlock"),     EBucket::OperatorBlock },
            { TEXT("RoomGraphBuild"),    EBucket::RoomGraphBuild },
            { TEXT("RoomGraphSdf"),      EBucket::RoomGraphSdf },
            { TEXT("TunnelCoreWorld"),   EBucket::TunnelCoreWorld },
            { TEXT("FusedDetail"),       EBucket::FusedDetail },
            { TEXT("FusedWorm"),         EBucket::FusedWorm },
            { TEXT("FusedStructural"),   EBucket::FusedStructural },
            { TEXT("ClassifierTotal"),   EBucket::ClassifierTotal },
            { TEXT("ClassifierIntervalProof"), EBucket::ClassifierIntervalProof },
            { TEXT("ClassifierExactCore"), EBucket::ClassifierExactCore },
            { TEXT("ClassifierExactFinal"), EBucket::ClassifierExactFinal },
            { TEXT("ClassifierRoomTail"), EBucket::ClassifierRoomTail },
            { TEXT("RuntimeStreamBuilding"), EBucket::RuntimeStreamBuilding },
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
        case EBucket::XYEdgeSealOp:     return TEXT("XYEdgeSealOp");
        case EBucket::OtherOp:           return TEXT("OtherOp");
        case EBucket::FusedEvaluator:    return TEXT("FusedEvaluator");
        case EBucket::InterpretedOpStack:return TEXT("InterpretedOpStack");
        case EBucket::OperatorBlock:     return TEXT("OperatorBlock");
        case EBucket::RoomGraphBuild:    return TEXT("RoomGraphBuild");
        case EBucket::RoomGraphSdf:      return TEXT("RoomGraphSdf");
        case EBucket::TunnelCoreWorld:   return TEXT("TunnelCoreWorld");
        case EBucket::FusedDetail:       return TEXT("FusedDetail");
        case EBucket::FusedWorm:         return TEXT("FusedWorm");
        case EBucket::FusedStructural:   return TEXT("FusedStructural");
        case EBucket::ClassifierTotal:   return TEXT("ClassifierTotal");
        case EBucket::ClassifierIntervalProof:return TEXT("ClassifierIntervalProof");
        case EBucket::ClassifierExactCore:return TEXT("ClassifierExactCore");
        case EBucket::ClassifierExactFinal:return TEXT("ClassifierExactFinal");
        case EBucket::ClassifierRoomTail:return TEXT("ClassifierRoomTail");
        case EBucket::RuntimeStreamBuilding:return TEXT("RuntimeStreamBuilding");
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
        FScopeToken Token{};
        if (!IsEnabledFast())
        {
            return Token;
        }
        const EMode Mode = GetMode();
        const bool bSampledMode = Mode == EMode::Sampled || Mode == EMode::Attribution;
        if ((Mode == EMode::Sampled && !IsSampledScopeEnabled(Bucket))
            || (Mode == EMode::Attribution && !IsAttributionScope(Bucket)))
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
        if (bSampledMode && Bucket == EBucket::GetDensityAt)
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
            // The selected call admits all nested component scopes.  Every other call exits the
            // inline FScopedTimer fast path before crossing into this translation unit.
            GScopeSampled = bBlockBoundary;
            GSampledScopeActive = bBlockBoundary;
            Token.bSampled = bBlockBoundary;
            return Token;
        }

        // Every bucket samples independently, but only one nested non-container scope owns a
        // timer at a time.  Otherwise a sampled GetDensityAt would also start timers for every
        // phase and operation inside that same call, measuring instrumentation instead of work.
        // Mesher container ranges deliberately remain transparent so their grid can still expose
        // density samples underneath them.
        const bool bContainerScope = Bucket == EBucket::MesherGenerateMesh
            || Bucket == EBucket::MesherDensityGrid;
        const bool bInheritedAttributionSample = Mode == EMode::Attribution
            && GSampledScopeActive;
        Token.bSampled = Mode == EMode::Full
            ? true
            : ((Mode == EMode::Attribution && IsAttributionAlwaysTimed(Bucket))
                ? true
                : (bInheritedAttributionSample
                    ? true
                    : (Mode == EMode::Attribution && IsAttributionScope(Bucket)
                        ? ShouldSample(Bucket)
                        : (!GSampledScopeActive && ShouldSample(Bucket)))));
        GScopeSampled = Token.bSampled;
        if (Token.bSampled)
        {
            if ((Mode == EMode::Sampled || Mode == EMode::Attribution) && !bContainerScope)
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

}
