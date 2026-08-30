// VoxelForgeStrateConnectivityTest.cpp
// Tier 2: the measurement pass and its coarse-connectivity false-positive guard.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelStrateMeasure.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeStrateConnectivityTest,
    "VoxelForge.Generation.StrateConnectivity",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeStrateConnectivityRefinementTest,
    "VoxelForge.Generation.StrateConnectivityRefinement",
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
            && A.ResolvedMarginVoxels == B.ResolvedMarginVoxels
            && A.SampledMinZ == B.SampledMinZ
            && A.SampledMaxZ == B.SampledMaxZ;
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
        case EVoxelConnectivityResult::OutOfWindow:    return TEXT("OUT_OF_WINDOW");
        case EVoxelConnectivityResult::CoarseLied:     return TEXT("COARSE_LIED");
        default:                                       return TEXT("UNKNOWN");
        }
    }

    FString ConnectivityProbeText(const FConnectivityProbe& Probe)
    {
        if (!Probe.bChecked)
        {
            return TEXT("NOT_CHECKED");
        }

        FString Text = ConnectivityResultName(Probe.Result);
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
                OutReport.ArrivalToDeparture.bGoalSnapped);
        }
        return true;
    }
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
     *    full-resolution solid sample; the controlled 3-voxel wall below must return COARSE_LIED.
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
    int32 NumAnchorCoarseLied = 0;
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
            Probe.bGoalSnapped);

        ++NumRoutesChecked;
        if (Probe.Result == EVoxelConnectivityResult::CoarseLied)
        {
            ++NumRoutesRefuted;
        }
        if (bCountInGuard)
        {
            ++NumGuardRoutesChecked;
            if (Probe.Result == EVoxelConnectivityResult::CoarseLied)
            {
                ++NumGuardRoutesRefuted;
            }
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
            case EVoxelConnectivityResult::CoarseLied:     ++NumAnchorCoarseLied; break;
            default: break;
            }
        }
        return Probe;
    };

    for (int32 StrateIndex = 0; StrateIndex < Layout.Num(); ++StrateIndex)
    {
        const FStrateSlot& Slot = Layout[StrateIndex];
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
        if (Probe.Result == EVoxelConnectivityResult::Connected
            && FullResolutionDirectSegmentHasSolid(*World.Generator, A, B))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: VF_AreConnected returned CONNECTED for %s while its "
                     "full-resolution direct walk contained a solid sample."),
                *ArchetypeName(Slot)));
            bAllChecksPassed = false;
        }
    }

    // Controlled negative route: at SampleStep 4, the two coarse centres at X=-2 and X=+2
    // remain air while the three full-resolution voxels at X=-1,0,+1 are filled solid.
    const FVoxelStrateMetrics& WallWindow = DerivedMetricsByIndex[0];
    const float WallZ = static_cast<float>(WallWindow.SampledMinZ)
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
    if (WallProbe.Result != EVoxelConnectivityResult::CoarseLied)
    {
        AddError(FString::Printf(
            TEXT("HARD FAILURE: the controlled coarse-connectivity lie returned %s instead of "
                 "COARSE_LIED."),
            ConnectivityResultName(WallProbe.Result)));
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
        TEXT("Endpoint snap repair: %d snapped endpoint events across all probes; %d in "
             "%d arrival->departure probes.\n"),
        NumSnappedEndpointEvents,
        NumArrivalDepartureSnappedEndpointEvents,
        NumArrivalDepartureRoutesChecked);
    Summary += FString::Printf(
        TEXT("Correct largest-component endpoint probes (%d, including the surface-entry auxiliary): "
             "connected=%d, not-connected-at-resolution=%d, start-cell-solid=%d, goal-cell-solid=%d, "
             "out-of-window=%d, coarse-lied=%d, snapped endpoint events=%d.\n"),
        NumAnchorProbes,
        NumAnchorConnected,
        NumAnchorNotConnectedAtThisResolution,
        NumAnchorStartCellSolid,
        NumAnchorGoalCellSolid,
        NumAnchorOutOfWindow,
        NumAnchorCoarseLied,
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
    BaseSettings.MaxCells = 8000000;
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
        {4, 128},
        {1, 128},
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
            Row.Probe.bGoalSnapped);

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
            Row.Probe.bGoalSnapped);

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
    int32 NumMouthToSpineCoarseLied = 0;
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
            NumMouthToSpineCoarseLied += Probe->Result == EVoxelConnectivityResult::CoarseLied;
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
        "radius 192), and (step 4, radius 128) vs (step 1, radius 128).\n");
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
             "spine component largest=%d/%d; mouth->spine CONNECTED=%d/%d, COARSE_LIED=%d.\n"),
        NumSpineFullWindowRuns,
        SpineReports.Num(),
        NumSpineOneOpenRuns,
        SpineReports.Num(),
        NumSpineLargestComponents,
        SpineReports.Num(),
        NumMouthToSpineConnected,
        NumMouthToSpineQueries,
        NumMouthToSpineCoarseLied);
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

    if (bMarginSweepUsable)
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
            TEXT("  diagnostic route verdict: %s; endpoint snaps: start=%s, goal=%s.\n"),
            ConnectivityResultName(Diagnostics.Result),
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
        int32 SeedArrivalToSpineResultCounts[6] = {};
        int32 SeedDepartureToSpineResultCounts[6] = {};
        int32 SeedArrivalToDepartureResultCounts[6] = {};
        int32 SeedFullWindowRuns = 0;
        int32 SeedOneOpenRuns = 0;
        int32 SeedSpineLargest = 0;
        int32 SeedSpineNoOpenCell = 0;
        float SeedSpineShareMin = FLT_MAX;
        float SeedSpineShareMax = -FLT_MAX;
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

            const auto CountSeedResult = [](const FConnectivityProbe& Probe, int32 (&Counts)[6])
            {
                if (Probe.bChecked)
                {
                    ++Counts[static_cast<int32>(Probe.Result)];
                }
            };
            CountSeedResult(SeedReport.ArrivalToSpine, SeedArrivalToSpineResultCounts);
            CountSeedResult(SeedReport.DepartureToSpine, SeedDepartureToSpineResultCounts);
            CountSeedResult(SeedReport.ArrivalToDeparture, SeedArrivalToDepartureResultCounts);

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
            if (SeedResult == EVoxelConnectivityResult::Connected
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
            TEXT("  seed result breakdown — arrival->spine: CONNECTED=%d, "
                 "NOT_CONNECTED_AT_THIS_RESOLUTION=%d, START_CELL_SOLID=%d, GOAL_CELL_SOLID=%d, "
                 "OUT_OF_WINDOW=%d, COARSE_LIED=%d; departure->spine: CONNECTED=%d, "
                 "NOT_CONNECTED_AT_THIS_RESOLUTION=%d, START_CELL_SOLID=%d, GOAL_CELL_SOLID=%d, "
                 "OUT_OF_WINDOW=%d, COARSE_LIED=%d; arrival->departure: CONNECTED=%d, "
                 "NOT_CONNECTED_AT_THIS_RESOLUTION=%d, START_CELL_SOLID=%d, GOAL_CELL_SOLID=%d, "
                 "OUT_OF_WINDOW=%d, COARSE_LIED=%d.\n"),
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::Connected)],
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::NotConnectedAtThisResolution)],
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::StartCellSolid)],
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::GoalCellSolid)],
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::OutOfWindow)],
            SeedArrivalToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::CoarseLied)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::Connected)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::NotConnectedAtThisResolution)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::StartCellSolid)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::GoalCellSolid)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::OutOfWindow)],
            SeedDepartureToSpineResultCounts[static_cast<int32>(EVoxelConnectivityResult::CoarseLied)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::Connected)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::NotConnectedAtThisResolution)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::StartCellSolid)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::GoalCellSolid)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::OutOfWindow)],
            SeedArrivalToDepartureResultCounts[static_cast<int32>(EVoxelConnectivityResult::CoarseLied)]);
        Summary += FString::Printf(
            TEXT("  seed endpoint snaps — arrival->spine=%d, departure->spine=%d, "
                 "arrival->departure=%d.\n"),
            SeedArrivalToSpineSnapEvents,
            SeedDepartureToSpineSnapEvents,
            SeedArrivalToDepartureSnapEvents);
        Summary += TEXT("  seed | spine air | full run | one open run | spine share | spine largest | "
                       "arrival->spine | departure->spine | arrival->departure\n");
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
            Summary += FString::Printf(
                TEXT("  %d | %.9g | %s | %s | %s | %s | %s | %s | %s\n"),
                SeedReport.Seed,
                SeedReport.Spine.AirFraction,
                SeedReport.Spine.bFullWindowRun ? TEXT("YES") : TEXT("NO"),
                SeedReport.Spine.bAirSamplesFormOneRun ? TEXT("YES") : TEXT("NO"),
                *SpineShare,
                *SpineLargest,
                *ConnectivityProbeText(SeedReport.ArrivalToSpine),
                *ConnectivityProbeText(SeedReport.DepartureToSpine),
                *ConnectivityProbeText(SeedReport.ArrivalToDeparture));
        }
    }
    else
    {
        Summary += TEXT(
            "Part C post-fix measurement and multi-seed sweep: SKIPPED because the margin "
            "measurement set was not usable.\n");
    }

    AddInfo(Summary);

    return bAllChecksPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
