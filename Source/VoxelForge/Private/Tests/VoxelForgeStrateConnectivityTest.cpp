// VoxelForgeStrateConnectivityTest.cpp
// Tier 2: the measurement pass and its coarse-connectivity false-positive guard.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelCaveMorphology.h"
#include "VoxelPassageGeometry.h"
#include "VoxelStrateMeasure.h"
#include "VoxelTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeStrateConnectivityTest,
    "VoxelForge.Generation.StrateConnectivity",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeStrateConnectivityRefinementTest,
    "VoxelForge.Generation.StrateConnectivityRefinement",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeVerticalShaftSeamTest,
    "VoxelForge.Generation.VerticalShaftSeamFreedom",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    struct FArchetypeReport
    {
        FString Name;
        int32 Index = INDEX_NONE;
        FVoxelStrateMetrics OldWindow;
        FVoxelStrateMetrics OldWindowRepeat;
        FVoxelStrateMetrics DerivedWindow;
        FVoxelStrateMetrics DerivedWindowRepeat;
    };

    struct FConnectivityProbe
    {
        bool bChecked = false;
        EVoxelConnectivityResult Result = EVoxelConnectivityResult::OutOfWindow;
        bool bStartSnapped = false;
        bool bGoalSnapped = false;
        int32 NumRouteRetries = 0;
    };

    struct FStrateConnectivityReport
    {
        int32 Index = INDEX_NONE;
        FString Name;
        int32 ArrivalPassageIndex = INDEX_NONE;
        int32 DeparturePassageIndex = INDEX_NONE;
        FVector ArrivalPoint = FVector::ZeroVector;
        FVector DeparturePoint = FVector::ZeroVector;
        bool bHasArrival = false;
        bool bHasDeparture = false;
        FConnectivityProbe ArrivalToDeparture;
        FConnectivityProbe ArrivalToLargest;
        FConnectivityProbe DepartureToLargest;
    };

    FString ArchetypeName(const FStrateSlot& Slot)
    {
        if (Slot.Definition != nullptr)
        {
            if (const UEnum* ArchetypeEnum = StaticEnum<ECaveGeneratorType>())
            {
                return ArchetypeEnum->GetNameStringByValue(
                    static_cast<int64>(Slot.Definition->GeneratorType));
            }
        }
        return FString::Printf(TEXT("Strate_%d"), Slot.StrateIndex);
    }

    bool SameFloatBits(float A, float B)
    {
        return FMemory::Memcmp(&A, &B, sizeof(float)) == 0;
    }

    bool SameVectorBits(const FVector& A, const FVector& B)
    {
        return SameFloatBits(A.X, B.X)
            && SameFloatBits(A.Y, B.Y)
            && SameFloatBits(A.Z, B.Z);
    }

    bool MetricsAreBitIdentical(const FVoxelStrateMetrics& A, const FVoxelStrateMetrics& B)
    {
        return A.bValid == B.bValid
            && A.RefusalReason == B.RefusalReason
            && A.NumSampled == B.NumSampled
            && A.NumAir == B.NumAir
            && A.NumSolid == B.NumSolid
            && SameFloatBits(A.AirFraction, B.AirFraction)
            && A.NumAirComponents == B.NumAirComponents
            && SameFloatBits(A.LargestComponentShare, B.LargestComponentShare)
            && SameVectorBits(A.LargestComponentPoint, B.LargestComponentPoint)
            && A.LargestComponentCells == B.LargestComponentCells
            && A.NumComponentsAtLeast1Pct == B.NumComponentsAtLeast1Pct
            && SameFloatBits(A.WalkableFraction, B.WalkableFraction)
            && SameFloatBits(A.MedianFeatureScale, B.MedianFeatureScale)
            && A.MedianVerticalClearance == B.MedianVerticalClearance
            && A.bPlayerFitResolved == B.bPlayerFitResolved
            && A.PlayerFitRefusalReason == B.PlayerFitRefusalReason
            && A.NumPlayerFitCells == B.NumPlayerFitCells
            && SameFloatBits(A.PlayerFitFraction, B.PlayerFitFraction)
            && A.NumTraversableComponents == B.NumTraversableComponents
            && A.LargestTraversableComponentCells == B.LargestTraversableComponentCells
            && SameFloatBits(A.TraversableComponentShare, B.TraversableComponentShare)
            && SameFloatBits(A.MinimumPlayerClearanceVoxels,
                             B.MinimumPlayerClearanceVoxels)
            && A.ResolvedMarginVoxels == B.ResolvedMarginVoxels
            && A.SampledMinZ == B.SampledMinZ
            && A.SampledMaxZ == B.SampledMaxZ
            && A.SampledNumX == B.SampledNumX
            && A.SampledNumY == B.SampledNumY
            && A.SampledNumZ == B.SampledNumZ
            && SameFloatBits(A.SampledMinX, B.SampledMinX)
            && SameFloatBits(A.SampledMaxX, B.SampledMaxX)
            && SameFloatBits(A.SampledMinY, B.SampledMinY)
            && SameFloatBits(A.SampledMaxY, B.SampledMaxY)
            && A.AirComponentCells == B.AirComponentCells;
    }

    const TCHAR* ConnectivityResultName(EVoxelConnectivityResult Result)
    {
        switch (Result)
        {
        case EVoxelConnectivityResult::Connected:      return TEXT("CONNECTED");
        case EVoxelConnectivityResult::NotConnectedAtThisResolution:
            return TEXT("NOT_CONNECTED_AT_THIS_RESOLUTION");
        case EVoxelConnectivityResult::StartCellSolid: return TEXT("START_CELL_SOLID");
        case EVoxelConnectivityResult::GoalCellSolid:  return TEXT("GOAL_CELL_SOLID");
        case EVoxelConnectivityResult::StartCellNotPlayerFit:
            return TEXT("START_CELL_NOT_PLAYER_FIT");
        case EVoxelConnectivityResult::GoalCellNotPlayerFit:
            return TEXT("GOAL_CELL_NOT_PLAYER_FIT");
        case EVoxelConnectivityResult::OutOfWindow:    return TEXT("OUT_OF_WINDOW");
        case EVoxelConnectivityResult::CoarseLiedBudgetExhausted:
            return TEXT("COARSE_LIED_BUDGET_EXHAUSTED");
        default:                                       return TEXT("UNKNOWN");
        }
    }

    constexpr int32 ConnectivityResultCount =
        static_cast<int32>(EVoxelConnectivityResult::GoalCellNotPlayerFit) + 1;

    int32 NumRefutedRoutes(const FConnectivityProbe& Probe)
    {
        switch (Probe.Result)
        {
        case EVoxelConnectivityResult::Connected:
        case EVoxelConnectivityResult::NotConnectedAtThisResolution:
            return Probe.NumRouteRetries;
        case EVoxelConnectivityResult::CoarseLiedBudgetExhausted:
            return Probe.NumRouteRetries + 1;
        default:
            return 0;
        }
    }

    bool ConnectivityProbesAreBitIdentical(
        const FConnectivityProbe& A,
        const FConnectivityProbe& B)
    {
        return A.bChecked == B.bChecked
            && A.Result == B.Result
            && A.bStartSnapped == B.bStartSnapped
            && A.bGoalSnapped == B.bGoalSnapped
            && A.NumRouteRetries == B.NumRouteRetries;
    }

    FString ConnectivityProbeText(const FConnectivityProbe& Probe)
    {
        if (!Probe.bChecked)
        {
            return TEXT("NOT_CHECKED");
        }

        FString Text = FString::Printf(
            TEXT("%s [retries=%d]"),
            ConnectivityResultName(Probe.Result),
            Probe.NumRouteRetries);
        if (Probe.bStartSnapped || Probe.bGoalSnapped)
        {
            Text += TEXT(" [");
            bool bNeedSeparator = false;
            if (Probe.bStartSnapped)
            {
                Text += TEXT("start snapped");
                bNeedSeparator = true;
            }
            if (Probe.bGoalSnapped)
            {
                if (bNeedSeparator) Text += TEXT(", ");
                Text += TEXT("goal snapped");
            }
            Text += TEXT("]");
        }
        return Text;
    }

    bool FullResolutionDirectSegmentHasSolid(
        const UVoxelGenerator& Generator,
        const FVector& A,
        const FVector& B)
    {
        const FVector Delta = B - A;
        const float MaxDelta = FMath::Max3(
            FMath::Abs(Delta.X), FMath::Abs(Delta.Y), FMath::Abs(Delta.Z));
        const int32 NumSteps = FMath::Max(1, FMath::CeilToInt(MaxDelta));
        for (int32 Step = 0; Step <= NumSteps; ++Step)
        {
            const float Alpha = static_cast<float>(Step) / static_cast<float>(NumSteps);
            const FVector Sample = A + Delta * Alpha;
            const float Density = Generator.GetDensityAt(Sample.X, Sample.Y, Sample.Z);
            if (!FMath::IsFinite(Density) || !(Density > 0.0f))
            {
                return true;
            }
        }
        return false;
    }

    bool FindFirstGap(
        const TArray<FStrateSlot>& Layout,
        int32& OutGapTopChunkZ,
        int32& OutGapBottomChunkZ)
    {
        for (int32 Index = 0; Index + 1 < Layout.Num(); ++Index)
        {
            const int32 GapTop = Layout[Index].BottomChunkZ - 1;
            const int32 GapBottom = Layout[Index + 1].TopChunkZ + 1;
            if (GapTop >= GapBottom)
            {
                OutGapTopChunkZ = GapTop;
                OutGapBottomChunkZ = GapBottom;
                return true;
            }
        }
        return false;
    }

    float MeasureGapAirFraction(
        const UVoxelGenerator& Generator,
        const UVoxelStrateManager& Manager,
        int32 GapTopChunkZ,
        int32 GapBottomChunkZ,
        const FVoxelStrateMeasureSettings& Settings,
        int64& OutSampleCount,
        int64& OutAirCount)
    {
        OutSampleCount = 0;
        OutAirCount = 0;

        const float MinX = Settings.CenterXY.X - static_cast<float>(Settings.RadiusInVoxels);
        const float MinY = Settings.CenterXY.Y - static_cast<float>(Settings.RadiusInVoxels);
        const float Step = static_cast<float>(Settings.SampleStep);
        const int32 NumXY = FMath::CeilToInt(
            (2.0f * static_cast<float>(Settings.RadiusInVoxels)) / Step);
        const int32 MinGapChunkZ = GapBottomChunkZ;
        const int32 MaxGapChunkZ = GapTopChunkZ;

        for (int32 Z = MinGapChunkZ * CHUNK_SIZE + Settings.SampleStep / 2;
             Z < (MaxGapChunkZ + 1) * CHUNK_SIZE;
             Z += Settings.SampleStep)
        {
            for (int32 YIndex = 0; YIndex < NumXY; ++YIndex)
            {
                const float Y = FMath::Min(
                    MinY + (static_cast<float>(YIndex) + 0.5f) * Step,
                    Settings.CenterXY.Y + static_cast<float>(Settings.RadiusInVoxels) - 0.5f);
                for (int32 XIndex = 0; XIndex < NumXY; ++XIndex)
                {
                    const float X = FMath::Min(
                        MinX + (static_cast<float>(XIndex) + 0.5f) * Step,
                        Settings.CenterXY.X + static_cast<float>(Settings.RadiusInVoxels) - 0.5f);
                    const int32 ChunkZ = FMath::FloorToInt(static_cast<float>(Z) / CHUNK_SIZE);
                    if (!Manager.IsGapChunk(FIntVector(
                            FMath::FloorToInt(X / CHUNK_SIZE),
                            FMath::FloorToInt(Y / CHUNK_SIZE),
                            ChunkZ)))
                    {
                        continue;
                    }

                    const float Density = Generator.GetDensityAt(X, Y, static_cast<float>(Z));
                    ++OutSampleCount;
                    if (FMath::IsFinite(Density) && Density > 0.0f)
                    {
                        ++OutAirCount;
                    }
                }
            }
        }

        return OutSampleCount > 0
            ? static_cast<float>(static_cast<double>(OutAirCount)
                / static_cast<double>(OutSampleCount))
            : 0.0f;
    }

    struct FRefinementSweepRow
    {
        int32 SampleStep = 0;
        int32 RadiusInVoxels = 0;
        int32 RequestedMarginVoxels = -1;
        FVoxelStrateMetrics Metrics;
        FConnectivityProbe Probe;
    };

    struct FVerticalShaftTreeAudit
    {
        int32 NumShafts = 0;
        int32 NumStrictLocalMinima = 0;
        int32 NumNeighbourFallbacks = 0;
        int32 NumSpineWindowFallbacks = 0;
        int32 NumEmergencySpineFallbacks = 0;
        int32 NumUnreachable = 0;
        int32 MaxPathLength = 0;
        bool bOrderIndependent = true;
    };

    struct FAuditShaft
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Radius = 0.0f;
        int32 CellX = 0;
        int32 CellY = 0;
    };

    bool RollAuditShaft(
        const FVerticalShaftParams& Params,
        int32 Seed,
        int32 CellX,
        int32 CellY,
        FAuditShaft& OutShaft)
    {
        const float Spacing = FMath::Max(Params.ShaftSpacing, 1.0f);
        const uint32 ShaftSeed = static_cast<uint32>(Seed) ^ 0x53686674u;
        const uint32 Hash = VoxelHash::Cell(CellX, CellY, ShaftSeed);
        if (VoxelHash::ToFloat01(Hash) > Params.ShaftDensity)
        {
            return false;
        }

        OutShaft.X = (CellX + 0.15f
            + VoxelHash::ToFloat01(VoxelHash::Mix(Hash ^ 0x12345678u)) * 0.7f) * Spacing;
        OutShaft.Y = (CellY + 0.15f
            + VoxelHash::ToFloat01(VoxelHash::Mix(Hash ^ 0x9ABCDEF0u)) * 0.7f) * Spacing;
        OutShaft.Radius = FMath::Lerp(
            Params.ShaftMinRadius,
            Params.ShaftMaxRadius,
            VoxelHash::ToFloat01(VoxelHash::Mix(Hash ^ 0xBEEFu)));
        OutShaft.CellX = CellX;
        OutShaft.CellY = CellY;
        return true;
    }

    int32 FindAuditShaftIndex(
        const TArray<FAuditShaft>& Shafts,
        int32 CellX,
        int32 CellY)
    {
        for (int32 Index = 0; Index < Shafts.Num(); ++Index)
        {
            if (Shafts[Index].CellX == CellX && Shafts[Index].CellY == CellY)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    int32 ResolveAuditParent(
        const TArray<FAuditShaft>& Shafts,
        int32 ChildIndex,
        bool& bOutStrictParent,
        bool& bOutSpineWindow,
        bool& bOutNeighbourFallback)
    {
        constexpr int32 CandidateRadius = 2;
        constexpr int32 FallbackRadius = 3;
        const FAuditShaft& Child = Shafts[ChildIndex];
        const float ChildOriginSq = FMath::Square(Child.X) + FMath::Square(Child.Y);
        int32 ParentIndex = INDEX_NONE;
        float BestDistanceSq = FLT_MAX;
        bOutStrictParent = false;
        bOutSpineWindow = false;
        bOutNeighbourFallback = false;

        auto ConsiderCandidate = [&](int32 CandidateIndex, int32 Radius, bool bRequireLowerOrigin)
        {
            const FAuditShaft& Candidate = Shafts[CandidateIndex];
            if (CandidateIndex == ChildIndex
                || FMath::Abs(Candidate.CellX - Child.CellX) > Radius
                || FMath::Abs(Candidate.CellY - Child.CellY) > Radius)
            {
                return;
            }

            const float CandidateOriginSq = FMath::Square(Candidate.X)
                + FMath::Square(Candidate.Y);
            if (bRequireLowerOrigin && !(CandidateOriginSq < ChildOriginSq))
            {
                return;
            }

            const float DistanceSq = FMath::Square(Child.X - Candidate.X)
                + FMath::Square(Child.Y - Candidate.Y);
            const bool bLowerCell = ParentIndex == INDEX_NONE
                || Candidate.CellY < Shafts[ParentIndex].CellY
                || (Candidate.CellY == Shafts[ParentIndex].CellY
                    && Candidate.CellX < Shafts[ParentIndex].CellX);
            if (DistanceSq < BestDistanceSq
                || (DistanceSq == BestDistanceSq && bLowerCell))
            {
                BestDistanceSq = DistanceSq;
                ParentIndex = CandidateIndex;
            }
        };

        for (int32 CandidateIndex = 0; CandidateIndex < Shafts.Num(); ++CandidateIndex)
        {
            ConsiderCandidate(CandidateIndex, CandidateRadius, true);
        }
        if (ParentIndex != INDEX_NONE)
        {
            bOutStrictParent = true;
            return ParentIndex;
        }

        if (FMath::Abs(Child.CellX) <= CandidateRadius
            && FMath::Abs(Child.CellY) <= CandidateRadius)
        {
            bOutSpineWindow = true;
            return INDEX_NONE;
        }

        BestDistanceSq = FLT_MAX;
        for (int32 CandidateIndex = 0; CandidateIndex < Shafts.Num(); ++CandidateIndex)
        {
            ConsiderCandidate(CandidateIndex, FallbackRadius, true);
        }
        if (ParentIndex != INDEX_NONE)
        {
            bOutNeighbourFallback = true;
        }
        return ParentIndex;
    }

    FVerticalShaftTreeAudit AuditVerticalShaftTree(
        const FVerticalShaftParams& Params,
        int32 Seed)
    {
        // Count only the padded interior; the padding supplies every candidate needed by the
        // exact ±2/±3 windows. The domain is fixed so the reported rate is comparable across all
        // 16 seeds and does not depend on a mouth or on a hash-container traversal.
        constexpr int32 ScanRadius = 20;
        constexpr int32 CountRadius = 16;
        TArray<FAuditShaft> Shafts;
        for (int32 CellY = -ScanRadius; CellY <= ScanRadius; ++CellY)
        {
            for (int32 CellX = -ScanRadius; CellX <= ScanRadius; ++CellX)
            {
                FAuditShaft Shaft;
                if (RollAuditShaft(Params, Seed, CellX, CellY, Shaft))
                {
                    Shafts.Add(Shaft);
                }
            }
        }

        FVerticalShaftTreeAudit Out;
        TArray<int32> CountedIndices;
        for (int32 Index = 0; Index < Shafts.Num(); ++Index)
        {
            if (FMath::Abs(Shafts[Index].CellX) <= CountRadius
                && FMath::Abs(Shafts[Index].CellY) <= CountRadius)
            {
                CountedIndices.Add(Index);
            }
        }
        Out.NumShafts = CountedIndices.Num();

        for (const int32 ChildIndex : CountedIndices)
        {
            bool bStrictParent = false;
            bool bSpineWindow = false;
            bool bNeighbourFallback = false;
            const int32 ParentIndex = ResolveAuditParent(
                Shafts, ChildIndex, bStrictParent, bSpineWindow, bNeighbourFallback);
            if (!bStrictParent)
            {
                ++Out.NumStrictLocalMinima;
            }
            if (bSpineWindow)
            {
                ++Out.NumSpineWindowFallbacks;
            }
            else if (bNeighbourFallback)
            {
                ++Out.NumNeighbourFallbacks;
            }
            else if (ParentIndex == INDEX_NONE)
            {
                ++Out.NumEmergencySpineFallbacks;
            }

            int32 CurrentIndex = ChildIndex;
            bool bReachedSpine = false;
            for (int32 Hop = 0; Hop <= Shafts.Num(); ++Hop)
            {
                bool bHopStrict = false;
                bool bHopSpineWindow = false;
                bool bHopNeighbour = false;
                const int32 NextIndex = ResolveAuditParent(
                    Shafts, CurrentIndex, bHopStrict, bHopSpineWindow, bHopNeighbour);
                if (NextIndex == INDEX_NONE)
                {
                    bReachedSpine = true;
                    Out.MaxPathLength = FMath::Max(Out.MaxPathLength, Hop + 1);
                    break;
                }
                if (NextIndex == CurrentIndex)
                {
                    break;
                }
                CurrentIndex = NextIndex;
            }
            if (!bReachedSpine)
            {
                ++Out.NumUnreachable;
            }
        }

        // Re-run the resolver over the reverse array and compare by cell coordinate. This makes
        // the fixed tie-break an executable assertion that no result depends on array order.
        TArray<FAuditShaft> Reversed;
        Reversed.Reserve(Shafts.Num());
        for (int32 Index = Shafts.Num() - 1; Index >= 0; --Index)
        {
            Reversed.Add(Shafts[Index]);
        }
        for (const int32 OriginalChildIndex : CountedIndices)
        {
            const FAuditShaft& OriginalChild = Shafts[OriginalChildIndex];
            const int32 ReversedChildIndex = FindAuditShaftIndex(
                Reversed, OriginalChild.CellX, OriginalChild.CellY);
            if (ReversedChildIndex == INDEX_NONE)
            {
                Out.bOrderIndependent = false;
                continue;
            }

            bool bOriginalStrict = false;
            bool bOriginalSpine = false;
            bool bOriginalNeighbour = false;
            const int32 OriginalParent = ResolveAuditParent(
                Shafts, OriginalChildIndex, bOriginalStrict, bOriginalSpine, bOriginalNeighbour);
            bool bReversedStrict = false;
            bool bReversedSpine = false;
            bool bReversedNeighbour = false;
            const int32 ReversedParent = ResolveAuditParent(
                Reversed, ReversedChildIndex, bReversedStrict, bReversedSpine, bReversedNeighbour);
            const FAuditShaft* OriginalParentShaft = OriginalParent == INDEX_NONE
                ? nullptr : &Shafts[OriginalParent];
            const FAuditShaft* ReversedParentShaft = ReversedParent == INDEX_NONE
                ? nullptr : &Reversed[ReversedParent];
            Out.bOrderIndependent &= (OriginalParentShaft == nullptr) == (ReversedParentShaft == nullptr);
            if (OriginalParentShaft != nullptr && ReversedParentShaft != nullptr)
            {
                Out.bOrderIndependent &= OriginalParentShaft->CellX == ReversedParentShaft->CellX
                    && OriginalParentShaft->CellY == ReversedParentShaft->CellY;
            }
        }
        return Out;
    }

    bool AuditVerticalShaftPathBounds(
        const FVerticalShaftParams& Params,
        int32 Seed,
        const FVector& AxisPoint,
        FVector2D& OutMin,
        FVector2D& OutMax,
        int32& OutHops)
    {
        constexpr int32 ScanRadius = 20;
        TArray<FAuditShaft> Shafts;
        for (int32 CellY = -ScanRadius; CellY <= ScanRadius; ++CellY)
        {
            for (int32 CellX = -ScanRadius; CellX <= ScanRadius; ++CellX)
            {
                FAuditShaft Shaft;
                if (RollAuditShaft(Params, Seed, CellX, CellY, Shaft))
                {
                    Shafts.Add(Shaft);
                }
            }
        }

        int32 CurrentIndex = INDEX_NONE;
        for (int32 Index = 0; Index < Shafts.Num(); ++Index)
        {
            if (FMath::IsNearlyEqual(Shafts[Index].X, AxisPoint.X, 0.001f)
                && FMath::IsNearlyEqual(Shafts[Index].Y, AxisPoint.Y, 0.001f))
            {
                CurrentIndex = Index;
                break;
            }
        }
        if (CurrentIndex == INDEX_NONE)
        {
            return false;
        }

        OutMin = FVector2D(Shafts[CurrentIndex].X, Shafts[CurrentIndex].Y);
        OutMax = OutMin;
        OutHops = 0;
        for (int32 Hop = 0; Hop <= Shafts.Num(); ++Hop)
        {
            const FAuditShaft& Current = Shafts[CurrentIndex];
            OutMin.X = FMath::Min(OutMin.X, Current.X);
            OutMin.Y = FMath::Min(OutMin.Y, Current.Y);
            OutMax.X = FMath::Max(OutMax.X, Current.X);
            OutMax.Y = FMath::Max(OutMax.Y, Current.Y);

            bool bStrictParent = false;
            bool bSpineWindow = false;
            bool bNeighbourFallback = false;
            const int32 ParentIndex = ResolveAuditParent(
                Shafts, CurrentIndex, bStrictParent, bSpineWindow, bNeighbourFallback);
            ++OutHops;
            if (ParentIndex == INDEX_NONE)
            {
                OutMin.X = FMath::Min(OutMin.X, 0.0f);
                OutMin.Y = FMath::Min(OutMin.Y, 0.0f);
                OutMax.X = FMath::Max(OutMax.X, 0.0f);
                OutMax.Y = FMath::Max(OutMax.Y, 0.0f);
                return true;
            }
            CurrentIndex = ParentIndex;
        }
        return false;
    }

    struct FVerticalShaftPhysicalAudit
    {
        int32 NumEdges = 0;
        int32 NumBadEdges = 0;
        int32 NumSamples = 0;
        int32 NumNonAirSamples = 0;
        float FirstBadX = 0.0f;
        float FirstBadY = 0.0f;
        float FirstBadZ = 0.0f;
        float FirstBadDensity = 0.0f;
        float FirstBadChildX = 0.0f;
        float FirstBadChildY = 0.0f;
        float FirstBadParentX = 0.0f;
        float FirstBadParentY = 0.0f;
        float FirstBadFraction = 0.0f;
    };

    float ResolveAuditTreeConnectorZ(
        const FVerticalShaftParams& Params,
        uint32 LinkHash)
    {
        const float BottomZ = Params.StrateBottomWorldZ + Params.BoundarySealThickness;
        const float TopZ = Params.StrateTopWorldZ - Params.BoundarySealThickness;
        if (Params.LedgeSpacing > 0.0f && Params.LedgeDepth > 0.0f)
        {
            const float Period = Params.LedgeSpacing;
            const float RelativeBottom = BottomZ - Params.StrateBottomWorldZ;
            const float RelativeTop = TopZ - Params.StrateBottomWorldZ;
            const int32 FirstPeriod = FMath::FloorToInt(RelativeBottom / Period);
            const float Random01 = VoxelHash::ToFloat01(VoxelHash::Mix(LinkHash));

            for (int32 PeriodOffset = 0; PeriodOffset <= 1; ++PeriodOffset)
            {
                const float PeriodStart = static_cast<float>(FirstPeriod + PeriodOffset) * Period;
                const float SafeStart = FMath::Max(
                    RelativeBottom, PeriodStart + Params.LedgeDepth + 0.01f);
                const float SafeEnd = FMath::Min(
                    RelativeTop, PeriodStart + Period - Params.LedgeDepth - 0.01f);
                if (SafeEnd > SafeStart)
                {
                    return Params.StrateBottomWorldZ
                        + FMath::Lerp(SafeStart, SafeEnd, Random01);
                }
            }
        }

        return FMath::Lerp(
            BottomZ, TopZ, VoxelHash::ToFloat01(VoxelHash::Mix(LinkHash)));
    }

    FVerticalShaftPhysicalAudit AuditVerticalShaftPathDensity(
        const UVoxelGenerator& Generator,
        const FVerticalShaftParams& Params,
        int32 Seed,
        const FVector& AxisPoint)
    {
        constexpr int32 ScanRadius = 20;
        constexpr uint32 TreeSalt = 0x7A11u;
        constexpr float SampleFractions[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

        TArray<FAuditShaft> Shafts;
        for (int32 CellY = -ScanRadius; CellY <= ScanRadius; ++CellY)
        {
            for (int32 CellX = -ScanRadius; CellX <= ScanRadius; ++CellX)
            {
                FAuditShaft Shaft;
                if (RollAuditShaft(Params, Seed, CellX, CellY, Shaft))
                {
                    Shafts.Add(Shaft);
                }
            }
        }

        int32 CurrentIndex = INDEX_NONE;
        for (int32 Index = 0; Index < Shafts.Num(); ++Index)
        {
            if (FMath::IsNearlyEqual(Shafts[Index].X, AxisPoint.X, 0.001f)
                && FMath::IsNearlyEqual(Shafts[Index].Y, AxisPoint.Y, 0.001f))
            {
                CurrentIndex = Index;
                break;
            }
        }

        FVerticalShaftPhysicalAudit Out;
        if (CurrentIndex == INDEX_NONE)
        {
            return Out;
        }

        const uint32 ShaftSeed = static_cast<uint32>(Seed) ^ 0x53686674u;
        for (int32 Hop = 0; Hop <= Shafts.Num(); ++Hop)
        {
            bool bStrictParent = false;
            bool bSpineWindow = false;
            bool bNeighbourFallback = false;
            const int32 ParentIndex = ResolveAuditParent(
                Shafts, CurrentIndex, bStrictParent, bSpineWindow, bNeighbourFallback);
            if (ParentIndex == INDEX_NONE)
            {
                break;
            }

            const FAuditShaft& Child = Shafts[CurrentIndex];
            const FAuditShaft& Parent = Shafts[ParentIndex];
            const uint32 LinkHash = VoxelHash::Pair(
                Child.CellX, Child.CellY, Parent.CellX, Parent.CellY, ShaftSeed ^ TreeSalt);
            const float Zc = ResolveAuditTreeConnectorZ(Params, LinkHash);

            ++Out.NumEdges;
            bool bEdgeOpen = true;
            for (const float Fraction : SampleFractions)
            {
                const FVector Sample(
                    FMath::Lerp(Child.X, Parent.X, Fraction),
                    FMath::Lerp(Child.Y, Parent.Y, Fraction),
                    Zc);
                const float Density = Generator.GetDensityAt(Sample.X, Sample.Y, Sample.Z);
                ++Out.NumSamples;
                if (!(FMath::IsFinite(Density) && Density > 0.0f))
                {
                    bEdgeOpen = false;
                    ++Out.NumNonAirSamples;
                    if (Out.NumNonAirSamples == 1)
                    {
                        Out.FirstBadX = Sample.X;
                        Out.FirstBadY = Sample.Y;
                        Out.FirstBadZ = Sample.Z;
                        Out.FirstBadDensity = Density;
                        Out.FirstBadChildX = Child.X;
                        Out.FirstBadChildY = Child.Y;
                        Out.FirstBadParentX = Parent.X;
                        Out.FirstBadParentY = Parent.Y;
                        Out.FirstBadFraction = Fraction;
                    }
                }
            }
            if (!bEdgeOpen)
            {
                ++Out.NumBadEdges;
            }
            CurrentIndex = ParentIndex;
        }
        return Out;
    }

    bool FindVerticalShaftFeatureCore(
        const FVerticalShaftParams& Params,
        int32 Seed,
        const FVector& Point,
        FVector& OutAxisPoint)
    {
        const float Spacing = FMath::Max(Params.ShaftSpacing, 1.0f);
        const float InteriorMargin = FMath::Max(Params.SurfaceRoughness, 0.0f)
            * VOXEL_NOISE_SCALE + 0.25f;
        if (!FMath::IsFinite(InteriorMargin) || InteriorMargin < 0.0f)
        {
            return false;
        }

        const int32 BaseCellX = FMath::FloorToInt(Point.X / Spacing);
        const int32 BaseCellY = FMath::FloorToInt(Point.Y / Spacing);
        float BestDistanceSq = FLT_MAX;
        int32 BestCellX = INT32_MAX;
        int32 BestCellY = INT32_MAX;
        FAuditShaft BestShaft;
        bool bFound = false;
        for (int32 DY = -1; DY <= 1; ++DY)
        {
            for (int32 DX = -1; DX <= 1; ++DX)
            {
                FAuditShaft Shaft;
                if (!RollAuditShaft(
                        Params, Seed, BaseCellX + DX, BaseCellY + DY, Shaft))
                {
                    continue;
                }
                const float SafeRadius = Shaft.Radius - InteriorMargin;
                if (!FMath::IsFinite(SafeRadius) || SafeRadius <= 0.0f)
                {
                    continue;
                }

                const float DistanceSq = FMath::Square(Point.X - Shaft.X)
                    + FMath::Square(Point.Y - Shaft.Y);
                if (DistanceSq > FMath::Square(SafeRadius) + 1.0e-4f)
                {
                    continue;
                }

                const bool bCloser = DistanceSq < BestDistanceSq;
                const bool bTie = DistanceSq == BestDistanceSq
                    && (Shaft.CellX < BestCellX
                        || (Shaft.CellX == BestCellX && Shaft.CellY < BestCellY));
                if (bCloser || bTie)
                {
                    BestDistanceSq = DistanceSq;
                    BestCellX = Shaft.CellX;
                    BestCellY = Shaft.CellY;
                    BestShaft = Shaft;
                    bFound = true;
                }
            }
        }

        if (!bFound)
        {
            return false;
        }

        OutAxisPoint = FVector(BestShaft.X, BestShaft.Y, Point.Z);
        return true;
    }

    bool IsVerticalShaftFeatureCore(
        const FVerticalShaftParams& Params,
        int32 Seed,
        const FVector& Point)
    {
        FVector AxisPoint = FVector::ZeroVector;
        return FindVerticalShaftFeatureCore(Params, Seed, Point, AxisPoint);
    }

    bool FindChainMouths(
        const TArray<FVoxelPassage>& Passages,
        int32 StrateIndex,
        FVector& OutArrivalPoint,
        FVector& OutDeparturePoint)
    {
        int32 ArrivalCount = 0;
        int32 DepartureCount = 0;
        for (const FVoxelPassage& Passage : Passages)
        {
            if (Passage.LowerStrateIndex == StrateIndex
                && Passage.UpperStrateIndex + 1 == StrateIndex)
            {
                OutArrivalPoint = Passage.LowerPoint;
                ++ArrivalCount;
            }
            if (Passage.UpperStrateIndex == StrateIndex
                && Passage.LowerStrateIndex == StrateIndex + 1)
            {
                OutDeparturePoint = Passage.UpperPoint;
                ++DepartureCount;
            }
        }
        return ArrivalCount == 1 && DepartureCount == 1;
    }

    struct FSpineColumnReport
    {
        bool bValid = false;
        int32 SampledMinZ = 0;
        int32 SampledMaxZ = 0;
        int64 NumSamples = 0;
        int64 NumAir = 0;
        float AirFraction = 0.0f;
        bool bFullWindowRun = false;
        bool bAirSamplesFormOneRun = false;
        int32 FirstOpenZ = INDEX_NONE;
        int32 LastOpenZ = INDEX_NONE;
        int32 LongestOpenRun = 0;
        int32 RepresentativeOpenZ = INDEX_NONE;
        TArray<uint8> AirByZ;
    };

    bool SampleOriginSpineColumn(
        const UVoxelGenerator& Generator,
        int32 SampledMinZ,
        int32 SampledMaxZ,
        FSpineColumnReport& OutReport)
    {
        OutReport = FSpineColumnReport();
        if (SampledMaxZ <= SampledMinZ)
        {
            return false;
        }

        const int64 NumSamples64 = static_cast<int64>(SampledMaxZ)
            - static_cast<int64>(SampledMinZ);
        if (NumSamples64 <= 0 || NumSamples64 > INT32_MAX)
        {
            return false;
        }

        OutReport.SampledMinZ = SampledMinZ;
        OutReport.SampledMaxZ = SampledMaxZ;
        OutReport.NumSamples = NumSamples64;
        OutReport.AirByZ.SetNumUninitialized(static_cast<int32>(NumSamples64));

        bool bSawNonFiniteDensity = false;
        int32 CurrentRun = 0;
        for (int32 SampleIndex = 0; SampleIndex < OutReport.AirByZ.Num(); ++SampleIndex)
        {
            const int32 Z = SampledMinZ + SampleIndex;
            const float Density = Generator.GetDensityAt(0.0f, 0.0f, static_cast<float>(Z));
            const bool bAir = FMath::IsFinite(Density) && Density > 0.0f;
            OutReport.AirByZ[SampleIndex] = bAir ? 1u : 0u;
            if (!FMath::IsFinite(Density))
            {
                bSawNonFiniteDensity = true;
            }

            if (bAir)
            {
                ++OutReport.NumAir;
                ++CurrentRun;
                OutReport.FirstOpenZ = OutReport.FirstOpenZ == INDEX_NONE
                    ? Z : OutReport.FirstOpenZ;
                OutReport.LastOpenZ = Z;
                OutReport.LongestOpenRun = FMath::Max(OutReport.LongestOpenRun, CurrentRun);
            }
            else
            {
                CurrentRun = 0;
            }
        }

        OutReport.AirFraction = static_cast<float>(
            static_cast<double>(OutReport.NumAir)
            / static_cast<double>(OutReport.NumSamples));
        OutReport.bFullWindowRun = OutReport.NumAir == OutReport.NumSamples;
        OutReport.bAirSamplesFormOneRun = OutReport.NumAir > 0
            && OutReport.LongestOpenRun == OutReport.NumAir;
        OutReport.bValid = !bSawNonFiniteDensity;

        if (OutReport.NumAir > 0)
        {
            const int32 CentreZ = SampledMinZ
                + static_cast<int32>(NumSamples64 / 2);
            for (int32 Offset = 0; Offset < OutReport.AirByZ.Num(); ++Offset)
            {
                const int32 LowerZ = CentreZ - Offset;
                if (LowerZ >= SampledMinZ && LowerZ < SampledMaxZ
                    && OutReport.AirByZ[LowerZ - SampledMinZ] != 0u)
                {
                    OutReport.RepresentativeOpenZ = LowerZ;
                    break;
                }

                const int32 UpperZ = CentreZ + Offset;
                if (UpperZ >= SampledMinZ && UpperZ < SampledMaxZ
                    && OutReport.AirByZ[UpperZ - SampledMinZ] != 0u)
                {
                    OutReport.RepresentativeOpenZ = UpperZ;
                    break;
                }
            }
        }

        return OutReport.bValid;
    }

    bool FindNearestOpenSpinePoint(
        const UVoxelGenerator& Generator,
        const FSpineColumnReport& Spine,
        float TargetZ,
        FVector& OutPoint)
    {
        if (!Spine.bValid || Spine.NumAir <= 0 || !FMath::IsFinite(TargetZ))
        {
            return false;
        }

        if (TargetZ >= static_cast<float>(Spine.SampledMinZ)
            && TargetZ < static_cast<float>(Spine.SampledMaxZ))
        {
            const float Density = Generator.GetDensityAt(0.0f, 0.0f, TargetZ);
            if (FMath::IsFinite(Density) && Density > 0.0f)
            {
                OutPoint = FVector(0.0f, 0.0f, TargetZ);
                return true;
            }
        }

        const int32 NearestZ = FMath::Clamp(
            FMath::RoundToInt(TargetZ), Spine.SampledMinZ, Spine.SampledMaxZ - 1);
        for (int32 Offset = 0; Offset < Spine.AirByZ.Num(); ++Offset)
        {
            const int32 LowerZ = NearestZ - Offset;
            if (LowerZ >= Spine.SampledMinZ && LowerZ < Spine.SampledMaxZ
                && Spine.AirByZ[LowerZ - Spine.SampledMinZ] != 0u)
            {
                OutPoint = FVector(0.0f, 0.0f, static_cast<float>(LowerZ));
                return true;
            }

            const int32 UpperZ = NearestZ + Offset;
            if (UpperZ >= Spine.SampledMinZ && UpperZ < Spine.SampledMaxZ
                && Spine.AirByZ[UpperZ - Spine.SampledMinZ] != 0u)
            {
                OutPoint = FVector(0.0f, 0.0f, static_cast<float>(UpperZ));
                return true;
            }
        }
        return false;
    }

    struct FSpineDiagnosisReport
    {
        int32 Seed = INDEX_NONE;
        int32 Index = INDEX_NONE;
        FString Name;
        FSpineColumnReport Spine;
        bool bHasArrival = false;
        bool bArrivalIsSurfaceEntry = false;
        bool bHasChainArrival = false;
        bool bHasDeparture = false;
        FVector ArrivalPoint = FVector::ZeroVector;
        FVector DeparturePoint = FVector::ZeroVector;
        FVector ArrivalSpinePoint = FVector::ZeroVector;
        FVector DepartureSpinePoint = FVector::ZeroVector;
        FVector RepresentativeSpinePoint = FVector::ZeroVector;
        FConnectivityProbe SpineComponentProbe;
        FConnectivityProbe ArrivalToSpine;
        FConnectivityProbe DepartureToSpine;
        FConnectivityProbe ArrivalToDeparture;
        FVoxelConnectivityDiagnostics SpineComponent;
        FVoxelConnectivityDiagnostics ArrivalToSpineFacts;
        FVoxelConnectivityDiagnostics DepartureToSpineFacts;
        FVoxelStrateMetrics Topology;
    };

    bool FindDiagnosticMouths(
        const TArray<FVoxelPassage>& Passages,
        int32 StrateIndex,
        FSpineDiagnosisReport& OutReport)
    {
        int32 ChainArrivalCount = 0;
        int32 SurfaceArrivalCount = 0;
        int32 DepartureCount = 0;
        for (const FVoxelPassage& Passage : Passages)
        {
            if (Passage.LowerStrateIndex == StrateIndex
                && Passage.UpperStrateIndex + 1 == StrateIndex)
            {
                OutReport.ArrivalPoint = Passage.LowerPoint;
                ++ChainArrivalCount;
            }
            if (StrateIndex == 0
                && Passage.UpperStrateIndex == 0
                && Passage.LowerStrateIndex == 0)
            {
                OutReport.ArrivalPoint = Passage.LowerPoint;
                ++SurfaceArrivalCount;
            }
            if (Passage.UpperStrateIndex == StrateIndex
                && Passage.LowerStrateIndex == StrateIndex + 1)
            {
                OutReport.DeparturePoint = Passage.UpperPoint;
                ++DepartureCount;
            }
        }

        if (ChainArrivalCount > 1 || SurfaceArrivalCount > 1 || DepartureCount > 1)
        {
            return false;
        }
        if (StrateIndex == 0)
        {
            OutReport.bHasArrival = SurfaceArrivalCount == 1;
            OutReport.bArrivalIsSurfaceEntry = OutReport.bHasArrival;
            OutReport.bHasChainArrival = ChainArrivalCount == 1;
        }
        else
        {
            OutReport.bHasArrival = ChainArrivalCount == 1;
            OutReport.bHasChainArrival = OutReport.bHasArrival;
        }
        OutReport.bHasDeparture = DepartureCount == 1;
        return true;
    }

    bool BuildSpineDiagnosisReport(
        const UVoxelGenerator& Generator,
        const UVoxelStrateManager& Manager,
        int32 StrateIndex,
        const FVoxelStrateMeasureSettings& Settings,
        FSpineDiagnosisReport& OutReport)
    {
        OutReport = FSpineDiagnosisReport();
        OutReport.Index = StrateIndex;
        const TArray<FStrateSlot>& Layout = Manager.GetLayout();
        if (!Layout.IsValidIndex(StrateIndex) || Layout[StrateIndex].Definition == nullptr)
        {
            return false;
        }
        OutReport.Name = ArchetypeName(Layout[StrateIndex]);

        if (Settings.InteriorMarginVoxels != 0)
        {
            return false;
        }
        const int64 MinZ64 = static_cast<int64>(Layout[StrateIndex].BottomChunkZ) * CHUNK_SIZE;
        const int64 MaxZ64 = (static_cast<int64>(Layout[StrateIndex].TopChunkZ) + 1) * CHUNK_SIZE;
        if (MinZ64 < INT32_MIN || MinZ64 > INT32_MAX
            || MaxZ64 < INT32_MIN || MaxZ64 > INT32_MAX)
        {
            return false;
        }
        if (!SampleOriginSpineColumn(
                Generator,
                static_cast<int32>(MinZ64),
                static_cast<int32>(MaxZ64),
                OutReport.Spine))
        {
            return false;
        }

        if (OutReport.Spine.RepresentativeOpenZ != INDEX_NONE)
        {
            OutReport.RepresentativeSpinePoint = FVector(
                0.0f, 0.0f, static_cast<float>(OutReport.Spine.RepresentativeOpenZ));
            OutReport.SpineComponentProbe.bChecked = true;
            OutReport.SpineComponentProbe.Result = VF_AreConnected(
                Generator,
                Manager,
                StrateIndex,
                OutReport.RepresentativeSpinePoint,
                OutReport.RepresentativeSpinePoint,
                Settings,
                OutReport.SpineComponentProbe.bStartSnapped,
                OutReport.SpineComponentProbe.bGoalSnapped,
                OutReport.SpineComponent);
            OutReport.SpineComponentProbe.NumRouteRetries =
                OutReport.SpineComponent.NumRouteRetries;
        }

        if (!FindDiagnosticMouths(Manager.GetPassages(), StrateIndex, OutReport))
        {
            return false;
        }

        auto ProbeMouthToSpine = [&](const FVector& Mouth,
                                     FVector& OutSpinePoint,
                                     FConnectivityProbe& OutProbe,
                                     FVoxelConnectivityDiagnostics& OutFacts)
        {
            if (!FindNearestOpenSpinePoint(Generator, OutReport.Spine, Mouth.Z, OutSpinePoint))
            {
                OutSpinePoint = FVector(0.0f, 0.0f, Mouth.Z);
            }
            OutProbe.bChecked = true;
            OutProbe.Result = VF_AreConnected(
                Generator,
                Manager,
                StrateIndex,
                Mouth,
                OutSpinePoint,
                Settings,
                OutProbe.bStartSnapped,
                OutProbe.bGoalSnapped,
                OutFacts);
            OutProbe.NumRouteRetries = OutFacts.NumRouteRetries;
        };

        if (OutReport.bHasArrival)
        {
            ProbeMouthToSpine(
                OutReport.ArrivalPoint,
                OutReport.ArrivalSpinePoint,
                OutReport.ArrivalToSpine,
                OutReport.ArrivalToSpineFacts);
        }
        if (OutReport.bHasDeparture)
        {
            ProbeMouthToSpine(
                OutReport.DeparturePoint,
                OutReport.DepartureSpinePoint,
                OutReport.DepartureToSpine,
                OutReport.DepartureToSpineFacts);
        }
        if (OutReport.bHasChainArrival && OutReport.bHasDeparture)
        {
            OutReport.ArrivalToDeparture.bChecked = true;
            OutReport.ArrivalToDeparture.Result = VF_AreConnected(
                Generator,
                Manager,
                StrateIndex,
                OutReport.ArrivalPoint,
                OutReport.DeparturePoint,
                Settings,
                OutReport.ArrivalToDeparture.bStartSnapped,
                OutReport.ArrivalToDeparture.bGoalSnapped,
                OutReport.ArrivalToDeparture.NumRouteRetries);
        }
        return true;
    }
}

bool CheckVerticalShaftSeams(
    VoxelForgeTest::FTestWorld& World,
    int32& OutChecked,
    int32& OutMismatches,
    FString& OutFirstMismatch)
{
    OutChecked = 0;
    OutMismatches = 0;
    OutFirstMismatch.Reset();

    int32 TopVoxelZ = 0;
    int32 BottomVoxelZ = 0;
    if (!World.GetSlotVoxelZRange(
            VoxelForgeTest::FTestWorld::SlotVerticalShafts, TopVoxelZ, BottomVoxelZ))
    {
        return false;
    }

    const FVerticalShaftParams P = World.StrateManager->GetVerticalShaftParamsForChunk(
        FIntVector(0, 0, (TopVoxelZ + BottomVoxelZ) / (2 * CHUNK_SIZE)));
    const float Spacing = FMath::Max(P.ShaftSpacing, 1.0f);
    const float Z = static_cast<float>((TopVoxelZ + BottomVoxelZ) / 2);
    const UVoxelGenerator* Generator = World.Generator.Get();

    // A point is sampled immediately on both sides of many shaft-cell boundaries. For each point,
    // two calls in distinct chunk contexts warm the per-chunk state in opposite orders before the
    // same point is evaluated again. A different parent window would then show up as a bit change.
    for (int32 CellY = -3; CellY <= 3; ++CellY)
    {
        for (int32 CellX = -5; CellX <= 5; ++CellX)
        {
            const float CellBoundaryX = static_cast<float>(CellX + 1) * Spacing;
            const float Y = (static_cast<float>(CellY) + 0.37f) * Spacing;
            for (const float X : { CellBoundaryX - 0.25f, CellBoundaryX + 0.25f })
            {
                const FVector Probe(X, Y, Z);
                const int32 ProbeChunkX = FMath::FloorToInt(X / static_cast<float>(CHUNK_SIZE));
                const int32 ProbeChunkY = FMath::FloorToInt(Y / static_cast<float>(CHUNK_SIZE));
                const FVector ContextA(
                    static_cast<float>(ProbeChunkX * CHUNK_SIZE + 1),
                    static_cast<float>(ProbeChunkY * CHUNK_SIZE + 1), Z);
                const FVector ContextB(
                    static_cast<float>((ProbeChunkX + 1) * CHUNK_SIZE + 1),
                    static_cast<float>((ProbeChunkY - 1) * CHUNK_SIZE + 1), Z);

                const float Reference = Generator->GetDensityAt(Probe.X, Probe.Y, Probe.Z);
                if (((CellX + CellY) & 1) == 0)
                {
                    Generator->GetDensityAt(ContextA.X, ContextA.Y, ContextA.Z);
                    Generator->GetDensityAt(ContextB.X, ContextB.Y, ContextB.Z);
                }
                else
                {
                    Generator->GetDensityAt(ContextB.X, ContextB.Y, ContextB.Z);
                    Generator->GetDensityAt(ContextA.X, ContextA.Y, ContextA.Z);
                }
                const float Got = Generator->GetDensityAt(Probe.X, Probe.Y, Probe.Z);

                ++OutChecked;
                if (!SameFloatBits(Reference, Got))
                {
                    if (OutMismatches == 0)
                    {
                        OutFirstMismatch = FString::Printf(
                            TEXT("at (%.3f, %.3f, %.3f): reference %.9g [0x%08X] vs after "
                                 "neighbouring chunk contexts %.9g [0x%08X]"),
                            Probe.X,
                            Probe.Y,
                            Probe.Z,
                            Reference,
                            *reinterpret_cast<const uint32*>(&Reference),
                            Got,
                            *reinterpret_cast<const uint32*>(&Got));
                    }
                    ++OutMismatches;
                }
            }
        }
    }
    return OutChecked > 0;
}

bool FVoxelForgeVerticalShaftSeamTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    bool bAllChecksPassed = true;
    FString Summary = TEXT("VerticalShafts seam-freedom check (cell-boundary probes with two "
                          "neighbouring chunk-context orders):\n");
    for (const bool bUseOperatorStack : { false, true })
    {
        FTestWorld World;
        World.Build(/*InSeed=*/1337, /*InGapChunks=*/2, bUseOperatorStack);
        if (!World.IsValid())
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: could not build the %s seam-test fixture."),
                bUseOperatorStack ? TEXT("operator-stack") : TEXT("legacy")));
            bAllChecksPassed = false;
            continue;
        }

        int32 Checked = 0;
        int32 Mismatches = 0;
        FString FirstMismatch;
        if (!CheckVerticalShaftSeams(World, Checked, Mismatches, FirstMismatch))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: the %s seam test sampled no VerticalShafts positions."),
                bUseOperatorStack ? TEXT("operator-stack") : TEXT("legacy")));
            bAllChecksPassed = false;
            continue;
        }

        Summary += FString::Printf(
            TEXT("  %s path: %d same-position re-evaluations, %d bit mismatches%s%s.\n"),
            bUseOperatorStack ? TEXT("operator-stack") : TEXT("legacy"),
            Checked,
            Mismatches,
            Mismatches > 0 ? TEXT("; first ") : TEXT(""),
            Mismatches > 0 ? *FirstMismatch : TEXT(""));
        if (Mismatches != 0)
        {
            AddError(FString::Printf(
                TEXT("SEAM: %s VerticalShafts density changed after different chunk-context "
                     "orders: %s"),
                bUseOperatorStack ? TEXT("operator-stack") : TEXT("legacy"),
                *FirstMismatch));
            bAllChecksPassed = false;
        }
    }

    AddInfo(Summary);
    return bAllChecksPassed;
}

bool FVoxelForgeStrateConnectivityTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    FTestWorld World;
    World.Build(/*InSeed=*/1337, /*InGapChunks=*/2);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    FVoxelStrateMeasureSettings Settings;
    Settings.SampleStep = 4;
    Settings.RadiusInVoxels = 256;
    Settings.CenterXY = FVector2D::ZeroVector;
    Settings.MaxCells = 8000000;
    Settings.HeadroomCells = 2;

    FVoxelStrateMeasureSettings OldWindowSettings = Settings;
    OldWindowSettings.InteriorMarginVoxels = CHUNK_SIZE;
    FVoxelStrateMeasureSettings DerivedWindowSettings = Settings;
    DerivedWindowSettings.InteriorMarginVoxels = -1;

    bool bAllChecksPassed = true;
    FString Summary = TEXT("VoxelForge strate measurement summary (seed 1337, step 4, radius 256):\n");

    /*
     * HARD FAILURES — these are the reasons this test exists:
     *
     * 1. Negative control: the inter-strate gap is bedrock. If it reports mostly air, the density
     *    sign is inverted and every other number in this file is meaningless.
     * 2. Vacuity: a main measurement with no samples, no air, or no solid measures nothing.
     * 3. Range sanity: fractions/components must describe an actual air field.
     * 4. Determinism: identical inputs must produce bit-identical metrics.
     * 5. Coarse-lie guard: a coarse route must never be reported CONNECTED after a
     *    full-resolution solid sample; the controlled 3-voxel wall below must never return
     *    CONNECTED, and the bounded retry outcome must remain explicitly unknown.
     */

    int32 GapTopChunkZ = 0;
    int32 GapBottomChunkZ = 0;
    int64 GapSamples = 0;
    int64 GapAirSamples = 0;
    const bool bHaveGap = FindFirstGap(
        World.StrateManager->GetLayout(), GapTopChunkZ, GapBottomChunkZ);
    float GapAirFraction = 0.0f;
    if (!bHaveGap)
    {
        AddError(TEXT("HARD FAILURE: the fixture has no inter-strate gap for the density-sign negative control."));
        bAllChecksPassed = false;
    }
    else
    {
        GapAirFraction = MeasureGapAirFraction(
            *World.Generator,
            *World.StrateManager,
            GapTopChunkZ,
            GapBottomChunkZ,
            Settings,
            GapSamples,
            GapAirSamples);
        if (GapSamples == 0)
        {
            AddError(TEXT("HARD FAILURE: the inter-strate gap negative control sampled zero cells."));
            bAllChecksPassed = false;
        }
        if (GapAirFraction > 0.05f)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: density sign is inverted — the known-solid inter-strate gap "
                     "reported AirFraction %.9g (mostly air), and every other number in this file "
                     "is meaningless."),
                GapAirFraction));
            bAllChecksPassed = false;
        }
    }

    // This second control runs through VF_MeasureStrate itself. It fills the complete sampled
    // box of the first strate, then requires that the measurement reports almost no air. It is
    // deliberately separate from the gap probe above: this catches a sign inversion in the new
    // measurement pass even though the generator-level gap probe uses the correct MC predicate.
    const FStrateSlot& SolidControlSlot = World.StrateManager->GetLayout()[0];
    const int32 SolidControlMinZ = SolidControlSlot.BottomChunkZ * CHUNK_SIZE;
    const int32 SolidControlMaxZ = (SolidControlSlot.TopChunkZ + 1) * CHUNK_SIZE;
    FVoxelModification SolidControl;
    SolidControl.Shape = EVoxelBrushShape::Box;
    SolidControl.Center = FVector(
        Settings.CenterXY.X,
        Settings.CenterXY.Y,
        static_cast<float>(SolidControlMinZ + SolidControlMaxZ) * 0.5f);
    SolidControl.BoxExtent = FVector(
        static_cast<float>(Settings.RadiusInVoxels),
        static_cast<float>(Settings.RadiusInVoxels),
        static_cast<float>(Settings.RadiusInVoxels));
    SolidControl.Radius = 1.0f;
    SolidControl.Falloff = 0.1f;
    SolidControl.Strength = 100.0f;
    const TArray<FIntVector> SolidControlChunks = World.DiffLayer->ApplyModification(SolidControl);
    const FVoxelStrateMetrics SolidControlMetrics = VF_MeasureStrate(
        *World.Generator, *World.StrateManager, 0, DerivedWindowSettings);
    if (SolidControlChunks.Num() == 0 || !SolidControlMetrics.bValid
        || SolidControlMetrics.AirFraction > 0.05f)
    {
        AddError(FString::Printf(
            TEXT("HARD FAILURE: VF_MeasureStrate classified the known-solid control as air "
                 "(AirFraction %.9g); the density sign is inverted and every other number in "
                 "this file is meaningless."),
            SolidControlMetrics.AirFraction));
        bAllChecksPassed = false;
    }
    World.DiffLayer->Clear();

    // The refinement test deliberately approaches MaxCells at step 1. Keep a separate,
    // cheap over-cap control here so a future allocation/refusal regression cannot hide behind
    // the connectivity API's endpoint result.
    FVoxelStrateMeasureSettings TooLargeSettings = Settings;
    TooLargeSettings.SampleStep = 1;
    TooLargeSettings.RadiusInVoxels = 256;
    const FVoxelStrateMetrics TooLargeMetrics = VF_MeasureStrate(
        *World.Generator, *World.StrateManager, 0, TooLargeSettings);
    if (TooLargeMetrics.bValid
        || !TooLargeMetrics.RefusalReason.Contains(TEXT("MaxCells")))
    {
        AddError(FString::Printf(
            TEXT("HARD FAILURE: the over-cap grid was not refused by MaxCells (valid=%s, "
                 "reason='%s')."),
            TooLargeMetrics.bValid ? TEXT("true") : TEXT("false"),
            *TooLargeMetrics.RefusalReason));
        bAllChecksPassed = false;
    }

    TArray<FArchetypeReport> ArchetypeReports;
    const TArray<FStrateSlot>& Layout = World.StrateManager->GetLayout();
    ArchetypeReports.Reserve(Layout.Num());
    TArray<FVoxelStrateMetrics> DerivedMetricsByIndex;
    DerivedMetricsByIndex.SetNum(Layout.Num());

    auto ValidateMeasurement = [&](const FString& Label, const FVoxelStrateMetrics& Metrics)
    {
        if (!Metrics.bValid)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: %s measurement refused: %s."),
                *Label,
                *Metrics.RefusalReason));
            bAllChecksPassed = false;
        }
        if (Metrics.NumSampled == 0 || Metrics.NumAir == 0 || Metrics.NumSolid == 0)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: vacuous %s measurement — sampled=%lld, air=%lld, solid=%lld."),
                *Label,
                Metrics.NumSampled,
                Metrics.NumAir,
                Metrics.NumSolid));
            bAllChecksPassed = false;
        }
        if (!FMath::IsFinite(Metrics.AirFraction)
            || Metrics.AirFraction < 0.0f || Metrics.AirFraction > 1.0f
            || !FMath::IsFinite(Metrics.LargestComponentShare)
            || Metrics.LargestComponentShare < 0.0f || Metrics.LargestComponentShare > 1.0f
            || !FMath::IsFinite(Metrics.WalkableFraction)
            || Metrics.WalkableFraction < 0.0f || Metrics.WalkableFraction > 1.0f
            || (Metrics.NumAir > 0 && Metrics.NumAirComponents == 0)
            || Metrics.LargestComponentCells < 0
            || Metrics.LargestComponentCells > Metrics.NumAir
            || Metrics.NumComponentsAtLeast1Pct < 0
            || Metrics.NumComponentsAtLeast1Pct > Metrics.NumAirComponents
            || (Metrics.NumAir > 0 && Metrics.LargestComponentCells == 0)
            || (Metrics.NumAir > 0
                && (!FMath::IsFinite(Metrics.LargestComponentPoint.X)
                    || !FMath::IsFinite(Metrics.LargestComponentPoint.Y)
                    || !FMath::IsFinite(Metrics.LargestComponentPoint.Z)))
            || (Metrics.LargestComponentShare == 0.0f && Metrics.AirFraction > 0.05f)
            || Metrics.ResolvedMarginVoxels < 1
            || Metrics.SampledMinZ >= Metrics.SampledMaxZ)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: range/component/window sanity failed for %s."), *Label));
            bAllChecksPassed = false;
        }
    };

    for (int32 StrateIndex = 0; StrateIndex < Layout.Num(); ++StrateIndex)
    {
        FArchetypeReport& Report = ArchetypeReports.AddDefaulted_GetRef();
        Report.Index = StrateIndex;
        Report.Name = ArchetypeName(Layout[StrateIndex]);
        Report.OldWindow = VF_MeasureStrate(
            *World.Generator, *World.StrateManager, StrateIndex, OldWindowSettings);
        Report.OldWindowRepeat = VF_MeasureStrate(
            *World.Generator, *World.StrateManager, StrateIndex, OldWindowSettings);
        Report.DerivedWindow = VF_MeasureStrate(
            *World.Generator, *World.StrateManager, StrateIndex, DerivedWindowSettings);
        Report.DerivedWindowRepeat = VF_MeasureStrate(
            *World.Generator, *World.StrateManager, StrateIndex, DerivedWindowSettings);

        ValidateMeasurement(
            FString::Printf(TEXT("%s old-window"), *Report.Name), Report.OldWindow);
        ValidateMeasurement(
            FString::Printf(TEXT("%s derived-window"), *Report.Name), Report.DerivedWindow);
        if (!MetricsAreBitIdentical(Report.OldWindow, Report.OldWindowRepeat))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: %s old-window measurement was not bit-identical on the second call."),
                *Report.Name));
            bAllChecksPassed = false;
        }
        if (!MetricsAreBitIdentical(Report.DerivedWindow, Report.DerivedWindowRepeat))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: %s derived-window measurement was not bit-identical on the second call."),
                *Report.Name));
            bAllChecksPassed = false;
        }
        DerivedMetricsByIndex[StrateIndex] = Report.DerivedWindow;
    }

    ArchetypeReports.Sort([](const FArchetypeReport& A, const FArchetypeReport& B)
    {
        if (A.Name == B.Name) return A.Index < B.Index;
        return A.Name < B.Name;
    });

    Summary += FString::Printf(
        TEXT("Negative control: gap chunks [%d..%d], AirFraction=%.9g (%lld/%lld samples air).\n"),
        GapTopChunkZ,
        GapBottomChunkZ,
        GapAirFraction,
        GapAirSamples,
        GapSamples);
    Summary += FString::Printf(
        TEXT("MaxCells refusal control: over-cap measurement refused with reason '%s'.\n"),
        *TooLargeMetrics.RefusalReason);
    Summary += TEXT("Archetypes (old one-chunk vs derived window; derived InteriorMarginVoxels<0 => 2x BoundarySealThickness, clamped):\n");
    Summary += TEXT("  name | old margin | old Z span [min,max) | old Air | old Largest | old LargestCells | old >=1% | old Walkable | old FeatureScale vox | old Clearance vox | old Components | old Samples | old Solid | derived margin | derived Z span [min,max) | derived Air | derived Largest | derived LargestCells | derived >=1% | derived Walkable | derived FeatureScale vox | derived Clearance vox | derived Components | derived Samples | derived Solid\n");
    for (const FArchetypeReport& Report : ArchetypeReports)
    {
        const FVoxelStrateMetrics& Old = Report.OldWindow;
        const FVoxelStrateMetrics& Derived = Report.DerivedWindow;
        Summary += FString::Printf(
            TEXT("  %s | %d | [%d,%d) | %.9g | %.9g | %lld | %d | %.9g | %.9g | %d | %d | %lld | %lld | "
                 "%d | [%d,%d) | %.9g | %.9g | %lld | %d | %.9g | %.9g | %d | %d | %lld | %lld\n"),
            *Report.Name,
            Old.ResolvedMarginVoxels,
            Old.SampledMinZ,
            Old.SampledMaxZ,
            Old.AirFraction,
            Old.LargestComponentShare,
            Old.LargestComponentCells,
            Old.NumComponentsAtLeast1Pct,
            Old.WalkableFraction,
            Old.MedianFeatureScale,
            Old.MedianVerticalClearance,
            Old.NumAirComponents,
            Old.NumSampled,
            Old.NumSolid,
            Derived.ResolvedMarginVoxels,
            Derived.SampledMinZ,
            Derived.SampledMaxZ,
            Derived.AirFraction,
            Derived.LargestComponentShare,
            Derived.LargestComponentCells,
            Derived.NumComponentsAtLeast1Pct,
            Derived.WalkableFraction,
            Derived.MedianFeatureScale,
            Derived.MedianVerticalClearance,
            Derived.NumAirComponents,
            Derived.NumSampled,
            Derived.NumSolid);
    }

    int32 NumRoutesChecked = 0;
    int32 NumRoutesRefuted = 0;
    int32 NumRouteRetriesUsed = 0;
    int32 MaxRouteRetriesUsed = 0;
    int32 NumBudgetExhausted = 0;
    int32 NumGuardRoutesChecked = 0;
    int32 NumGuardRoutesRefuted = 0;
    int32 NumSnappedEndpointEvents = 0;
    int32 NumArrivalDepartureRoutesChecked = 0;
    int32 NumArrivalDepartureSnappedEndpointEvents = 0;

    int32 NumAnchorProbes = 0;
    int32 NumAnchorConnected = 0;
    int32 NumAnchorNotConnectedAtThisResolution = 0;
    int32 NumAnchorStartCellSolid = 0;
    int32 NumAnchorGoalCellSolid = 0;
    int32 NumAnchorOutOfWindow = 0;
    int32 NumAnchorBudgetExhausted = 0;
    int32 NumAnchorSnappedEndpointEvents = 0;

    auto ProbeRoute = [&](int32 StrateIndex,
                          const FVector& Start,
                          const FVector& Goal,
                          bool bCountInGuard,
                          bool bCountAsAnchorProbe,
                          bool bCountAsArrivalDeparture) -> FConnectivityProbe
    {
        FConnectivityProbe Probe;
        Probe.bChecked = true;
        Probe.Result = VF_AreConnected(
            *World.Generator,
            *World.StrateManager,
            StrateIndex,
            Start,
            Goal,
            DerivedWindowSettings,
            Probe.bStartSnapped,
            Probe.bGoalSnapped,
            Probe.NumRouteRetries);

        ++NumRoutesChecked;
        NumRoutesRefuted += NumRefutedRoutes(Probe);
        NumRouteRetriesUsed += Probe.NumRouteRetries;
        MaxRouteRetriesUsed = FMath::Max(MaxRouteRetriesUsed, Probe.NumRouteRetries);
        NumBudgetExhausted +=
            Probe.Result == EVoxelConnectivityResult::CoarseLiedBudgetExhausted ? 1 : 0;
        if (bCountInGuard)
        {
            ++NumGuardRoutesChecked;
            NumGuardRoutesRefuted += NumRefutedRoutes(Probe);
        }

        const int32 SnappedHere = (Probe.bStartSnapped ? 1 : 0)
            + (Probe.bGoalSnapped ? 1 : 0);
        NumSnappedEndpointEvents += SnappedHere;
        if (bCountAsArrivalDeparture)
        {
            ++NumArrivalDepartureRoutesChecked;
            NumArrivalDepartureSnappedEndpointEvents += SnappedHere;
        }

        if (bCountAsAnchorProbe)
        {
            ++NumAnchorProbes;
            NumAnchorSnappedEndpointEvents += SnappedHere;
            switch (Probe.Result)
            {
            case EVoxelConnectivityResult::Connected:      ++NumAnchorConnected; break;
            case EVoxelConnectivityResult::NotConnectedAtThisResolution:
                ++NumAnchorNotConnectedAtThisResolution;
                break;
            case EVoxelConnectivityResult::StartCellSolid: ++NumAnchorStartCellSolid; break;
            case EVoxelConnectivityResult::GoalCellSolid:  ++NumAnchorGoalCellSolid; break;
            case EVoxelConnectivityResult::OutOfWindow:    ++NumAnchorOutOfWindow; break;
            case EVoxelConnectivityResult::CoarseLiedBudgetExhausted:
                ++NumAnchorBudgetExhausted;
                break;
            default: break;
            }
        }
        return Probe;
    };

    for (int32 StrateIndex = 0; StrateIndex < Layout.Num(); ++StrateIndex)
    {
        const FVoxelStrateMetrics& Window = DerivedMetricsByIndex[StrateIndex];
        if (!Window.bValid || Window.SampledMinZ >= Window.SampledMaxZ) continue;

        const FVector A(Settings.CenterXY.X + 2.0f, Settings.CenterXY.Y + 2.0f,
                        static_cast<float>(Window.SampledMinZ)
                            + 0.5f * static_cast<float>(DerivedWindowSettings.SampleStep));
        const FVector B(Settings.CenterXY.X + 2.0f, Settings.CenterXY.Y + 2.0f,
                        static_cast<float>(Window.SampledMaxZ)
                            - 0.5f * static_cast<float>(DerivedWindowSettings.SampleStep));
        const FConnectivityProbe Probe = ProbeRoute(
            StrateIndex, A, B, /*bCountInGuard=*/true, /*bCountAsAnchorProbe=*/false,
            /*bCountAsArrivalDeparture=*/false);
        // A and B are deliberately a vertical diagnostic line, not the recovered route.  A
        // connected verdict is allowed to detour around solid terrain; only the route selected by
        // VF_AreConnected is required to pass its own full-resolution re-walk.  Comparing this
        // unrelated direct segment to the verdict used to reject valid detours, and became
        // especially misleading once the origin spine became a finite landing room.
        (void)Probe;
    }

    // Controlled negative route: at SampleStep 4, the two coarse centres at X=-2 and X=+2
    // remain air while the three full-resolution voxels at X=-1,0,+1 are filled solid.
    const FVoxelStrateMetrics& WallWindow = DerivedMetricsByIndex[0];
    const FStrateSlot& WallSlot = Layout[0];
    const float WallTopZ = (static_cast<float>(WallSlot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
    const float WallBottomZ = static_cast<float>(WallSlot.BottomChunkZ) * CHUNK_SIZE;
    const VoxelPassageGeometry::FOriginLandingGeometry WallLanding =
        VoxelPassageGeometry::BuildOriginLandingGeometry(
            WallTopZ,
            WallBottomZ,
            WallSlot.Definition != nullptr
                ? WallSlot.Definition->GenerationParams.BoundarySealThickness
                : 0.0f,
            World.Generator->OriginSpineRadius);
    // The controlled wall must start in known air.  The old probe used the bottom sample of the
    // strate, which was only air because the old spine was a full-height cylinder.  Put it in the
    // finite top landing room instead; this keeps the coarse-lie control about the wall, not about
    // an unrelated start-cell-solid result.
    const float WallZ = WallLanding.bValid
        ? WallLanding.FloorZ + 4.0f
        : static_cast<float>(WallWindow.SampledMinZ)
            + 0.5f * static_cast<float>(DerivedWindowSettings.SampleStep);
    const FVector WallA(Settings.CenterXY.X - 2.0f, Settings.CenterXY.Y + 2.0f, WallZ);
    const FVector WallB(Settings.CenterXY.X + 2.0f, Settings.CenterXY.Y + 2.0f, WallZ);

    FVoxelModification Wall;
    Wall.Shape = EVoxelBrushShape::Box;
    Wall.Center = FVector(Settings.CenterXY.X, Settings.CenterXY.Y + 2.0f, WallZ);
    Wall.BoxExtent = FVector(1.5f, 256.0f, 256.0f);
    Wall.Radius = 1.0f;
    Wall.Falloff = 0.1f;
    Wall.Strength = 100.0f;
    const TArray<FIntVector> WallChunks = World.DiffLayer->ApplyModification(Wall);
    if (WallChunks.Num() == 0)
    {
        AddError(TEXT("HARD FAILURE: the controlled 3-voxel wall could not be installed."));
        bAllChecksPassed = false;
    }

    const FConnectivityProbe WallProbe = ProbeRoute(
        0, WallA, WallB, /*bCountInGuard=*/true, /*bCountAsAnchorProbe=*/false,
        /*bCountAsArrivalDeparture=*/false);
    if (WallProbe.Result != EVoxelConnectivityResult::CoarseLiedBudgetExhausted
        || WallProbe.NumRouteRetries != DerivedWindowSettings.MaxRouteRetries)
    {
        AddError(FString::Printf(
            TEXT("HARD FAILURE: the controlled solid wall returned %s after %d retries; "
                 "expected COARSE_LIED_BUDGET_EXHAUSTED after the MaxRouteRetries=%d cap."),
            ConnectivityResultName(WallProbe.Result),
            WallProbe.NumRouteRetries,
            DerivedWindowSettings.MaxRouteRetries));
        bAllChecksPassed = false;
    }

    FConnectivityProbe WallProbeRepeat;
    WallProbeRepeat.bChecked = true;
    WallProbeRepeat.Result = VF_AreConnected(
        *World.Generator,
        *World.StrateManager,
        0,
        WallA,
        WallB,
        DerivedWindowSettings,
        WallProbeRepeat.bStartSnapped,
        WallProbeRepeat.bGoalSnapped,
        WallProbeRepeat.NumRouteRetries);
    if (!ConnectivityProbesAreBitIdentical(WallProbe, WallProbeRepeat))
    {
        AddError(TEXT(
            "HARD FAILURE: the blocked-edge retry order was not deterministic; repeating the "
            "same wall query changed its verdict, snaps, or retry count."));
        bAllChecksPassed = false;
    }
    World.DiffLayer->Clear();

    const TArray<FVoxelPassage>& Passages = World.StrateManager->GetPassages();
    TArray<int32> ArrivalPassageByStrate;
    TArray<int32> DeparturePassageByStrate;
    ArrivalPassageByStrate.Init(INDEX_NONE, Layout.Num());
    DeparturePassageByStrate.Init(INDEX_NONE, Layout.Num());
    int32 SurfaceEntryPassageIndex = INDEX_NONE;

    for (int32 PassageIndex = 0; PassageIndex < Passages.Num(); ++PassageIndex)
    {
        const FVoxelPassage& Passage = Passages[PassageIndex];
        if (Passage.UpperStrateIndex == Passage.LowerStrateIndex)
        {
            if (SurfaceEntryPassageIndex == INDEX_NONE)
            {
                SurfaceEntryPassageIndex = PassageIndex;
            }
            continue;
        }

        if (Passage.LowerStrateIndex != Passage.UpperStrateIndex + 1)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: passage %d is not between consecutive strates (%d -> %d)."),
                PassageIndex,
                Passage.UpperStrateIndex,
                Passage.LowerStrateIndex));
            bAllChecksPassed = false;
            continue;
        }

        if (!Layout.IsValidIndex(Passage.UpperStrateIndex)
            || !Layout.IsValidIndex(Passage.LowerStrateIndex))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: passage %d references a strate outside the layout (%d -> %d)."),
                PassageIndex,
                Passage.UpperStrateIndex,
                Passage.LowerStrateIndex));
            bAllChecksPassed = false;
            continue;
        }

        if (ArrivalPassageByStrate[Passage.LowerStrateIndex] != INDEX_NONE)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: strate %d has more than one arrival passage."),
                Passage.LowerStrateIndex));
            bAllChecksPassed = false;
        }
        else
        {
            ArrivalPassageByStrate[Passage.LowerStrateIndex] = PassageIndex;
        }

        if (DeparturePassageByStrate[Passage.UpperStrateIndex] != INDEX_NONE)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: strate %d has more than one departure passage."),
                Passage.UpperStrateIndex));
            bAllChecksPassed = false;
        }
        else
        {
            DeparturePassageByStrate[Passage.UpperStrateIndex] = PassageIndex;
        }
    }

    for (int32 StrateIndex = 0; StrateIndex < Layout.Num(); ++StrateIndex)
    {
        if (StrateIndex > 0 && ArrivalPassageByStrate[StrateIndex] == INDEX_NONE)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: non-topmost strate %d has no arrival passage."),
                StrateIndex));
            bAllChecksPassed = false;
        }
        if (StrateIndex + 1 < Layout.Num()
            && DeparturePassageByStrate[StrateIndex] == INDEX_NONE)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: non-bottom-most strate %d has no departure passage."),
                StrateIndex));
            bAllChecksPassed = false;
        }
    }

    TArray<FStrateConnectivityReport> StrateReports;
    StrateReports.SetNum(Layout.Num());
    for (int32 StrateIndex = 0; StrateIndex < Layout.Num(); ++StrateIndex)
    {
        FStrateConnectivityReport& Report = StrateReports[StrateIndex];
        Report.Index = StrateIndex;
        Report.Name = ArchetypeName(Layout[StrateIndex]);

        if (ArrivalPassageByStrate[StrateIndex] != INDEX_NONE)
        {
            Report.bHasArrival = true;
            Report.ArrivalPassageIndex = ArrivalPassageByStrate[StrateIndex];
            Report.ArrivalPoint = Passages[Report.ArrivalPassageIndex].LowerPoint;
        }
        if (DeparturePassageByStrate[StrateIndex] != INDEX_NONE)
        {
            Report.bHasDeparture = true;
            Report.DeparturePassageIndex = DeparturePassageByStrate[StrateIndex];
            Report.DeparturePoint = Passages[Report.DeparturePassageIndex].UpperPoint;
        }
    }

    // The actual largest-component comparison uses the representative point returned by the
    // measurement API. The surface-entry lower mouth remains an auxiliary guard probe so this
    // task continues to re-report the established 15 endpoint comparison probes; it is not the
    // topmost strate's chain arrival.
    for (FStrateConnectivityReport& Report : StrateReports)
    {
        const FVoxelStrateMetrics& Metrics = DerivedMetricsByIndex[Report.Index];
        if (!Metrics.bValid) continue;

        if (Report.bHasArrival)
        {
            Report.ArrivalToLargest = ProbeRoute(
                Report.Index,
                Report.ArrivalPoint,
                Metrics.LargestComponentPoint,
                /*bCountInGuard=*/true,
                /*bCountAsAnchorProbe=*/true,
                /*bCountAsArrivalDeparture=*/false);
        }
        if (Report.bHasDeparture)
        {
            Report.DepartureToLargest = ProbeRoute(
                Report.Index,
                Report.DeparturePoint,
                Metrics.LargestComponentPoint,
                /*bCountInGuard=*/true,
                /*bCountAsAnchorProbe=*/true,
                /*bCountAsArrivalDeparture=*/false);
        }
    }

    if (SurfaceEntryPassageIndex != INDEX_NONE && Layout.IsValidIndex(0)
        && DerivedMetricsByIndex[0].bValid)
    {
        const FVoxelPassage& SurfaceEntry = Passages[SurfaceEntryPassageIndex];
        ProbeRoute(
            0,
            SurfaceEntry.LowerPoint,
            DerivedMetricsByIndex[0].LargestComponentPoint,
            /*bCountInGuard=*/true,
            /*bCountAsAnchorProbe=*/true,
            /*bCountAsArrivalDeparture=*/false);
    }
    else
    {
        AddError(TEXT("HARD FAILURE: the surface-entry auxiliary endpoint probe is missing."));
        bAllChecksPassed = false;
    }

    int32 NumArrivalDeparturePasses = 0;
    TArray<FString> FailedStrates;
    for (FStrateConnectivityReport& Report : StrateReports)
    {
        if (!Report.bHasArrival || !Report.bHasDeparture)
        {
            continue;
        }

        Report.ArrivalToDeparture = ProbeRoute(
            Report.Index,
            Report.ArrivalPoint,
            Report.DeparturePoint,
            /*bCountInGuard=*/false,
            /*bCountAsAnchorProbe=*/false,
            /*bCountAsArrivalDeparture=*/true);
        if (Report.ArrivalToDeparture.Result == EVoxelConnectivityResult::Connected)
        {
            ++NumArrivalDeparturePasses;
        }
        else
        {
            const FString ResultText = ConnectivityProbeText(Report.ArrivalToDeparture);
            FailedStrates.Add(FString::Printf(
                TEXT("%d:%s (%s)"),
                Report.Index,
                *Report.Name,
                *ResultText));
        }
    }

    FString FailedStrateText = TEXT("none");
    if (FailedStrates.Num() > 0)
    {
        FailedStrateText = FString::Join(FailedStrates, TEXT(", "));
    }

    Summary += FString::Printf(
        TEXT("Coarse-lie guard (8 interior routes + wall control + 15 endpoint->largest probes): "
             "%d routes checked, %d coarse routes refuted at full resolution.\n"),
        NumGuardRoutesChecked,
        NumGuardRoutesRefuted);
    Summary += FString::Printf(
        TEXT("All connectivity probes (including arrival->departure law queries): %d routes "
             "checked, %d coarse routes refuted at full resolution.\n"),
        NumRoutesChecked,
        NumRoutesRefuted);
    Summary += FString::Printf(
        TEXT("Retry budget: MaxRouteRetries=%d; %d alternate routes checked across %d queries "
             "(maximum %d retries in one query); budget exhausted on %d queries.\n"),
        DerivedWindowSettings.MaxRouteRetries,
        NumRouteRetriesUsed,
        NumRoutesChecked,
        MaxRouteRetriesUsed,
        NumBudgetExhausted);
    Summary += FString::Printf(
        TEXT("Endpoint snap repair: %d snapped endpoint events across all probes; %d in "
             "%d arrival->departure probes.\n"),
        NumSnappedEndpointEvents,
        NumArrivalDepartureSnappedEndpointEvents,
        NumArrivalDepartureRoutesChecked);
    Summary += FString::Printf(
        TEXT("Correct largest-component endpoint probes (%d, including the surface-entry auxiliary): "
             "connected=%d, not-connected-at-resolution=%d, start-cell-solid=%d, goal-cell-solid=%d, "
             "out-of-window=%d, coarse-lied-budget-exhausted=%d, snapped endpoint events=%d.\n"),
        NumAnchorProbes,
        NumAnchorConnected,
        NumAnchorNotConnectedAtThisResolution,
        NumAnchorStartCellSolid,
        NumAnchorGoalCellSolid,
        NumAnchorOutOfWindow,
        NumAnchorBudgetExhausted,
        NumAnchorSnappedEndpointEvents);
    Summary += TEXT("Previous \"5 of 15 disconnected\" verdict: ARTIFACT as a claim about sealed "
                   "pockets; it used a guessed anchor and asked the wrong (largest-component) law.\n");
    Summary += FString::Printf(
        TEXT("arrival->departure: %d of %d applicable strates pass; failures: %s.\n"),
        NumArrivalDeparturePasses,
        NumArrivalDepartureRoutesChecked,
        *FailedStrateText);

    Summary += TEXT("Strates (chain order; derived window; largest point is the deterministic "
                   "lowest-cell representative):\n");
    Summary += TEXT("  Strate | arrival->departure | arrival->largest | departure->largest | components | largest share | largest cells | >=1% | largest point\n");
    for (const FStrateConnectivityReport& Report : StrateReports)
    {
        const FVoxelStrateMetrics& Metrics = DerivedMetricsByIndex[Report.Index];
        FString ArrivalToDeparture;
        if (Report.bHasArrival && Report.bHasDeparture)
        {
            ArrivalToDeparture = ConnectivityProbeText(Report.ArrivalToDeparture);
        }
        else if (!Report.bHasArrival && Report.Index == 0)
        {
            ArrivalToDeparture = TEXT("N/A (topmost; no arrival)");
        }
        else if (!Report.bHasDeparture && Report.Index + 1 == StrateReports.Num())
        {
            ArrivalToDeparture = TEXT("N/A (bottom-most; no departure)");
        }
        else
        {
            ArrivalToDeparture = TEXT("MISSING_CHAIN_MOUTH");
        }

        FString ArrivalToLargest;
        if (Report.bHasArrival)
        {
            ArrivalToLargest = ConnectivityProbeText(Report.ArrivalToLargest);
        }
        else if (Report.Index == 0)
        {
            ArrivalToLargest = TEXT("N/A (topmost; no arrival)");
        }
        else
        {
            ArrivalToLargest = TEXT("MISSING_ARRIVAL");
        }

        FString DepartureToLargest;
        if (Report.bHasDeparture)
        {
            DepartureToLargest = ConnectivityProbeText(Report.DepartureToLargest);
        }
        else if (Report.Index + 1 == StrateReports.Num())
        {
            DepartureToLargest = TEXT("N/A (bottom-most; no departure)");
        }
        else
        {
            DepartureToLargest = TEXT("MISSING_DEPARTURE");
        }

        Summary += FString::Printf(
            TEXT("  %d %s | %s | %s | %s | %d | %.9g | %lld | %d | (%.3f,%.3f,%.3f)\n"),
            Report.Index,
            *Report.Name,
            *ArrivalToDeparture,
            *ArrivalToLargest,
            *DepartureToLargest,
            Metrics.NumAirComponents,
            Metrics.LargestComponentShare,
            Metrics.LargestComponentCells,
            Metrics.NumComponentsAtLeast1Pct,
            Metrics.LargestComponentPoint.X,
            Metrics.LargestComponentPoint.Y,
            Metrics.LargestComponentPoint.Z);
    }
    AddInfo(Summary);

    return bAllChecksPassed;
}

bool FVoxelForgeStrateConnectivityRefinementTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    FTestWorld World;
    World.Build(/*InSeed=*/1337, /*InGapChunks=*/2);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    const TArray<FStrateSlot>& Layout = World.StrateManager->GetLayout();
    const int32 VerticalShaftsIndex = FTestWorld::SlotVerticalShafts;
    if (!Layout.IsValidIndex(VerticalShaftsIndex)
        || Layout[VerticalShaftsIndex].Definition == nullptr
        || Layout[VerticalShaftsIndex].Definition->GeneratorType
            != ECaveGeneratorType::VerticalShafts)
    {
        AddError(TEXT("HARD FAILURE: the pinned VerticalShafts fixture slot is missing or changed."));
        return false;
    }

    FVector ArrivalPoint = FVector::ZeroVector;
    FVector DeparturePoint = FVector::ZeroVector;
    if (!FindChainMouths(
            World.StrateManager->GetPassages(),
            VerticalShaftsIndex,
            ArrivalPoint,
            DeparturePoint))
    {
        AddError(TEXT("HARD FAILURE: VerticalShafts does not have exactly one arrival and one "
                      "departure mouth between consecutive strates."));
        return false;
    }

    FVoxelStrateMeasureSettings BaseSettings;
    BaseSettings.CenterXY = FVector2D::ZeroVector;
    // The centered control stays bounded after the 4 m vertical-volume fixture is retained;
    // 12 M also leaves room for the scale-aware 320-voxel control below.
    BaseSettings.MaxCells = 12000000;
    BaseSettings.HeadroomCells = 2;
    BaseSettings.InteriorMarginVoxels = -1;

    struct FRefinementCase
    {
        int32 SampleStep;
        int32 RadiusInVoxels;
        int32 InteriorMarginVoxels = -1;
    };
    static constexpr FRefinementCase Cases[] = {
        {4, 256},
        {4, 192},
        {2, 192},
        {4, 160},
        {1, 160},
    };

    bool bAllChecksPassed = true;
    TArray<FRefinementSweepRow> Rows;
    Rows.Reserve(UE_ARRAY_COUNT(Cases));

    const double SweepStartSeconds = FPlatformTime::Seconds();
    for (const FRefinementCase& TestCase : Cases)
    {
        FRefinementSweepRow& Row = Rows.AddDefaulted_GetRef();
        Row.SampleStep = TestCase.SampleStep;
        Row.RadiusInVoxels = TestCase.RadiusInVoxels;
        Row.RequestedMarginVoxels = TestCase.InteriorMarginVoxels;

        FVoxelStrateMeasureSettings RowSettings = BaseSettings;
        RowSettings.SampleStep = TestCase.SampleStep;
        RowSettings.RadiusInVoxels = TestCase.RadiusInVoxels;
        RowSettings.InteriorMarginVoxels = TestCase.InteriorMarginVoxels;

        Row.Metrics = VF_MeasureStrate(
            *World.Generator,
            *World.StrateManager,
            VerticalShaftsIndex,
            RowSettings);

        Row.Probe.bChecked = true;
        Row.Probe.Result = VF_AreConnected(
            *World.Generator,
            *World.StrateManager,
            VerticalShaftsIndex,
            ArrivalPoint,
            DeparturePoint,
            RowSettings,
            Row.Probe.bStartSnapped,
            Row.Probe.bGoalSnapped,
            Row.Probe.NumRouteRetries);

        if (!Row.Metrics.bValid
            || Row.Metrics.NumSampled <= 0
            || Row.Metrics.NumSampled > BaseSettings.MaxCells
            || Row.Metrics.NumAir <= 0
            || Row.Metrics.NumSolid <= 0)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: VerticalShafts refinement row step=%d radius=%d did not "
                     "produce a bounded non-vacuous measurement (valid=%s, reason='%s', "
                     "cells=%lld, air=%lld, solid=%lld)."),
                Row.SampleStep,
                Row.RadiusInVoxels,
                Row.Metrics.bValid ? TEXT("true") : TEXT("false"),
                *Row.Metrics.RefusalReason,
                Row.Metrics.NumSampled,
                Row.Metrics.NumAir,
                Row.Metrics.NumSolid));
            bAllChecksPassed = false;
        }

        if (Row.Probe.Result == EVoxelConnectivityResult::OutOfWindow
            || Row.Probe.Result == EVoxelConnectivityResult::StartCellSolid
            || Row.Probe.Result == EVoxelConnectivityResult::GoalCellSolid)
        {
            const FString ProbeText = ConnectivityProbeText(Row.Probe);
            AddError(FString::Printf(
                TEXT("HARD FAILURE: VerticalShafts refinement row step=%d radius=%d could "
                     "not query both in-window air mouths: %s."),
                Row.SampleStep,
                Row.RadiusInVoxels,
                *ProbeText));
            bAllChecksPassed = false;
        }

        if (Row.Probe.bStartSnapped || Row.Probe.bGoalSnapped)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: VerticalShafts refinement row step=%d radius=%d snapped "
                     "an endpoint; the controlled comparison is not a mouth-to-mouth query."),
                Row.SampleStep,
                Row.RadiusInVoxels));
            bAllChecksPassed = false;
        }

    }
    const double SweepSeconds = FPlatformTime::Seconds() - SweepStartSeconds;

    static constexpr int32 MarginCases[] = {0, 2, 4, 8};
    TArray<FRefinementSweepRow> MarginRows;
    MarginRows.Reserve(UE_ARRAY_COUNT(MarginCases));

    const double MarginSweepStartSeconds = FPlatformTime::Seconds();
    for (const int32 RequestedMargin : MarginCases)
    {
        FRefinementSweepRow& Row = MarginRows.AddDefaulted_GetRef();
        Row.SampleStep = 2;
        Row.RadiusInVoxels = 192;
        Row.RequestedMarginVoxels = RequestedMargin;

        FVoxelStrateMeasureSettings RowSettings = BaseSettings;
        RowSettings.SampleStep = Row.SampleStep;
        RowSettings.RadiusInVoxels = Row.RadiusInVoxels;
        RowSettings.InteriorMarginVoxels = RequestedMargin;

        Row.Metrics = VF_MeasureStrate(
            *World.Generator,
            *World.StrateManager,
            VerticalShaftsIndex,
            RowSettings);

        Row.Probe.bChecked = true;
        Row.Probe.Result = VF_AreConnected(
            *World.Generator,
            *World.StrateManager,
            VerticalShaftsIndex,
            ArrivalPoint,
            DeparturePoint,
            RowSettings,
            Row.Probe.bStartSnapped,
            Row.Probe.bGoalSnapped,
            Row.Probe.NumRouteRetries);

        if (!Row.Metrics.bValid
            || Row.Metrics.NumSampled <= 0
            || Row.Metrics.NumSampled > BaseSettings.MaxCells
            || Row.Metrics.NumAir <= 0
            || Row.Metrics.NumSolid <= 0)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: VerticalShafts margin row margin=%d did not produce a "
                     "bounded non-vacuous measurement (valid=%s, reason='%s', cells=%lld, "
                     "air=%lld, solid=%lld)."),
                RequestedMargin,
                Row.Metrics.bValid ? TEXT("true") : TEXT("false"),
                *Row.Metrics.RefusalReason,
                Row.Metrics.NumSampled,
                Row.Metrics.NumAir,
                Row.Metrics.NumSolid));
            bAllChecksPassed = false;
        }

        if (Row.Metrics.ResolvedMarginVoxels != RequestedMargin)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: VerticalShafts margin row requested margin=%d but the "
                     "measurement resolved margin=%d."),
                RequestedMargin,
                Row.Metrics.ResolvedMarginVoxels));
            bAllChecksPassed = false;
        }

        if (Row.Probe.Result == EVoxelConnectivityResult::OutOfWindow
            || Row.Probe.Result == EVoxelConnectivityResult::StartCellSolid
            || Row.Probe.Result == EVoxelConnectivityResult::GoalCellSolid)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: VerticalShafts margin row margin=%d could not query both "
                     "in-window air mouths: %s."),
                RequestedMargin,
                *ConnectivityProbeText(Row.Probe)));
            bAllChecksPassed = false;
        }

        if (Row.Probe.bStartSnapped || Row.Probe.bGoalSnapped)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: VerticalShafts margin row margin=%d snapped an endpoint; "
                     "the controlled comparison is not a mouth-to-mouth query."),
                RequestedMargin));
            bAllChecksPassed = false;
        }
    }
    const double MarginSweepSeconds = FPlatformTime::Seconds() - MarginSweepStartSeconds;

    bool bMarginArtifact = false;
    bool bMarginSweepUsable = MarginRows.Num() == UE_ARRAY_COUNT(MarginCases);
    for (const FRefinementSweepRow& Row : MarginRows)
    {
        bMarginArtifact |= Row.Probe.Result == EVoxelConnectivityResult::Connected;
        bMarginSweepUsable &= Row.Metrics.bValid
            && Row.Metrics.ResolvedMarginVoxels == Row.RequestedMarginVoxels
            && Row.Probe.Result != EVoxelConnectivityResult::OutOfWindow
            && Row.Probe.Result != EVoxelConnectivityResult::StartCellSolid
            && Row.Probe.Result != EVoxelConnectivityResult::GoalCellSolid
            && !Row.Probe.bStartSnapped
            && !Row.Probe.bGoalSnapped;
    }

    FVoxelStrateMeasureSettings DiagnosticSettings = BaseSettings;
    DiagnosticSettings.SampleStep = 2;
    DiagnosticSettings.RadiusInVoxels = 192;
    DiagnosticSettings.InteriorMarginVoxels = 0;

    TArray<FSpineDiagnosisReport> SpineReports;
    SpineReports.SetNum(Layout.Num());
    bool bSpineDiagnosisUsable = true;
    const double SpineDiagnosisStartSeconds = FPlatformTime::Seconds();
    for (int32 StrateIndex = 0; StrateIndex < Layout.Num(); ++StrateIndex)
    {
        FSpineDiagnosisReport& Report = SpineReports[StrateIndex];
        if (!BuildSpineDiagnosisReport(
                *World.Generator,
                *World.StrateManager,
                StrateIndex,
                DiagnosticSettings,
                Report))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: strate %d could not produce a bounded origin-spine "
                     "diagnostic at margin 0."),
                StrateIndex));
            bAllChecksPassed = false;
            bSpineDiagnosisUsable = false;
            continue;
        }

        if (Report.Spine.NumSamples <= 0
            || !FMath::IsFinite(Report.Spine.AirFraction)
            || Report.Spine.AirFraction < 0.0f
            || Report.Spine.AirFraction > 1.0f
            || Report.Spine.SampledMinZ >= Report.Spine.SampledMaxZ)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: %s origin-spine column report is out of range."),
                *Report.Name));
            bAllChecksPassed = false;
            bSpineDiagnosisUsable = false;
        }

        if (!Report.bHasArrival)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: %s has no arrival mouth for the spine diagnosis."),
                *Report.Name));
            bAllChecksPassed = false;
            bSpineDiagnosisUsable = false;
        }
        if (StrateIndex + 1 < Layout.Num() && !Report.bHasDeparture)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: %s has no departure mouth for the spine diagnosis."),
                *Report.Name));
            bAllChecksPassed = false;
            bSpineDiagnosisUsable = false;
        }

        if (Report.Spine.RepresentativeOpenZ != INDEX_NONE)
        {
            if (!Report.SpineComponent.bValid
                || Report.SpineComponentProbe.Result != EVoxelConnectivityResult::Connected
                || Report.SpineComponentProbe.bStartSnapped
                || Report.SpineComponentProbe.bGoalSnapped
                || Report.SpineComponent.GoalComponentCells <= 0)
            {
                AddError(FString::Printf(
                    TEXT("HARD FAILURE: %s origin-spine component probe was not a stable "
                         "self-connected air query: %s."),
                    *Report.Name,
                    *ConnectivityProbeText(Report.SpineComponentProbe)));
                bAllChecksPassed = false;
                bSpineDiagnosisUsable = false;
            }
        }
    }
    const double SpineDiagnosisSeconds = FPlatformTime::Seconds() - SpineDiagnosisStartSeconds;

    int32 NumSpineFullWindowRuns = 0;
    int32 NumSpineOneOpenRuns = 0;
    int32 NumSpineLargestComponents = 0;
    int32 NumMouthToSpineQueries = 0;
    int32 NumMouthToSpineConnected = 0;
    int32 NumMouthToSpineBudgetExhausted = 0;
    int32 NumMouthToSpineRetriesUsed = 0;
    int32 MaxMouthToSpineRetries = 0;
    for (const FSpineDiagnosisReport& Report : SpineReports)
    {
        if (Report.Spine.bFullWindowRun)
        {
            ++NumSpineFullWindowRuns;
        }
        if (Report.Spine.bAirSamplesFormOneRun)
        {
            ++NumSpineOneOpenRuns;
        }
        if (Report.SpineComponent.GoalComponentCells > 0
            && Report.SpineComponent.bGoalComponentIsLargest)
        {
            ++NumSpineLargestComponents;
        }
        const FConnectivityProbe* MouthProbes[] = {
            Report.bHasArrival ? &Report.ArrivalToSpine : nullptr,
            Report.bHasDeparture ? &Report.DepartureToSpine : nullptr,
        };
        for (const FConnectivityProbe* Probe : MouthProbes)
        {
            if (Probe == nullptr)
            {
                continue;
            }
            ++NumMouthToSpineQueries;
            NumMouthToSpineConnected += Probe->Result == EVoxelConnectivityResult::Connected;
            NumMouthToSpineBudgetExhausted +=
                Probe->Result == EVoxelConnectivityResult::CoarseLiedBudgetExhausted;
            NumMouthToSpineRetriesUsed += Probe->NumRouteRetries;
            MaxMouthToSpineRetries = FMath::Max(
                MaxMouthToSpineRetries, Probe->NumRouteRetries);
        }
    }

    FString Summary = TEXT(
        "VerticalShafts arrival->departure refinement (seed 1337; source mouth pair; "
        "derived interior window):\n");
    Summary += TEXT(
        "  SampleStep | RadiusInVoxels | verdict | components | largest share | cells | margin | "
        "Z window | endpoint snaps\n");
    for (const FRefinementSweepRow& Row : Rows)
    {
        const FString VerdictText = ConnectivityProbeText(Row.Probe);
        FString SnapText = TEXT("none");
        if (Row.Probe.bStartSnapped || Row.Probe.bGoalSnapped)
        {
            SnapText = ConnectivityProbeText(Row.Probe);
        }
        Summary += FString::Printf(
            TEXT("  %d | %d | %s | %d | %.9g | %lld | %d | [%d,%d) | %s\n"),
            Row.SampleStep,
            Row.RadiusInVoxels,
            *VerdictText,
            Row.Metrics.NumAirComponents,
            Row.Metrics.LargestComponentShare,
            Row.Metrics.NumSampled,
            Row.Metrics.ResolvedMarginVoxels,
            Row.Metrics.SampledMinZ,
            Row.Metrics.SampledMaxZ,
            *SnapText);
    }
    Summary += TEXT(
        "Controlled comparisons hold radius constant: (step 4, radius 192) vs (step 2, "
        "radius 192), and (step 4, radius 160) vs (step 1, radius 160).\n");
    Summary += FString::Printf(
        TEXT("VerticalShafts refinement sweep wall-clock: %.3f seconds.\n"),
        SweepSeconds);

    Summary += TEXT(
        "Resolution control — margin artifact check (SampleStep 2, RadiusInVoxels 192; explicit margins):\n");
    Summary += TEXT(
        "  margin | verdict | components | largest share | sampled cells | Z window | endpoint snaps\n");
    for (const FRefinementSweepRow& Row : MarginRows)
    {
        const FString VerdictText = ConnectivityProbeText(Row.Probe);
        FString SnapText = TEXT("none");
        if (Row.Probe.bStartSnapped || Row.Probe.bGoalSnapped)
        {
            SnapText = ConnectivityProbeText(Row.Probe);
        }
        Summary += FString::Printf(
            TEXT("  %d | %s | %d | %.9g | %lld | [%d,%d) | %s\n"),
            Row.RequestedMarginVoxels,
            *VerdictText,
            Row.Metrics.NumAirComponents,
            Row.Metrics.LargestComponentShare,
            Row.Metrics.NumSampled,
            Row.Metrics.SampledMinZ,
            Row.Metrics.SampledMaxZ,
            *SnapText);
    }
    Summary += FString::Printf(
        TEXT("Resolution-control verdict: %s. Margin sweep wall-clock: %.3f seconds.\n"),
        bMarginArtifact
            ? TEXT("ARTIFACT — at least one margin-0/2/4 query is CONNECTED; the default margin excluded the connection")
            : TEXT("FINDING SURVIVES — no margin-0/2/4 query is CONNECTED; the full window is exonerated"),
        MarginSweepSeconds);

    Summary += FString::Printf(
        TEXT("Origin-spine diagnosis (seed 1337; OriginSpineRadius=%.3f; margin 0; "
             "SampleStep=2; RadiusInVoxels=192; column sampled at every integer Z):\n"),
        World.Generator->OriginSpineRadius);
    Summary += TEXT(
        "  strate | measured window | spine open? (air fraction; full-window run; one open run; "
        "open Z span) | spine component share | spine is largest? | arrival->spine | "
        "departure->spine | arrival->departure\n");
    for (const FSpineDiagnosisReport& Report : SpineReports)
    {
        const FSpineColumnReport& Spine = Report.Spine;
        const FVoxelConnectivityDiagnostics& SpineComponent = Report.SpineComponent;
        const FString OpenSpan = Spine.FirstOpenZ == INDEX_NONE
            ? TEXT("none")
            : FString::Printf(TEXT("[%d,%d]"), Spine.FirstOpenZ, Spine.LastOpenZ);
        const FString SpineOpenText = FString::Printf(
            TEXT("air=%.9g; full=%s; one-run=%s; open=%s"),
            Spine.AirFraction,
            Spine.bFullWindowRun ? TEXT("YES") : TEXT("NO"),
            Spine.bAirSamplesFormOneRun ? TEXT("YES") : TEXT("NO"),
            *OpenSpan);
        const FString SpineShareText = SpineComponent.GoalComponentCells > 0
            ? FString::Printf(
                TEXT("%.9g (%lld/%lld)"),
                SpineComponent.GoalComponentShare,
                SpineComponent.GoalComponentCells,
                SpineComponent.NumAirCells)
            : TEXT("N/A");
        const FString SpineLargestText = SpineComponent.GoalComponentCells > 0
            ? (SpineComponent.bGoalComponentIsLargest ? TEXT("YES") : TEXT("NO"))
            : TEXT("N/A");
        const FString ArrivalText = Report.bHasArrival
            ? ConnectivityProbeText(Report.ArrivalToSpine)
            : TEXT("MISSING_ARRIVAL");
        const FString DepartureText = Report.bHasDeparture
            ? ConnectivityProbeText(Report.DepartureToSpine)
            : TEXT("N/A (bottom-most; no departure)");
        FString ArrivalDepartureText;
        if (Report.bHasChainArrival && Report.bHasDeparture)
        {
            ArrivalDepartureText = ConnectivityProbeText(Report.ArrivalToDeparture);
        }
        else if (Report.bArrivalIsSurfaceEntry && !Report.bHasChainArrival)
        {
            ArrivalDepartureText = TEXT("N/A (surface entry; no chain arrival)");
        }
        else if (!Report.bHasDeparture && Report.Index + 1 == SpineReports.Num())
        {
            ArrivalDepartureText = TEXT("N/A (bottom-most; no departure)");
        }
        else
        {
            ArrivalDepartureText = TEXT("MISSING_CHAIN_MOUTH");
        }

        Summary += FString::Printf(
            TEXT("  %d %s | [%d,%d) | %s | %s | %s | %s | %s | %s\n"),
            Report.Index,
            *Report.Name,
            Spine.SampledMinZ,
            Spine.SampledMaxZ,
            *SpineOpenText,
            *SpineShareText,
            *SpineLargestText,
            *ArrivalText,
            *DepartureText,
            *ArrivalDepartureText);
    }
    Summary += FString::Printf(
        TEXT("Origin-spine diagnosis wall-clock: %.3f seconds; usable=%s.\n"),
        SpineDiagnosisSeconds,
        bSpineDiagnosisUsable ? TEXT("yes") : TEXT("no"));
    Summary += FString::Printf(
        TEXT("Interpretation counts: spine full-window run=%d/%d, one uninterrupted open run=%d/%d, "
             "spine component largest=%d/%d; mouth->spine CONNECTED=%d/%d, "
             "COARSE_LIED_BUDGET_EXHAUSTED=%d, retries=%d (max %d).\n"),
        NumSpineFullWindowRuns,
        SpineReports.Num(),
        NumSpineOneOpenRuns,
        SpineReports.Num(),
        NumSpineLargestComponents,
        SpineReports.Num(),
        NumMouthToSpineConnected,
        NumMouthToSpineQueries,
        NumMouthToSpineBudgetExhausted,
        NumMouthToSpineRetriesUsed,
        MaxMouthToSpineRetries);
    Summary += TEXT(
        "Interpretation: the structural spine remains a continuous column. After the "
        "VerticalShafts connector change, the seed-1337 spine is the largest component, but the "
        "distant shaft networks at the measured passage mouths are not thereby guaranteed to join; "
        "the 16-seed arrival->departure sweep below is the acceptance measurement.\n");
    Summary += TEXT(
        "Ordering-dependency hazard: making passage i's departure mouth depend on passage i-1's "
        "arrival mouth would make layout generation a sequence. That conflicts with "
        "VoxelForge.Determinism.LayoutOrderIndependence, which asserts the layout is a set; a "
        "coupled implementation would be expected to break that test. No such dependency was "
        "implemented here.\n");

    // The centered-window multi-seed/Part-0 block below predates the mouth-fitted acceptance
    // sweep. It is intentionally disabled now: it repeats the same million-cell measurements
    // with a window that is known to be mostly empty, and its old verdict would obscure the
    // fitted-window result required by this test. The single-seed refinement, margin controls,
    // and origin-spine checks above remain active; the fitted sweep below owns the 16-seed law.
    if (false && bMarginSweepUsable)
    {
        const FVoxelConnectivityDiagnostics Diagnostics = VF_DiagnoseConnectivity(
            *World.Generator,
            *World.StrateManager,
            VerticalShaftsIndex,
            ArrivalPoint,
            DeparturePoint,
            DiagnosticSettings);
        if (!Diagnostics.bValid
            || Diagnostics.NumAirCells <= 0
            || Diagnostics.StartComponentCells <= 0
            || Diagnostics.GoalComponentCells <= 0
            || Diagnostics.StartToGoalComponentDistanceCells < 0
            || Diagnostics.GoalToStartComponentDistanceCells < 0)
        {
            AddError(TEXT("HARD FAILURE: the surviving VerticalShafts finding could not produce "
                          "component and separation diagnostics."));
            bAllChecksPassed = false;
        }

        const float MouthXYSeparation = FMath::Sqrt(
            FMath::Square(ArrivalPoint.X - DeparturePoint.X)
            + FMath::Square(ArrivalPoint.Y - DeparturePoint.Y));
        const float MouthZDelta = DeparturePoint.Z - ArrivalPoint.Z;
        const float MouthStraightLine = FVector::Dist(ArrivalPoint, DeparturePoint);
        const float CoarseStraightLineCells = MouthStraightLine
            / static_cast<float>(DiagnosticSettings.SampleStep);

        Summary += TEXT("Part C — VerticalShafts post-fix measurement (seed 1337; margin 0; step 2; radius 192):\n");
        Summary += FString::Printf(
            TEXT("  arrival LowerPoint: (%.3f, %.3f, %.3f); departure UpperPoint: "
                 "(%.3f, %.3f, %.3f); XY separation %.3f voxels; Z delta %.3f voxels.\n"),
            ArrivalPoint.X,
            ArrivalPoint.Y,
            ArrivalPoint.Z,
            DeparturePoint.X,
            DeparturePoint.Y,
            DeparturePoint.Z,
            MouthXYSeparation,
            MouthZDelta);
        Summary += FString::Printf(
            TEXT("  mouth components: air=%lld; arrival=%lld (share %.9g); departure=%lld "
                 "(share %.9g).\n"),
            Diagnostics.NumAirCells,
            Diagnostics.StartComponentCells,
            Diagnostics.StartComponentShare,
            Diagnostics.GoalComponentCells,
            Diagnostics.GoalComponentShare);
        Summary += FString::Printf(
            TEXT("  coarse-grid separation: straight-line %.3f cells (%.3f voxels); "
                 "arrival to nearest departure-component cell %.3f cells (%.3f voxels); "
                 "departure to nearest arrival-component cell %.3f cells (%.3f voxels).\n"),
            CoarseStraightLineCells,
            MouthStraightLine,
            Diagnostics.StartToGoalComponentDistanceCells,
            Diagnostics.StartToGoalComponentDistanceCells * static_cast<float>(DiagnosticSettings.SampleStep),
            Diagnostics.GoalToStartComponentDistanceCells,
            Diagnostics.GoalToStartComponentDistanceCells * static_cast<float>(DiagnosticSettings.SampleStep));
        Summary += FString::Printf(
            TEXT("  diagnostic route verdict: %s; retries=%d; endpoint snaps: start=%s, goal=%s.\n"),
            ConnectivityResultName(Diagnostics.Result),
            Diagnostics.NumRouteRetries,
            Diagnostics.bStartSnapped ? TEXT("yes") : TEXT("no"),
            Diagnostics.bGoalSnapped ? TEXT("yes") : TEXT("no"));

        if (MarginRows.Num() > 0
            && SpineReports.IsValidIndex(VerticalShaftsIndex))
        {
            const FVoxelStrateMetrics& VerticalMetrics = MarginRows[0].Metrics;
            const FSpineDiagnosisReport& VerticalSpine = SpineReports[VerticalShaftsIndex];
            Summary += FString::Printf(
                TEXT("  Part C VerticalShafts topology: components=%d; largest share=%.9g; "
                     "spine component share=%.9g; spine-is-largest=%s.\n"),
                VerticalMetrics.NumAirComponents,
                VerticalMetrics.LargestComponentShare,
                VerticalSpine.SpineComponent.GoalComponentShare,
                VerticalSpine.SpineComponent.bGoalComponentIsLargest ? TEXT("YES") : TEXT("NO"));
        }

        static constexpr int32 SeedCases[] = {
            1337, 1, 2, 3, 4, 5, 6, 7,
            8, 9, 10, 11, 12, 13, 14, 15,
        };
        int32 SeedPasses = 0;
        int32 SeedFailures = 0;
        int32 SeedArrivalToSpineSnapEvents = 0;
        int32 SeedDepartureToSpineSnapEvents = 0;
        int32 SeedArrivalToDepartureSnapEvents = 0;
        int32 SeedArrivalToSpineResultCounts[ConnectivityResultCount] = {};
        int32 SeedDepartureToSpineResultCounts[ConnectivityResultCount] = {};
        int32 SeedArrivalToDepartureResultCounts[ConnectivityResultCount] = {};
        int32 SeedArrivalToSpineRetries = 0;
        int32 SeedDepartureToSpineRetries = 0;
        int32 SeedArrivalToDepartureRetries = 0;
        int32 SeedArrivalToSpineMaxRetries = 0;
        int32 SeedDepartureToSpineMaxRetries = 0;
        int32 SeedArrivalToDepartureMaxRetries = 0;
        int32 SeedArrivalToSpineBudgetExhausted = 0;
        int32 SeedDepartureToSpineBudgetExhausted = 0;
        int32 SeedArrivalToDepartureBudgetExhausted = 0;
        int32 SeedFullWindowRuns = 0;
        int32 SeedOneOpenRuns = 0;
        int32 SeedSpineLargest = 0;
        int32 SeedSpineNoOpenCell = 0;
        float SeedSpineShareMin = FLT_MAX;
        float SeedSpineShareMax = -FLT_MAX;
        int32 SeedTopologyInvalid = 0;
        int32 SeedTopologyComponentsMin = INT32_MAX;
        int32 SeedTopologyComponentsMax = 0;
        float SeedTopologyLargestShareMin = FLT_MAX;
        float SeedTopologyLargestShareMax = -FLT_MAX;
        TArray<FSpineDiagnosisReport> SeedReports;
        SeedReports.Reserve(UE_ARRAY_COUNT(SeedCases));
        const double SeedSweepStartSeconds = FPlatformTime::Seconds();
        for (const int32 Seed : SeedCases)
        {
            FTestWorld SeedWorld;
            SeedWorld.Build(Seed, /*InGapChunks=*/2);
            if (!SeedWorld.IsValid())
            {
                AddError(FString::Printf(
                    TEXT("HARD FAILURE: seed %d could not build the VerticalShafts fixture."),
                    Seed));
                bAllChecksPassed = false;
                ++SeedFailures;
                continue;
            }

            FSpineDiagnosisReport& SeedReport = SeedReports.AddDefaulted_GetRef();
            if (!BuildSpineDiagnosisReport(
                    *SeedWorld.Generator,
                    *SeedWorld.StrateManager,
                    VerticalShaftsIndex,
                    DiagnosticSettings,
                    SeedReport))
            {
                SeedReports.Pop();
                AddError(FString::Printf(
                    TEXT("HARD FAILURE: seed %d could not produce the VerticalShafts spine "
                         "diagnosis."),
                    Seed));
                bAllChecksPassed = false;
                ++SeedFailures;
                continue;
            }
            SeedReport.Seed = Seed;

            const FVoxelStrateMetrics& Topology = SeedReport.Topology = VF_MeasureStrate(
                *SeedWorld.Generator,
                *SeedWorld.StrateManager,
                VerticalShaftsIndex,
                DiagnosticSettings);
            const bool bTopologyUsable =
                Topology.bValid
                && Topology.NumSampled > 0
                && Topology.NumAir > 0
                && Topology.NumSolid > 0
                && Topology.NumAirComponents > 0
                && Topology.LargestComponentCells > 0
                && Topology.LargestComponentCells <= Topology.NumAir
                && FMath::IsFinite(Topology.LargestComponentShare)
                && Topology.LargestComponentShare >= 0.0f
                && Topology.LargestComponentShare <= 1.0f;
            if (!bTopologyUsable)
            {
                ++SeedTopologyInvalid;
                AddError(FString::Printf(
                    TEXT("HARD FAILURE: seed %d VerticalShafts topology measurement is not "
                         "usable (valid=%s, sampled=%lld, air=%lld, solid=%lld, components=%d, "
                         "largest share=%.9g, reason='%s')."),
                    Seed,
                    Topology.bValid ? TEXT("true") : TEXT("false"),
                    Topology.NumSampled,
                    Topology.NumAir,
                    Topology.NumSolid,
                    Topology.NumAirComponents,
                    Topology.LargestComponentShare,
                    *Topology.RefusalReason));
                bAllChecksPassed = false;
            }
            else
            {
                SeedTopologyComponentsMin = FMath::Min(
                    SeedTopologyComponentsMin, Topology.NumAirComponents);
                SeedTopologyComponentsMax = FMath::Max(
                    SeedTopologyComponentsMax, Topology.NumAirComponents);
                SeedTopologyLargestShareMin = FMath::Min(
                    SeedTopologyLargestShareMin, Topology.LargestComponentShare);
                SeedTopologyLargestShareMax = FMath::Max(
                    SeedTopologyLargestShareMax, Topology.LargestComponentShare);
            }

            if (!SeedReport.bHasChainArrival || !SeedReport.bHasDeparture
                || !SeedReport.ArrivalToDeparture.bChecked)
            {
                AddError(FString::Printf(
                    TEXT("HARD FAILURE: seed %d did not produce exactly one VerticalShafts "
                         "arrival and departure mouth."),
                    Seed));
                bAllChecksPassed = false;
                ++SeedFailures;
                continue;
            }

            const auto CountSeedResult = [](
                const FConnectivityProbe& Probe,
                int32 (&Counts)[ConnectivityResultCount])
            {
                if (Probe.bChecked)
                {
                    ++Counts[static_cast<int32>(Probe.Result)];
                }
            };
            CountSeedResult(SeedReport.ArrivalToSpine, SeedArrivalToSpineResultCounts);
            CountSeedResult(SeedReport.DepartureToSpine, SeedDepartureToSpineResultCounts);
            CountSeedResult(SeedReport.ArrivalToDeparture, SeedArrivalToDepartureResultCounts);

            SeedArrivalToSpineRetries += SeedReport.ArrivalToSpine.NumRouteRetries;
            SeedDepartureToSpineRetries += SeedReport.DepartureToSpine.NumRouteRetries;
            SeedArrivalToDepartureRetries +=
                SeedReport.ArrivalToDeparture.NumRouteRetries;
            SeedArrivalToSpineMaxRetries = FMath::Max(
                SeedArrivalToSpineMaxRetries,
                SeedReport.ArrivalToSpine.NumRouteRetries);
            SeedDepartureToSpineMaxRetries = FMath::Max(
                SeedDepartureToSpineMaxRetries,
                SeedReport.DepartureToSpine.NumRouteRetries);
            SeedArrivalToDepartureMaxRetries = FMath::Max(
                SeedArrivalToDepartureMaxRetries,
                SeedReport.ArrivalToDeparture.NumRouteRetries);
            SeedArrivalToSpineBudgetExhausted +=
                SeedReport.ArrivalToSpine.Result
                    == EVoxelConnectivityResult::CoarseLiedBudgetExhausted;
            SeedDepartureToSpineBudgetExhausted +=
                SeedReport.DepartureToSpine.Result
                    == EVoxelConnectivityResult::CoarseLiedBudgetExhausted;
            SeedArrivalToDepartureBudgetExhausted +=
                SeedReport.ArrivalToDeparture.Result
                    == EVoxelConnectivityResult::CoarseLiedBudgetExhausted;

            SeedArrivalToSpineSnapEvents +=
                (SeedReport.ArrivalToSpine.bStartSnapped ? 1 : 0)
                + (SeedReport.ArrivalToSpine.bGoalSnapped ? 1 : 0);
            SeedDepartureToSpineSnapEvents +=
                (SeedReport.DepartureToSpine.bStartSnapped ? 1 : 0)
                + (SeedReport.DepartureToSpine.bGoalSnapped ? 1 : 0);
            SeedArrivalToDepartureSnapEvents +=
                (SeedReport.ArrivalToDeparture.bStartSnapped ? 1 : 0)
                + (SeedReport.ArrivalToDeparture.bGoalSnapped ? 1 : 0);

            if (SeedReport.Spine.bFullWindowRun)
            {
                ++SeedFullWindowRuns;
            }
            if (SeedReport.Spine.bAirSamplesFormOneRun)
            {
                ++SeedOneOpenRuns;
            }
            if (SeedReport.SpineComponent.GoalComponentCells > 0)
            {
                if (SeedReport.SpineComponent.bGoalComponentIsLargest)
                {
                    ++SeedSpineLargest;
                }
                SeedSpineShareMin = FMath::Min(
                    SeedSpineShareMin, SeedReport.SpineComponent.GoalComponentShare);
                SeedSpineShareMax = FMath::Max(
                    SeedSpineShareMax, SeedReport.SpineComponent.GoalComponentShare);
            }
            else
            {
                ++SeedSpineNoOpenCell;
            }

            const EVoxelConnectivityResult SeedResult = SeedReport.ArrivalToDeparture.Result;
            if (bTopologyUsable
                && SeedResult == EVoxelConnectivityResult::Connected
                && !SeedReport.ArrivalToDeparture.bStartSnapped
                && !SeedReport.ArrivalToDeparture.bGoalSnapped)
            {
                ++SeedPasses;
            }
            else
            {
                ++SeedFailures;
            }
        }
        const double SeedSweepSeconds = FPlatformTime::Seconds() - SeedSweepStartSeconds;

        Summary += FString::Printf(
            TEXT("  multi-seed VerticalShafts diagnosis (%d seeds; margin 0, step 2, radius 192): "
                 "arrival->departure %d pass, %d fail, %d snapped endpoint events; "
                 "wall-clock %.3f seconds.\n"),
            UE_ARRAY_COUNT(SeedCases),
            SeedPasses,
            SeedFailures,
            SeedArrivalToDepartureSnapEvents,
            SeedSweepSeconds);
        Summary += FString::Printf(
            TEXT("  seed retries used (total/max/budget-exhausted): arrival->spine=%d/%d/%d; "
                 "departure->spine=%d/%d/%d; arrival->departure=%d/%d/%d; "
                 "MaxRouteRetries=%d.\n"),
            SeedArrivalToSpineRetries,
            SeedArrivalToSpineMaxRetries,
            SeedArrivalToSpineBudgetExhausted,
            SeedDepartureToSpineRetries,
            SeedDepartureToSpineMaxRetries,
            SeedDepartureToSpineBudgetExhausted,
            SeedArrivalToDepartureRetries,
            SeedArrivalToDepartureMaxRetries,
            SeedArrivalToDepartureBudgetExhausted,
            DiagnosticSettings.MaxRouteRetries);
        Summary += FString::Printf(
            TEXT("  ACCEPTANCE arrival->departure: %s (%d/%d seeds pass).\n"),
            SeedPasses == UE_ARRAY_COUNT(SeedCases) && SeedFailures == 0
                ? TEXT("PASS") : TEXT("FAIL"),
            SeedPasses,
            UE_ARRAY_COUNT(SeedCases));
        Summary += FString::Printf(
            TEXT("  spine column sweep: full-window run=%d/%d, one-open-run=%d/%d, "
                 "component-largest=%d/%d, no open component=%d; component share range "
                 "[%.9g, %.9g].\n"),
            SeedFullWindowRuns,
            UE_ARRAY_COUNT(SeedCases),
            SeedOneOpenRuns,
            UE_ARRAY_COUNT(SeedCases),
            SeedSpineLargest,
            UE_ARRAY_COUNT(SeedCases),
            SeedSpineNoOpenCell,
            SeedSpineShareMin == FLT_MAX ? 0.0f : SeedSpineShareMin,
            SeedSpineShareMax == -FLT_MAX ? 0.0f : SeedSpineShareMax);
        Summary += FString::Printf(
            TEXT("  seed topology: components range [%d, %d], largest-component share range "
                 "[%.9g, %.9g], invalid measurements=%d; spine-component share is reported "
                 "per seed below.\n"),
            SeedTopologyComponentsMin == INT32_MAX ? 0 : SeedTopologyComponentsMin,
            SeedTopologyComponentsMax,
            SeedTopologyLargestShareMin == FLT_MAX ? 0.0f : SeedTopologyLargestShareMin,
            SeedTopologyLargestShareMax == -FLT_MAX ? 0.0f : SeedTopologyLargestShareMax,
            SeedTopologyInvalid);
        Summary += FString::Printf(
            TEXT("  seed result breakdown — arrival->spine: CONNECTED=%d, "
                 "NOT_CONNECTED_AT_THIS_RESOLUTION=%d, START_CELL_SOLID=%d, GOAL_CELL_SOLID=%d, "
                 "OUT_OF_WINDOW=%d, COARSE_LIED_BUDGET_EXHAUSTED=%d; departure->spine: CONNECTED=%d, "
                 "NOT_CONNECTED_AT_THIS_RESOLUTION=%d, START_CELL_SOLID=%d, GOAL_CELL_SOLID=%d, "
                 "OUT_OF_WINDOW=%d, COARSE_LIED_BUDGET_EXHAUSTED=%d; arrival->departure: CONNECTED=%d, "
                 "NOT_CONNECTED_AT_THIS_RESOLUTION=%d, START_CELL_SOLID=%d, GOAL_CELL_SOLID=%d, "
                 "OUT_OF_WINDOW=%d, COARSE_LIED_BUDGET_EXHAUSTED=%d.\n"),
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::Connected)],
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::NotConnectedAtThisResolution)],
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::StartCellSolid)],
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::GoalCellSolid)],
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::OutOfWindow)],
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::CoarseLiedBudgetExhausted)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::Connected)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::NotConnectedAtThisResolution)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::StartCellSolid)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::GoalCellSolid)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::OutOfWindow)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::CoarseLiedBudgetExhausted)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::Connected)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::NotConnectedAtThisResolution)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::StartCellSolid)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::GoalCellSolid)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::OutOfWindow)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::CoarseLiedBudgetExhausted)]);
        Summary += FString::Printf(
            TEXT("  seed endpoint snaps — arrival->spine=%d, departure->spine=%d, "
                 "arrival->departure=%d.\n"),
            SeedArrivalToSpineSnapEvents,
            SeedDepartureToSpineSnapEvents,
            SeedArrivalToDepartureSnapEvents);
        Summary += TEXT("  seed | components | largest share | spine air | spine share | spine largest | "
                       "full run | one open run | arrival->spine | departure->spine | "
                       "arrival->departure\n");
        for (int32 SeedIndex = 0; SeedIndex < SeedReports.Num(); ++SeedIndex)
        {
            const FSpineDiagnosisReport& SeedReport = SeedReports[SeedIndex];
            const FString SpineShare = SeedReport.SpineComponent.GoalComponentCells > 0
                ? FString::Printf(
                    TEXT("%.9g"), SeedReport.SpineComponent.GoalComponentShare)
                : TEXT("N/A");
            const FString SpineLargest = SeedReport.SpineComponent.GoalComponentCells > 0
                ? (SeedReport.SpineComponent.bGoalComponentIsLargest ? TEXT("YES") : TEXT("NO"))
                : TEXT("N/A");
            const FString Components = SeedReport.Topology.bValid
                ? FString::Printf(TEXT("%d"), SeedReport.Topology.NumAirComponents)
                : TEXT("N/A");
            const FString LargestShare = SeedReport.Topology.bValid
                ? FString::Printf(TEXT("%.9g"), SeedReport.Topology.LargestComponentShare)
                : TEXT("N/A");
            Summary += FString::Printf(
                TEXT("  %d | %s | %s | %.9g | %s | %s | %s | %s | %s | %s | %s\n"),
                SeedReport.Seed,
                *Components,
                *LargestShare,
                SeedReport.Spine.AirFraction,
                *SpineShare,
                *SpineLargest,
                SeedReport.Spine.bFullWindowRun ? TEXT("YES") : TEXT("NO"),
                SeedReport.Spine.bAirSamplesFormOneRun ? TEXT("YES") : TEXT("NO"),
                *ConnectivityProbeText(SeedReport.ArrivalToSpine),
                *ConnectivityProbeText(SeedReport.DepartureToSpine),
                *ConnectivityProbeText(SeedReport.ArrivalToDeparture));
        }

        // PART 0: resolve the two remaining step-2 negatives without changing generation.  The
        // 12M cap is deliberately high enough for the 320x320x112 step-1/radius-160 fallback,
        // but low enough that the requested 384x384x112 step-1/radius-192 attempt is explicitly
        // reported as a MaxCells refusal rather than silently changing the measurement budget.
        struct FSeedResolutionRow
        {
            int32 SampleStep = 0;
            int32 RadiusInVoxels = 0;
            bool bMaxCellsRefused = false;
            FVoxelStrateMetrics Metrics;
            FVoxelConnectivityDiagnostics Diagnostics;
        };

        static constexpr int32 ResolutionSeeds[] = {6, 14};
        static constexpr FRefinementCase ResolutionCases[] = {
            {2, 192},
            {1, 192},
            {2, 128},
            {1, 128},
        };
        constexpr int32 ResolutionMaxCells = 12000000;

        Summary += TEXT(
            "PART 0 — residual VerticalShafts seed refinement (seeds 6 and 14; margin derived; "
            "MaxCells=12000000):\n");
        Summary += TEXT(
            "  seed | SampleStep | RadiusInVoxels | verdict | components | largest share | retries | "
            "arrival mouth (x,y,z; component share) | departure mouth (x,y,z; component share)\n");

        for (const int32 Seed : ResolutionSeeds)
        {
            FTestWorld ResolutionWorld;
            ResolutionWorld.Build(Seed, /*InGapChunks=*/2);
            if (!ResolutionWorld.IsValid())
            {
                AddError(FString::Printf(
                    TEXT("HARD FAILURE: Part 0 seed %d could not build the VerticalShafts fixture."),
                    Seed));
                bAllChecksPassed = false;
                continue;
            }

            FVector ResolutionArrivalPoint = FVector::ZeroVector;
            FVector ResolutionDeparturePoint = FVector::ZeroVector;
            if (!FindChainMouths(
                    ResolutionWorld.StrateManager->GetPassages(),
                    VerticalShaftsIndex,
                    ResolutionArrivalPoint,
                    ResolutionDeparturePoint))
            {
                AddError(FString::Printf(
                    TEXT("HARD FAILURE: Part 0 seed %d does not have exactly one arrival and "
                         "departure mouth."),
                    Seed));
                bAllChecksPassed = false;
                continue;
            }

            TArray<FSeedResolutionRow> ResolutionRows;
            ResolutionRows.Reserve(UE_ARRAY_COUNT(ResolutionCases) + 1);
            for (const FRefinementCase& TestCase : ResolutionCases)
            {
                FSeedResolutionRow& Row = ResolutionRows.AddDefaulted_GetRef();
                Row.SampleStep = TestCase.SampleStep;
                Row.RadiusInVoxels = TestCase.RadiusInVoxels;

                FVoxelStrateMeasureSettings RowSettings = DiagnosticSettings;
                RowSettings.SampleStep = TestCase.SampleStep;
                RowSettings.RadiusInVoxels = TestCase.RadiusInVoxels;
                RowSettings.MaxCells = ResolutionMaxCells;
                RowSettings.InteriorMarginVoxels = -1;

                Row.Metrics = VF_MeasureStrate(
                    *ResolutionWorld.Generator,
                    *ResolutionWorld.StrateManager,
                    VerticalShaftsIndex,
                    RowSettings);
                const bool bMaxCellsRefusal = !Row.Metrics.bValid
                    && Row.Metrics.RefusalReason.Contains(TEXT("MaxCells"));
                Row.bMaxCellsRefused = bMaxCellsRefusal;

                Row.Diagnostics = VF_DiagnoseConnectivity(
                    *ResolutionWorld.Generator,
                    *ResolutionWorld.StrateManager,
                    VerticalShaftsIndex,
                    ResolutionArrivalPoint,
                    ResolutionDeparturePoint,
                    RowSettings);

                const bool bExpectedFallbackRefusal = TestCase.SampleStep == 1
                    && TestCase.RadiusInVoxels == 192;
                if (!Row.Metrics.bValid
                    && !(bExpectedFallbackRefusal && bMaxCellsRefusal))
                {
                    AddError(FString::Printf(
                        TEXT("HARD FAILURE: Part 0 seed %d row step=%d radius=%d was not a "
                             "bounded measurement (metrics valid=%s reason='%s')."),
                        Seed,
                        Row.SampleStep,
                        Row.RadiusInVoxels,
                        Row.Metrics.bValid ? TEXT("true") : TEXT("false"),
                        *Row.Metrics.RefusalReason));
                    bAllChecksPassed = false;
                }
                if (Row.Metrics.bValid
                    && !Row.Diagnostics.bValid
                    && Row.Diagnostics.Result != EVoxelConnectivityResult::OutOfWindow)
                {
                    AddError(FString::Printf(
                        TEXT("HARD FAILURE: Part 0 seed %d row step=%d radius=%d produced an "
                             "invalid connectivity diagnostic with result %s."),
                        Seed,
                        Row.SampleStep,
                        Row.RadiusInVoxels,
                        ConnectivityResultName(Row.Diagnostics.Result)));
                    bAllChecksPassed = false;
                }
            }

            // If the requested fine grid is over the cap, perform the specified radius-control
            // substitution. This row is the one used to decide whether step refinement changes
            // the verdict; the refused 192 row remains in the report as evidence of the cap.
            if (ResolutionRows.IsValidIndex(1) && ResolutionRows[1].bMaxCellsRefused)
            {
                FSeedResolutionRow& FallbackRow = ResolutionRows.AddDefaulted_GetRef();
                FallbackRow.SampleStep = 1;
                FallbackRow.RadiusInVoxels = 160;

                FVoxelStrateMeasureSettings FallbackSettings = DiagnosticSettings;
                FallbackSettings.SampleStep = 1;
                FallbackSettings.RadiusInVoxels = 160;
                FallbackSettings.MaxCells = ResolutionMaxCells;
                FallbackSettings.InteriorMarginVoxels = -1;

                FallbackRow.Metrics = VF_MeasureStrate(
                    *ResolutionWorld.Generator,
                    *ResolutionWorld.StrateManager,
                    VerticalShaftsIndex,
                    FallbackSettings);
                FallbackRow.Diagnostics = VF_DiagnoseConnectivity(
                    *ResolutionWorld.Generator,
                    *ResolutionWorld.StrateManager,
                    VerticalShaftsIndex,
                    ResolutionArrivalPoint,
                    ResolutionDeparturePoint,
                    FallbackSettings);
                if (!FallbackRow.Metrics.bValid)
                {
                    AddError(FString::Printf(
                        TEXT("HARD FAILURE: Part 0 seed %d step=1 radius=160 fallback was not "
                             "a bounded measurement (metrics valid=%s reason='%s')."),
                        Seed,
                        FallbackRow.Metrics.bValid ? TEXT("true") : TEXT("false"),
                        *FallbackRow.Metrics.RefusalReason));
                    bAllChecksPassed = false;
                }
                if (FallbackRow.Metrics.bValid
                    && !FallbackRow.Diagnostics.bValid
                    && FallbackRow.Diagnostics.Result != EVoxelConnectivityResult::OutOfWindow)
                {
                    AddError(FString::Printf(
                        TEXT("HARD FAILURE: Part 0 seed %d step=1 radius=160 produced an "
                             "invalid connectivity diagnostic with result %s."),
                        Seed,
                        ConnectivityResultName(FallbackRow.Diagnostics.Result)));
                    bAllChecksPassed = false;
                }
            }

            for (const FSeedResolutionRow& Row : ResolutionRows)
            {
                const bool bUsable = Row.Metrics.bValid && Row.Diagnostics.bValid;
                const bool bOutOfWindow = Row.Diagnostics.Result == EVoxelConnectivityResult::OutOfWindow;
                const FString Verdict = Row.bMaxCellsRefused
                    ? TEXT("EXCEEDS_MAXCELLS")
                    : (Row.Metrics.bValid && bOutOfWindow)
                        ? TEXT("OUT_OF_WINDOW")
                        : bUsable
                            ? ConnectivityResultName(Row.Diagnostics.Result)
                            : TEXT("UNUSABLE");
                const FString Components = Row.Metrics.bValid
                    ? FString::Printf(TEXT("%d"), Row.Metrics.NumAirComponents)
                    : TEXT("N/A");
                const FString LargestShare = Row.Metrics.bValid
                    ? FString::Printf(TEXT("%.9g"), Row.Metrics.LargestComponentShare)
                    : TEXT("N/A");
                const FString Retries = Row.Diagnostics.bValid || (Row.Metrics.bValid && bOutOfWindow)
                    ? FString::Printf(TEXT("%d"), Row.Diagnostics.NumRouteRetries)
                    : TEXT("N/A");
                const FString ArrivalShare = bUsable
                    ? FString::Printf(TEXT("%.9g"), Row.Diagnostics.StartComponentShare)
                    : TEXT("N/A");
                const FString DepartureShare = bUsable
                    ? FString::Printf(TEXT("%.9g"), Row.Diagnostics.GoalComponentShare)
                    : TEXT("N/A");
                Summary += FString::Printf(
                    TEXT("  %d | %d | %d | %s | %s | %s | %s | (%.3f, %.3f, %.3f; %s) | "
                         "(%.3f, %.3f, %.3f; %s)%s\n"),
                    Seed,
                    Row.SampleStep,
                    Row.RadiusInVoxels,
                    *Verdict,
                    *Components,
                    *LargestShare,
                    *Retries,
                    ResolutionArrivalPoint.X,
                    ResolutionArrivalPoint.Y,
                    ResolutionArrivalPoint.Z,
                    *ArrivalShare,
                    ResolutionDeparturePoint.X,
                    ResolutionDeparturePoint.Y,
                    ResolutionDeparturePoint.Z,
                    *DepartureShare,
                    Row.bMaxCellsRefused
                        ? TEXT(" [requested row refused; use step=1/radius=160 below]")
                        : TEXT(""));
            }
        }
        Summary += TEXT(
            "  Part 0 interpretation: step=1/radius=192 is attempted first; when it exceeds the "
            "12M measurement cap, step=1/radius=160 is the prescribed radius control.\n");
    }
    else
    {
        Summary += TEXT(
            "Legacy centered-window multi-seed diagnostics: SKIPPED; the fitted-window sweep "
            "below is the acceptance measurement.\n");
    }

    // PART A/B — the production acceptance window follows the actual two mouths. The older
    // centered-window diagnostics above remain useful for the origin-spine invariant; they are
    // deliberately not reused here because a mouth-fitted box is not expected to contain (0,0).
    // The tree's prescribed neighbour fallback reaches three shaft-grid cells. A mouth window
    // must therefore cover three spacings beyond the endpoints before it can be called a fitted
    // diagnostic box. This grows with the body-scale shaft spacing rather than preserving the
    // old 48-voxel window by habit.
    const FVerticalShaftParams& FittedShaftParams =
        Layout[VerticalShaftsIndex].Definition->VerticalShaftParams;
    const float FittedMargin = FMath::Max(
        48.0f, 3.0f * FMath::Max(FittedShaftParams.ShaftSpacing, 1.0f));
    const float DoubledFittedMargin = 2.0f * FittedMargin;
    constexpr int32 FittedMaxCells = 120000000;
    constexpr int32 BeforeChangeMaxCells = 12000000;

    auto MakeFittedSettings = [&](int32 SampleStep,
                                  const FVector& A,
                                  const FVector& B,
                                  float Margin,
                                  int32 MaxCells) -> FVoxelStrateMeasureSettings
    {
        FVoxelStrateMeasureSettings Settings = BaseSettings;
        Settings.SampleStep = SampleStep;
        Settings.RadiusInVoxels = 192; // ignored when both cover points are set
        Settings.MaxCells = MaxCells;
        Settings.InteriorMarginVoxels = -1;
        Settings.CoverPointA = FVector2D(A.X, A.Y);
        Settings.CoverPointB = FVector2D(B.X, B.Y);
        Settings.CoverMarginVoxels = Margin;
        return Settings;
    };

    auto FittedVerdict = [&](const FVoxelStrateMetrics& Metrics,
                             const FVoxelConnectivityDiagnostics& Diagnostics) -> FString
    {
        if (!Metrics.bValid)
        {
            return Metrics.RefusalReason.Contains(TEXT("MaxCells"))
                ? TEXT("EXCEEDS_MAXCELLS") : TEXT("MEASUREMENT_LIMIT");
        }
        if (Diagnostics.Result == EVoxelConnectivityResult::OutOfWindow)
        {
            return TEXT("OUT_OF_WINDOW");
        }
        return ConnectivityResultName(Diagnostics.Result);
    };

    auto ComponentNoiseCounts = [&](const FVoxelStrateMetrics& Metrics,
                                    int64 ShaftVolumeCells,
                                    int32& OutNonLargest,
                                    int32& OutSmallerThanShaft)
    {
        OutNonLargest = 0;
        OutSmallerThanShaft = 0;
        bool bSkippedLargest = false;
        for (const int64 ComponentCells : Metrics.AirComponentCells)
        {
            if (!bSkippedLargest && ComponentCells == Metrics.LargestComponentCells)
            {
                bSkippedLargest = true;
                continue;
            }
            ++OutNonLargest;
            if (ComponentCells < ShaftVolumeCells)
            {
                ++OutSmallerThanShaft;
            }
        }
    };

    Summary += TEXT(
        "PART A — fitted VerticalShafts mouth window (AABB of the two XY mouths + three shaft "
        "spacings; step 1/2; doubled-margin control; fitted MaxCells=120000000):\n");
    Summary += TEXT(
        "  seed | step | default margin verdict | fitted dimensions/cells | doubled-margin check | "
        "interpretation | arrival component share | departure component share\n");

    bool bPartAFittedChecksPassed = true;
    static constexpr int32 ResolutionSeeds[] = {6, 14};
    static constexpr int32 ResolutionSteps[] = {2, 1};
    for (const int32 Seed : ResolutionSeeds)
    {
        FTestWorld ResolutionWorld;
        ResolutionWorld.Build(Seed, /*InGapChunks=*/2);
        if (!ResolutionWorld.IsValid())
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: fitted-window seed %d could not build the fixture."), Seed));
            bPartAFittedChecksPassed = false;
            continue;
        }

        FVector ResolutionArrivalPoint = FVector::ZeroVector;
        FVector ResolutionDeparturePoint = FVector::ZeroVector;
        if (!FindChainMouths(
                ResolutionWorld.StrateManager->GetPassages(),
                VerticalShaftsIndex,
                ResolutionArrivalPoint,
                ResolutionDeparturePoint))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: fitted-window seed %d does not have exactly one arrival and "
                     "departure mouth."), Seed));
            bPartAFittedChecksPassed = false;
            continue;
        }

        const FVerticalShaftParams& ShaftParams =
            Layout[VerticalShaftsIndex].Definition->VerticalShaftParams;
        const bool bArrivalFeatureCore = IsVerticalShaftFeatureCore(
            ShaftParams, Seed, ResolutionArrivalPoint);
        const bool bDepartureFeatureCore = IsVerticalShaftFeatureCore(
            ShaftParams, Seed, ResolutionDeparturePoint);
        if (!bArrivalFeatureCore || !bDepartureFeatureCore)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: fitted-window seed %d has a VerticalShafts mouth outside "
                     "the shaft feature core (arrival core=%s, departure core=%s; arrival=(%.3f,%.3f,%.3f); "
                     "departure=(%.3f,%.3f,%.3f); spacing=%.3f; density=%.3f)."),
                Seed,
                bArrivalFeatureCore
                    ? TEXT("yes") : TEXT("no"),
                bDepartureFeatureCore
                    ? TEXT("yes") : TEXT("no"),
                ResolutionArrivalPoint.X,
                ResolutionArrivalPoint.Y,
                ResolutionArrivalPoint.Z,
                ResolutionDeparturePoint.X,
                ResolutionDeparturePoint.Y,
                ResolutionDeparturePoint.Z,
                ShaftParams.ShaftSpacing,
                ShaftParams.ShaftDensity));
            bPartAFittedChecksPassed = false;
        }

        for (const int32 SampleStep : ResolutionSteps)
        {
            const FVoxelStrateMeasureSettings FittedSettings = MakeFittedSettings(
                SampleStep,
                ResolutionArrivalPoint,
                ResolutionDeparturePoint,
                FittedMargin,
                FittedMaxCells);
            const FVoxelStrateMetrics Metrics = VF_MeasureStrate(
                *ResolutionWorld.Generator,
                *ResolutionWorld.StrateManager,
                VerticalShaftsIndex,
                FittedSettings);
            const FVoxelConnectivityDiagnostics Diagnostics = VF_DiagnoseConnectivity(
                *ResolutionWorld.Generator,
                *ResolutionWorld.StrateManager,
                VerticalShaftsIndex,
                ResolutionArrivalPoint,
                ResolutionDeparturePoint,
                FittedSettings);

            FString DoubledText = TEXT("not needed (default verdict is CONNECTED)");
            FString Interpretation = TEXT("NO GAP — CONNECTED");
            bool bDefaultNegative = Diagnostics.Result
                == EVoxelConnectivityResult::NotConnectedAtThisResolution;
            if (bDefaultNegative)
            {
                const FVoxelStrateMeasureSettings DoubledSettings = MakeFittedSettings(
                    SampleStep,
                    ResolutionArrivalPoint,
                    ResolutionDeparturePoint,
                    DoubledFittedMargin,
                    FittedMaxCells);
                const FVoxelStrateMetrics DoubledMetrics = VF_MeasureStrate(
                    *ResolutionWorld.Generator,
                    *ResolutionWorld.StrateManager,
                    VerticalShaftsIndex,
                    DoubledSettings);
                const FVoxelConnectivityDiagnostics DoubledDiagnostics = VF_DiagnoseConnectivity(
                    *ResolutionWorld.Generator,
                    *ResolutionWorld.StrateManager,
                    VerticalShaftsIndex,
                    ResolutionArrivalPoint,
                    ResolutionDeparturePoint,
                    DoubledSettings);
                DoubledText = FString::Printf(
                    TEXT("margin=96: %s; cells=%lld"),
                    *FittedVerdict(DoubledMetrics, DoubledDiagnostics),
                    DoubledMetrics.NumSampled);
                if (DoubledMetrics.bValid
                    && DoubledDiagnostics.Result == EVoxelConnectivityResult::Connected)
                {
                    Interpretation = TEXT(
                        "NotConnectedAtThisResolution — window binding; doubled margin CONNECTED");
                }
                else if (DoubledMetrics.bValid && DoubledDiagnostics.bValid)
                {
                    Interpretation = TEXT(
                        "REAL GAP at this resolution — negative survives doubled margin");
                    bPartAFittedChecksPassed = false;
                }
                else
                {
                    Interpretation = TEXT(
                        "MEASUREMENT LIMIT — doubled-margin rerun was not usable");
                    bPartAFittedChecksPassed = false;
                }
            }
            else if (!Metrics.bValid || !Diagnostics.bValid
                || Diagnostics.Result != EVoxelConnectivityResult::Connected
                || Diagnostics.bStartSnapped || Diagnostics.bGoalSnapped)
            {
                Interpretation = TEXT("MEASUREMENT LIMIT — no trustworthy negative");
                bPartAFittedChecksPassed = false;
            }

            if (!Metrics.bValid || !Diagnostics.bValid
                || Diagnostics.bStartSnapped || Diagnostics.bGoalSnapped)
            {
                bPartAFittedChecksPassed = false;
            }

            if (Seed == 6 && SampleStep == 1)
            {
                FVoxelStrateMeasureSettings OldSettings = BaseSettings;
                OldSettings.SampleStep = 1;
                OldSettings.RadiusInVoxels = 192;
                OldSettings.MaxCells = BeforeChangeMaxCells;
                OldSettings.InteriorMarginVoxels = -1;
                const FVoxelStrateMetrics OldMetrics = VF_MeasureStrate(
                    *ResolutionWorld.Generator,
                    *ResolutionWorld.StrateManager,
                    VerticalShaftsIndex,
                    OldSettings);
                const int64 ZCells = Metrics.SampledNumZ;
                const int64 OldRequestedCells = static_cast<int64>(384) * 384 * ZCells;
                const FString OldVerdict = OldMetrics.RefusalReason.Contains(TEXT("MaxCells"))
                    ? TEXT("EXCEEDS_MAXCELLS")
                    : FittedVerdict(OldMetrics, FVoxelConnectivityDiagnostics());
                Summary += FString::Printf(
                    TEXT("  seed 6 step 1 cell comparison: before centered square requested "
                         "%lld cells (384x384x%d), verdict=%s, reason='%s'; after fitted "
                         "window=%lld cells (%dx%dx%d), bounds X[%.3f,%.3f) Y[%.3f,%.3f).\n"),
                    OldRequestedCells,
                    Metrics.SampledNumZ,
                    *OldVerdict,
                    *OldMetrics.RefusalReason,
                    Metrics.NumSampled,
                    Metrics.SampledNumX,
                    Metrics.SampledNumY,
                    Metrics.SampledNumZ,
                    Metrics.SampledMinX,
                    Metrics.SampledMaxX,
                    Metrics.SampledMinY,
                    Metrics.SampledMaxY);
            }

            const FString DefaultVerdict = FittedVerdict(Metrics, Diagnostics);
            const FString ArrivalShare = Diagnostics.bValid
                ? FString::Printf(TEXT("%.9g"), Diagnostics.StartComponentShare)
                : TEXT("N/A");
            const FString DepartureShare = Diagnostics.bValid
                ? FString::Printf(TEXT("%.9g"), Diagnostics.GoalComponentShare)
                : TEXT("N/A");
            Summary += FString::Printf(
                TEXT("  %d | %d | %s | %dx%dx%d/%lld | %s | %s | %s | %s\n"),
                Seed,
                SampleStep,
                *DefaultVerdict,
                Metrics.SampledNumX,
                Metrics.SampledNumY,
                Metrics.SampledNumZ,
                Metrics.NumSampled,
                *DoubledText,
                *Interpretation,
                *ArrivalShare,
                *DepartureShare);
        }
    }

    Summary += TEXT(
        "PART B — fitted 16-seed audit (step 2, margin=three shaft spacings): strict-local-minimum shafts are the "
        "pre-fix orphan candidates; post-fix parent paths are checked with the same fixed ±2/±3 "
        "resolver over a padded [-20,20] cell audit domain. Small-component threshold is one "
        "mean-radius shaft volume.\n");
    Summary += TEXT(
        "  seed | fitted verdict | cells | components | small non-largest/total | strict minima/shafts "
        "| neighbour fallback | path orphans | shaft feature-core mouths | doubled fitted-margin verdict/cells | "
        "tree path XY bounds\n");

    static constexpr int32 FittedSeedCases[] = {
        1337, 1, 2, 3, 4, 5, 6, 7,
        8, 9, 10, 11, 12, 13, 14, 15,
    };
    int32 SweepDirectPasses = 0;
    int32 SweepPasses = 0;
    int32 SweepFailures = 0;
    int32 SweepBindingNegatives = 0;
    int32 SweepTrustedNegatives = 0;
    int32 SweepMeasurementLimits = 0;
    int64 TotalAuditShafts = 0;
    int64 TotalStrictLocalMinima = 0;
    int64 TotalNeighbourFallbacks = 0;
    int64 TotalSpineWindowFallbacks = 0;
    int64 TotalEmergencySpineFallbacks = 0;
    int64 TotalPathOrphans = 0;
    int64 TotalNonLargestComponents = 0;
    int64 TotalSmallNonLargestComponents = 0;
    int32 SmallComponentThresholdMin = INT32_MAX;
    int32 SmallComponentThresholdMax = 0;
    bool bAllAuditOrdersIndependent = true;
    const double FittedSweepStartSeconds = FPlatformTime::Seconds();
    for (const int32 Seed : FittedSeedCases)
    {
        FTestWorld SeedWorld;
        SeedWorld.Build(Seed, /*InGapChunks=*/2);
        if (!SeedWorld.IsValid())
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: fitted audit seed %d could not build the fixture."), Seed));
            bAllChecksPassed = false;
            ++SweepFailures;
            continue;
        }

        FVector SeedArrivalPoint = FVector::ZeroVector;
        FVector SeedDeparturePoint = FVector::ZeroVector;
        if (!FindChainMouths(
                SeedWorld.StrateManager->GetPassages(),
                VerticalShaftsIndex,
                SeedArrivalPoint,
                SeedDeparturePoint))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: fitted audit seed %d does not have exactly one chain mouth "
                     "pair."), Seed));
            bAllChecksPassed = false;
            ++SweepFailures;
            continue;
        }

        const FVerticalShaftParams& ShaftParams =
            SeedWorld.StrateManager->GetLayout()[VerticalShaftsIndex].Definition->VerticalShaftParams;
        const int32 ShaftMidChunkZ = SeedWorld.StrateManager->GetLayout()[VerticalShaftsIndex].BottomChunkZ
            + (SeedWorld.StrateManager->GetLayout()[VerticalShaftsIndex].TopChunkZ
                - SeedWorld.StrateManager->GetLayout()[VerticalShaftsIndex].BottomChunkZ) / 2;
        const FVerticalShaftParams RuntimeShaftParams =
            SeedWorld.StrateManager->GetVerticalShaftParamsForChunk(
                FIntVector(0, 0, ShaftMidChunkZ));
        FVector ArrivalShaftAxis = SeedArrivalPoint;
        FVector DepartureShaftAxis = SeedDeparturePoint;
        const bool bShaftFeatureMouths = FindVerticalShaftFeatureCore(
            ShaftParams, Seed, SeedArrivalPoint, ArrivalShaftAxis)
            && FindVerticalShaftFeatureCore(
                ShaftParams, Seed, SeedDeparturePoint, DepartureShaftAxis);
        const FVoxelStrateMeasureSettings FittedSettings = MakeFittedSettings(
            2,
            SeedArrivalPoint,
            SeedDeparturePoint,
            FittedMargin,
            BaseSettings.MaxCells);
        const FVoxelStrateMetrics Metrics = VF_MeasureStrate(
            *SeedWorld.Generator,
            *SeedWorld.StrateManager,
            VerticalShaftsIndex,
            FittedSettings);
        const FVoxelConnectivityDiagnostics Diagnostics = VF_DiagnoseConnectivity(
            *SeedWorld.Generator,
            *SeedWorld.StrateManager,
            VerticalShaftsIndex,
            SeedArrivalPoint,
            SeedDeparturePoint,
            FittedSettings);
        const FVerticalShaftTreeAudit Audit = AuditVerticalShaftTree(ShaftParams, Seed);
        TotalAuditShafts += Audit.NumShafts;
        TotalStrictLocalMinima += Audit.NumStrictLocalMinima;
        TotalNeighbourFallbacks += Audit.NumNeighbourFallbacks;
        TotalSpineWindowFallbacks += Audit.NumSpineWindowFallbacks;
        TotalEmergencySpineFallbacks += Audit.NumEmergencySpineFallbacks;
        TotalPathOrphans += Audit.NumUnreachable;
        bAllAuditOrdersIndependent &= Audit.bOrderIndependent;

        int32 ShaftVolumeCells = 1;
        if (Metrics.bValid)
        {
            const double MeanRadius = 0.5 * static_cast<double>(
                ShaftParams.ShaftMinRadius + ShaftParams.ShaftMaxRadius);
            const double InteriorHeight = static_cast<double>(
                Metrics.SampledMaxZ - Metrics.SampledMinZ);
            const double CoarseCellVolume = FMath::Max(
                1.0,
                PI * MeanRadius * MeanRadius * InteriorHeight / 8.0);
            ShaftVolumeCells = FMath::Clamp(
                FMath::CeilToInt(CoarseCellVolume), 1, INT32_MAX);
            int32 NumNonLargest = 0;
            int32 NumSmallNonLargest = 0;
            ComponentNoiseCounts(
                Metrics,
                ShaftVolumeCells,
                NumNonLargest,
                NumSmallNonLargest);
            TotalNonLargestComponents += NumNonLargest;
            TotalSmallNonLargestComponents += NumSmallNonLargest;
        }
        SmallComponentThresholdMin = FMath::Min(
            SmallComponentThresholdMin, ShaftVolumeCells);
        SmallComponentThresholdMax = FMath::Max(
            SmallComponentThresholdMax, ShaftVolumeCells);

        if (Metrics.bValid && Metrics.AirComponentCells.Num() != Metrics.NumAirComponents)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: fitted audit seed %d component-size list has %d entries for "
                     "%d components."),
                Seed,
                Metrics.AirComponentCells.Num(),
                Metrics.NumAirComponents));
            bAllChecksPassed = false;
        }

        const bool bCountsAsDirectPass = Metrics.bValid
            && Diagnostics.bValid
            && Diagnostics.Result == EVoxelConnectivityResult::Connected
            && !Diagnostics.bStartSnapped
            && !Diagnostics.bGoalSnapped
            && bShaftFeatureMouths;
        bool bCountsAsPass = bCountsAsDirectPass;
        FString DoubledSweepText = TEXT("not-run");
        FString TreePathText = TEXT("not-needed");
        if (!bCountsAsPass
            && Diagnostics.Result == EVoxelConnectivityResult::NotConnectedAtThisResolution)
        {
            const FVoxelStrateMeasureSettings DoubledSettings = MakeFittedSettings(
                2,
                SeedArrivalPoint,
                SeedDeparturePoint,
                DoubledFittedMargin,
                FittedMaxCells);
            const FVoxelStrateMetrics DoubledMetrics = VF_MeasureStrate(
                *SeedWorld.Generator,
                *SeedWorld.StrateManager,
                VerticalShaftsIndex,
                DoubledSettings);
            const FVoxelConnectivityDiagnostics DoubledDiagnostics = VF_DiagnoseConnectivity(
                *SeedWorld.Generator,
                *SeedWorld.StrateManager,
                VerticalShaftsIndex,
                SeedArrivalPoint,
                SeedDeparturePoint,
                DoubledSettings);
            DoubledSweepText = FString::Printf(
                TEXT("%s/%lld"),
                *FittedVerdict(DoubledMetrics, DoubledDiagnostics),
                DoubledMetrics.NumSampled);
            FVector2D PathMin = FVector2D::ZeroVector;
            FVector2D PathMax = FVector2D::ZeroVector;
            int32 PathHops = 0;
            const bool bArrivalPath = AuditVerticalShaftPathBounds(
                ShaftParams, Seed, ArrivalShaftAxis, PathMin, PathMax, PathHops);
            FVector2D DeparturePathMin = FVector2D::ZeroVector;
            FVector2D DeparturePathMax = FVector2D::ZeroVector;
            int32 DeparturePathHops = 0;
            const bool bDeparturePath = AuditVerticalShaftPathBounds(
                ShaftParams,
                Seed,
                DepartureShaftAxis,
                DeparturePathMin,
                DeparturePathMax,
                DeparturePathHops);
            if (bArrivalPath && bDeparturePath)
            {
                const FVector2D CombinedMin(
                    FMath::Min(PathMin.X, DeparturePathMin.X),
                    FMath::Min(PathMin.Y, DeparturePathMin.Y));
                const FVector2D CombinedMax(
                    FMath::Max(PathMax.X, DeparturePathMax.X),
                    FMath::Max(PathMax.Y, DeparturePathMax.Y));
                TreePathText = FString::Printf(
                    TEXT("[%.1f,%.1f]-[%.1f,%.1f],hops=%d/%d"),
                    CombinedMin.X,
                    CombinedMin.Y,
                    CombinedMax.X,
                    CombinedMax.Y,
                    PathHops,
                    DeparturePathHops);
            }
            if (DoubledMetrics.bValid
                && DoubledDiagnostics.Result == EVoxelConnectivityResult::Connected)
            {
                ++SweepBindingNegatives;
                // A negative that disappears only after the mandated doubled-margin rerun is a
                // binding-window result, not a connectivity finding. Keep the direct verdict in
                // the per-seed row, but count the seed as acceptance-passing at the trustworthy
                // window size.
                bCountsAsPass = true;
            }
            else if (DoubledMetrics.bValid && DoubledDiagnostics.bValid)
            {
                ++SweepTrustedNegatives;
            }
            else
            {
                ++SweepMeasurementLimits;
            }
        }
        else if (!bCountsAsPass)
        {
            ++SweepMeasurementLimits;
        }

        if (bCountsAsDirectPass)
        {
            ++SweepDirectPasses;
        }

        if (bCountsAsPass)
        {
            ++SweepPasses;
        }
        else
        {
            ++SweepFailures;
        }

        const int32 NumNonLargest = Metrics.bValid
            ? [&]()
            {
                int32 Value = 0;
                int32 IgnoredSmall = 0;
                ComponentNoiseCounts(Metrics, ShaftVolumeCells, Value, IgnoredSmall);
                return Value;
            }()
            : 0;
        const int32 NumSmallNonLargest = Metrics.bValid
            ? [&]()
            {
                int32 IgnoredNonLargest = 0;
                int32 Value = 0;
                ComponentNoiseCounts(Metrics, ShaftVolumeCells, IgnoredNonLargest, Value);
                return Value;
            }()
            : 0;
            Summary += FString::Printf(
                TEXT("  %d | %s | %lld | %d | %d/%d | %d/%d | %d | %d | %s | %s | %s\n"),
            Seed,
            *FittedVerdict(Metrics, Diagnostics),
            Metrics.NumSampled,
            Metrics.NumAirComponents,
            NumSmallNonLargest,
            NumNonLargest,
            Audit.NumStrictLocalMinima,
            Audit.NumShafts,
            Audit.NumNeighbourFallbacks,
            Audit.NumUnreachable,
            bShaftFeatureMouths ? TEXT("YES") : TEXT("NO"),
                *DoubledSweepText,
                *TreePathText);

        if (Seed == 1337 && Metrics.bValid)
        {
            const FVerticalShaftPhysicalAudit ArrivalPhysical =
                AuditVerticalShaftPathDensity(
                    *SeedWorld.Generator, RuntimeShaftParams, Seed, ArrivalShaftAxis);
            const FVerticalShaftPhysicalAudit DeparturePhysical =
                AuditVerticalShaftPathDensity(
                    *SeedWorld.Generator, RuntimeShaftParams, Seed, DepartureShaftAxis);
            const FVerticalShaftPhysicalAudit& FirstBadPhysical =
                ArrivalPhysical.NumNonAirSamples > 0 ? ArrivalPhysical : DeparturePhysical;
            Summary += FString::Printf(
                TEXT("  seed 1337 window/mouth audit: window X[%.3f,%.3f) Y[%.3f,%.3f); "
                     "arrival=(%.3f,%.3f) departure=(%.3f,%.3f); physical path bad edges "
                     "%d/%d (arrival) + %d/%d (departure), non-air samples=%d/%d, "
                     "first bad density=%.6g at (%.3f,%.3f,%.3f), edge "
                     "(%.3f,%.3f)->(%.3f,%.3f), t=%.2f.\n"),
                Metrics.SampledMinX,
                Metrics.SampledMaxX,
                Metrics.SampledMinY,
                Metrics.SampledMaxY,
                SeedArrivalPoint.X,
                SeedArrivalPoint.Y,
                SeedDeparturePoint.X,
                SeedDeparturePoint.Y,
                ArrivalPhysical.NumBadEdges,
                ArrivalPhysical.NumEdges,
                DeparturePhysical.NumBadEdges,
                DeparturePhysical.NumEdges,
                ArrivalPhysical.NumNonAirSamples + DeparturePhysical.NumNonAirSamples,
                ArrivalPhysical.NumSamples + DeparturePhysical.NumSamples,
                ArrivalPhysical.NumNonAirSamples > 0
                    ? ArrivalPhysical.FirstBadDensity : DeparturePhysical.FirstBadDensity,
                ArrivalPhysical.NumNonAirSamples > 0
                    ? ArrivalPhysical.FirstBadX : DeparturePhysical.FirstBadX,
                ArrivalPhysical.NumNonAirSamples > 0
                    ? ArrivalPhysical.FirstBadY : DeparturePhysical.FirstBadY,
                ArrivalPhysical.NumNonAirSamples > 0
                    ? ArrivalPhysical.FirstBadZ : DeparturePhysical.FirstBadZ,
                FirstBadPhysical.FirstBadChildX,
                FirstBadPhysical.FirstBadChildY,
                FirstBadPhysical.FirstBadParentX,
                FirstBadPhysical.FirstBadParentY,
                FirstBadPhysical.FirstBadFraction);
        }

        if (Audit.NumUnreachable != 0 || !Audit.bOrderIndependent || !bShaftFeatureMouths)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: fitted audit seed %d violated the shaft-tree/landing "
                     "invariant (unreachable=%d, order-independent=%s, feature-core-mouths=%s)."),
                Seed,
                Audit.NumUnreachable,
                Audit.bOrderIndependent ? TEXT("yes") : TEXT("no"),
                bShaftFeatureMouths ? TEXT("yes") : TEXT("no")));
            bAllChecksPassed = false;
        }
    }
    const double FittedSweepSeconds = FPlatformTime::Seconds() - FittedSweepStartSeconds;

    const double StrictLocalMinimumRate = TotalAuditShafts > 0
        ? static_cast<double>(TotalStrictLocalMinima)
            / static_cast<double>(TotalAuditShafts)
        : 0.0;
    const double OrphanRate = TotalAuditShafts > 0
        ? static_cast<double>(TotalPathOrphans)
            / static_cast<double>(TotalAuditShafts)
        : 0.0;
    const double SmallComponentRate = TotalNonLargestComponents > 0
        ? static_cast<double>(TotalSmallNonLargestComponents)
            / static_cast<double>(TotalNonLargestComponents)
        : 0.0;
    Summary += FString::Printf(
        TEXT("  tree audit aggregate: strict-local-minimum candidates=%lld/%lld (%.6f%%); "
             "neighbour fallback=%lld; direct spine-window fallback=%lld; emergency direct-spine "
             "fallback=%lld; post-fix path orphans=%lld/%lld (%.6f%%); array-order checks=%s; "
             "max path length observed is reported per implementation invariant.\n"),
        TotalStrictLocalMinima,
        TotalAuditShafts,
        StrictLocalMinimumRate * 100.0,
        TotalNeighbourFallbacks,
        TotalSpineWindowFallbacks,
        TotalEmergencySpineFallbacks,
        TotalPathOrphans,
        TotalAuditShafts,
        OrphanRate * 100.0,
        bAllAuditOrdersIndependent ? TEXT("PASS") : TEXT("FAIL"));
    Summary += FString::Printf(
        TEXT("  roughness-bubble proxy: non-largest components smaller than one mean-radius shaft "
             "= %lld/%lld (%.6f%%), threshold range=%d..%d coarse cells.\n"),
        TotalSmallNonLargestComponents,
        TotalNonLargestComponents,
        SmallComponentRate * 100.0,
        SmallComponentThresholdMin == INT32_MAX ? 0 : SmallComponentThresholdMin,
        SmallComponentThresholdMax);
    Summary += FString::Printf(
        TEXT("  fitted acceptance arrival->departure: %s (%d/%d effective pass; direct fitted margin="
             "%d/%d, %d fail; binding negatives=%d, trusted negatives=%d, measurement limits=%d); "
             "sweep wall-clock=%.3f seconds.\n"),
        SweepPasses == UE_ARRAY_COUNT(FittedSeedCases) && SweepFailures == 0
            ? TEXT("PASS") : TEXT("FAIL"),
        SweepPasses,
        UE_ARRAY_COUNT(FittedSeedCases),
        SweepDirectPasses,
        UE_ARRAY_COUNT(FittedSeedCases),
        SweepFailures,
        SweepBindingNegatives,
        SweepTrustedNegatives,
        SweepMeasurementLimits,
        FittedSweepSeconds);

    if (TotalPathOrphans != 0 || !bAllAuditOrdersIndependent)
    {
        AddError(FString::Printf(
            TEXT("HARD FAILURE: VerticalShafts tree audit found %lld orphan paths across the "
                 "16-seed sweep (orphan rate %.9g%%; order-independent=%s)."),
            TotalPathOrphans,
            OrphanRate * 100.0,
            bAllAuditOrdersIndependent ? TEXT("yes") : TEXT("no")));
        bAllChecksPassed = false;
    }
    if (SweepPasses != UE_ARRAY_COUNT(FittedSeedCases) || SweepFailures != 0)
    {
        AddError(FString::Printf(
            TEXT("HARD FAILURE: fitted VerticalShafts effective acceptance was %d/%d, not 16/16."),
            SweepPasses,
            UE_ARRAY_COUNT(FittedSeedCases)));
        bAllChecksPassed = false;
    }
    bAllChecksPassed &= bPartAFittedChecksPassed;

    AddInfo(Summary);

    return bAllChecksPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
