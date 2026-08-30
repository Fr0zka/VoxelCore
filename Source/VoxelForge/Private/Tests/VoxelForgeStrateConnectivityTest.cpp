// VoxelForgeStrateConnectivityTest.cpp
// Tier 2: the measurement pass and its coarse-connectivity false-positive guard.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelStrateMeasure.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeStrateConnectivityTest,
    "VoxelForge.Generation.StrateConnectivity",
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
        case EVoxelConnectivityResult::NotConnected:   return TEXT("NOT_CONNECTED");
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
    int32 NumAnchorNotConnected = 0;
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
            case EVoxelConnectivityResult::NotConnected:   ++NumAnchorNotConnected; break;
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
             "connected=%d, not-connected=%d, start-cell-solid=%d, goal-cell-solid=%d, "
             "out-of-window=%d, coarse-lied=%d, snapped endpoint events=%d.\n"),
        NumAnchorProbes,
        NumAnchorConnected,
        NumAnchorNotConnected,
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

#endif // WITH_DEV_AUTOMATION_TESTS
