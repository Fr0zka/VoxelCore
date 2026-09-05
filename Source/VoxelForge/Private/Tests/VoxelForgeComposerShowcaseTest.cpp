// Morning review: one hard-gated, walkable candidate per archetype plus an offline page.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"
#include "VoxelSettings.h"
#include "VoxelStrateComposer.h"
#include "VoxelStrateMeasure.h"
#include "VoxelStratePreview.h"

namespace
{
    // DA_Settings currently leaves UVoxelSettings::Seed at its declared default (0). The
    // composer seed is independent, but keeping the fixture's live generator seed equal to the
    // project seed makes the standalone stack oracle reproduce the PIE hand-off exactly.
    constexpr int32 BaseComposerSeed = 0;
    constexpr int32 DefaultTargetStrateIndex = VoxelForgeTest::FTestWorld::SlotFlatPlain;
    constexpr int32 CandidateIndicesPerSeed = 64;
    constexpr float LargestComponentSurvivalThreshold = 0.50f;
    constexpr float ClearanceNormalizationVoxels = 64.0f;
    constexpr int32 FinePreviewSampleStep = 1;
    constexpr int32 FinePreviewRadiusVoxels = 64;
    constexpr int32 FinePreviewMaxCells = 2000000;

    const ECaveGeneratorType GShowcaseArchetypes[] = {
        ECaveGeneratorType::CrystalChamber,
        ECaveGeneratorType::FlatPlain,
        ECaveGeneratorType::FloatingIslands,
        ECaveGeneratorType::Maze,
        ECaveGeneratorType::SurfaceWorld,
        ECaveGeneratorType::TunnelNetwork,
        ECaveGeneratorType::Underwater,
        ECaveGeneratorType::VerticalShafts,
    };

    int32 VF_ShowcaseArchetypeIndex(ECaveGeneratorType Archetype)
    {
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(GShowcaseArchetypes); ++Index)
        {
            if (GShowcaseArchetypes[Index] == Archetype)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    const TCHAR* VF_ShowcaseConnectivityName(EVoxelConnectivityResult Result)
    {
        switch (Result)
        {
        case EVoxelConnectivityResult::Connected:                   return TEXT("Connected");
        case EVoxelConnectivityResult::NotConnectedAtThisResolution: return TEXT("NotConnectedAtThisResolution");
        case EVoxelConnectivityResult::StartCellSolid:              return TEXT("StartCellSolid");
        case EVoxelConnectivityResult::GoalCellSolid:               return TEXT("GoalCellSolid");
        case EVoxelConnectivityResult::OutOfWindow:                 return TEXT("OutOfWindow");
        case EVoxelConnectivityResult::CoarseLiedBudgetExhausted:   return TEXT("CoarseLiedBudgetExhausted");
        }
        return TEXT("Unknown");
    }

    float VF_ShowcaseBoundarySeal(const FVoxelStrateArchetypeParams& Params,
                                  ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return Params.TunnelNetworkParams.BoundarySealThickness;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return Params.SlabParams.BoundarySealThickness;
        case ECaveGeneratorType::Maze:
            return Params.MazeParams.BoundarySealThickness;
        case ECaveGeneratorType::SurfaceWorld:
            return Params.SurfaceParams.BoundarySealThickness;
        case ECaveGeneratorType::VerticalShafts:
            return Params.VerticalShaftParams.BoundarySealThickness;
        case ECaveGeneratorType::FloatingIslands:
            return Params.FloatingIslandParams.BoundarySealThickness;
        default:
            return 0.0f;
        }
    }

    class FShowcaseStackSampler final : public IVoxelStrateDensitySampler
    {
    public:
        explicit FShowcaseStackSampler(const FVoxelOpStack& InStack) : Stack(InStack) {}

        float SampleDensity(float WorldX, float WorldY, float WorldZ) const override
        {
            return Stack.EvalMC(WorldX, WorldY, WorldZ);
        }

    private:
        const FVoxelOpStack& Stack;
    };

    struct FShowcaseSample
    {
        FVoxelStrateComposerCandidate Candidate;
        FVoxelStrateMetrics Metrics;
        double SelectionScore = -1.0;
        FString LawText;
        int32 TargetStrateIndex = INDEX_NONE;
        int32 TopVoxelZ = 0;
        int32 BottomVoxelZ = 0;
    };

    struct FShowcaseStats
    {
        int32 EvaluatedCandidates = 0;
        TArray<FShowcaseSample> Survivors;
        TOptional<FShowcaseSample> Best;
        FString LastFailure;
    };

    void VF_AddMetricSample(FShowcaseStats& Stats, const FShowcaseSample& Sample)
    {
        Stats.Survivors.Add(Sample);
        if (!Stats.Best.IsSet()
            || Sample.SelectionScore > Stats.Best->SelectionScore
            || (Sample.SelectionScore == Stats.Best->SelectionScore
                && (Sample.Candidate.Seed < Stats.Best->Candidate.Seed
                    || (Sample.Candidate.Seed == Stats.Best->Candidate.Seed
                        && Sample.Candidate.Index < Stats.Best->Candidate.Index))))
        {
            Stats.Best = Sample;
        }
    }

    bool VF_BuildShowcaseStack(
        const VoxelForgeTest::FTestWorld& World,
        const FVoxelStrateComposerCandidate& Candidate,
        int32 TopVoxelZ,
        int32 BottomVoxelZ,
        FVoxelOpStack& OutStack,
        FVoxelOpContext& OutContext,
        FString& OutError)
    {
        if (!Candidate.bValid || !Candidate.Regions.IsSingleRegion())
        {
            OutError = TEXT("showcase candidates must be valid single-region candidates");
            return false;
        }

        FVoxelStrateArchetypeParams Params = Candidate.ArchetypeParams;
        VF_SetStrateArchetypeRuntimeBounds(
            Params, static_cast<float>(TopVoxelZ) + 1.0f, static_cast<float>(BottomVoxelZ));
        const bool bBuilt = VF_BuildNativeStrateStackForCandidate(
            Candidate.Archetype, Params, World.Generator->Seed,
            World.Generator->OriginSpineRadius, World.Generator->WorldRadiusVoxels,
            VF_ShowcaseBoundarySeal(Candidate.ArchetypeParams, Candidate.Archetype),
            World.StrateManager.Get(), OutStack, OutContext);
        if (!bBuilt)
        {
            OutError = TEXT("native candidate stack materialisation failed");
            return false;
        }
        if (OutContext.WorldRadiusVoxels != 0.0f)
        {
            OutError = TEXT("native showcase stack changed WorldRadiusVoxels from zero");
            return false;
        }
        return true;
    }

    double VF_ShowcaseSelectionScore(const FVoxelStrateMetrics& Metrics)
    {
        const double Area = FMath::Max(0.0, static_cast<double>(Metrics.WalkableFloorAreaFraction));
        const double Clearance = FMath::Clamp(
            static_cast<double>(Metrics.MedianVerticalClearance)
                / static_cast<double>(ClearanceNormalizationVoxels),
            0.0, 1.0);
        const double Surface = FMath::Max(
            0.0, static_cast<double>(Metrics.LargestWalkableSurfaceShare));
        // Floor area is the primary signal; clearance and projected continuity break ties in a
        // deterministic way without making a tall but tiny ledge beat a broad floor.
        return Area + 0.25 * Clearance + 0.10 * Surface;
    }

    bool VF_GetShowcaseMouthPair(
        const VoxelForgeTest::FTestWorld& World,
        int32 InTargetStrateIndex,
        FVector& OutArrivalPoint,
        FVector& OutDeparturePoint)
    {
        int32 ArrivalCount = 0;
        int32 DepartureCount = 0;
        for (const FVoxelPassage& Passage : World.StrateManager->GetPassages())
        {
            if (Passage.LowerStrateIndex == InTargetStrateIndex
                && Passage.UpperStrateIndex + 1 == InTargetStrateIndex)
            {
                OutArrivalPoint = Passage.LowerPoint;
                ++ArrivalCount;
            }
            if (Passage.UpperStrateIndex == InTargetStrateIndex
                && Passage.LowerStrateIndex == InTargetStrateIndex + 1)
            {
                OutDeparturePoint = Passage.UpperPoint;
                ++DepartureCount;
            }
        }
        return ArrivalCount == 1 && DepartureCount == 1;
    }

    bool VF_EvaluateShowcaseCandidate(
        const VoxelForgeTest::FTestWorld& World,
        const FVoxelStrateComposerCandidate& Candidate,
        int32 InTargetStrateIndex,
        const FVoxelStrateMeasureSettings& MeasureSettings,
        FShowcaseSample& OutSample,
        FString* OutFailureReason = nullptr)
    {
        auto Fail = [OutFailureReason](const FString& Reason) -> bool
        {
            if (OutFailureReason != nullptr)
            {
                *OutFailureReason = Reason;
            }
            return false;
        };
        if (!Candidate.bValid || !Candidate.Regions.IsSingleRegion())
        {
            return Fail(TEXT("invalid or multi-region candidate"));
        }

        int32 TopVoxelZ = 0;
        int32 BottomVoxelZ = 0;
        if (!World.GetSlotVoxelZRange(InTargetStrateIndex, TopVoxelZ, BottomVoxelZ))
        {
            return Fail(TEXT("target slot has no Z range"));
        }
        FVector ArrivalPoint = FVector::ZeroVector;
        FVector DeparturePoint = FVector::ZeroVector;
        if (!VF_GetShowcaseMouthPair(World, InTargetStrateIndex,
                                     ArrivalPoint, DeparturePoint))
        {
            return Fail(TEXT("target slot does not have exactly one arrival/departure mouth"));
        }

        FVoxelOpStack Stack;
        FVoxelOpContext Context;
        FString BuildError;
        if (!VF_BuildShowcaseStack(World, Candidate, TopVoxelZ, BottomVoxelZ,
                                   Stack, Context, BuildError))
        {
            return Fail(BuildError);
        }
        Stack.PrepareChunk(Context);
        FShowcaseStackSampler Sampler(Stack);
        const FVoxelStrateMetrics Metrics = VF_MeasureStrateWithSampler(
            Sampler, BottomVoxelZ, TopVoxelZ + 1, Context.EdgeSealThickness,
            MeasureSettings, nullptr);
        if (!Metrics.bValid)
        {
            return Fail(FString::Printf(TEXT("measurement refused: %s"), *Metrics.RefusalReason));
        }
        const FVoxelConnectivityDiagnostics Law = VF_DiagnoseConnectivityWithSampler(
            Sampler, BottomVoxelZ, TopVoxelZ + 1, Context.EdgeSealThickness,
            ArrivalPoint, DeparturePoint, MeasureSettings);
        const bool bNonVacuous = Metrics.NumSampled > 0
            && Metrics.NumAir > 0 && Metrics.NumSolid > 0;
        const bool bLargestEnough = bNonVacuous
            && Metrics.LargestComponentShare >= LargestComponentSurvivalThreshold;
        const bool bLawPass = Law.bValid
            && Law.Result == EVoxelConnectivityResult::Connected
            && !Law.bStartSnapped && !Law.bGoalSnapped;
        if (!(bNonVacuous && bLargestEnough && bLawPass))
        {
            return Fail(FString::Printf(
                TEXT("nonv=%d largest=%d law=%s snapped=%d/%d air=%.6f largest_share=%.6f "
                     "walk_area=%.6f clearance=%d"),
                bNonVacuous ? 1 : 0,
                bLargestEnough ? 1 : 0,
                VF_ShowcaseConnectivityName(Law.Result),
                Law.bStartSnapped ? 1 : 0,
                Law.bGoalSnapped ? 1 : 0,
                Metrics.AirFraction,
                Metrics.LargestComponentShare,
                Metrics.WalkableFloorAreaFraction,
                Metrics.MedianVerticalClearance));
        }

        OutSample = FShowcaseSample();
        OutSample.Candidate = Candidate;
        OutSample.Metrics = Metrics;
        OutSample.SelectionScore = VF_ShowcaseSelectionScore(Metrics);
        OutSample.LawText = FString::Printf(
            TEXT("%s%s"), VF_ShowcaseConnectivityName(Law.Result),
            (Law.bStartSnapped || Law.bGoalSnapped) ? TEXT(" (snapped)") : TEXT(""));
        OutSample.TargetStrateIndex = InTargetStrateIndex;
        OutSample.TopVoxelZ = TopVoxelZ;
        OutSample.BottomVoxelZ = BottomVoxelZ;
        return true;
    }

    FString VF_FormatNativeParameterRecipe(const FVoxelStrateComposerCandidate& Candidate)
    {
        FString Parents;
        for (int32 ParentIndex = 0;
             ParentIndex < Candidate.ParameterRoll.ParentEntryIndices.Num(); ++ParentIndex)
        {
            if (ParentIndex > 0)
            {
                Parents += TEXT(",");
            }
            const float Weight = Candidate.ParameterRoll.ParentWeights.IsValidIndex(ParentIndex)
                ? Candidate.ParameterRoll.ParentWeights[ParentIndex] : 0.0f;
            Parents += FString::Printf(
                TEXT("#%d@%.6f"),
                Candidate.ParameterRoll.ParentEntryIndices[ParentIndex], Weight);
        }
        return FString::Printf(
            TEXT("parameter-roll|native=%s|parents=[%s]|dominant_parent=%d|single_region=true"),
            VF_GetStrateArchetypeName(Candidate.Archetype), *Parents,
            Candidate.ParameterRoll.DominantParentPosition);
    }

    float VF_MedianFloat(TArray<float> Values)
    {
        if (Values.IsEmpty())
        {
            return 0.0f;
        }
        Values.Sort();
        const int32 Middle = Values.Num() / 2;
        return (Values.Num() & 1) != 0
            ? Values[Middle]
            : 0.5f * (Values[Middle - 1] + Values[Middle]);
    }

    int32 VF_LowerMedianInt(TArray<int32> Values)
    {
        if (Values.IsEmpty())
        {
            return 0;
        }
        Values.Sort();
        return Values[(Values.Num() - 1) / 2];
    }

    FString VF_FormatFloatDistribution(const TArray<FShowcaseSample>& Samples,
                                       TFunctionRef<float(const FVoxelStrateMetrics&)> Getter)
    {
        if (Samples.IsEmpty())
        {
            return TEXT("none (0 hard-gate survivors)");
        }
        TArray<float> Values;
        Values.Reserve(Samples.Num());
        for (const FShowcaseSample& Sample : Samples)
        {
            Values.Add(Getter(Sample.Metrics));
        }
        TArray<float> Sorted = Values;
        Sorted.Sort();
        return FString::Printf(
            TEXT("%.6f; %.6f–%.6f"), VF_MedianFloat(MoveTemp(Values)),
            Sorted[0], Sorted.Last());
    }

    FString VF_FormatIntDistribution(const TArray<FShowcaseSample>& Samples,
                                     TFunctionRef<int32(const FVoxelStrateMetrics&)> Getter)
    {
        if (Samples.IsEmpty())
        {
            return TEXT("none (0 hard-gate survivors)");
        }
        TArray<int32> Values;
        Values.Reserve(Samples.Num());
        for (const FShowcaseSample& Sample : Samples)
        {
            Values.Add(Getter(Sample.Metrics));
        }
        TArray<int32> Sorted = Values;
        Sorted.Sort();
        return FString::Printf(
            TEXT("%d; %d–%d voxels"), VF_LowerMedianInt(MoveTemp(Values)),
            Sorted[0], Sorted.Last());
    }

    FVoxelStratePreviewArchetypeSummary VF_MakeSummary(
        const FShowcaseStats& Stats,
        const FVoxelStratePreviewWindow& SelectedWindow,
        const FString& WindowSummary,
        ECaveGeneratorType Archetype)
    {
        FVoxelStratePreviewArchetypeSummary Summary;
        Summary.ArchetypeName = VF_GetStrateArchetypeName(Archetype);
        Summary.EvaluatedCandidates = Stats.EvaluatedCandidates;
        Summary.HardGateSurvivors = Stats.Survivors.Num();
        Summary.Window = SelectedWindow;
        Summary.WindowSummary = WindowSummary;
        Summary.WalkableFractionSummary = VF_FormatFloatDistribution(
            Stats.Survivors,
            [](const FVoxelStrateMetrics& Metrics) { return Metrics.WalkableFraction; });
        Summary.WalkableFloorAreaSummary = VF_FormatFloatDistribution(
            Stats.Survivors,
            [](const FVoxelStrateMetrics& Metrics) { return Metrics.WalkableFloorAreaFraction; });
        Summary.MedianVerticalClearanceSummary = VF_FormatIntDistribution(
            Stats.Survivors,
            [](const FVoxelStrateMetrics& Metrics) { return Metrics.MedianVerticalClearance; });
        Summary.LargestWalkableSurfaceSummary = VF_FormatFloatDistribution(
            Stats.Survivors,
            [](const FVoxelStrateMetrics& Metrics) { return Metrics.LargestWalkableSurfaceShare; });
        return Summary;
    }

    FString VF_FormatShowcaseSummaryWindow(const FShowcaseStats& Stats)
    {
        TArray<FString> SurvivorWindows;
        for (const FShowcaseSample& Sample : Stats.Survivors)
        {
            const FVoxelStrateMetrics& Metrics = Sample.Metrics;
            SurvivorWindows.AddUnique(FString::Printf(
                TEXT("target=%d; margin=%d; Z=[%d,%d); XY=[%.0f,%.0f) x [%.0f,%.0f); grid=%dx%dx%d"),
                Sample.TargetStrateIndex,
                Metrics.ResolvedMarginVoxels,
                Metrics.SampledMinZ,
                Metrics.SampledMaxZ,
                Metrics.SampledMinX,
                Metrics.SampledMaxX,
                Metrics.SampledMinY,
                Metrics.SampledMaxY,
                Metrics.SampledNumX,
                Metrics.SampledNumY,
                Metrics.SampledNumZ));
        }
        SurvivorWindows.Sort();
        return FString::Printf(
            TEXT("settings: step=4; HeadroomCells=2; MaxCells=8000000; exact survivor windows: %s"),
            SurvivorWindows.IsEmpty() ? TEXT("none") : *FString::Join(SurvivorWindows, TEXT(" | ")));
    }

    bool VF_ShowcaseMetricsEqual(const FVoxelStrateMetrics& A,
                                 const FVoxelStrateMetrics& B)
    {
        return A.bValid == B.bValid
            && A.RefusalReason == B.RefusalReason
            && A.NumSampled == B.NumSampled
            && A.NumAir == B.NumAir
            && A.NumSolid == B.NumSolid
            && A.AirFraction == B.AirFraction
            && A.NumAirComponents == B.NumAirComponents
            && A.LargestComponentShare == B.LargestComponentShare
            && A.LargestComponentPoint == B.LargestComponentPoint
            && A.LargestComponentCells == B.LargestComponentCells
            && A.NumComponentsAtLeast1Pct == B.NumComponentsAtLeast1Pct
            && A.AirComponentCells == B.AirComponentCells
            && A.WalkableFraction == B.WalkableFraction
            && A.WalkableFloorColumns == B.WalkableFloorColumns
            && A.WalkableFloorAreaFraction == B.WalkableFloorAreaFraction
            && A.NumWalkableSurfaceComponents == B.NumWalkableSurfaceComponents
            && A.LargestWalkableSurfaceColumns == B.LargestWalkableSurfaceColumns
            && A.LargestWalkableSurfaceShare == B.LargestWalkableSurfaceShare
            && A.MedianFeatureScale == B.MedianFeatureScale
            && A.MedianVerticalClearance == B.MedianVerticalClearance
            && A.ResolvedMarginVoxels == B.ResolvedMarginVoxels
            && A.SampledMinZ == B.SampledMinZ
            && A.SampledMaxZ == B.SampledMaxZ
            && A.SampledNumX == B.SampledNumX
            && A.SampledNumY == B.SampledNumY
            && A.SampledNumZ == B.SampledNumZ
            && A.SampledMinX == B.SampledMinX
            && A.SampledMaxX == B.SampledMaxX
            && A.SampledMinY == B.SampledMinY
            && A.SampledMaxY == B.SampledMaxY;
    }

    bool VF_ShowcaseCandidatesEqual(
        const FVoxelStrateComposerCandidate& A,
        const FVoxelStrateComposerCandidate& B)
    {
        if (A.bValid != B.bValid
            || A.FailureReason != B.FailureReason
            || A.Seed != B.Seed
            || A.Index != B.Index
            || A.bStructureRoll != B.bStructureRoll
            || A.Archetype != B.Archetype
            || !VF_AreStrateParamsBitIdentical(A.ParameterRoll.Params, B.ParameterRoll.Params)
            || A.ParameterRoll.ParentEntryIndices != B.ParameterRoll.ParentEntryIndices
            || A.ParameterRoll.ParentWeights != B.ParameterRoll.ParentWeights
            || A.ParameterRoll.DominantParentPosition != B.ParameterRoll.DominantParentPosition
            || A.ParameterRoll.BootstrapJitterFieldCount != B.ParameterRoll.BootstrapJitterFieldCount
            || A.ParameterRoll.BootstrapJitterFieldNames
                != B.ParameterRoll.BootstrapJitterFieldNames)
        {
            return false;
        }
        if (A.bValid
            && !VF_AreStrateArchetypeParamsBitIdentical(
                A.ArchetypeParams, B.ArchetypeParams, A.Archetype))
        {
            return false;
        }
        if (A.ParameterRoll.bValid != B.ParameterRoll.bValid
            || A.ParameterRoll.FailureReason != B.ParameterRoll.FailureReason
            || A.ParameterRoll.Archetype != B.ParameterRoll.Archetype
            || (A.ParameterRoll.bValid
                && !VF_AreStrateArchetypeParamsBitIdentical(
                    A.ParameterRoll.ArchetypeParams,
                    B.ParameterRoll.ArchetypeParams,
                    A.ParameterRoll.Archetype)))
        {
            return false;
        }

        const FVoxelStrateRegionManifest& AR = A.Regions;
        const FVoxelStrateRegionManifest& BR = B.Regions;
        if (AR.bValid != BR.bValid
            || AR.FailureReason != BR.FailureReason
            || AR.Seed != BR.Seed
            || AR.StrateIndex != BR.StrateIndex
            || AR.RegionCount != BR.RegionCount
            || AR.PartitionSeed != BR.PartitionSeed
            || AR.LatticeCellSize != BR.LatticeCellSize
            || AR.BlendWidth != BR.BlendWidth
            || AR.bHasGlobalStructuralParams != BR.bHasGlobalStructuralParams
            || AR.StructuralParamBlock != BR.StructuralParamBlock
            || AR.StrateTopWorldZ != BR.StrateTopWorldZ
            || AR.StrateBottomWorldZ != BR.StrateBottomWorldZ
            || AR.BoundarySealThickness != BR.BoundarySealThickness
            || AR.BaseDensity != BR.BaseDensity
            || AR.Regions.Num() != BR.Regions.Num())
        {
            return false;
        }
        for (int32 RegionIndex = 0; RegionIndex < AR.Regions.Num(); ++RegionIndex)
        {
            const FVoxelStrateRegion& RegionA = AR.Regions[RegionIndex];
            const FVoxelStrateRegion& RegionB = BR.Regions[RegionIndex];
            if (RegionA.RegionIndex != RegionB.RegionIndex
                || RegionA.Seed != RegionB.Seed
                || RegionA.Archetype != RegionB.Archetype
                || RegionA.bUsesRecipe != RegionB.bUsesRecipe
                || !VF_AreStrateArchetypeParamsBitIdentical(
                    RegionA.ArchetypeParams, RegionB.ArchetypeParams, RegionA.Archetype)
                || !VF_AreStrateStructureRecipesIdentical(RegionA.Recipe, RegionB.Recipe))
            {
                return false;
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeComposerShowcaseTest,
    "VoxelForge.Composer.Showcase",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeComposerShowcaseTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;
    (void)Parameters;
    const double TestStartSeconds = FPlatformTime::Seconds();

    TestTrue(TEXT("lateral regions remain gated off for the showcase"),
             !VF_LateralRegionsAreShippable());

    UVoxelSettings* AuthoredSettings = LoadObject<UVoxelSettings>(
        nullptr, TEXT("/Game/VoxelForge/DA_Settings.DA_Settings"));
    if (AuthoredSettings == nullptr)
    {
        AddError(TEXT("Could not load /Game/VoxelForge/DA_Settings.DA_Settings."));
        return false;
    }

    FVoxelStrateCorpus Corpus;
    FString CorpusReport;
    const bool bCorpusLoaded = Corpus.LoadFromAssetRegistry(CorpusReport);
    AddInfo(CorpusReport);
    TestTrue(TEXT("showcase corpus is usable"), bCorpusLoaded && Corpus.IsValid());
    if (!bCorpusLoaded || !Corpus.IsValid())
    {
        return false;
    }

    FTestWorld World;
    World.Build(BaseComposerSeed, 2, true);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }
    TestTrue(TEXT("showcase fixture keeps Settings WorldRadiusVoxels at zero"),
             World.Settings->WorldRadiusVoxels == 0.0f);
    TestTrue(TEXT("showcase fixture keeps Generator WorldRadiusVoxels at zero"),
             World.Generator->WorldRadiusVoxels == 0.0f);

    int32 TopVoxelZ = 0;
    int32 BottomVoxelZ = 0;
    if (!World.GetSlotVoxelZRange(DefaultTargetStrateIndex, TopVoxelZ, BottomVoxelZ))
    {
        AddError(TEXT("showcase target slot has no Z range"));
        return false;
    }

    FVector ArrivalPoint = FVector::ZeroVector;
    FVector DeparturePoint = FVector::ZeroVector;
    int32 ArrivalCount = 0;
    int32 DepartureCount = 0;
    for (const FVoxelPassage& Passage : World.StrateManager->GetPassages())
    {
        if (Passage.LowerStrateIndex == DefaultTargetStrateIndex
            && Passage.UpperStrateIndex + 1 == DefaultTargetStrateIndex)
        {
            ArrivalPoint = Passage.LowerPoint;
            ++ArrivalCount;
        }
        if (Passage.UpperStrateIndex == DefaultTargetStrateIndex
            && Passage.LowerStrateIndex == DefaultTargetStrateIndex + 1)
        {
            DeparturePoint = Passage.UpperPoint;
            ++DepartureCount;
        }
    }
    TestEqual(TEXT("showcase target has one arrival mouth"), ArrivalCount, 1);
    TestEqual(TEXT("showcase target has one departure mouth"), DepartureCount, 1);

    FVoxelStrateMeasureSettings MeasureSettings;
    MeasureSettings.SampleStep = 4;
    MeasureSettings.RadiusInVoxels = 256;
    MeasureSettings.CenterXY = FVector2D::ZeroVector;
    MeasureSettings.MaxCells = 8000000;
    MeasureSettings.MaxRouteRetries = 16;
    MeasureSettings.HeadroomCells = 2;
    MeasureSettings.InteriorMarginVoxels = -1;

    FVoxelStrateFinePreviewSettings FinePreviewSettings;
    FinePreviewSettings.SampleStep = FinePreviewSampleStep;
    FinePreviewSettings.RadiusInVoxels = FinePreviewRadiusVoxels;
    FinePreviewSettings.MaxCells = FinePreviewMaxCells;
    TestTrue(TEXT("showcase fine ROI settings are valid and bounded"),
             FinePreviewSettings.IsValid());

    FShowcaseStats Stats[UE_ARRAY_COUNT(GShowcaseArchetypes)];
    bool bHaveAllArchetypes = false;
    int32 NumRollAttempts = 0;
    int32 NumSingleRegionRolls = 0;
    int32 NumLateralRollsSkipped = 0;
    const int32 SearchSeeds[] = { BaseComposerSeed, 7331 };

    // Exhaust the bounded roll set before selecting. This makes "best" mean best among the
    // stated seed/index search, rather than the first candidate that happens to fill a family.
    for (const int32 SearchSeed : SearchSeeds)
    {
        for (int32 CandidateIndex = 0;
             CandidateIndex < CandidateIndicesPerSeed;
             ++CandidateIndex)
        {
            ++NumRollAttempts;
            const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
                Corpus, SearchSeed, CandidateIndex, false);
            const FVoxelStrateComposerCandidate RepeatCandidate = VF_RollStrateCandidate(
                Corpus, SearchSeed, CandidateIndex, false);
            TestTrue(FString::Printf(TEXT("seed %d candidate %d rerolls bit-identically"),
                                     SearchSeed, CandidateIndex),
                     VF_ShowcaseCandidatesEqual(Candidate, RepeatCandidate));
            if (!Candidate.bValid)
            {
                continue;
            }
            TestTrue(FString::Printf(TEXT("seed %d candidate %d has a valid native roll"),
                                     SearchSeed, CandidateIndex),
                     Candidate.ParameterRoll.bValid
                         && Candidate.Archetype == Candidate.ParameterRoll.Archetype);

            // A multi-region manifest is intentionally not considered for the morning page. It
            // would be a use of the currently gated seam feature, even though it can be measured.
            if (!Candidate.Regions.IsSingleRegion())
            {
                ++NumLateralRollsSkipped;
                continue;
            }
            ++NumSingleRegionRolls;
            const int32 ArchetypeIndex = VF_ShowcaseArchetypeIndex(Candidate.Archetype);
            if (ArchetypeIndex == INDEX_NONE)
            {
                continue;
            }
            ++Stats[ArchetypeIndex].EvaluatedCandidates;
            FShowcaseSample Sample;
            FString FailureReason;
            if (VF_EvaluateShowcaseCandidate(
                    World, Candidate, DefaultTargetStrateIndex, MeasureSettings, Sample,
                    &FailureReason))
            {
                VF_AddMetricSample(Stats[ArchetypeIndex], Sample);
            }
            else
            {
                Stats[ArchetypeIndex].LastFailure = MoveTemp(FailureReason);
            }

        }
    }

    bHaveAllArchetypes = true;
    for (const FShowcaseStats& ArchetypeStats : Stats)
    {
        bHaveAllArchetypes = bHaveAllArchetypes && ArchetypeStats.Best.IsSet();
    }

    // Some native families only fit the generated landing mouths when installed into a different
    // existing slot. That is still a density-only PIE override: layout, placement, and passages
    // are unchanged. Search the six interior slots as a deterministic fallback for any archetype
    // absent from the first target-slot screen. Topmost/bottommost slots are excluded because one
    // of the two primordial mouths does not exist there.
    if (!bHaveAllArchetypes)
    {
        // Every missing family is measured at the exact reported step-4 settings for every
        // interior target slot. There is no coarse pre-gate here: a coarse route can be a false
        // negative, so it must not silently remove a candidate from the "best" set.
        for (const int32 SearchSeed : SearchSeeds)
        {
            for (int32 CandidateIndex = 0;
                 CandidateIndex < CandidateIndicesPerSeed;
                 ++CandidateIndex)
            {
                ++NumRollAttempts;
                const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
                    Corpus, SearchSeed, CandidateIndex, false);
                if (!Candidate.bValid)
                {
                    continue;
                }
                if (!Candidate.Regions.IsSingleRegion())
                {
                    ++NumLateralRollsSkipped;
                    continue;
                }
                ++NumSingleRegionRolls;
                const int32 ArchetypeIndex = VF_ShowcaseArchetypeIndex(Candidate.Archetype);
                if (ArchetypeIndex == INDEX_NONE || Stats[ArchetypeIndex].Best.IsSet())
                {
                    continue;
                }
                for (int32 FallbackTarget = FTestWorld::SlotFlatPlain;
                     FallbackTarget <= FTestWorld::SlotFloatingIsland;
                     ++FallbackTarget)
                {
                    FString FailureReason;
                    FShowcaseSample Sample;
                    if (VF_EvaluateShowcaseCandidate(
                            World, Candidate, FallbackTarget, MeasureSettings, Sample,
                            &FailureReason))
                    {
                        VF_AddMetricSample(Stats[ArchetypeIndex], Sample);
                    }
                    else
                    {
                        Stats[ArchetypeIndex].LastFailure = MoveTemp(FailureReason);
                    }
                }

            }
        }
    }

    bHaveAllArchetypes = true;
    for (const FShowcaseStats& ArchetypeStats : Stats)
    {
        bHaveAllArchetypes = bHaveAllArchetypes && ArchetypeStats.Best.IsSet();
    }

    AddInfo(FString::Printf(
        TEXT("Showcase search: %d roll evaluations (fallback replays included), "
             "%d eligible single-region evaluations (fallback replays included), "
             "%d multi-region evaluations skipped while the lateral gate is OFF."),
        NumRollAttempts, NumSingleRegionRolls, NumLateralRollsSkipped));
    for (int32 ArchetypeIndex = 0;
         ArchetypeIndex < UE_ARRAY_COUNT(GShowcaseArchetypes);
         ++ArchetypeIndex)
    {
        AddInfo(FString::Printf(
            TEXT("Showcase search detail %s: eligible=%d hard_gate_survivors=%d last_failure=%s"),
            VF_GetStrateArchetypeName(GShowcaseArchetypes[ArchetypeIndex]),
            Stats[ArchetypeIndex].EvaluatedCandidates,
            Stats[ArchetypeIndex].Survivors.Num(),
            Stats[ArchetypeIndex].LastFailure.IsEmpty()
                ? TEXT("none") : *Stats[ArchetypeIndex].LastFailure));
    }
    for (int32 ArchetypeIndex = 0;
         ArchetypeIndex < UE_ARRAY_COUNT(GShowcaseArchetypes);
         ++ArchetypeIndex)
    {
        const FShowcaseStats& ArchetypeStats = Stats[ArchetypeIndex];
        TestTrue(FString::Printf(TEXT("%s has at least one hard-gate survivor"),
                                 VF_GetStrateArchetypeName(GShowcaseArchetypes[ArchetypeIndex])),
                 ArchetypeStats.Best.IsSet());
    }
    if (!bHaveAllArchetypes)
    {
        AddError(TEXT("The bounded deterministic search did not find one hard-gate survivor for every archetype."));
        return false;
    }

    const FString CoarseWindowSummary = FString::Printf(
        TEXT("coarse settings: step=%d; XY=[-256,256) x [-256,256) voxels; HeadroomCells=%d; "
             "MaxCells=%d; interior Z margin derived per candidate; exact survivor windows "
             "are listed in this row and selected windows are repeated on cards"),
        MeasureSettings.SampleStep, MeasureSettings.HeadroomCells, MeasureSettings.MaxCells);
    TArray<FVoxelStratePreviewArchetypeSummary> Summaries;
    Summaries.Reserve(UE_ARRAY_COUNT(GShowcaseArchetypes));
    for (int32 ArchetypeIndex = 0;
         ArchetypeIndex < UE_ARRAY_COUNT(GShowcaseArchetypes);
         ++ArchetypeIndex)
    {
        Summaries.Add(VF_MakeSummary(
            Stats[ArchetypeIndex],
            FVoxelStratePreviewWindow(),
            CoarseWindowSummary,
            GShowcaseArchetypes[ArchetypeIndex]));
    }

    const FString ShowcaseDirectory = FPaths::ProjectSavedDir()
        / TEXT("VoxelForge/Showcase");
    TArray<FVoxelStratePreviewCandidate> PreviewCandidates;
    PreviewCandidates.Reserve(UE_ARRAY_COUNT(GShowcaseArchetypes));
    FVoxelStratePreviewWindow CommonWindow;
    bool bCommonWindowSet = false;
    int32 FineRendered = 0;
    int32 FineRefused = 0;
    int32 FineBlank = 0;

    for (int32 ArchetypeIndex = 0;
         ArchetypeIndex < UE_ARRAY_COUNT(GShowcaseArchetypes);
         ++ArchetypeIndex)
    {
        const FShowcaseSample& Selected = Stats[ArchetypeIndex].Best.GetValue();
        const FVoxelStrateComposerCandidate& Candidate = Selected.Candidate;

        // The output filename namespace is local to this page. Keep it distinct even if a future
        // search selects equal candidate indices from two different composer seeds; the PIE fields
        // below still retain the exact real candidate index.
        const int32 RenderIndex = 1000 + ArchetypeIndex;
        FVoxelOpStack Stack;
        FVoxelOpContext Context;
        FString BuildError;
        TestTrue(FString::Printf(TEXT("selected %s rebuilds for preview"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 VF_BuildShowcaseStack(
                     World, Candidate, Selected.TopVoxelZ, Selected.BottomVoxelZ,
                     Stack, Context, BuildError));
        if (!BuildError.IsEmpty())
        {
            AddError(FString::Printf(TEXT("selected %s rebuild error: %s"),
                                     VF_GetStrateArchetypeName(Candidate.Archetype), *BuildError));
            return false;
        }
        Stack.PrepareChunk(Context);
        FShowcaseStackSampler Sampler(Stack);

        const FVoxelStrateMetrics RepeatMetrics = VF_MeasureStrateWithSampler(
            Sampler, Selected.BottomVoxelZ, Selected.TopVoxelZ + 1, Context.EdgeSealThickness,
            MeasureSettings, nullptr);
        TestTrue(FString::Printf(TEXT("selected %s measurement is bit-identical on rerun"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 VF_ShowcaseMetricsEqual(Selected.Metrics, RepeatMetrics));

        FVoxelStrateSampleGrid SampleGrid;
        const FVoxelStrateMetrics Metrics = VF_MeasureStrateWithSampler(
            Sampler, Selected.BottomVoxelZ, Selected.TopVoxelZ + 1, Context.EdgeSealThickness,
            MeasureSettings, &SampleGrid);
        int32 DensitySignMismatches = 0;
        for (int32 CellIndex = 0; CellIndex < SampleGrid.CellCount; ++CellIndex)
        {
            const bool bAirByDensity = SampleGrid.Density[CellIndex] > 0.0f;
            const bool bAirByBit = SampleGrid.Air[CellIndex] != 0u;
            if (bAirByDensity != bAirByBit)
            {
                ++DensitySignMismatches;
            }
        }
        TestEqual(FString::Printf(TEXT("selected %s preserves density sign polarity"),
                                  VF_GetStrateArchetypeName(Candidate.Archetype)),
                  DensitySignMismatches, 0);

        FVoxelStratePreviewCandidate PreviewCandidate;
        FString PreviewError;
        const bool bCoarseWritten = VF_WriteStratePreviewCandidate(
            ShowcaseDirectory, RenderIndex,
            VF_FormatNativeParameterRecipe(Candidate),
            SampleGrid, Metrics, MeasureSettings.HeadroomCells,
            Selected.SelectionScore, Selected.LawText, false, TEXT(""),
            PreviewCandidate, PreviewError);
        TestTrue(FString::Printf(TEXT("selected %s coarse preview writes"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 bCoarseWritten && PreviewCandidate.bRendered
                     && PreviewCandidate.bContourRendered);
        if (!bCoarseWritten)
        {
            AddError(FString::Printf(TEXT("selected %s coarse preview failed: %s"),
                                     VF_GetStrateArchetypeName(Candidate.Archetype), *PreviewError));
            return false;
        }

        if (!bCommonWindowSet)
        {
            CommonWindow = PreviewCandidate.Window;
            bCommonWindowSet = true;
        }

        FVoxelStrateMeasureSettings FineMeasureSettings =
            FinePreviewSettings.MakeMeasureSettings(MeasureSettings);
        FineMeasureSettings.CenterXY = FVector2D(
            Metrics.LargestComponentPoint.X, Metrics.LargestComponentPoint.Y);
        FVoxelStrateSampleGrid FineGrid;
        const FVoxelStrateMetrics FineMetrics = VF_MeasureStrateWithSampler(
            Sampler, Selected.BottomVoxelZ, Selected.TopVoxelZ + 1, Context.EdgeSealThickness,
            FineMeasureSettings, &FineGrid);
        const FVector2D FineResolvedCenter(
            0.5f * (FineGrid.MinX + FineGrid.MaxX),
            0.5f * (FineGrid.MinY + FineGrid.MaxY));
        TestTrue(FString::Printf(TEXT("selected %s fine ROI is centred on LargestComponentPoint"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 !FineMetrics.bValid
                     || (FMath::IsNearlyEqual(
                             FineResolvedCenter.X, Metrics.LargestComponentPoint.X, 0.5f)
                         && FMath::IsNearlyEqual(
                             FineResolvedCenter.Y, Metrics.LargestComponentPoint.Y, 0.5f)));
        TestTrue(FString::Printf(TEXT("selected %s fine ROI stays within its cap"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 !FineMetrics.bValid || FineGrid.CellCount <= FinePreviewMaxCells);
        const bool bFineWritten = VF_WriteStratePreviewFineCandidate(
            ShowcaseDirectory, RenderIndex, FineGrid,
            FineMeasureSettings.HeadroomCells,
            FineMetrics.bValid ? FString() : FineMetrics.RefusalReason,
            PreviewCandidate, PreviewError);
        TestTrue(FString::Printf(TEXT("selected %s fine preview records"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 bFineWritten);
        if (!bFineWritten)
        {
            AddError(FString::Printf(TEXT("selected %s fine preview failed: %s"),
                                     VF_GetStrateArchetypeName(Candidate.Archetype), *PreviewError));
            return false;
        }
        if (FineMetrics.bValid)
        {
            TestTrue(FString::Printf(TEXT("selected %s fine preview has filled and contour views"),
                                     VF_GetStrateArchetypeName(Candidate.Archetype)),
                     PreviewCandidate.bFineRendered && PreviewCandidate.bFineContourRendered);
        }
        if (FineMetrics.bValid && PreviewCandidate.bFineRendered)
        {
            ++FineRendered;
            if (PreviewCandidate.bFineBlank)
            {
                ++FineBlank;
            }
        }
        else if (!FineMetrics.bValid)
        {
            ++FineRefused;
        }

        PreviewCandidate.CandidateIndex = Candidate.Index;
        PreviewCandidate.ArchetypeName = VF_GetStrateArchetypeName(Candidate.Archetype);
        PreviewCandidate.bShowcaseCard = true;
        PreviewCandidate.ComposerSeed = Candidate.Seed;
        PreviewCandidate.ComposerTargetStrateIndex = Selected.TargetStrateIndex;
        PreviewCandidate.bComposerRollStructure = false;
        PreviewCandidate.SelectionReason = FString::Printf(
            TEXT("Hard gates passed: non-vacuous, largest air component >= %.2f, and exact "
                 "unsnapped arrival → departure connectivity. Selected by score "
                 "floor_area + 0.25*clamp(clearance/%d,0,1) + 0.10*largest_surface = %.6f; "
                 "floor area is the primary walking signal. Applied to existing target slot %d."),
            LargestComponentSurvivalThreshold,
            static_cast<int32>(ClearanceNormalizationVoxels),
            Selected.SelectionScore,
            Selected.TargetStrateIndex);
        PreviewCandidates.Add(MoveTemp(PreviewCandidate));

        TestTrue(FString::Printf(TEXT("selected %s retains a valid exact coarse window"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 Metrics.bValid && SampleGrid.IsValid());
    }

    for (int32 ArchetypeIndex = 0;
         ArchetypeIndex < UE_ARRAY_COUNT(GShowcaseArchetypes);
         ++ArchetypeIndex)
    {
        const FShowcaseSample& Selected = Stats[ArchetypeIndex].Best.GetValue();
        Summaries[ArchetypeIndex].Window = PreviewCandidates[ArchetypeIndex].Window;
        Summaries[ArchetypeIndex].WindowSummary = VF_FormatShowcaseSummaryWindow(
            Stats[ArchetypeIndex]);
        AddInfo(FString::Printf(
            TEXT("%s: hard-gate survivors=%d, selected seed=%d index=%d, "
                 "walkable=%.6f, floor_area=%.6f, clearance=%d voxels, largest_surface=%.6f, "
                 "window=%s"),
            VF_GetStrateArchetypeName(GShowcaseArchetypes[ArchetypeIndex]),
            Stats[ArchetypeIndex].Survivors.Num(),
            Selected.Candidate.Seed, Selected.Candidate.Index,
            Selected.Metrics.WalkableFraction,
            Selected.Metrics.WalkableFloorAreaFraction,
            Selected.Metrics.MedianVerticalClearance,
            Selected.Metrics.LargestWalkableSurfaceShare,
            *PreviewCandidates[ArchetypeIndex].Window.Describe()));
    }

    FString IndexPath;
    FString IndexError;
    const bool bIndexWritten = VF_WriteStratePreviewIndex(
        ShowcaseDirectory,
        TEXT("VoxelForge Showcase — one strate per archetype"),
        CommonWindow,
        PreviewCandidates,
        IndexPath,
        IndexError,
        &Summaries);
    TestTrue(TEXT("showcase index writes to Saved/VoxelForge/Showcase"),
             bIndexWritten && IndexPath == (ShowcaseDirectory / TEXT("index.html")));
    if (!bIndexWritten)
    {
        AddError(IndexError);
        return false;
    }

    TestEqual(TEXT("showcase page has exactly eight archetype cards"),
              PreviewCandidates.Num(), static_cast<int32>(UE_ARRAY_COUNT(GShowcaseArchetypes)));
    TestEqual(TEXT("showcase fine preview rendered count"), FineRendered, 8 - FineRefused);
    AddInfo(FString::Printf(
        TEXT("Showcase output: %s; fine rendered=%d, fine refused=%d, fine blank=%d. "
             "Cards use existing interior target slots and bComposerRollStructure=false; each "
             "card names its target slot, and no target layout/passages were regenerated."),
        *IndexPath, FineRendered, FineRefused, FineBlank));

    const double TestSeconds = FPlatformTime::Seconds() - TestStartSeconds;
    AddInfo(FString::Printf(TEXT("Composer.Showcase total runtime %.3fs."), TestSeconds));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
