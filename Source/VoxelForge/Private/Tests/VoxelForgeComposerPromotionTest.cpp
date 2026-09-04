// Tier 5 — promotion, corpus persistence, re-verification, and compounding simulation.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Misc/Paths.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"
#include "VoxelSettings.h"
#include "VoxelStrateComposer.h"
#include "VoxelStrateMeasure.h"

namespace
{
    constexpr int32 NumSimulatedSeasons = 5;
    constexpr int32 CandidatesPerSeason = 24;
    constexpr int32 PromotionCapPerSeason = 6;
    constexpr float LargestComponentSurvivalThreshold = 0.50f;
    constexpr int32 SimulationSampleStep = 8;
    constexpr int32 SimulationRadius = 256;

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
        }
        return TEXT("Unknown");
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

    float VF_GetBoundarySeal(const FVoxelStrateArchetypeParams& Params,
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

    ECaveGeneratorType VF_ArchetypeForRecipe(const FVoxelOpStackRecipe& Recipe)
    {
        switch (Recipe.StructuralParamBlock)
        {
        case EVoxelStrateParamBlock::TunnelNetwork: return ECaveGeneratorType::TunnelNetwork;
        case EVoxelStrateParamBlock::Slab:           return ECaveGeneratorType::FlatPlain;
        case EVoxelStrateParamBlock::Maze:           return ECaveGeneratorType::Maze;
        case EVoxelStrateParamBlock::Surface:        return ECaveGeneratorType::SurfaceWorld;
        case EVoxelStrateParamBlock::VerticalShaft:  return ECaveGeneratorType::VerticalShafts;
        case EVoxelStrateParamBlock::FloatingIsland: return ECaveGeneratorType::FloatingIslands;
        default:                                     return ECaveGeneratorType::TunnelNetwork;
        }
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

    class FPromotionVerifier final : public IVoxelStratePromotionVerifier
    {
    public:
        FPromotionVerifier(const VoxelForgeTest::FTestWorld& InWorld,
                           int32 InTopVoxelZ,
                           int32 InBottomVoxelZ,
                           const FVector& InArrivalPoint,
                           const FVector& InDeparturePoint,
                           const FVoxelStrateMeasureSettings& InMeasureSettings,
                           float InLargestThreshold)
            : World(InWorld)
            , TopVoxelZ(InTopVoxelZ)
            , BottomVoxelZ(InBottomVoxelZ)
            , ArrivalPoint(InArrivalPoint)
            , DeparturePoint(InDeparturePoint)
            , MeasureSettings(InMeasureSettings)
            , LargestThreshold(InLargestThreshold)
        {
        }

        bool Verify(const FVoxelStratePromotableRecord& Record,
                    FVoxelStratePromotionVerification& OutVerification) const override
        {
            ++NumVerifications;
            OutVerification = FVoxelStratePromotionVerification();

            if (Record.Archetype != VF_ArchetypeForRecipe(Record.Recipe))
            {
                OutVerification.FailureReason = TEXT("record archetype does not match recipe post block");
                return false;
            }

            FVoxelStrateArchetypeParams Params = Record.Params;
            VF_SetAllRuntimeBounds(Params, static_cast<float>(TopVoxelZ) + 1.0f,
                                   static_cast<float>(BottomVoxelZ));

            FVoxelOpStack Stack;
            FVoxelOpContext Context;
            FString BuildError;
            if (!VF_BuildStackFromRecipe(
                    Record.Recipe, Params, Record.Seed, 14.0f,
                    World.StrateManager.Get(), Stack, Context, &BuildError))
            {
                OutVerification.FailureReason = FString::Printf(
                    TEXT("recipe rebuild failed: %s"), *BuildError);
                return false;
            }
            if (Context.WorldRadiusVoxels != 0.0f)
            {
                OutVerification.FailureReason = TEXT("re-verified recipe changed WorldRadiusVoxels");
                return false;
            }

            Stack.PrepareChunk(Context);
            FStackDensitySampler Sampler(Stack);
            const FVoxelStrateMetrics Metrics = VF_MeasureStrateWithSampler(
                Sampler, BottomVoxelZ, TopVoxelZ + 1, Context.EdgeSealThickness,
                MeasureSettings, nullptr);
            OutVerification.MeasuredMetrics = VF_SummarizeStrateMetrics(Metrics);
            OutVerification.bPassedNonVacuous = Metrics.bValid
                && Metrics.NumSampled > 0 && Metrics.NumAir > 0 && Metrics.NumSolid > 0;
            OutVerification.bPassedLargestComponent = OutVerification.bPassedNonVacuous
                && Metrics.LargestComponentShare >= LargestThreshold;

            const FVoxelConnectivityDiagnostics Law = VF_DiagnoseConnectivityWithSampler(
                Sampler, BottomVoxelZ, TopVoxelZ + 1, Context.EdgeSealThickness,
                ArrivalPoint, DeparturePoint, MeasureSettings);
            OutVerification.bPassedPrimordialLaw = Law.bValid
                && Law.Result == EVoxelConnectivityResult::Connected
                && !Law.bStartSnapped && !Law.bGoalSnapped;

            if (!OutVerification.PassedAllGates())
            {
                OutVerification.FailureReason = FString::Printf(
                    TEXT("fresh gates failed: non-vacuous=%s largest=%s law=%s"),
                    OutVerification.bPassedNonVacuous ? TEXT("yes") : TEXT("no"),
                    OutVerification.bPassedLargestComponent ? TEXT("yes") : TEXT("no"),
                    VF_ConnectivityName(Law.Result));
            }
            return true;
        }

        bool MeasureCorpusEntry(const FVoxelStrateCorpusEntry& Entry,
                                FVoxelStrateMeasuredMetrics& OutMetrics) const override;

        mutable int32 NumVerifications = 0;

    private:
        const VoxelForgeTest::FTestWorld& World;
        int32 TopVoxelZ = 0;
        int32 BottomVoxelZ = 0;
        FVector ArrivalPoint = FVector::ZeroVector;
        FVector DeparturePoint = FVector::ZeroVector;
        FVoxelStrateMeasureSettings MeasureSettings;
        float LargestThreshold = 0.50f;
    };

    bool VF_MeasureNativeCorpusEntry(
        const VoxelForgeTest::FTestWorld& World,
        const FVoxelStrateCorpusEntry& Entry,
        int32 TopVoxelZ,
        int32 BottomVoxelZ,
        const FVoxelStrateMeasureSettings& Settings,
        FVoxelStrateMeasuredMetrics& OutMetrics)
    {
        FVoxelStrateArchetypeParams Params = Entry.ArchetypeParams;
        VF_SetAllRuntimeBounds(Params, static_cast<float>(TopVoxelZ) + 1.0f,
                               static_cast<float>(BottomVoxelZ));

        FVoxelOpStack Stack;
        FVoxelOpContext Context;
        if (!VF_BuildNativeStrateStackForCandidate(
                Entry.Archetype, Params, 1337, 14.0f, 0.0f,
                VF_GetBoundarySeal(Params, Entry.Archetype),
                World.StrateManager.Get(), Stack, Context)
            || Context.WorldRadiusVoxels != 0.0f)
        {
            return false;
        }

        Stack.PrepareChunk(Context);
        FStackDensitySampler Sampler(Stack);
        const FVoxelStrateMetrics Metrics = VF_MeasureStrateWithSampler(
            Sampler, BottomVoxelZ, TopVoxelZ + 1, Context.EdgeSealThickness,
            Settings, nullptr);
        OutMetrics = VF_SummarizeStrateMetrics(Metrics);
        return Metrics.bValid;
    }

    bool FPromotionVerifier::MeasureCorpusEntry(
        const FVoxelStrateCorpusEntry& Entry,
        FVoxelStrateMeasuredMetrics& OutMetrics) const
    {
        return VF_MeasureNativeCorpusEntry(
            World, Entry, TopVoxelZ, BottomVoxelZ, MeasureSettings, OutMetrics);
    }

    FVoxelStratePromotableRecord VF_EvaluateCandidate(
        const VoxelForgeTest::FTestWorld& World,
        const FVoxelStrateComposerCandidate& Candidate,
        int32 Season,
        uint32 CorpusHash,
        int32 TopVoxelZ,
        int32 BottomVoxelZ,
        const FVector& ArrivalPoint,
        const FVector& DeparturePoint,
        const FVoxelStrateMeasureSettings& Settings,
        float LargestThreshold,
        bool& bOutSurvivor)
    {
        bOutSurvivor = false;
        FVoxelStratePromotableRecord Record = VF_MakeStratePromotableRecord(
            Candidate, FVoxelStrateMetrics(), Season, CorpusHash, false, false, false);
        if (!Candidate.bValid)
        {
            return Record;
        }

        FVoxelStrateArchetypeParams Params = Candidate.ArchetypeParams;
        VF_SetAllRuntimeBounds(Params, static_cast<float>(TopVoxelZ) + 1.0f,
                               static_cast<float>(BottomVoxelZ));
        Record.Params = Params;

        FVoxelOpStack Stack;
        FVoxelOpContext Context;
        FString BuildError;
        if (!VF_BuildStackFromRecipe(
                Candidate.Recipe, Params, Candidate.Seed, 14.0f,
                World.StrateManager.Get(), Stack, Context, &BuildError)
            || Context.WorldRadiusVoxels != 0.0f)
        {
            return Record;
        }
        Stack.PrepareChunk(Context);
        FStackDensitySampler Sampler(Stack);
        const FVoxelStrateMetrics Metrics = VF_MeasureStrateWithSampler(
            Sampler, BottomVoxelZ, TopVoxelZ + 1, Context.EdgeSealThickness,
            Settings, nullptr);
        const bool bNonVacuous = Metrics.bValid
            && Metrics.NumSampled > 0 && Metrics.NumAir > 0 && Metrics.NumSolid > 0;
        const bool bLargestEnough = bNonVacuous
            && Metrics.LargestComponentShare >= LargestThreshold;
        const FVoxelConnectivityDiagnostics Law = VF_DiagnoseConnectivityWithSampler(
            Sampler, BottomVoxelZ, TopVoxelZ + 1, Context.EdgeSealThickness,
            ArrivalPoint, DeparturePoint, Settings);
        const bool bLawPass = Law.bValid
            && Law.Result == EVoxelConnectivityResult::Connected
            && !Law.bStartSnapped && !Law.bGoalSnapped;

        Record = VF_MakeStratePromotableRecord(
            Candidate, Metrics, Season, CorpusHash,
            bNonVacuous, bLargestEnough, bLawPass);
        Record.Params = Params;
        bOutSurvivor = bNonVacuous && bLargestEnough && bLawPass;
        return Record;
    }

    double VF_MeanMeasuredSpread(const FVoxelStrateCorpus& Corpus,
                                 const TArray<FVoxelStratePromotableRecord>& NewRecords,
                                 const FVoxelStratePromotionPolicy& Policy)
    {
        TArray<FVoxelStrateMeasuredMetrics> Metrics;
        for (const FVoxelStrateCorpusEntry& Entry : Corpus.GetEntries())
        {
            if (Entry.bHasMeasuredMetrics && Entry.MeasuredMetrics.IsUsable())
            {
                Metrics.Add(Entry.MeasuredMetrics);
            }
        }
        for (const FVoxelStratePromotableRecord& Record : NewRecords)
        {
            if (Record.MeasuredMetrics.IsUsable())
            {
                Metrics.Add(Record.MeasuredMetrics);
            }
        }
        if (Metrics.Num() < 2)
        {
            return 0.0;
        }

        double Sum = 0.0;
        int64 PairCount = 0;
        for (int32 A = 0; A < Metrics.Num(); ++A)
        {
            for (int32 B = A + 1; B < Metrics.Num(); ++B)
            {
                const double Distance = VF_NormalizedMeasuredMetricDistance(
                    Metrics[A], Metrics[B], Policy);
                if (FMath::IsFinite(Distance) && Distance < TNumericLimits<double>::Max())
                {
                    Sum += Distance;
                    ++PairCount;
                }
            }
        }
        return PairCount > 0 ? Sum / static_cast<double>(PairCount) : 0.0;
    }

    bool VF_PromotionBatchesIdentical(
        const FVoxelStratePromotionBatchResult& A,
        const FVoxelStratePromotionBatchResult& B)
    {
        if (A.Promoted.Num() != B.Promoted.Num())
        {
            return false;
        }
        for (int32 Index = 0; Index < A.Promoted.Num(); ++Index)
        {
            const FVoxelStratePromotableRecord& Left = A.Promoted[Index];
            const FVoxelStratePromotableRecord& Right = B.Promoted[Index];
            if (Left.RecordId != Right.RecordId
                || Left.Archetype != Right.Archetype
                || !VF_AreStrateStructureRecipesIdentical(Left.Recipe, Right.Recipe)
                || !VF_AreStrateArchetypeParamsBitIdentical(
                    Left.Params, Right.Params, Left.Archetype)
                || Left.MeasuredMetrics.AirFraction != Right.MeasuredMetrics.AirFraction
                || Left.MeasuredMetrics.LargestComponentShare
                    != Right.MeasuredMetrics.LargestComponentShare)
            {
                return false;
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeComposerPromotionTest,
    "VoxelForge.Composer.Promotion",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeComposerPromotionTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;
    (void)Parameters;
    const double TestStartSeconds = FPlatformTime::Seconds();

    UVoxelSettings* AuthoredSettings = LoadObject<UVoxelSettings>(
        nullptr, TEXT("/Game/VoxelForge/DA_Settings.DA_Settings"));
    if (AuthoredSettings == nullptr)
    {
        AddError(TEXT("Could not load authored settings; promotion simulation did not run."));
        return false;
    }

    FTestWorld World;
    World.Build(1337, 2, true);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }
    TestTrue(TEXT("promotion fixture keeps WorldRadiusVoxels at 0"),
             World.Settings->WorldRadiusVoxels == 0.0f);

    constexpr int32 CandidateStrateIndex = FTestWorld::SlotFlatPlain;
    int32 TopVoxelZ = 0;
    int32 BottomVoxelZ = 0;
    if (!World.GetSlotVoxelZRange(CandidateStrateIndex, TopVoxelZ, BottomVoxelZ))
    {
        AddError(TEXT("promotion fixture has no candidate Z range"));
        return false;
    }

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
    TestEqual(TEXT("promotion fixture has exactly one arrival mouth"), ArrivalCount, 1);
    TestEqual(TEXT("promotion fixture has exactly one departure mouth"), DepartureCount, 1);
    if (ArrivalCount != 1 || DepartureCount != 1)
    {
        return false;
    }

    FVoxelStrateMeasureSettings MeasureSettings;
    MeasureSettings.SampleStep = SimulationSampleStep;
    MeasureSettings.RadiusInVoxels = SimulationRadius;
    MeasureSettings.CenterXY = FVector2D::ZeroVector;
    MeasureSettings.MaxCells = 8000000;
    MeasureSettings.MaxRouteRetries = 16;
    MeasureSettings.HeadroomCells = 2;
    // Use one fixed window for both authored/default members and candidates. Deriving a margin
    // from each member's boundary seal can make a small authored strate have no measurable
    // interior, which would make the duplicate metric space member-dependent.
    MeasureSettings.InteriorMarginVoxels = 0;

    FPromotionVerifier Verifier(
        World, TopVoxelZ, BottomVoxelZ, ArrivalPoint, DeparturePoint,
        MeasureSettings, LargestComponentSurvivalThreshold);

    // The fixed path is keyed by the authored corpus hash and simulation constants. Starting with
    // an empty JSON object makes reruns independent of an older test artifact without deleting it.
    const FString SimulationDirectory = FPaths::ProjectSavedDir() / TEXT("ComposerPromotion")
        / FString::Printf(TEXT("seed_%d_step_%d_radius_%d"),
                          AuthoredSettings->Seed, SimulationSampleStep, SimulationRadius);
    const FString PromotedStorePath = SimulationDirectory / TEXT("promoted_strates.json");
    const FString CorruptStorePath = SimulationDirectory / TEXT("corrupt_promoted_strates.json");
    FString StoreReport;
    TestTrue(TEXT("promotion simulation can initialise its empty JSON store"),
             VF_SaveStratePromotedRecords(
                 PromotedStorePath, TArray<FVoxelStratePromotableRecord>(), StoreReport));
    AddInfo(StoreReport);

    FVoxelStrateCorpus BaseCorpus;
    FString BaseReport;
    TestTrue(TEXT("promotion simulation loads its base project/default corpus"),
             BaseCorpus.LoadFromAssetRegistry(BaseReport, PromotedStorePath, &Verifier));
    AddInfo(BaseReport);
    if (!BaseCorpus.IsValid())
    {
        AddError(TEXT("base corpus was invalid; promotion simulation did not run."));
        return false;
    }

    TMap<FString, FVoxelStrateMeasuredMetrics> BaseMetrics;
    int32 NumVacuousBaseMetrics = 0;
    for (const FVoxelStrateCorpusEntry& Entry : BaseCorpus.GetEntries())
    {
        if (Entry.Provenance == EVoxelStrateCorpusProvenance::Promoted)
        {
            continue;
        }
        TestTrue(FString::Printf(TEXT("base member %s is measured before policy selection"),
                                 *Entry.SourcePath),
                 Entry.bHasMeasuredMetrics && Entry.MeasuredMetrics.bValid);
        if (Entry.bHasMeasuredMetrics && !Entry.MeasuredMetrics.IsUsable())
        {
            ++NumVacuousBaseMetrics;
            AddInfo(FString::Printf(
                TEXT("base member %s measured but vacuous in the fixed promotion window: "
                     "air=%lld solid=%lld components=%d"),
                *Entry.SourcePath, Entry.MeasuredMetrics.NumAir,
                Entry.MeasuredMetrics.NumSolid, Entry.MeasuredMetrics.NumAirComponents));
        }
        if (Entry.bHasMeasuredMetrics && Entry.MeasuredMetrics.IsUsable())
        {
            BaseMetrics.Add(Entry.SourcePath, Entry.MeasuredMetrics);
        }
    }
    TestTrue(TEXT("base project/default members have measured metrics for spread"),
             BaseMetrics.Num() > 0);
    AddInfo(FString::Printf(
        TEXT("Base metric audit for duplicate policy: %d usable members, %d measured-but-vacuous "
             "members; vacuous members use exact parameter identity as the conservative fallback."),
        BaseMetrics.Num(), NumVacuousBaseMetrics));

    FVoxelStratePromotionPolicy Policy;
    Policy.MinimumNormalizedMeasuredMetricDistance = 0.20;
    Policy.MaxPromotionsPerSeason = PromotionCapPerSeason;
    Policy.FeatureScaleNormalizationVoxels = 256.0f;
    Policy.ClearanceNormalizationVoxels = 64.0f;

    TArray<FVoxelStratePromotableRecord> AllPromoted;
    FString SeasonTable = TEXT(
        "season | input corpus | corpus after | project/default/promoted | survivors/candidates "
        "| survival | measured spread | promoted | near-dup/hash/cap\n");
    double FirstSpread = -1.0;
    double LastSpread = -1.0;
    int32 NumSeasonLoadsWithPromotions = 0;

    for (int32 Season = 1; Season <= NumSimulatedSeasons; ++Season)
    {
        FVoxelStrateCorpus Corpus;
        FString CorpusReport;
        const bool bLoaded = Corpus.LoadFromAssetRegistry(
            CorpusReport, PromotedStorePath, &Verifier);
        AddInfo(FString::Printf(TEXT("Season %d corpus load: %s"), Season, *CorpusReport));
        TestTrue(FString::Printf(TEXT("season %d reloads the cumulative corpus safely"), Season),
                 bLoaded && Corpus.IsValid());
        if (!bLoaded || !Corpus.IsValid())
        {
            return false;
        }

        for (const TPair<FString, FVoxelStrateMeasuredMetrics>& Pair : BaseMetrics)
        {
            TestTrue(FString::Printf(TEXT("season %d restores base metric %s"),
                                     Season, *Pair.Key),
                     Corpus.SetMeasuredMetrics(Pair.Key, Pair.Value));
        }

        if (AllPromoted.Num() > 0)
        {
            ++NumSeasonLoadsWithPromotions;
            TestTrue(FString::Printf(TEXT("season %d re-verifies every stored promotion"), Season),
                     Verifier.NumVerifications >= AllPromoted.Num());
        }

        const uint32 InputCorpusHash = Corpus.GetContentsHash();
        const uint32 SeasonSeedBits = static_cast<uint32>(AuthoredSettings->Seed)
            ^ (static_cast<uint32>(Season) * 0x9E3779B9u);
        const int32 SeasonSeed = static_cast<int32>(SeasonSeedBits);
        TArray<FVoxelStratePromotableRecord> CandidateRecords;
        CandidateRecords.Reserve(CandidatesPerSeason);
        int32 NumSurvivors = 0;
        for (int32 CandidateIndex = 0;
             CandidateIndex < CandidatesPerSeason;
             ++CandidateIndex)
        {
            const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
                Corpus, SeasonSeed, CandidateIndex, true);
            bool bSurvivor = false;
            CandidateRecords.Add(VF_EvaluateCandidate(
                World, Candidate, Season, InputCorpusHash, TopVoxelZ, BottomVoxelZ,
                ArrivalPoint, DeparturePoint, MeasureSettings,
                LargestComponentSurvivalThreshold, bSurvivor));
            if (bSurvivor)
            {
                ++NumSurvivors;
            }
        }

        const FVoxelStratePromotionBatchResult Batch = VF_SelectStratePromotions(
            Corpus, CandidateRecords, Policy);
        const FVoxelStratePromotionBatchResult RepeatBatch = VF_SelectStratePromotions(
            Corpus, CandidateRecords, Policy);
        TestTrue(FString::Printf(TEXT("season %d promotion selection is deterministic"), Season),
                 VF_PromotionBatchesIdentical(Batch, RepeatBatch));
        TestTrue(FString::Printf(TEXT("season %d uses a valid explicit promotion policy"), Season),
                 Batch.bPolicyValid);
        TestTrue(FString::Printf(TEXT("season %d respects its promotion cap"), Season),
                 Batch.Promoted.Num() <= PromotionCapPerSeason);

        const double Spread = VF_MeanMeasuredSpread(Corpus, Batch.Promoted, Policy);
        if (FirstSpread < 0.0) { FirstSpread = Spread; }
        LastSpread = Spread;

        const int32 ProjectCount = Corpus.NumForProvenance(EVoxelStrateCorpusProvenance::Project);
        const int32 DefaultCount = Corpus.NumForProvenance(EVoxelStrateCorpusProvenance::Default);
        const int32 ExistingPromotedCount = Corpus.NumForProvenance(
            EVoxelStrateCorpusProvenance::Promoted);
        const int32 CorpusAfter = Corpus.Num() + Batch.Promoted.Num();
        const int32 PromotedAfter = ExistingPromotedCount + Batch.Promoted.Num();
        const float SurvivalRate = CandidateRecords.Num() > 0
            ? static_cast<float>(NumSurvivors) / static_cast<float>(CandidateRecords.Num())
            : 0.0f;
        SeasonTable += FString::Printf(
            TEXT("%d | %08x | %d | %d/%d/%d | %d/%d | %.1f%% | %.6f | %d | %d/%d/%d\n"),
            Season, InputCorpusHash, CorpusAfter, ProjectCount, DefaultCount, PromotedAfter,
            NumSurvivors, CandidateRecords.Num(), 100.0f * SurvivalRate, Spread,
            Batch.Promoted.Num(), Batch.NumNearDuplicateRejected,
            Batch.NumCorpusHashRejected, Batch.NumCapRejected);

        FVoxelStrateSeasonManifest Manifest;
        Manifest.Season = Season;
        Manifest.Seed = SeasonSeed;
        Manifest.InputCorpusHash = InputCorpusHash;
        Manifest.CandidateCount = CandidateRecords.Num();
        Manifest.SurvivorCount = NumSurvivors;
        Manifest.PromotedCount = Batch.Promoted.Num();
        Manifest.CorpusSize = CorpusAfter;
        Manifest.ProjectCount = ProjectCount;
        Manifest.DefaultCount = DefaultCount;
        Manifest.PromotedCorpusCount = PromotedAfter;
        Manifest.SurvivalRate = SurvivalRate;
        Manifest.MeasuredSpread = Spread;
        FString ManifestReport;
        const FString ManifestPath = SimulationDirectory
            / FString::Printf(TEXT("season_%02d_manifest.json"), Season);
        TestTrue(FString::Printf(TEXT("season %d manifest is written beside the store"), Season),
                 VF_SaveStrateSeasonManifest(ManifestPath, Manifest, ManifestReport));
        AddInfo(ManifestReport);

        AllPromoted.Append(Batch.Promoted);
        TestTrue(FString::Printf(TEXT("season %d cumulative promoted store is written"), Season),
                 VF_SaveStratePromotedRecords(PromotedStorePath, AllPromoted, StoreReport));
    }

    AddInfo(SeasonTable);
    AddInfo(FString::Printf(
        TEXT("Promotion policy: normalized measured-metric distance < %.2f rejects a near duplicate; "
             "dimensions=(air, largest share, walkable, feature/%.0f voxels, clearance/%.0f voxels); "
             "cap=%d per season; records=%d; reloads with promotions=%d."),
        Policy.MinimumNormalizedMeasuredMetricDistance,
        Policy.FeatureScaleNormalizationVoxels, Policy.ClearanceNormalizationVoxels,
        Policy.MaxPromotionsPerSeason, AllPromoted.Num(), NumSeasonLoadsWithPromotions));
    AddInfo(FString::Printf(
        TEXT("Measured spread is mean pairwise distance in that normalized five-dimensional space: "
             "season 1=%.6f, season 5=%.6f, delta=%+.6f."),
        FirstSpread, LastSpread, LastSpread - FirstSpread));

    TestEqual(TEXT("five simulated seasons completed"), NumSeasonLoadsWithPromotions + 1,
              NumSimulatedSeasons);
    TestTrue(TEXT("promotion compounding produced at least one promoted member"),
             AllPromoted.Num() > 0);
    TestEqual(TEXT("promotion cap was never exceeded in cumulative output"),
              FMath::Min(AllPromoted.Num(), NumSimulatedSeasons * PromotionCapPerSeason),
              AllPromoted.Num());
    TestTrue(TEXT("promotion corpus contains project members"),
             BaseCorpus.NumForProvenance(EVoxelStrateCorpusProvenance::Project) > 0);
    TestTrue(TEXT("promotion corpus contains default members"),
             BaseCorpus.NumForProvenance(EVoxelStrateCorpusProvenance::Default) > 0);

    // Deliberately corrupt only the stored metric. The loader must still admit the record only
    // after rebuilding it, and the corpus entry must expose the fresh value rather than the JSON.
    if (AllPromoted.Num() > 0)
    {
        FVoxelStratePromotableRecord CorruptRecord = AllPromoted[0];
        const float OriginalAir = CorruptRecord.MeasuredMetrics.AirFraction;
        CorruptRecord.MeasuredMetrics.AirFraction = OriginalAir > 0.5f ? 0.01f : 0.99f;
        TArray<FVoxelStratePromotableRecord> CorruptRecords;
        CorruptRecords.Add(CorruptRecord);
        FString CorruptReport;
        TestTrue(TEXT("corrupt-metric promotion store can be written for the safety test"),
                 VF_SaveStratePromotedRecords(CorruptStorePath, CorruptRecords, CorruptReport));

        FVoxelStrateCorpus CorruptCorpus;
        FString CorruptLoadReport;
        TestTrue(TEXT("corrupt stored metric is reverified before corpus admission"),
                 CorruptCorpus.LoadFromAssetRegistry(
                     CorruptLoadReport, CorruptStorePath, &Verifier));
        AddInfo(CorruptLoadReport);
        TestTrue(TEXT("corrupt stored metric was detected and replaced"),
                 CorruptLoadReport.Contains(TEXT("stored metrics replaced for 1 records")));

        FVoxelStratePromotionVerification ExpectedVerification;
        TestTrue(TEXT("safety verifier can independently rebuild the corrupted record"),
                 Verifier.Verify(CorruptRecord, ExpectedVerification));
        bool bFoundReverifiedEntry = false;
        for (const FVoxelStrateCorpusEntry& Entry : CorruptCorpus.GetEntries())
        {
            if (Entry.Provenance == EVoxelStrateCorpusProvenance::Promoted
                && Entry.PromotionRecordId == CorruptRecord.RecordId)
            {
                bFoundReverifiedEntry = true;
                AddInfo(FString::Printf(
                    TEXT("stale metric audit %s: stored/corpus air=%.9g, fresh air=%.9g, "
                         "corrupted JSON air=%.9g"),
                    *CorruptRecord.RecordId,
                    Entry.MeasuredMetrics.AirFraction,
                    ExpectedVerification.MeasuredMetrics.AirFraction,
                    CorruptRecord.MeasuredMetrics.AirFraction));
                TestTrue(TEXT("loaded promoted metric equals fresh measurement, not stale JSON"),
                         FMath::IsNearlyEqual(
                             Entry.MeasuredMetrics.AirFraction,
                             ExpectedVerification.MeasuredMetrics.AirFraction));
            }
        }
        TestTrue(TEXT("corrupt safety test found its promoted corpus entry"),
                 bFoundReverifiedEntry);
    }

    const double TestSeconds = FPlatformTime::Seconds() - TestStartSeconds;
    AddInfo(FString::Printf(
        TEXT("Promotion total runtime %.3fs; WorldRadiusVoxels=0; JSON store=%s."),
        TestSeconds, *PromotedStorePath));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
