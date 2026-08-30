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
        FVoxelStrateMetrics First;
        FVoxelStrateMetrics Second;
    };

    struct FPassageReport
    {
        int32 Index = INDEX_NONE;
        FString Name;
        FString UpperResult;
        FString LowerResult;
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
            && SameFloatBits(A.WalkableFraction, B.WalkableFraction)
            && SameFloatBits(A.MedianFeatureScale, B.MedianFeatureScale)
            && A.MedianVerticalClearance == B.MedianVerticalClearance;
    }

    bool IsStrictInteriorPoint(const FStrateSlot& Slot, const FVector& Point)
    {
        const float MinZ = (static_cast<float>(Slot.BottomChunkZ) + 1.0f) * CHUNK_SIZE;
        const float MaxZ = static_cast<float>(Slot.TopChunkZ) * CHUNK_SIZE;
        return Point.Z >= MinZ && Point.Z < MaxZ;
    }

    FVector LargestComponentAnchor(const FStrateSlot& Slot, const FVector2D& CenterXY)
    {
        // The fixture's origin spine is the deterministic hub/void anchor. The measurement API
        // intentionally returns metrics rather than a component coordinate, so this report probes
        // the known spine point that represents the largest reachable component in this fixture.
        const float AnchorZ = (static_cast<float>(Slot.BottomChunkZ) + 1.0f) * CHUNK_SIZE + 2.0f;
        return FVector(CenterXY.X + 2.0f, CenterXY.Y + 2.0f, AnchorZ);
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
     * 5. Coarse-lie guard: a coarse route must never be reported connected after a full-resolution
     *    solid sample; the controlled 3-voxel wall below must make bOutCoarseLied fire.
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
    const int32 SolidControlMinZ = (SolidControlSlot.BottomChunkZ + 1) * CHUNK_SIZE;
    const int32 SolidControlMaxZ = SolidControlSlot.TopChunkZ * CHUNK_SIZE;
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
        *World.Generator, *World.StrateManager, 0, Settings);
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
    for (int32 StrateIndex = 0; StrateIndex < Layout.Num(); ++StrateIndex)
    {
        FArchetypeReport& Report = ArchetypeReports.AddDefaulted_GetRef();
        Report.Index = StrateIndex;
        Report.Name = ArchetypeName(Layout[StrateIndex]);
        Report.First = VF_MeasureStrate(
            *World.Generator, *World.StrateManager, StrateIndex, Settings);
        Report.Second = VF_MeasureStrate(
            *World.Generator, *World.StrateManager, StrateIndex, Settings);

        const FVoxelStrateMetrics& Metrics = Report.First;
        if (!Metrics.bValid)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: %s measurement refused: %s."),
                *Report.Name,
                *Metrics.RefusalReason));
            bAllChecksPassed = false;
        }
        if (Metrics.NumSampled == 0 || Metrics.NumAir == 0 || Metrics.NumSolid == 0)
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: vacuous %s measurement — sampled=%lld, air=%lld, solid=%lld."),
                *Report.Name,
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
            || (Metrics.LargestComponentShare == 0.0f && Metrics.AirFraction > 0.05f))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: range/component sanity failed for %s."), *Report.Name));
            bAllChecksPassed = false;
        }
        if (!MetricsAreBitIdentical(Report.First, Report.Second))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: %s measurement was not bit-identical on the second call."),
                *Report.Name));
            bAllChecksPassed = false;
        }
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
    Summary += TEXT("Archetypes (sorted):\n");
    for (const FArchetypeReport& Report : ArchetypeReports)
    {
        const FVoxelStrateMetrics& Metrics = Report.First;
        Summary += FString::Printf(
            TEXT("  %s: Air=%.9g, Largest=%.9g, Walkable=%.9g, FeatureScale=%.9g vox, "
                 "Clearance=%d vox, Components=%d, Samples=%lld, Solid=%lld\n"),
            *Report.Name,
            Metrics.AirFraction,
            Metrics.LargestComponentShare,
            Metrics.WalkableFraction,
            Metrics.MedianFeatureScale,
            Metrics.MedianVerticalClearance,
            Metrics.NumAirComponents,
            Metrics.NumSampled,
            Metrics.NumSolid);
    }

    int32 NumRoutesChecked = 0;
    int32 NumRoutesRefuted = 0;
    for (int32 StrateIndex = 0; StrateIndex < Layout.Num(); ++StrateIndex)
    {
        const FStrateSlot& Slot = Layout[StrateIndex];
        const int32 InteriorMinZ = (Slot.BottomChunkZ + 1) * CHUNK_SIZE;
        const int32 InteriorMaxZ = Slot.TopChunkZ * CHUNK_SIZE;
        if (InteriorMinZ >= InteriorMaxZ) continue;

        const FVector A(Settings.CenterXY.X + 2.0f, Settings.CenterXY.Y + 2.0f,
                        static_cast<float>(InteriorMinZ + 2));
        const FVector B(Settings.CenterXY.X + 2.0f, Settings.CenterXY.Y + 2.0f,
                        static_cast<float>(InteriorMaxZ - 2));
        bool bCoarseLied = false;
        const bool bConnected = VF_AreConnected(
            *World.Generator, *World.StrateManager, StrateIndex, A, B, Settings, bCoarseLied);
        ++NumRoutesChecked;
        if (bCoarseLied) ++NumRoutesRefuted;
        if (bConnected && FullResolutionDirectSegmentHasSolid(*World.Generator, A, B))
        {
            AddError(FString::Printf(
                TEXT("HARD FAILURE: VF_AreConnected returned true for %s while its full-resolution "
                     "direct walk contained a solid sample."),
                *ArchetypeName(Slot)));
            bAllChecksPassed = false;
        }
    }

    // Controlled negative route: at SampleStep 4, the two coarse centres at X=-2 and X=+2
    // remain air while the three full-resolution voxels at X=-1,0,+1 are filled solid.
    const FStrateSlot& WallSlot = Layout[0];
    const int32 WallInteriorMinZ = (WallSlot.BottomChunkZ + 1) * CHUNK_SIZE;
    const float WallZ = static_cast<float>(WallInteriorMinZ + 2);
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

    bool bWallCoarseLied = false;
    const bool bWallConnected = VF_AreConnected(
        *World.Generator,
        *World.StrateManager,
        0,
        WallA,
        WallB,
        Settings,
        bWallCoarseLied);
    ++NumRoutesChecked;
    if (bWallCoarseLied) ++NumRoutesRefuted;
    if (bWallConnected)
    {
        AddError(TEXT("HARD FAILURE: VF_AreConnected reported true across a full-resolution solid 3-voxel wall."));
        bAllChecksPassed = false;
    }
    if (!bWallCoarseLied)
    {
        AddError(TEXT("HARD FAILURE: the controlled coarse-connectivity lie did not set bOutCoarseLied."));
        bAllChecksPassed = false;
    }
    World.DiffLayer->Clear();

    TArray<FPassageReport> PassageReports;
    const TArray<FVoxelPassage>& Passages = World.StrateManager->GetPassages();
    PassageReports.Reserve(Passages.Num());
    for (int32 PassageIndex = 0; PassageIndex < Passages.Num(); ++PassageIndex)
    {
        const FVoxelPassage& Passage = Passages[PassageIndex];
        FPassageReport& Report = PassageReports.AddDefaulted_GetRef();
        Report.Index = PassageIndex;
        Report.Name = FString::Printf(TEXT("%03d"), PassageIndex);

        auto CheckEndpoint = [&](int32 EndpointStrateIndex, const FVector& Endpoint) -> FString
        {
            if (!Layout.IsValidIndex(EndpointStrateIndex)) return TEXT("invalid-strate");
            const FStrateSlot& Slot = Layout[EndpointStrateIndex];
            if (!IsStrictInteriorPoint(Slot, Endpoint)) return TEXT("out-of-band");

            const FVector Anchor = LargestComponentAnchor(Slot, Settings.CenterXY);
            bool bCoarseLied = false;
            const bool bConnected = VF_AreConnected(
                *World.Generator,
                *World.StrateManager,
                EndpointStrateIndex,
                Endpoint,
                Anchor,
                Settings,
                bCoarseLied);
            ++NumRoutesChecked;
            if (bCoarseLied) ++NumRoutesRefuted;
            return bConnected ? TEXT("connected") : TEXT("not-connected");
        };

        Report.UpperResult = CheckEndpoint(Passage.UpperStrateIndex, Passage.UpperPoint);
        Report.LowerResult = CheckEndpoint(Passage.LowerStrateIndex, Passage.LowerPoint);
    }

    PassageReports.Sort([](const FPassageReport& A, const FPassageReport& B)
    {
        return A.Index < B.Index;
    });
    Summary += FString::Printf(
        TEXT("Connectivity guard: %d routes checked, %d coarse routes refuted at full resolution.\n"),
        NumRoutesChecked,
        NumRoutesRefuted);
    Summary += TEXT("Passages (endpoint -> largest-component spine anchor, sorted):\n");
    for (const FPassageReport& Report : PassageReports)
    {
        Summary += FString::Printf(
            TEXT("  passage %s: upper=%s, lower=%s\n"),
            *Report.Name,
            *Report.UpperResult,
            *Report.LowerResult);
    }
    AddInfo(Summary);

    return bAllChecksPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
