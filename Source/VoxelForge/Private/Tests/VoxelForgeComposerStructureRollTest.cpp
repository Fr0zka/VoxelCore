// Tier 4b — deterministic structure recipes, dependency-safe materialisation, and measurement.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"
#include "VoxelSettings.h"
#include "VoxelStrateComposer.h"
#include "VoxelStrateMeasure.h"
#include "VoxelStratePreview.h"
#include "Misc/Paths.h"

namespace
{
    constexpr int32 NumCandidates = 64;
    constexpr float LargestComponentSurvivalThreshold = 0.50f;
    constexpr int32 BoxChecksPerCandidate = 40;
    constexpr int32 BoxStep = 1;
    constexpr int32 BoxCells = 8;
    constexpr int32 FinePreviewSampleStep = 1;
    constexpr int32 FinePreviewRadiusVoxels = 64;
    constexpr int32 FinePreviewMaxCells = 2000000;

    const TCHAR* VF_ConnectivityName(EVoxelConnectivityResult Result)
    {
        switch (Result)
        {
        case EVoxelConnectivityResult::Connected:                   return TEXT("Connected");
        case EVoxelConnectivityResult::NotConnectedAtThisResolution: return TEXT("NotConnected");
        case EVoxelConnectivityResult::StartCellSolid:              return TEXT("StartCellSolid");
        case EVoxelConnectivityResult::GoalCellSolid:               return TEXT("GoalCellSolid");
        case EVoxelConnectivityResult::OutOfWindow:                 return TEXT("OutOfWindow");
        case EVoxelConnectivityResult::CoarseLiedBudgetExhausted:   return TEXT("CoarseLiedBudget");
        case EVoxelConnectivityResult::StartCellNotPlayerFit:       return TEXT("StartCellNotPlayerFit");
        case EVoxelConnectivityResult::GoalCellNotPlayerFit:        return TEXT("GoalCellNotPlayerFit");
        }
        return TEXT("Unknown");
    }

    const TCHAR* VF_ShapeName(EVoxelStrateOpClass OpClass)
    {
        switch (OpClass)
        {
        case EVoxelStrateOpClass::RoomGraphSource:     return TEXT("RoomGraph");
        case EVoxelStrateOpClass::LatticeCorridorSource:return TEXT("Lattice");
        case EVoxelStrateOpClass::ShaftFieldSource:    return TEXT("Shaft");
        case EVoxelStrateOpClass::IslandBlobSource:    return TEXT("Island");
        case EVoxelStrateOpClass::NoiseRibbonSource:   return TEXT("Noise");
        default:                                       return TEXT("invalid");
        }
    }

    int32 VF_FloorDiv(int32 A, int32 B)
    {
        const int32 Q = A / B;
        const int32 R = A % B;
        return (R != 0 && (R < 0) != (B < 0)) ? Q - 1 : Q;
    }

    bool VF_BitEqual(float A, float B)
    {
        uint32 ABits = 0;
        uint32 BBits = 0;
        FMemory::Memcpy(&ABits, &A, sizeof(ABits));
        FMemory::Memcpy(&BBits, &B, sizeof(BBits));
        return ABits == BBits;
    }

    struct FBlockSpec
    {
        EVoxelStrateParamBlock Block;
        ECaveGeneratorType Archetype;
        uint32 SeedSalt;
    };

    static const FBlockSpec GBlockSpecs[] =
    {
        { EVoxelStrateParamBlock::TunnelNetwork,  ECaveGeneratorType::TunnelNetwork,  0x1001u },
        { EVoxelStrateParamBlock::Slab,            ECaveGeneratorType::FlatPlain,       0x1003u },
        { EVoxelStrateParamBlock::Maze,            ECaveGeneratorType::Maze,            0x1005u },
        { EVoxelStrateParamBlock::Surface,         ECaveGeneratorType::SurfaceWorld,    0x1007u },
        { EVoxelStrateParamBlock::VerticalShaft,   ECaveGeneratorType::VerticalShafts,  0x1009u },
        { EVoxelStrateParamBlock::FloatingIsland,  ECaveGeneratorType::FloatingIslands, 0x100Bu },
    };

    void VF_CopyRollBlock(FVoxelStrateArchetypeParams& OutParams,
                          const FVoxelStrateRollInfo& Roll,
                          ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
            OutParams.TunnelNetworkParams = Roll.ArchetypeParams.TunnelNetworkParams;
            break;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            OutParams.SlabParams = Roll.ArchetypeParams.SlabParams;
            break;
        case ECaveGeneratorType::Maze:
            OutParams.MazeParams = Roll.ArchetypeParams.MazeParams;
            break;
        case ECaveGeneratorType::SurfaceWorld:
            OutParams.SurfaceParams = Roll.ArchetypeParams.SurfaceParams;
            break;
        case ECaveGeneratorType::VerticalShafts:
            OutParams.VerticalShaftParams = Roll.ArchetypeParams.VerticalShaftParams;
            break;
        case ECaveGeneratorType::FloatingIslands:
            OutParams.FloatingIslandParams = Roll.ArchetypeParams.FloatingIslandParams;
            break;
        default:
            break;
        }
    }

    void VF_SetAllRuntimeBounds(FVoxelStrateArchetypeParams& Params,
                                float TopWorldZ, float BottomWorldZ)
    {
        Params.TunnelNetworkParams.StrateTopWorldZ = TopWorldZ;
        Params.TunnelNetworkParams.StrateBottomWorldZ = BottomWorldZ;
        Params.SlabParams.StrateTopWorldZ = TopWorldZ;
        Params.SlabParams.StrateBottomWorldZ = BottomWorldZ;
        Params.MazeParams.StrateTopWorldZ = TopWorldZ;
        Params.MazeParams.StrateBottomWorldZ = BottomWorldZ;
        Params.SurfaceParams.StrateTopWorldZ = TopWorldZ;
        Params.SurfaceParams.StrateBottomWorldZ = BottomWorldZ;
        Params.VerticalShaftParams.StrateTopWorldZ = TopWorldZ;
        Params.VerticalShaftParams.StrateBottomWorldZ = BottomWorldZ;
        Params.FloatingIslandParams.StrateTopWorldZ = TopWorldZ;
        Params.FloatingIslandParams.StrateBottomWorldZ = BottomWorldZ;
    }

    class FStackDensitySampler final : public IVoxelStrateDensitySampler
    {
    public:
        explicit FStackDensitySampler(const FVoxelOpStack& InStack) : Stack(InStack) {}

        float SampleDensity(float WorldX, float WorldY, float WorldZ) const override
        {
            return Stack.EvalMC(WorldX, WorldY, WorldZ);
        }

    private:
        const FVoxelOpStack& Stack;
    };

    struct FBoxVerdictReport
    {
        int32 Mixed = 0;
        int32 AllSolid = 0;
        int32 AllAir = 0;
        int32 Proved = 0;
        int64 CheckedVoxels = 0;
        int32 Violations = 0;
        FString FirstViolation;
    };

    FBoxVerdictReport VF_CheckStackBoxVerdicts(const FVoxelOpStack& Stack,
                                               const FVoxelOpContext& Context,
                                               int32 CandidateIndex,
                                               int32 BottomVoxelZ,
                                               int32 TopVoxelZ)
    {
        FBoxVerdictReport Report;
        constexpr int32 Extent = BoxStep * BoxCells;
        constexpr int32 SpanCells = 40;
        const int32 LoTile = VF_FloorDiv(BottomVoxelZ, Extent);
        const int32 HiTile = FMath::Max(LoTile, VF_FloorDiv(TopVoxelZ, Extent));
        FRandomStream Rng(0x6C617731 ^ CandidateIndex);

        for (int32 BoxIndex = 0; BoxIndex < BoxChecksPerCandidate; ++BoxIndex)
        {
            const FIntVector Origin(
                Rng.RandRange(-SpanCells, SpanCells) * Extent,
                Rng.RandRange(-SpanCells, SpanCells) * Extent,
                Rng.RandRange(LoTile, HiTile) * Extent);
            const FBox Box(
                FVector((float)(Origin.X - BoxStep), (float)(Origin.Y - BoxStep),
                        (float)(Origin.Z - BoxStep)),
                FVector((float)(Origin.X + (BoxCells + 1) * BoxStep),
                        (float)(Origin.Y + (BoxCells + 1) * BoxStep),
                        (float)(Origin.Z + (BoxCells + 1) * BoxStep)));
            const EVoxelTileClass Verdict = Stack.ClassifyBox(Box, Context);
            if (Verdict == EVoxelTileClass::Mixed)
            {
                ++Report.Mixed;
                continue;
            }
            if (Verdict == EVoxelTileClass::AllSolid)
            {
                ++Report.AllSolid;
            }
            else
            {
                ++Report.AllAir;
            }
            ++Report.Proved;

            const int32 GridDim = BoxCells + 1;
            const bool bClaimsSolid = Verdict == EVoxelTileClass::AllSolid;
            for (int32 GZ = -1; GZ <= GridDim; ++GZ)
            for (int32 GY = -1; GY <= GridDim; ++GY)
            for (int32 GX = -1; GX <= GridDim; ++GX)
            {
                const float X = (float)(Origin.X + GX * BoxStep);
                const float Y = (float)(Origin.Y + GY * BoxStep);
                const float Z = (float)(Origin.Z + GZ * BoxStep);
                const float Density = Stack.EvalMC(X, Y, Z);
                ++Report.CheckedVoxels;
                const bool bAgrees = bClaimsSolid ? (Density < 0.0f) : (Density >= 0.0f);
                if (!bAgrees)
                {
                    ++Report.Violations;
                    if (Report.FirstViolation.IsEmpty())
                    {
                        Report.FirstViolation = FString::Printf(
                            TEXT("candidate %d box %d origin (%d,%d,%d) said %s but MC density "
                                 "at (%.0f,%.0f,%.0f) was %.9g"),
                            CandidateIndex, BoxIndex, Origin.X, Origin.Y, Origin.Z,
                            bClaimsSolid ? TEXT("AllSolid") : TEXT("AllAir"),
                            X, Y, Z, Density);
                    }
                }
            }
        }
        return Report;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeComposerStructureRollTest,
    "VoxelForge.Composer.StructureRoll",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeComposerStructureRollTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;
    (void)Parameters;

    const double TestStartSeconds = FPlatformTime::Seconds();
    const FString SettingsPath = TEXT("/Game/VoxelForge/DA_Settings.DA_Settings");
    UVoxelSettings* AuthoredSettings = LoadObject<UVoxelSettings>(nullptr, *SettingsPath);
    if (AuthoredSettings == nullptr)
    {
        AddError(FString::Printf(TEXT("Could not load %s; structure roll did not run."), *SettingsPath));
        return false;
    }

    FVoxelStrateCorpus Corpus;
    FString CorpusReport;
    const bool bCorpusLoaded = Corpus.LoadFromAssetRegistry(CorpusReport);
    AddInfo(CorpusReport);
    TestTrue(TEXT("structure roll has a usable corpus for parameter blocks"),
             bCorpusLoaded && Corpus.IsValid());
    if (!bCorpusLoaded || !Corpus.IsValid())
    {
        return false;
    }

    FTestWorld World;
    World.Build(1337, 2);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }
    TestTrue(TEXT("structure-roll fixture keeps WorldRadiusVoxels at 0"),
             World.Settings->WorldRadiusVoxels == 0.0f);

    constexpr int32 CandidateStrateIndex = FTestWorld::SlotFlatPlain;
    int32 TopVoxelZ = 0;
    int32 BottomVoxelZ = 0;
    if (!World.GetSlotVoxelZRange(CandidateStrateIndex, TopVoxelZ, BottomVoxelZ))
    {
        AddError(TEXT("structure-roll fixture has no candidate Z range"));
        return false;
    }
    const float TopWorldZ = (float)TopVoxelZ + 1.0f;
    const float BottomWorldZ = (float)BottomVoxelZ;

    FVector ArrivalPoint = FVector::ZeroVector;
    FVector DeparturePoint = FVector::ZeroVector;
    int32 ArrivalCount = 0;
    int32 DepartureCount = 0;
    for (const FVoxelPassage& Passage : World.StrateManager->GetPassages())
    {
        if (Passage.LowerStrateIndex == CandidateStrateIndex
            && Passage.UpperStrateIndex + 1 == CandidateStrateIndex)
        {
            ArrivalPoint = Passage.LowerPoint;
            ++ArrivalCount;
        }
        if (Passage.UpperStrateIndex == CandidateStrateIndex
            && Passage.LowerStrateIndex == CandidateStrateIndex + 1)
        {
            DeparturePoint = Passage.UpperPoint;
            ++DepartureCount;
        }
    }
    TestEqual(TEXT("candidate has exactly one arrival mouth"), ArrivalCount, 1);
    TestEqual(TEXT("candidate has exactly one departure mouth"), DepartureCount, 1);

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
    TestTrue(TEXT("fine preview defaults are a bounded valid ROI"), FinePreviewSettings.IsValid());
    const FVoxelStrateMeasureSettings FineMeasureSettings =
        FinePreviewSettings.MakeMeasureSettings(MeasureSettings);

    FString CandidateTable = TEXT(
        "candidate | recipe (compact) | air | largest share | walkable | feature scale | arrival->departure\n");
    int32 NumSurvivors = 0;
    int32 NumNonVacuous = 0;
    int32 NumLargestEnough = 0;
    int32 NumLawPasses = 0;
    int32 NumInvalidRecipes = 0;
    int32 NumParameterRollFailures = 0;
    int32 TotalBoxMixed = 0;
    int32 TotalBoxAllSolid = 0;
    int32 TotalBoxAllAir = 0;
    int32 TotalBoxProved = 0;
    int64 TotalBoxCheckedVoxels = 0;
    int32 TotalBoxViolations = 0;
    FString FirstBoxViolation;
    int32 TotalBootstrapJitterFields = 0;
    TSet<FString> BootstrapJitterFieldNames;
    TArray<FVoxelStratePreviewCandidate> PreviewCandidates;
    PreviewCandidates.Reserve(NumCandidates);
    FVoxelStratePreviewWindow PreviewWindow;
    bool bPreviewWindowSet = false;
    int32 NumFineRequested = 0;
    int32 NumFineRendered = 0;
    int32 NumFineContourRendered = 0;
    int32 NumFineRefused = 0;
    int32 NumFinePlanBlankBeforeCentering = 0;
    int32 NumFinePlanBlankAfterCentering = 0;
    int32 NumFineCardBlankBeforeCentering = 0;
    int32 NumFineCardBlankAfterCentering = 0;
    int64 TotalFineCells = 0;
    int64 PeakFineCaptureBytes = 0;
    int64 PeakFineRasterBytes = 0;
    double FineMeasureSeconds = 0.0;
    double FineRenderSeconds = 0.0;
    double CoarsePreviewSeconds = 0.0;
    bool bFineCapRefusalChecked = false;
    const FString PreviewRunId = FString::Printf(
        TEXT("structure_seed_%d_corpus_%08x_step_%d_radius_%d_fine_step_%d_fine_radius_%d"),
        AuthoredSettings->Seed, Corpus.GetContentsHash(),
        MeasureSettings.SampleStep, MeasureSettings.RadiusInVoxels,
        FinePreviewSettings.SampleStep, FinePreviewSettings.RadiusInVoxels);
    const FString PreviewDirectory = FPaths::ProjectSavedDir()
        / TEXT("ComposerPreview") / PreviewRunId;
    TArray<int32> ShapeCounts;
    ShapeCounts.Init(0, 5);
    int32 KCounts[9] = { 0 };
    TArray<FVoxelOpStackRecipe> DistinctRecipes;
    DistinctRecipes.Reserve(NumCandidates);

    const double RollAndMeasureStartSeconds = FPlatformTime::Seconds();
    for (int32 CandidateIndex = 0; CandidateIndex < NumCandidates; ++CandidateIndex)
    {
#if WITH_EDITOR
        // Keep the editor hand-off and the structure test on the same recipe/block roll seam.
        const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
            Corpus, AuthoredSettings->Seed, CandidateIndex, true);
        TestTrue(FString::Printf(TEXT("candidate %d hand-off roll is valid"), CandidateIndex),
                 Candidate.bValid);
        const FVoxelOpStackRecipe Recipe = Candidate.Recipe;
#else
        const FVoxelOpStackRecipe Recipe = VF_RollStrateStructure(
            AuthoredSettings->Seed, CandidateIndex);
#endif
        const FVoxelOpStackRecipe RepeatRecipe = VF_RollStrateStructure(
            AuthoredSettings->Seed, CandidateIndex);
        TestTrue(FString::Printf(TEXT("candidate %d recipe reroll is identical"), CandidateIndex),
                 VF_AreStrateStructureRecipesIdentical(Recipe, RepeatRecipe));

        const uint32 RecipeHash = VF_HashStrateStructureRecipe(Recipe);
        bool bSeenRecipe = false;
        for (const FVoxelOpStackRecipe& Existing : DistinctRecipes)
        {
            if (VF_HashStrateStructureRecipe(Existing) == RecipeHash
                && VF_AreStrateStructureRecipesIdentical(Existing, Recipe))
            {
                bSeenRecipe = true;
                break;
            }
        }
        if (!bSeenRecipe) { DistinctRecipes.Add(Recipe); }

        switch (Recipe.ShapeSource.OpClass)
        {
        case EVoxelStrateOpClass::RoomGraphSource:      ++ShapeCounts[0]; break;
        case EVoxelStrateOpClass::LatticeCorridorSource:++ShapeCounts[1]; break;
        case EVoxelStrateOpClass::ShaftFieldSource:     ++ShapeCounts[2]; break;
        case EVoxelStrateOpClass::IslandBlobSource:     ++ShapeCounts[3]; break;
        case EVoxelStrateOpClass::NoiseRibbonSource:    ++ShapeCounts[4]; break;
        default: break;
        }
        if (Recipe.Modifiers.IsValidIndex(0))
        {
            ++KCounts[Recipe.Modifiers.Num()];
        }

        FVoxelStrateArchetypeParams CandidateParams;
        bool bParameterRollsValid = true;
        double CandidateDistanceSquared = 0.0;
        for (int32 BlockIndex = 0; BlockIndex < UE_ARRAY_COUNT(GBlockSpecs); ++BlockIndex)
        {
#if WITH_EDITOR
            const FBlockSpec& Spec = GBlockSpecs[BlockIndex];
            if (!Candidate.StructureBlockRolls.IsValidIndex(BlockIndex))
            {
                bParameterRollsValid = false;
                AddError(FString::Printf(TEXT("candidate %d missing parameter block %d"),
                                         CandidateIndex, static_cast<int32>(Spec.Block)));
                continue;
            }
            const FVoxelStrateRollInfo& BlockRoll = Candidate.StructureBlockRolls[BlockIndex];
#else
            const FBlockSpec& Spec = GBlockSpecs[BlockIndex];
            const FVoxelStrateRollInfo BlockRoll = VF_RollStrateParamsDetailedForArchetype(
                Corpus, Spec.Archetype,
                AuthoredSettings->Seed ^ static_cast<int32>(Spec.SeedSalt), CandidateIndex);
#endif
            if (!BlockRoll.bValid)
            {
                bParameterRollsValid = false;
                ++NumParameterRollFailures;
                AddError(FString::Printf(TEXT("candidate %d parameter block %d failed: %s"),
                                         CandidateIndex, static_cast<int32>(Spec.Block),
                                         *BlockRoll.FailureReason));
                continue;
            }
            TotalBootstrapJitterFields += BlockRoll.BootstrapJitterFieldCount;
            for (const FString& FieldName : BlockRoll.BootstrapJitterFieldNames)
            {
                BootstrapJitterFieldNames.Add(FieldName);
            }
            const double BlockDistance = VF_DistanceFromStrateCorpusCentroid(
                Corpus, BlockRoll.ArchetypeParams, Spec.Archetype);
            CandidateDistanceSquared += BlockDistance * BlockDistance;
            VF_CopyRollBlock(CandidateParams, BlockRoll, Spec.Archetype);
        }
        VF_SetAllRuntimeBounds(CandidateParams, TopWorldZ, BottomWorldZ);

        FString AirText = TEXT("invalid");
        FString LargestText = TEXT("invalid");
        FString WalkableText = TEXT("invalid");
        FString FeatureText = TEXT("invalid");
        FString LawText = TEXT("not measured");

        FVoxelOpStack Stack;
        FVoxelOpContext Context;
        FString BuildError;
        const bool bBuilt = bParameterRollsValid && VF_BuildStackFromRecipe(
            Recipe, CandidateParams, AuthoredSettings->Seed, 14.0f,
            World.StrateManager.Get(), Stack, Context, &BuildError);
        if (!bBuilt)
        {
            ++NumInvalidRecipes;
            CandidateTable += FString::Printf(
                TEXT("%d | %s | invalid | invalid | invalid | invalid | invalid | build=%s\n"),
                CandidateIndex, *VF_FormatStrateStructureRecipe(Recipe),
                bParameterRollsValid ? *BuildError : TEXT("parameter block roll failed"));
            TestTrue(FString::Printf(TEXT("candidate %d recipe materialises"), CandidateIndex), false);
            continue;
        }

        TestEqual(FString::Printf(TEXT("candidate %d has four mandatory structural posts"), CandidateIndex),
                  Stack.Num(), Recipe.Modifiers.Num() + 7);
        FString ValidationError;
        TestTrue(FString::Printf(TEXT("candidate %d passes ValidateChannelOrder"), CandidateIndex),
                 Stack.ValidateChannelOrder(&ValidationError));
        TestTrue(FString::Printf(TEXT("candidate %d context keeps WorldRadiusVoxels at 0"), CandidateIndex),
                 Context.WorldRadiusVoxels == 0.0f);

        Stack.PrepareChunk(Context);
        FStackDensitySampler Sampler(Stack);

        if (!bFineCapRefusalChecked)
        {
            FVoxelStrateMeasureSettings RefusalSettings = FineMeasureSettings;
            RefusalSettings.MaxCells = 1;
            FVoxelStrateSampleGrid RefusedFineGrid;
            const FVoxelStrateMetrics RefusedFineMetrics = VF_MeasureStrateWithSampler(
                Sampler, BottomVoxelZ, TopVoxelZ + 1,
                Context.EdgeSealThickness, RefusalSettings, &RefusedFineGrid);
            TestTrue(TEXT("fine preview refuses an over-cap ROI before allocation"),
                     !RefusedFineMetrics.bValid
                         && RefusedFineGrid.Air.Num() == 0
                         && RefusedFineGrid.Density.Num() == 0
                         && RefusedFineMetrics.RefusalReason.Contains(TEXT("MaxCells")));
            bFineCapRefusalChecked = true;
        }

        FVoxelStrateSampleGrid SampleGrid;
        const FVoxelStrateMetrics Metrics = VF_MeasureStrateWithSampler(
            Sampler, BottomVoxelZ, TopVoxelZ + 1,
            Context.EdgeSealThickness, MeasureSettings, &SampleGrid);

        FVoxelConnectivityDiagnostics Law;
        if (ArrivalCount == 1 && DepartureCount == 1)
        {
            Law = VF_DiagnoseConnectivityWithSampler(
                Sampler, BottomVoxelZ, TopVoxelZ + 1,
                Context.EdgeSealThickness, ArrivalPoint, DeparturePoint, MeasureSettings);
            LawText = VF_ConnectivityName(Law.Result);
            if (Law.bStartSnapped || Law.bGoalSnapped) { LawText += TEXT(" (snapped)"); }
        }
        else
        {
            LawText = FString::Printf(TEXT("mouths=%d/%d"), ArrivalCount, DepartureCount);
        }

        const bool bNonVacuous = Metrics.bValid
            && Metrics.NumSampled > 0 && Metrics.NumAir > 0 && Metrics.NumSolid > 0;
        const bool bLargestEnough = bNonVacuous
            && Metrics.LargestComponentShare >= LargestComponentSurvivalThreshold;
        const bool bLawPass = ArrivalCount == 1 && DepartureCount == 1
            && Law.bValid && Law.Result == EVoxelConnectivityResult::Connected
            && !Law.bStartSnapped && !Law.bGoalSnapped;
        if (bNonVacuous) { ++NumNonVacuous; }
        if (bLargestEnough) { ++NumLargestEnough; }
        if (bLawPass) { ++NumLawPasses; }
        if (bNonVacuous && bLargestEnough && bLawPass) { ++NumSurvivors; }

        if (Metrics.bValid)
        {
            AirText = FString::Printf(TEXT("%.6f"), Metrics.AirFraction);
            LargestText = FString::Printf(TEXT("%.6f"), Metrics.LargestComponentShare);
            WalkableText = FString::Printf(TEXT("%.6f"), Metrics.WalkableFraction);
            FeatureText = FString::Printf(TEXT("%.6f"), Metrics.MedianFeatureScale);
        }

        FString RejectionReason;
        if (!bNonVacuous)
        {
            RejectionReason = TEXT("vacuous");
        }
        else
        {
            if (!bLargestEnough)
            {
                RejectionReason = TEXT("fragmented");
            }
            if (!bLawPass)
            {
                if (!RejectionReason.IsEmpty())
                {
                    RejectionReason += TEXT("; ");
                }
                RejectionReason += TEXT("law failed");
            }
        }

        const FBoxVerdictReport BoxReport = VF_CheckStackBoxVerdicts(
            Stack, Context, CandidateIndex, BottomVoxelZ, TopVoxelZ);
        TotalBoxMixed += BoxReport.Mixed;
        TotalBoxAllSolid += BoxReport.AllSolid;
        TotalBoxAllAir += BoxReport.AllAir;
        TotalBoxProved += BoxReport.Proved;
        TotalBoxCheckedVoxels += BoxReport.CheckedVoxels;
        TotalBoxViolations += BoxReport.Violations;
        if (FirstBoxViolation.IsEmpty() && !BoxReport.FirstViolation.IsEmpty())
        {
            FirstBoxViolation = BoxReport.FirstViolation;
        }

        const double CandidateDistance = FMath::Sqrt(
            FMath::Max(0.0, CandidateDistanceSquared));
        const FVoxelStratePreviewWindow CandidateWindow =
            VF_GetStratePreviewWindow(SampleGrid);
        if (CandidateWindow.IsValid())
        {
            if (!bPreviewWindowSet)
            {
                PreviewWindow = CandidateWindow;
                bPreviewWindowSet = true;
            }
        }
        TestTrue(FString::Printf(TEXT("candidate %d preview captures its measurement grid"),
                                 CandidateIndex),
                 !Metrics.bValid || SampleGrid.HasScalarDensity());

        FVoxelStratePreviewCandidate PreviewCandidate;
        FString PreviewError;
        const double CoarsePreviewStartSeconds = FPlatformTime::Seconds();
        const bool bPreviewWritten = VF_WriteStratePreviewCandidate(
            PreviewDirectory,
            CandidateIndex,
            VF_FormatStrateStructureRecipe(Recipe),
            SampleGrid,
            Metrics,
            MeasureSettings.HeadroomCells,
            CandidateDistance,
            LawText,
            !(bNonVacuous && bLargestEnough && bLawPass),
            RejectionReason,
            PreviewCandidate,
            PreviewError);
        CoarsePreviewSeconds += FPlatformTime::Seconds() - CoarsePreviewStartSeconds;
        TestTrue(FString::Printf(TEXT("candidate %d preview rasterises without a second sample"),
                                 CandidateIndex),
                 bPreviewWritten);
        TestTrue(FString::Printf(TEXT("candidate %d has images when its measurement is valid"),
                                 CandidateIndex),
                 !Metrics.bValid
                     || (PreviewCandidate.bRendered && PreviewCandidate.bContourRendered));
        if (!bPreviewWritten)
        {
            AddError(FString::Printf(TEXT("candidate %d preview failed: %s"),
                                     CandidateIndex, *PreviewError));
        }

        const bool bCandidateSurvivor = bNonVacuous && bLargestEnough && bLawPass;
        if (bCandidateSurvivor)
        {
            ++NumFineRequested;

            // Render the old origin-centred pass into the same filenames first. The temporary
            // record gives an exact before count from the renderer's content-variation check; the
            // corrected capture below overwrites those files. The two grids are sequential, so
            // peak retained memory remains one fine capture.
            FVoxelStrateSampleGrid OriginFineGrid;
            const double OriginFineMeasureStartSeconds = FPlatformTime::Seconds();
            const FVoxelStrateMetrics OriginFineMetrics = VF_MeasureStrateWithSampler(
                Sampler, BottomVoxelZ, TopVoxelZ + 1,
                Context.EdgeSealThickness, FineMeasureSettings, &OriginFineGrid);
            FineMeasureSeconds += FPlatformTime::Seconds() - OriginFineMeasureStartSeconds;
            FVoxelStratePreviewCandidate OriginFinePreview;
            FString OriginFinePreviewError;
            const double OriginFineRenderStartSeconds = FPlatformTime::Seconds();
            const bool bOriginFineWritten = VF_WriteStratePreviewFineCandidate(
                PreviewDirectory, CandidateIndex, OriginFineGrid,
                FineMeasureSettings.HeadroomCells,
                OriginFineMetrics.bValid ? FString() : OriginFineMetrics.RefusalReason,
                OriginFinePreview, OriginFinePreviewError);
            FineRenderSeconds += FPlatformTime::Seconds() - OriginFineRenderStartSeconds;
            TestTrue(FString::Printf(TEXT("candidate %d origin baseline fine render records"),
                                     CandidateIndex),
                     bOriginFineWritten);
            if (OriginFinePreview.bFineRendered && !OriginFinePreview.bFinePlanContentVaries)
            {
                ++NumFinePlanBlankBeforeCentering;
            }
            if (OriginFinePreview.bFineRendered && OriginFinePreview.bFineBlank)
            {
                ++NumFineCardBlankBeforeCentering;
            }

            FVoxelStrateMeasureSettings CenteredFineMeasureSettings = FineMeasureSettings;
            if (Metrics.bValid && FMath::IsFinite(Metrics.LargestComponentPoint.X)
                && FMath::IsFinite(Metrics.LargestComponentPoint.Y))
            {
                CenteredFineMeasureSettings.CenterXY = FVector2D(
                    Metrics.LargestComponentPoint.X, Metrics.LargestComponentPoint.Y);
            }
            FVoxelStrateSampleGrid FineGrid;
            const double FineMeasureStartSeconds = FPlatformTime::Seconds();
            const FVoxelStrateMetrics FineMetrics = VF_MeasureStrateWithSampler(
                Sampler, BottomVoxelZ, TopVoxelZ + 1,
                Context.EdgeSealThickness, CenteredFineMeasureSettings, &FineGrid);
            FineMeasureSeconds += FPlatformTime::Seconds() - FineMeasureStartSeconds;

            const FString FineFailureReason = FineMetrics.bValid
                ? FString() : FineMetrics.RefusalReason;
            const double FineRenderStartSeconds = FPlatformTime::Seconds();
            FString FinePreviewError;
            const bool bFineWritten = VF_WriteStratePreviewFineCandidate(
                PreviewDirectory, CandidateIndex, FineGrid,
                FineMeasureSettings.HeadroomCells, FineFailureReason,
                PreviewCandidate, FinePreviewError);
            FineRenderSeconds += FPlatformTime::Seconds() - FineRenderStartSeconds;

            TestTrue(FString::Printf(TEXT("candidate %d fine preview records without a second renderer sample"),
                                     CandidateIndex),
                     bFineWritten);
            if (PreviewCandidate.bFineRendered && !PreviewCandidate.bFinePlanContentVaries)
            {
                ++NumFinePlanBlankAfterCentering;
            }
            if (PreviewCandidate.bFineRendered && PreviewCandidate.bFineBlank)
            {
                ++NumFineCardBlankAfterCentering;
            }
            TestTrue(FString::Printf(TEXT("candidate %d fine capture keeps scalar density"),
                                     CandidateIndex),
                     !FineMetrics.bValid || FineGrid.HasScalarDensity());
            if (FineMetrics.bValid)
            {
                TotalFineCells += FineGrid.CellCount;
                PeakFineCaptureBytes = FMath::Max(
                    PeakFineCaptureBytes,
                    static_cast<int64>(FineGrid.Air.GetAllocatedSize())
                        + static_cast<int64>(FineGrid.Density.GetAllocatedSize()));
            }
            if (!FineMetrics.bValid)
            {
                ++NumFineRefused;
            }
            if (PreviewCandidate.bFineRendered)
            {
                ++NumFineRendered;
                PeakFineRasterBytes = FMath::Max(
                    PeakFineRasterBytes,
                    static_cast<int64>(PreviewCandidate.FineVerticalImageWidth)
                        * static_cast<int64>(PreviewCandidate.FineVerticalImageHeight)
                        * static_cast<int64>(sizeof(FColor)));
                PeakFineRasterBytes = FMath::Max(
                    PeakFineRasterBytes,
                    static_cast<int64>(PreviewCandidate.FinePlanImageWidth)
                        * static_cast<int64>(PreviewCandidate.FinePlanImageHeight)
                        * static_cast<int64>(sizeof(FColor)));
            }
            if (PreviewCandidate.bFineContourRendered)
            {
                ++NumFineContourRendered;
            }
            if (!bFineWritten)
            {
                AddError(FString::Printf(TEXT("candidate %d fine preview failed: %s"),
                                         CandidateIndex, *FinePreviewError));
            }
        }
        PreviewCandidates.Add(MoveTemp(PreviewCandidate));

        // Same recipe + same block vectors + same seed must yield the same world values. This is
        // checked on a fixed point array for every candidate, including cache-backed sources.
        FVoxelOpStack RepeatStack;
        FVoxelOpContext RepeatContext;
        FString RepeatBuildError;
        const bool bRepeatBuilt = VF_BuildStackFromRecipe(
            Recipe, CandidateParams, AuthoredSettings->Seed, 14.0f,
            World.StrateManager.Get(), RepeatStack, RepeatContext, &RepeatBuildError);
        TestTrue(FString::Printf(TEXT("candidate %d same recipe rebuild succeeds"), CandidateIndex),
                 bRepeatBuilt);
        if (bRepeatBuilt)
        {
            RepeatStack.PrepareChunk(RepeatContext);
            const FVector ProbePoints[] = {
                FVector(-91.0f, -37.0f, BottomWorldZ + 17.0f),
                FVector(0.0f, 0.0f, (BottomWorldZ + TopWorldZ) * 0.5f),
                FVector(73.0f, 119.0f, TopWorldZ - 19.0f),
                FVector(-256.0f, 256.0f, BottomWorldZ + 61.0f),
            };
            bool bSameWorld = true;
            for (const FVector& Point : ProbePoints)
            {
                if (!VF_BitEqual(Stack.EvalMC(Point.X, Point.Y, Point.Z),
                                 RepeatStack.EvalMC(Point.X, Point.Y, Point.Z)))
                {
                    bSameWorld = false;
                    break;
                }
            }
            TestTrue(FString::Printf(TEXT("candidate %d same seed yields same world"), CandidateIndex),
                     bSameWorld);
        }

        CandidateTable += FString::Printf(
            TEXT("%d | %s | %s | %s | %s | %s | %s\n"),
            CandidateIndex,
            *VF_FormatStrateStructureRecipe(Recipe),
            *AirText, *LargestText, *WalkableText, *FeatureText, *LawText);
    }
    const double RollAndMeasureSeconds = FPlatformTime::Seconds() - RollAndMeasureStartSeconds;

    AddInfo(CandidateTable);
    FString ShapeDistribution = TEXT("Shape source distribution:");
    for (int32 Index = 0; Index < 5; ++Index)
    {
        const EVoxelStrateOpClass ShapeClass[] = {
            EVoxelStrateOpClass::RoomGraphSource,
            EVoxelStrateOpClass::LatticeCorridorSource,
            EVoxelStrateOpClass::ShaftFieldSource,
            EVoxelStrateOpClass::IslandBlobSource,
            EVoxelStrateOpClass::NoiseRibbonSource,
        };
        ShapeDistribution += FString::Printf(TEXT(" %s=%d"),
                                              VF_ShapeName(ShapeClass[Index]), ShapeCounts[Index]);
    }
    FString KDistribution = TEXT("k distribution:");
    for (int32 K = 4; K <= 8; ++K)
    {
        KDistribution += FString::Printf(TEXT(" k%d=%d"), K, KCounts[K]);
    }
    AddInfo(ShapeDistribution);
    AddInfo(KDistribution);
    AddInfo(FString::Printf(
        TEXT("Structure diversity: %d distinct recipes of %d; invalid recipes emitted=%d; "
             "parameter-block roll failures=%d; recipe/build/measure time=%.3fs."),
        DistinctRecipes.Num(), NumCandidates, NumInvalidRecipes,
        NumParameterRollFailures, RollAndMeasureSeconds));
    AddInfo(FString::Printf(
        TEXT("Survival: %d/%d (%.1f%%). Criteria: non-vacuous (valid, sampled, air>0, solid>0), "
             "largest component share >= %.2f, and exact unsnapped arrival->departure law "
             "Result=Connected. Intermediate counts: non-vacuous=%d, largest threshold=%d, "
             "law pass=%d. Lower survival is expected for aggressive structure rolls."),
        NumSurvivors, NumCandidates, 100.0f * (float)NumSurvivors / (float)NumCandidates,
        LargestComponentSurvivalThreshold, NumNonVacuous, NumLargestEnough, NumLawPasses));
    AddInfo(FString::Printf(
        TEXT("Rolled custom-stack box verdicts: mixed=%d, AllSolid=%d, AllAir=%d, proved=%d, "
             "brute-force voxels checked=%lld, violations=%d. %s"),
        TotalBoxMixed, TotalBoxAllSolid, TotalBoxAllAir, TotalBoxProved,
        TotalBoxCheckedVoxels, TotalBoxViolations,
        FirstBoxViolation.IsEmpty() ? TEXT("No violations.") : *FirstBoxViolation));
    AddInfo(FString::Printf(
        TEXT("Preview cost: coarse step=%d 64-card pass render %.3fs; fine ROI requested=%d "
             "(survivors=%d), rendered=%d, contour pairs=%d, refused=%d; "
             "blank plan origin=%d, blank plan largest-open-space=%d; blank card origin=%d, "
             "blank card largest-open-space=%d; fine sample %.3fs + "
             "render %.3fs; fine cells sampled=%lld; peak fine Air+Density capture=%lld bytes; "
             "peak single RGBA raster=%lld bytes; ROI step=%d radius=%d MaxCells=%d."),
        MeasureSettings.SampleStep, CoarsePreviewSeconds,
        NumFineRequested, NumSurvivors, NumFineRendered, NumFineContourRendered, NumFineRefused,
        NumFinePlanBlankBeforeCentering, NumFinePlanBlankAfterCentering,
        NumFineCardBlankBeforeCentering, NumFineCardBlankAfterCentering,
        FineMeasureSeconds, FineRenderSeconds, TotalFineCells,
        PeakFineCaptureBytes, PeakFineRasterBytes,
        FinePreviewSettings.SampleStep, FinePreviewSettings.RadiusInVoxels,
        FinePreviewSettings.MaxCells));
    FString PreviewIndexPath;
    FString PreviewError;
    const bool bPreviewIndexWritten = bPreviewWindowSet
        && PreviewCandidates.Num() == NumCandidates
        && VF_WriteStratePreviewIndex(
            PreviewDirectory,
            FString::Printf(TEXT("VoxelForge composer structure preview — seed %d"),
                             AuthoredSettings->Seed),
            PreviewWindow,
            PreviewCandidates,
            PreviewIndexPath,
            PreviewError);
    TestTrue(TEXT("64-candidate composer preview index is written"), bPreviewIndexWritten);
    if (!bPreviewIndexWritten)
    {
        AddError(FString::Printf(TEXT("composer preview index failed: %s"), *PreviewError));
    }
    if (bPreviewWindowSet && PreviewCandidates.Num() > 0)
    {
        const FVoxelStratePreviewCandidate& FirstPreview = PreviewCandidates[0];
        int32 NumRenderedPairs = 0;
        int32 NumRenderedContourPairs = 0;
        for (const FVoxelStratePreviewCandidate& Candidate : PreviewCandidates)
        {
            if (Candidate.bRendered)
            {
                ++NumRenderedPairs;
            }
            if (Candidate.bContourRendered)
            {
                ++NumRenderedContourPairs;
            }
        }
        AddInfo(FString::Printf(
            TEXT("Composer preview: %d candidates, %d filled pairs + %d contour pairs, "
                 "vertical=%dx%d, plan=%dx%d, "
                 "plan Z chosen from each candidate's measured layer with the highest solid/air "
                 "boundary count (first candidate Z=%d); index=%s; first-candidate window=%s; "
                 "fine previews are linked on survivor cards and centred on each largest open "
                 "space point (never the strate midpoint); image cap=512 px including 20 px "
                 "scale footer."),
            PreviewCandidates.Num(),
            NumRenderedPairs,
            NumRenderedContourPairs,
            FirstPreview.VerticalImageWidth, FirstPreview.VerticalImageHeight,
            FirstPreview.PlanImageWidth, FirstPreview.PlanImageHeight,
            FirstPreview.PlanSliceWorldZ,
            *PreviewIndexPath,
            *PreviewWindow.Describe()));
    }
    AddInfo(FString::Printf(
        TEXT("Bootstrap jitter: %d field applications across the six structure blocks; %d "
             "distinct archetype fields took the bootstrap path."),
        TotalBootstrapJitterFields, BootstrapJitterFieldNames.Num()));

    TestEqual(TEXT("exactly 64 structure candidates were requested"), NumCandidates, 64);
    TestEqual(TEXT("invalid recipes emitted by the roller/build path"), NumInvalidRecipes, 0);
    TestEqual(TEXT("custom-stack box verdict violations"), TotalBoxViolations, 0);
    TestTrue(TEXT("fine cap refusal was checked before any fine allocation"),
             bFineCapRefusalChecked);
    TestEqual(TEXT("fine previews are requested only for coarse survivors"),
              NumFineRequested, NumSurvivors);
    TestEqual(TEXT("fine preview outcomes account for every requested survivor"),
              NumFineRendered + NumFineRefused, NumFineRequested);
    AddInfo(FString::Printf(TEXT(
        "Fine blank baseline: plan projection %d/%d origin-centred versus %d/%d centred on "
        "LargestComponentPoint; complete two-projection cards %d/%d versus %d/%d. A blank "
        "projection/card means its filled-cell pixels were uniform (scale footer excluded), "
        "not that the measurement was refused."),
        NumFinePlanBlankBeforeCentering, NumFineRequested,
        NumFinePlanBlankAfterCentering, NumFineRequested,
        NumFineCardBlankBeforeCentering, NumFineRequested,
        NumFineCardBlankAfterCentering, NumFineRequested));
    TestEqual(TEXT("every rendered fine preview has a scalar contour pair"),
              NumFineContourRendered, NumFineRendered);
    TestTrue(TEXT("structure roller emits 64 distinct structural recipes"),
             DistinctRecipes.Num() == NumCandidates);

    const double TestSeconds = FPlatformTime::Seconds() - TestStartSeconds;
    AddInfo(FString::Printf(TEXT("StructureRoll total runtime %.3fs; WorldRadiusVoxels=0."),
                            TestSeconds));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
