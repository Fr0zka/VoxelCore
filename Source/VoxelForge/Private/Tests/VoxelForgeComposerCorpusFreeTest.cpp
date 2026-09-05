// Corpus-free composer measurement: current corpus blend versus independent and constrained rolls.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"
#include "VoxelSettings.h"
#include "VoxelStrateComposer.h"
#include "VoxelStrateDefinition.h"
#include "VoxelStrateMeasure.h"

namespace
{
    // 256x3 and 64x3 were attempted first. The latter exceeded 27 minutes before reaching
    // its aggregate report because the fixed 8M-cell law/box checks are expensive for the
    // corpus-free fields. Keep the final reduced count equal across all three arms.
    constexpr int32 NumCandidates = 16;
    constexpr float LargestComponentSurvivalThreshold = 0.50f;
    constexpr int32 BoxChecksPerCandidate = 40;
    constexpr int32 BoxStep = 1;
    constexpr int32 BoxCells = 8;

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

    int32 VF_FloorDiv(int32 A, int32 B)
    {
        const int32 Q = A / B;
        const int32 R = A % B;
        return (R != 0 && (R < 0) != (B < 0)) ? Q - 1 : Q;
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
        { EVoxelStrateParamBlock::FloatingIsland,  ECaveGeneratorType::FloatingIslands,  0x100Bu },
    };

    void VF_CopyRollBlock(FVoxelStrateArchetypeParams& OutParams,
                          const FVoxelStrateRollInfo& Roll,
                          ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
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

    struct FQualityStats
    {
        int32 Count = 0;
        float AirMin = FLT_MAX;
        float AirMax = -FLT_MAX;
        float WalkableMin = FLT_MAX;
        float WalkableMax = -FLT_MAX;
        float FeatureMin = FLT_MAX;
        float FeatureMax = -FLT_MAX;
        double AirSum = 0.0;
        double WalkableSum = 0.0;
        double FeatureSum = 0.0;

        void Add(const FVoxelStrateMetrics& Metrics)
        {
            ++Count;
            AirMin = FMath::Min(AirMin, Metrics.AirFraction);
            AirMax = FMath::Max(AirMax, Metrics.AirFraction);
            WalkableMin = FMath::Min(WalkableMin, Metrics.WalkableFraction);
            WalkableMax = FMath::Max(WalkableMax, Metrics.WalkableFraction);
            FeatureMin = FMath::Min(FeatureMin, Metrics.MedianFeatureScale);
            FeatureMax = FMath::Max(FeatureMax, Metrics.MedianFeatureScale);
            AirSum += Metrics.AirFraction;
            WalkableSum += Metrics.WalkableFraction;
            FeatureSum += Metrics.MedianFeatureScale;
        }

        FString RangeText(float Min, float Max) const
        {
            return Count > 0
                ? FString::Printf(TEXT("%.6f..%.6f"), Min, Max)
                : TEXT("n/a");
        }

        FString SummaryText() const
        {
            return Count > 0
                ? FString::Printf(
                    TEXT("n=%d air=%s (mean %.6f), walkable=%s (mean %.6f), feature=%s (mean %.6f)"),
                    Count, *RangeText(AirMin, AirMax), (float)(AirSum / Count),
                    *RangeText(WalkableMin, WalkableMax), (float)(WalkableSum / Count),
                    *RangeText(FeatureMin, FeatureMax), (float)(FeatureSum / Count))
                : TEXT("n=0");
        }
    };

    struct FArmStats
    {
        const TCHAR* Name = TEXT("unknown");
        int32 Requested = 0;
        int32 RollFailures = 0;
        int32 BuildFailures = 0;
        int32 Unmeasured = 0;
        int32 Vacuous = 0;
        int32 Fragmented = 0;
        int32 LawFailed = 0;
        int32 LawFailedObserved = 0;
        int32 LawSkipped = 0;
        int32 Survivors = 0;
        int32 NonVacuous = 0;
        int32 LargestEnough = 0;
        int32 LawPasses = 0;
        int32 DeterminismFailures = 0;
        int32 InvalidStackCount = 0;
        int32 ChannelOrderFailures = 0;
        int32 ContextViolations = 0;
        int32 LawResults[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        FQualityStats AllQuality;
        FQualityStats SurvivorQuality;
        FBoxVerdictReport Box;
        FString FirstFailure;

        void RememberFailure(const FString& Failure)
        {
            if (FirstFailure.IsEmpty())
            {
                FirstFailure = Failure;
            }
        }

        FString SurvivalText() const
        {
            return FString::Printf(TEXT("%d/%d (%.1f%%)"), Survivors, Requested,
                                   Requested > 0 ? 100.0f * (float)Survivors / (float)Requested : 0.0f);
        }
    };

    int32 VF_LawResultIndex(EVoxelConnectivityResult Result)
    {
        switch (Result)
        {
        case EVoxelConnectivityResult::Connected:                   return 0;
        case EVoxelConnectivityResult::NotConnectedAtThisResolution: return 1;
        case EVoxelConnectivityResult::StartCellSolid:              return 2;
        case EVoxelConnectivityResult::GoalCellSolid:               return 3;
        case EVoxelConnectivityResult::OutOfWindow:                 return 4;
        case EVoxelConnectivityResult::CoarseLiedBudgetExhausted:   return 5;
        case EVoxelConnectivityResult::StartCellNotPlayerFit:       return 6;
        case EVoxelConnectivityResult::GoalCellNotPlayerFit:        return 7;
        }
        return 5;
    }

    bool VF_BitEqual(float A, float B)
    {
        uint32 ABits = 0;
        uint32 BBits = 0;
        FMemory::Memcpy(&ABits, &A, sizeof(ABits));
        FMemory::Memcpy(&BBits, &B, sizeof(BBits));
        return ABits == BBits;
    }

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeComposerCorpusFreeTest,
    "VoxelForge.Composer.CorpusFree",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeComposerCorpusFreeTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;
    (void)Parameters;
    const double TestStartSeconds = FPlatformTime::Seconds();

    const FString SettingsPath = TEXT("/Game/VoxelForge/DA_Settings.DA_Settings");
    UVoxelSettings* AuthoredSettings = LoadObject<UVoxelSettings>(nullptr, *SettingsPath);
    if (AuthoredSettings == nullptr)
    {
        AddError(FString::Printf(TEXT("Could not load %s; corpus-free measurement did not run."),
                                 *SettingsPath));
        return false;
    }

    FVoxelStrateCorpus Corpus;
    FString CorpusReport;
    const bool bCorpusLoaded = Corpus.LoadFromAssetRegistry(CorpusReport);
    AddInfo(CorpusReport);
    TestTrue(TEXT("corpus blend arm has a usable authored corpus"),
             bCorpusLoaded && Corpus.IsValid());
    if (!bCorpusLoaded || !Corpus.IsValid())
    {
        return false;
    }
    TestEqual(TEXT("corpus contains the four project vectors plus eight defaults"),
              Corpus.GetEntries().Num(), 12);

    FTestWorld World;
    World.Build(1337, 2, true);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }
    TestTrue(TEXT("corpus-free fixture keeps WorldRadiusVoxels at 0"),
             World.Settings->WorldRadiusVoxels == 0.0f);

    constexpr int32 CandidateStrateIndex = FTestWorld::SlotFlatPlain;
    int32 TopVoxelZ = 0;
    int32 BottomVoxelZ = 0;
    if (!World.GetSlotVoxelZRange(CandidateStrateIndex, TopVoxelZ, BottomVoxelZ))
    {
        AddError(TEXT("corpus-free fixture has no candidate Z range"));
        return false;
    }
    const float TopWorldZ = (float)TopVoxelZ + 1.0f;
    const float BottomWorldZ = (float)BottomVoxelZ;
    const float StrateHeightInVoxels = TopWorldZ - BottomWorldZ;

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
    TestEqual(TEXT("corpus-free candidate has exactly one arrival mouth"), ArrivalCount, 1);
    TestEqual(TEXT("corpus-free candidate has exactly one departure mouth"), DepartureCount, 1);

    FVoxelStrateMeasureSettings MeasureSettings;
    MeasureSettings.SampleStep = 4;
    MeasureSettings.RadiusInVoxels = 256;
    MeasureSettings.CenterXY = FVector2D::ZeroVector;
    MeasureSettings.MaxCells = 8000000;
    MeasureSettings.MaxRouteRetries = 16;
    MeasureSettings.HeadroomCells = 2;
    MeasureSettings.InteriorMarginVoxels = -1;

    FArmStats CorpusBlend;
    CorpusBlend.Name = TEXT("corpus blend (today)");
    FArmStats Naive;
    Naive.Name = TEXT("naive uniform");
    FArmStats Constraint;
    Constraint.Name = TEXT("constraint-sampled");

    int32 RecipeDeterminismFailures = 0;
    int32 ConstraintParameterViolations = 0;
    FString FirstConstraintParameterViolation;
    int32 ConstraintBoxStopCount = 0;
    FString FirstConstraintBoxViolation;
    bool bStopAfterConstraintViolation = false;

    const double MeasurementStartSeconds = FPlatformTime::Seconds();
    for (int32 CandidateIndex = 0;
         CandidateIndex < NumCandidates && !bStopAfterConstraintViolation;
         ++CandidateIndex)
    {
        const FVoxelOpStackRecipe Recipe = VF_RollStrateStructure(
            AuthoredSettings->Seed, CandidateIndex);
        const FVoxelOpStackRecipe RepeatRecipe = VF_RollStrateStructure(
            AuthoredSettings->Seed, CandidateIndex);
        if (!VF_AreStrateStructureRecipesIdentical(Recipe, RepeatRecipe))
        {
            ++RecipeDeterminismFailures;
        }

        FArmStats* Arms[] = { &CorpusBlend, &Naive, &Constraint };
        const EVoxelStrateCorpusFreeSamplingMode Modes[] = {
            EVoxelStrateCorpusFreeSamplingMode::NaiveUniform,
            EVoxelStrateCorpusFreeSamplingMode::ConstraintSampled,
        };

        for (int32 ArmIndex = 0; ArmIndex < 3 && !bStopAfterConstraintViolation; ++ArmIndex)
        {
            FArmStats& Arm = *Arms[ArmIndex];
            ++Arm.Requested;
            FVoxelStrateArchetypeParams CandidateParams;
            bool bParameterRollsValid = true;
            bool bConstraintParameterViolationThisCandidate = false;

            for (int32 BlockIndex = 0; BlockIndex < UE_ARRAY_COUNT(GBlockSpecs); ++BlockIndex)
            {
                const FBlockSpec& Spec = GBlockSpecs[BlockIndex];
                const int32 BlockSeed = AuthoredSettings->Seed
                    ^ static_cast<int32>(Spec.SeedSalt);
                FVoxelStrateRollInfo BlockRoll;
                if (ArmIndex == 0)
                {
                    BlockRoll = VF_RollStrateParamsDetailedForArchetype(
                        Corpus, Spec.Archetype, BlockSeed, CandidateIndex);
                    const FVoxelStrateRollInfo RepeatRoll = VF_RollStrateParamsDetailedForArchetype(
                        Corpus, Spec.Archetype, BlockSeed, CandidateIndex);
                    if (!BlockRoll.bValid || !RepeatRoll.bValid
                        || !VF_AreStrateArchetypeParamsBitIdentical(
                            BlockRoll.ArchetypeParams, RepeatRoll.ArchetypeParams, Spec.Archetype))
                    {
                        ++Arm.DeterminismFailures;
                    }
                }
                else
                {
                    BlockRoll = VF_RollStrateParamsCorpusFree(
                        Spec.Archetype, BlockSeed, CandidateIndex, Modes[ArmIndex - 1],
                        StrateHeightInVoxels);
                    const FVoxelStrateRollInfo RepeatRoll = VF_RollStrateParamsCorpusFree(
                        Spec.Archetype, BlockSeed, CandidateIndex, Modes[ArmIndex - 1],
                        StrateHeightInVoxels);
                    if (!BlockRoll.bValid || !RepeatRoll.bValid
                        || !VF_AreStrateArchetypeParamsBitIdentical(
                            BlockRoll.ArchetypeParams, RepeatRoll.ArchetypeParams, Spec.Archetype))
                    {
                        ++Arm.DeterminismFailures;
                    }
                    if (ArmIndex == 2 && BlockRoll.bValid)
                    {
                        FString Violation;
                        if (!VF_ValidateStrateCorpusFreeConstraints(
                                Spec.Archetype, BlockRoll.ArchetypeParams,
                                StrateHeightInVoxels, Violation))
                        {
                            ++ConstraintParameterViolations;
                            if (FirstConstraintParameterViolation.IsEmpty())
                            {
                                FirstConstraintParameterViolation = FString::Printf(
                                    TEXT("candidate %d block %d (%s): %s"), CandidateIndex,
                                    static_cast<int32>(Spec.Block),
                                    VF_GetStrateArchetypeName(Spec.Archetype), *Violation);
                            }
                            bParameterRollsValid = false;
                            bConstraintParameterViolationThisCandidate = true;
                        }
                    }
                }

                if (!BlockRoll.bValid)
                {
                    bParameterRollsValid = false;
                    ++Arm.RollFailures;
                    Arm.RememberFailure(FString::Printf(
                        TEXT("candidate %d block %d roll failed: %s"), CandidateIndex,
                        static_cast<int32>(Spec.Block), *BlockRoll.FailureReason));
                    continue;
                }
                VF_CopyRollBlock(CandidateParams, BlockRoll, Spec.Archetype);
            }

            if (!bParameterRollsValid)
            {
                ++Arm.Unmeasured;
                if (ArmIndex == 2 && bConstraintParameterViolationThisCandidate)
                {
                    // The constraint relation itself is a precondition of this arm; do not
                    // spend the remaining budget hiding a violated declared relation.
                    bStopAfterConstraintViolation = true;
                }
                continue;
            }
            VF_SetAllRuntimeBounds(CandidateParams, TopWorldZ, BottomWorldZ);

            FVoxelOpStack Stack;
            FVoxelOpContext Context;
            FString BuildError;
            const bool bBuilt = VF_BuildStackFromRecipe(
                Recipe, CandidateParams, AuthoredSettings->Seed, 14.0f,
                World.StrateManager.Get(), Stack, Context, &BuildError);
            if (!bBuilt)
            {
                ++Arm.BuildFailures;
                ++Arm.Unmeasured;
                Arm.RememberFailure(FString::Printf(
                    TEXT("candidate %d build failed: %s"), CandidateIndex, *BuildError));
                continue;
            }
            if (Stack.Num() != Recipe.Modifiers.Num() + 7)
            {
                ++Arm.InvalidStackCount;
            }
            FString ChannelError;
            if (!Stack.ValidateChannelOrder(&ChannelError))
            {
                ++Arm.ChannelOrderFailures;
                Arm.RememberFailure(FString::Printf(
                    TEXT("candidate %d channel order failed: %s"), CandidateIndex, *ChannelError));
            }
            if (Context.WorldRadiusVoxels != 0.0f)
            {
                ++Arm.ContextViolations;
                Arm.RememberFailure(FString::Printf(
                    TEXT("candidate %d changed WorldRadiusVoxels to %.9g"),
                    CandidateIndex, Context.WorldRadiusVoxels));
            }

            Stack.PrepareChunk(Context);
            FStackDensitySampler Sampler(Stack);
            const FVoxelStrateMetrics Metrics = VF_MeasureStrateWithSampler(
                Sampler, BottomVoxelZ, TopVoxelZ + 1,
                Context.EdgeSealThickness, MeasureSettings, nullptr);
            if (!Metrics.bValid)
            {
                ++Arm.Unmeasured;
                Arm.RememberFailure(FString::Printf(
                    TEXT("candidate %d measurement refused: %s"), CandidateIndex,
                    *Metrics.RefusalReason));
                continue;
            }
            Arm.AllQuality.Add(Metrics);

            const bool bNonVacuous = Metrics.NumSampled > 0
                && Metrics.NumAir > 0 && Metrics.NumSolid > 0;
            const bool bLargestEnough = bNonVacuous
                && Metrics.LargestComponentShare >= LargestComponentSurvivalThreshold;

            bool bLawPass = false;
            if (bLargestEnough)
            {
                FVoxelConnectivityDiagnostics Law;
                if (ArrivalCount == 1 && DepartureCount == 1)
                {
                    Law = VF_DiagnoseConnectivityWithSampler(
                        Sampler, BottomVoxelZ, TopVoxelZ + 1,
                        Context.EdgeSealThickness, ArrivalPoint, DeparturePoint, MeasureSettings);
                    ++Arm.LawResults[VF_LawResultIndex(Law.Result)];
                }
                else
                {
                    Law.bValid = false;
                    Law.Result = EVoxelConnectivityResult::OutOfWindow;
                    ++Arm.LawResults[VF_LawResultIndex(Law.Result)];
                }

                bLawPass = ArrivalCount == 1 && DepartureCount == 1
                    && Law.bValid && Law.Result == EVoxelConnectivityResult::Connected
                    && !Law.bStartSnapped && !Law.bGoalSnapped;
                if (bLawPass) { ++Arm.LawPasses; }
                if (!bLawPass) { ++Arm.LawFailedObserved; }
            }
            else
            {
                ++Arm.LawSkipped;
            }

            if (bNonVacuous) { ++Arm.NonVacuous; }
            if (bLargestEnough) { ++Arm.LargestEnough; }

            const bool bSurvivor = bNonVacuous && bLargestEnough && bLawPass;
            if (bSurvivor)
            {
                ++Arm.Survivors;
                Arm.SurvivorQuality.Add(Metrics);
            }
            else if (!bNonVacuous)
            {
                ++Arm.Vacuous;
            }
            else if (!bLargestEnough)
            {
                ++Arm.Fragmented;
            }
            else
            {
                ++Arm.LawFailed;
            }

            const FBoxVerdictReport BoxReport = VF_CheckStackBoxVerdicts(
                Stack, Context, CandidateIndex, BottomVoxelZ, TopVoxelZ);
            Arm.Box.Mixed += BoxReport.Mixed;
            Arm.Box.AllSolid += BoxReport.AllSolid;
            Arm.Box.AllAir += BoxReport.AllAir;
            Arm.Box.Proved += BoxReport.Proved;
            Arm.Box.CheckedVoxels += BoxReport.CheckedVoxels;
            Arm.Box.Violations += BoxReport.Violations;
            if (Arm.Box.FirstViolation.IsEmpty() && !BoxReport.FirstViolation.IsEmpty())
            {
                Arm.Box.FirstViolation = BoxReport.FirstViolation;
            }
            if (ArmIndex == 2 && BoxReport.Violations > 0)
            {
                ++ConstraintBoxStopCount;
                bStopAfterConstraintViolation = true;
                FirstConstraintBoxViolation = BoxReport.FirstViolation;
            }
        }
    }
    const double MeasurementSeconds = FPlatformTime::Seconds() - MeasurementStartSeconds;

    // Audit the same declared relations against the 12 current authored corpus vectors. Project
    // assets carry their own height; the eight C++ defaults use the fixture's 8-chunk height.
    int32 AuthoredConstraintViolations = 0;
    FString AuthoredViolationList;
    for (const FVoxelStrateCorpusEntry& Entry : Corpus.GetEntries())
    {
        float EntryHeight = 8.0f * (float)CHUNK_SIZE;
        if (!Entry.SourcePath.Contains(TEXT("/VoxelForge/ComposerDefaults/")))
        {
            UVoxelStrateDefinition* Definition = LoadObject<UVoxelStrateDefinition>(
                nullptr, *Entry.SourcePath);
            if (Definition != nullptr)
            {
                EntryHeight = FMath::Max(1.0f, (float)Definition->StrateHeightInChunks)
                    * (float)CHUNK_SIZE;
            }
        }

        FString Violation;
        if (!VF_ValidateStrateCorpusFreeConstraints(
                Entry.Archetype, Entry.ArchetypeParams, EntryHeight, Violation))
        {
            ++AuthoredConstraintViolations;
            if (!AuthoredViolationList.IsEmpty())
            {
                AuthoredViolationList += TEXT("; ");
            }
            AuthoredViolationList += FString::Printf(
                TEXT("%s (%s, H=%.0f): %s"), *Entry.SourceName,
                VF_GetStrateArchetypeName(Entry.Archetype), EntryHeight, *Violation);
        }
    }

    const FArmStats* Arms[] = { &CorpusBlend, &Naive, &Constraint };
    AddInfo(FString::Printf(
        TEXT("Corpus-free measurement: requested %d candidates per arm; structure recipes were "
             "shared across all three arms; sample step=%d radius=%d MaxCells=%d, fixed law mouths, "
             "criterion precedence is vacuous > fragmented > law failed. Runtime %.3fs, roll/measure "
             "%.3fs."),
        NumCandidates, MeasureSettings.SampleStep, MeasureSettings.RadiusInVoxels,
        MeasureSettings.MaxCells, FPlatformTime::Seconds() - TestStartSeconds, MeasurementSeconds));

    FString Table = TEXT(
        "approach | survival | vacuous | fragmented | law failed | air range | walkable range | feature scale range\n");
    FString SurvivorTable = TEXT(
        "survivor quality (ranges are over survivors only; main table ranges are over all measured candidates)\n"
        "approach | survivors | air range (mean) | walkable range (mean) | feature scale range (mean)\n");
    for (const FArmStats* Arm : Arms)
    {
        Table += FString::Printf(
            TEXT("%s | %s | %d | %d | %d | %s | %s | %s\n"),
            Arm->Name, *Arm->SurvivalText(), Arm->Vacuous, Arm->Fragmented, Arm->LawFailed,
            *Arm->AllQuality.RangeText(Arm->AllQuality.AirMin, Arm->AllQuality.AirMax),
            *Arm->AllQuality.RangeText(Arm->AllQuality.WalkableMin, Arm->AllQuality.WalkableMax),
            *Arm->AllQuality.RangeText(Arm->AllQuality.FeatureMin, Arm->AllQuality.FeatureMax));
        SurvivorTable += FString::Printf(
            TEXT("%s | %d | %s | %s | %s\n"),
            Arm->Name, Arm->SurvivorQuality.Count,
            *Arm->SurvivorQuality.RangeText(Arm->SurvivorQuality.AirMin, Arm->SurvivorQuality.AirMax),
            *Arm->SurvivorQuality.RangeText(Arm->SurvivorQuality.WalkableMin, Arm->SurvivorQuality.WalkableMax),
            *Arm->SurvivorQuality.RangeText(Arm->SurvivorQuality.FeatureMin, Arm->SurvivorQuality.FeatureMax));
    }
    AddInfo(Table);
    AddInfo(SurvivorTable);

    for (const FArmStats* Arm : Arms)
    {
        AddInfo(FString::Printf(
            TEXT("%s: non-vacuous=%d, largest>=%.2f=%d, law pass=%d, law failures observed=%d, "
                 "law skipped=%d, "
                 "roll failures=%d, build failures=%d, unmeasured=%d, deterministic roll failures=%d; "
                 "stack-count failures=%d, channel-order failures=%d, context violations=%d; "
                 "law results Connected=%d NotConnected=%d StartSolid=%d GoalSolid=%d OutOfWindow=%d Budget=%d "
                 "StartNotPlayerFit=%d GoalNotPlayerFit=%d; "
                 "quality all %s; survivor quality %s; first failure=%s."),
            Arm->Name, Arm->NonVacuous, LargestComponentSurvivalThreshold, Arm->LargestEnough,
            Arm->LawPasses, Arm->LawFailedObserved, Arm->LawSkipped,
            Arm->RollFailures, Arm->BuildFailures,
            Arm->Unmeasured, Arm->DeterminismFailures,
            Arm->InvalidStackCount, Arm->ChannelOrderFailures, Arm->ContextViolations,
            Arm->LawResults[0], Arm->LawResults[1], Arm->LawResults[2],
            Arm->LawResults[3], Arm->LawResults[4], Arm->LawResults[5],
            Arm->LawResults[6], Arm->LawResults[7],
            *Arm->AllQuality.SummaryText(), *Arm->SurvivorQuality.SummaryText(),
            Arm->FirstFailure.IsEmpty() ? TEXT("none") : *Arm->FirstFailure));
        AddInfo(FString::Printf(
            TEXT("%s box verdicts: mixed=%d AllSolid=%d AllAir=%d proved=%d voxels checked=%lld "
                 "violations=%d; %s"),
            Arm->Name, Arm->Box.Mixed, Arm->Box.AllSolid, Arm->Box.AllAir, Arm->Box.Proved,
            Arm->Box.CheckedVoxels, Arm->Box.Violations,
            Arm->Box.FirstViolation.IsEmpty() ? TEXT("no violations") : *Arm->Box.FirstViolation));
    }

    AddInfo(FString::Printf(
        TEXT("Authored-corpus constraint audit: %d/%d vectors violated at their declared strate "
             "height. %s"),
        AuthoredConstraintViolations, Corpus.GetEntries().Num(),
        AuthoredViolationList.IsEmpty() ? TEXT("none") : *AuthoredViolationList));
    AddInfo(FString::Printf(
        TEXT("Shared structure determinism failures=%d; per-arm channel/context violations are "
             "reported below; constraint parameter violations=%d; constraint box-stop count=%d. %s%s"),
        RecipeDeterminismFailures,
        ConstraintParameterViolations, ConstraintBoxStopCount,
        FirstConstraintParameterViolation.IsEmpty() ? TEXT("") : *FirstConstraintParameterViolation,
        FirstConstraintBoxViolation.IsEmpty() ? TEXT("")
            : *FString::Printf(TEXT("; first box violation: %s"), *FirstConstraintBoxViolation)));

    TestEqual(TEXT("configured candidate count requested for corpus blend"), CorpusBlend.Requested, NumCandidates);
    TestEqual(TEXT("configured candidate count requested for naive uniform"), Naive.Requested, NumCandidates);
    TestEqual(TEXT("configured candidate count requested for constraint-sampled"), Constraint.Requested, NumCandidates);
    TestEqual(TEXT("corpus blend roll failures"), CorpusBlend.RollFailures, 0);
    TestEqual(TEXT("naive uniform roll failures"), Naive.RollFailures, 0);
    TestEqual(TEXT("constraint-sampled roll failures"), Constraint.RollFailures, 0);
    TestEqual(TEXT("corpus blend build failures"), CorpusBlend.BuildFailures, 0);
    TestEqual(TEXT("naive uniform build failures"), Naive.BuildFailures, 0);
    TestEqual(TEXT("constraint-sampled build failures"), Constraint.BuildFailures, 0);
    TestEqual(TEXT("corpus-free parameter determinism failures"),
              Naive.DeterminismFailures + Constraint.DeterminismFailures, 0);
    TestEqual(TEXT("corpus parameter determinism failures"), CorpusBlend.DeterminismFailures, 0);
    TestEqual(TEXT("shared structure recipe determinism failures"), RecipeDeterminismFailures, 0);
    TestEqual(TEXT("constraint-sampled declared-parameter violations"),
              ConstraintParameterViolations, 0);
    TestEqual(TEXT("constraint-sampled box verdict violations"),
              Constraint.Box.Violations, 0);
    TestEqual(TEXT("corpus blend box verdict violations"), CorpusBlend.Box.Violations, 0);
    TestEqual(TEXT("naive uniform box verdict violations"), Naive.Box.Violations, 0);
    TestEqual(TEXT("corpus blend WorldRadiusVoxels/context violations"),
              CorpusBlend.InvalidStackCount + CorpusBlend.ChannelOrderFailures
                  + CorpusBlend.ContextViolations, 0);
    TestEqual(TEXT("naive uniform WorldRadiusVoxels/context violations"),
              Naive.InvalidStackCount + Naive.ChannelOrderFailures + Naive.ContextViolations, 0);
    TestEqual(TEXT("constraint-sampled WorldRadiusVoxels/context violations"),
              Constraint.InvalidStackCount + Constraint.ChannelOrderFailures
                  + Constraint.ContextViolations, 0);
    TestTrue(TEXT("constraint sampler did not stop early on a declared relation or box violation"),
             !bStopAfterConstraintViolation);

    const double TestSeconds = FPlatformTime::Seconds() - TestStartSeconds;
    AddInfo(FString::Printf(TEXT("CorpusFree total runtime %.3fs; all three arms requested %d each; "
                                 "WorldRadiusVoxels=0."), TestSeconds, NumCandidates));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
