// Morning review: one hard-gated, walkable candidate per archetype plus an offline page.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelCaveMorphology.h"
#include "VoxelDensityOpStack.h"
#include "VoxelSettings.h"
#include "VoxelStrateComposer.h"
#include "VoxelStrateMeasure.h"
#include "VoxelStratePreview.h"
#include "VoxelTypes.h"

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
    constexpr int32 FinePreviewMaxCells = 4000000;

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
        case EVoxelConnectivityResult::StartCellNotPlayerFit:       return TEXT("StartCellNotPlayerFit");
        case EVoxelConnectivityResult::GoalCellNotPlayerFit:        return TEXT("GoalCellNotPlayerFit");
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
        FVoxelStrateMetrics FineMetrics;
        FVoxelConnectivityDiagnostics LegacyLaw;
        FVoxelConnectivityDiagnostics PlayerFitLaw;
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
        // Best coarse-measured candidate, retained even when a hard gate rejects it. This keeps
        // the scale audit useful when the player-fit law correctly finds no survivor.
        TOptional<FShowcaseSample> BestMeasured;
        int32 MeasuredCandidates = 0;
        FString LastFailure;
    };

    bool VF_IsBetterShowcaseSample(const FShowcaseSample& Sample,
                                   const TOptional<FShowcaseSample>& Current)
    {
        return !Current.IsSet()
            || Sample.SelectionScore > Current->SelectionScore
            || (Sample.SelectionScore == Current->SelectionScore
                && (Sample.Candidate.Seed < Current->Candidate.Seed
                    || (Sample.Candidate.Seed == Current->Candidate.Seed
                        && Sample.Candidate.Index < Current->Candidate.Index)));
    }

    void VF_AddMetricSample(FShowcaseStats& Stats, const FShowcaseSample& Sample)
    {
        Stats.Survivors.Add(Sample);
        if (VF_IsBetterShowcaseSample(Sample, Stats.Best))
        {
            Stats.Best = Sample;
        }
    }

    void VF_AddMeasuredSample(FShowcaseStats& Stats, const FShowcaseSample& Sample)
    {
        // NumSampled is the direct evidence that the coarse grid was built. Keep this diagnostic
        // path independent of the legacy gate's bool return and of any future metric-validity
        // policy changes: a rejected candidate can still be the most useful scale evidence.
        if (Sample.Candidate.bValid && Sample.Metrics.NumSampled > 0)
        {
            ++Stats.MeasuredCandidates;
            if (VF_IsBetterShowcaseSample(Sample, Stats.BestMeasured))
            {
                Stats.BestMeasured = Sample;
            }
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

    FVoxelStrateMeasureSettings VF_MakeFinePlayerFitSettings(
        const FVoxelStrateMeasureSettings& BaseSettings,
        const FVoxelStrateFinePreviewSettings& FinePreviewSettings,
        const FVector& ArrivalPoint,
        const FVector& DeparturePoint)
    {
        FVoxelStrateMeasureSettings Settings =
            FinePreviewSettings.MakeMeasureSettings(BaseSettings);
        // The old fine preview was centred on the coarse largest-component representative. That
        // is useful for a picture, but it can put both mouths outside the ROI and turn a real
        // player-fit route into a meaningless OutOfWindow result. Fit the fine ROI to the two
        // existing mouth XY points instead; the small explicit margin leaves room for the
        // capsule's horizontal stencil at either endpoint without changing placement.
        Settings.CenterXY = FVector2D(
            0.5f * (ArrivalPoint.X + DeparturePoint.X),
            0.5f * (ArrivalPoint.Y + DeparturePoint.Y));
        Settings.CoverPointA = FVector2D(ArrivalPoint.X, ArrivalPoint.Y);
        Settings.CoverPointB = FVector2D(DeparturePoint.X, DeparturePoint.Y);
        Settings.CoverMarginVoxels = FMath::CeilToFloat(
            Settings.PlayerCapsuleRadiusVoxels) + 2.0f;
        return Settings;
    }

    bool VF_EvaluateShowcaseCandidate(
        const VoxelForgeTest::FTestWorld& World,
        const FVoxelStrateComposerCandidate& Candidate,
        int32 InTargetStrateIndex,
        const FVoxelStrateMeasureSettings& MeasureSettings,
        FShowcaseSample& OutSample,
        FString* OutFailureReason = nullptr,
        bool bMeasurePlayerFitRegardlessOfLegacy = false)
    {
        // Keep the evidence that was measured before a hard-gate rejection. The previous
        // showcase must be auditable even when the new player-fit gate rejects every one of its
        // eight records; returning only a bool would hide exactly the failure this test exists to
        // expose.
        OutSample = FShowcaseSample();
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
        OutSample.Candidate = Candidate;
        OutSample.Metrics = Metrics;
        OutSample.LegacyLaw = Law;
        OutSample.SelectionScore = VF_ShowcaseSelectionScore(Metrics);
        OutSample.LawText = FString::Printf(
            TEXT("legacy=%s%s"),
            VF_ShowcaseConnectivityName(Law.Result),
            (Law.bStartSnapped || Law.bGoalSnapped) ? TEXT(" (snapped)") : TEXT(""));
        OutSample.TargetStrateIndex = InTargetStrateIndex;
        OutSample.TopVoxelZ = TopVoxelZ;
        OutSample.BottomVoxelZ = BottomVoxelZ;
        if (!(bNonVacuous && bLargestEnough && bLawPass)
            && !bMeasurePlayerFitRegardlessOfLegacy)
        {
            // The player-fit gate is conjunctive with the established legacy gates. Once a
            // candidate already fails one of those necessary conditions, skip its expensive
            // step-1 capsule pass; this cannot change the survivor set or any accepted metric.
            return Fail(FString::Printf(
                TEXT("nonv=%d largest=%d legacy_law=%s snapped=%d/%d"),
                bNonVacuous ? 1 : 0,
                bLargestEnough ? 1 : 0,
                VF_ShowcaseConnectivityName(Law.Result),
                Law.bStartSnapped ? 1 : 0,
                Law.bGoalSnapped ? 1 : 0));
        }

        FVoxelStrateFinePreviewSettings FinePreviewSettings;
        FinePreviewSettings.SampleStep = FinePreviewSampleStep;
        FinePreviewSettings.RadiusInVoxels = FinePreviewRadiusVoxels;
        FinePreviewSettings.MaxCells = FinePreviewMaxCells;
        FVoxelStrateMeasureSettings FineMeasureSettings =
            VF_MakeFinePlayerFitSettings(
                MeasureSettings, FinePreviewSettings, ArrivalPoint, DeparturePoint);
        FVoxelStrateMetrics FineMetrics;
        const FVoxelConnectivityDiagnostics PlayerFitLaw =
            VF_DiagnosePlayerFitConnectivityWithSampler(
                Sampler, BottomVoxelZ, TopVoxelZ + 1, Context.EdgeSealThickness,
                ArrivalPoint, DeparturePoint, FineMeasureSettings, &FineMetrics);
        OutSample.FineMetrics = FineMetrics;
        OutSample.PlayerFitLaw = PlayerFitLaw;
        OutSample.LawText = FString::Printf(
            TEXT("legacy=%s%s; player_fit=%s%s"),
            VF_ShowcaseConnectivityName(Law.Result),
            (Law.bStartSnapped || Law.bGoalSnapped) ? TEXT(" (snapped)") : TEXT(""),
            VF_ShowcaseConnectivityName(PlayerFitLaw.Result),
            (PlayerFitLaw.bStartSnapped || PlayerFitLaw.bGoalSnapped)
                ? TEXT(" (snapped)") : TEXT(""));
        const bool bPlayerFitExists = FineMetrics.bValid
            && FineMetrics.bPlayerFitResolved
            && FineMetrics.NumPlayerFitCells > 0
            && FineMetrics.NumTraversableComponents > 0
            && FineMetrics.TraversableComponentShare > 0.0f;
        const bool bPlayerFitLawPass = bPlayerFitExists
            && PlayerFitLaw.bValid
            && PlayerFitLaw.Result == EVoxelConnectivityResult::Connected;
        if (!(bNonVacuous && bLargestEnough && bLawPass && bPlayerFitLawPass))
        {
            return Fail(FString::Printf(
                TEXT("nonv=%d largest=%d legacy_law=%s snapped=%d/%d player_law=%s "
                     "player_snapped=%d/%d player_fit=%d fit_fraction=%.6f traversable=%.6f "
                     "air=%.6f largest_share=%.6f walk_area=%.6f clearance=%d"),
                bNonVacuous ? 1 : 0,
                bLargestEnough ? 1 : 0,
                VF_ShowcaseConnectivityName(Law.Result),
                Law.bStartSnapped ? 1 : 0,
                Law.bGoalSnapped ? 1 : 0,
                VF_ShowcaseConnectivityName(PlayerFitLaw.Result),
                PlayerFitLaw.bStartSnapped ? 1 : 0,
                PlayerFitLaw.bGoalSnapped ? 1 : 0,
                bPlayerFitExists ? 1 : 0,
                FineMetrics.PlayerFitFraction,
                FineMetrics.TraversableComponentShare,
                Metrics.AirFraction,
                Metrics.LargestComponentShare,
                Metrics.WalkableFloorAreaFraction,
                Metrics.MedianVerticalClearance));
        }

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

    float VF_RoughnessReach(float SurfaceRoughness)
    {
        // The source note's bound is sup|FBM| = 1.5, and VOXEL_NOISE_SCALE is 1.25.
        return FMath::Max(0.0f, SurfaceRoughness) * VOXEL_NOISE_SCALE * 1.5f;
    }

    float VF_GetSurfaceRoughness(const FVoxelStrateComposerCandidate& Candidate)
    {
        switch (Candidate.Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return Candidate.ArchetypeParams.TunnelNetworkParams.SurfaceRoughness;
        case ECaveGeneratorType::Maze:
            return Candidate.ArchetypeParams.MazeParams.SurfaceRoughness;
        case ECaveGeneratorType::VerticalShafts:
            return Candidate.ArchetypeParams.VerticalShaftParams.SurfaceRoughness;
        case ECaveGeneratorType::FloatingIslands:
            return Candidate.ArchetypeParams.FloatingIslandParams.SurfaceRoughness;
        case ECaveGeneratorType::SurfaceWorld:
            return Candidate.ArchetypeParams.SurfaceParams.SurfaceRoughness;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
        default:
            return 0.0f;
        }
    }

    FString VF_FormatRoughnessRadiusReport(const TArray<FShowcaseSample>& Samples)
    {
        FString Report = TEXT(
            "Part B roughness reach / characteristic radius (reach = roughness * "
            "VOXEL_NOISE_SCALE * 1.5; source parameter values):\n");
        for (const FShowcaseSample& Sample : Samples)
        {
            const FVoxelStrateArchetypeParams& Params = Sample.Candidate.ArchetypeParams;
            const float SurfaceRoughness = VF_GetSurfaceRoughness(Sample.Candidate);
            const float Reach = VF_RoughnessReach(SurfaceRoughness);
            FString Detail;
            switch (Sample.Candidate.Archetype)
            {
            case ECaveGeneratorType::TunnelNetwork:
            case ECaveGeneratorType::Underwater:
            {
                const FStrateGenerationParams& P = Params.TunnelNetworkParams;
                const float RoomRadius = 0.5f * (P.MinRoomRadius + P.MaxRoomRadius);
                const float TunnelRadius = 0.5f * (P.TunnelMinRadius + P.TunnelMaxRadius);
                Detail = FString::Printf(
                    TEXT("room r=%.3f (radius %.3f), corridor r=%.3f (radius %.3f)"),
                    Reach / FMath::Max(RoomRadius, KINDA_SMALL_NUMBER), RoomRadius,
                    Reach / FMath::Max(TunnelRadius, KINDA_SMALL_NUMBER), TunnelRadius);
                break;
            }
            case ECaveGeneratorType::Maze:
            {
                const FMazeGenerationParams& P = Params.MazeParams;
                Detail = FString::Printf(
                    TEXT("corridor r=%.3f (radius %.3f)"),
                    Reach / FMath::Max(P.CorridorRadius, KINDA_SMALL_NUMBER),
                    P.CorridorRadius);
                break;
            }
            case ECaveGeneratorType::VerticalShafts:
            {
                const FVerticalShaftParams& P = Params.VerticalShaftParams;
                const float ShaftRadius = 0.5f * (P.ShaftMinRadius + P.ShaftMaxRadius);
                Detail = FString::Printf(
                    TEXT("shaft r=%.3f (radius %.3f), connector r=%.3f (radius %.3f)"),
                    Reach / FMath::Max(ShaftRadius, KINDA_SMALL_NUMBER), ShaftRadius,
                    Reach / FMath::Max(P.ConnectorRadius, KINDA_SMALL_NUMBER),
                    P.ConnectorRadius);
                break;
            }
            case ECaveGeneratorType::FloatingIslands:
            {
                const FFloatingIslandParams& P = Params.FloatingIslandParams;
                const float IslandRadius = 0.5f * (P.IslandMinRadius + P.IslandMaxRadius);
                Detail = FString::Printf(
                    TEXT("island r=%.3f (radius %.3f)"),
                    Reach / FMath::Max(IslandRadius, KINDA_SMALL_NUMBER), IslandRadius);
                break;
            }
            case ECaveGeneratorType::FlatPlain:
            case ECaveGeneratorType::CrystalChamber:
            {
                const FSlabGenerationParams& P = Params.SlabParams;
                const float ColumnRadius = 0.5f * (P.ColumnMinRadius + P.ColumnMaxRadius);
                const float FloorReach = VF_RoughnessReach(P.FloorRoughness);
                const float CeilingReach = VF_RoughnessReach(P.CeilingRoughness);
                if (P.ColumnDensity > 0.0f)
                {
                    Detail = FString::Printf(
                        TEXT("floor/ceiling reach-to-column-radius=%.3f/%.3f "
                             "(column radius %.3f; slab has no corridor/room radius)"),
                        FloorReach / FMath::Max(ColumnRadius, KINDA_SMALL_NUMBER),
                        CeilingReach / FMath::Max(ColumnRadius, KINDA_SMALL_NUMBER), ColumnRadius);
                }
                else
                {
                    Detail = FString::Printf(
                        TEXT("floor reach %.3f, ceiling reach %.3f; radius=N/A "
                             "(slab has no corridor/room feature)"),
                        FloorReach, CeilingReach);
                }
                break;
            }
            case ECaveGeneratorType::SurfaceWorld:
            {
                const FSurfaceGenerationParams& P = Params.SurfaceParams;
                const float ReliefReach = VF_RoughnessReach(P.SurfaceRoughness);
                // SurfaceWorld is a heightfield, so it has no radial cave feature. ElevationRange
                // is reported as the nearest characteristic vertical relief scale and is labelled
                // explicitly rather than pretending it is a tunnel/room radius.
                Detail = FString::Printf(
                    TEXT("terrain relief=%.3f (reach %.3f / ElevationRange %.3f; "
                         "no radial feature)"),
                    ReliefReach / FMath::Max(P.ElevationRange, KINDA_SMALL_NUMBER), ReliefReach,
                    P.ElevationRange);
                break;
            }
            default:
                Detail = TEXT("unsupported");
                break;
            }
            Report += FString::Printf(
                TEXT("  %s seed=%d index=%d roughness=%.3f reach=%.3f: %s\n"),
                VF_GetStrateArchetypeName(Sample.Candidate.Archetype),
                Sample.Candidate.Seed, Sample.Candidate.Index,
                SurfaceRoughness, Reach,
                *Detail);
        }
        return Report;
    }

    FString VF_FormatWorldScaleAudit()
    {
        constexpr float VoxelMetres = 0.25f;
        constexpr float StrateHeightVoxels = 8.0f * static_cast<float>(CHUNK_SIZE);
        FString Report = TEXT(
            "WORLD SCALE AUDIT (all distances are authored in voxels; METRES = voxels * 0.25; "
            "player radius=1.36 vox/0.34 m, height=7.04 vox/1.76 m; frequency rows show nominal "
            "period=1/frequency)\n"
            "archetype | name | voxels | METRES | body/level meaning\n");
        auto Add = [&Report](const TCHAR* Archetype, const TCHAR* Name,
                             float Voxels, const TCHAR* Meaning)
        {
            Report += FString::Printf(
                TEXT("%s | %s | %.2f | %.2f | %s\n"), Archetype, Name,
                Voxels, Voxels * 0.25f, Meaning);
        };
        auto AddRange = [&Report](const TCHAR* Archetype, const TCHAR* Name,
                                  float MinVoxels, float MaxVoxels,
                                  const TCHAR* Meaning)
        {
            Report += FString::Printf(
                TEXT("%s | %s | %.2f..%.2f | %.2f..%.2f | %s\n"), Archetype, Name,
                MinVoxels, MaxVoxels, MinVoxels * 0.25f, MaxVoxels * 0.25f, Meaning);
        };
        const TCHAR* TunnelNames[] = { TEXT("TunnelNetwork"), TEXT("Underwater") };
        for (const TCHAR* Archetype : TunnelNames)
        {
            Add(Archetype, TEXT("StrateHeight"), StrateHeightVoxels,
                TEXT("8 chunks: a 64 m vertical envelope for tall rooms and passages"));
            Add(Archetype, TEXT("BoundarySealThickness"), 4.0f,
                TEXT("1 m solid geological boundary; not player space"));
            Add(Archetype, TEXT("WormNetworkRange"), 64.0f,
                TEXT("16 m side-passage reach around the room/tunnel skeleton"));
            Add(Archetype, TEXT("WormFrequency (nominal period)"), 1.0f / 0.015f,
                TEXT("16.7 m noise period: about two chunks for broad bends"));
            Add(Archetype, TEXT("RoomSpacing"), 128.0f,
                TEXT("32 m room-cell spacing; leaves a 6 m radial margin around a 20 m room"));
            AddRange(Archetype, TEXT("RoomRadius"), 16.0f, 40.0f,
                     TEXT("8 m minimum room to 20 m large chamber diameter"));
            Add(Archetype, TEXT("OriginRoomRadius"), 48.0f,
                TEXT("24 m hub diameter: a three-chunk boss-space anchor"));
            AddRange(Archetype, TEXT("TunnelRadius"), 6.0f, 8.0f,
                     TEXT("3-4 m bores: body height plus generous headroom and fight width"));
            Add(Archetype, TEXT("MaxTunnelLength"), 360.0f,
                TEXT("90 m: reaches the worst 32 m cell-jitter neighbour with margin"));
            Add(Archetype, TEXT("TunnelWarpStrength"), 24.0f,
                TEXT("6 m lateral bend, visibly organic but smaller than the 90 m reach"));
            Add(Archetype, TEXT("SDFBlendRadius"), 4.0f,
                TEXT("1 m junction rounding, below the 1.5 m minimum tunnel radius"));
            Add(Archetype, TEXT("CaveWarpStrength"), 16.0f,
                TEXT("4 m skeleton distortion"));
            Add(Archetype, TEXT("SurfaceRoughness"), 2.0f,
                TEXT("0.5 m wall texture; bounded reach is 0.94 m"));
            Add(Archetype, TEXT("FloorReliefStrength"), 4.0f,
                TEXT("1 m floor undulation, small relative to a 3 m bore"));
            Add(Archetype, TEXT("FloorReliefFrequency (nominal period)"), 1.0f / 0.015f,
                TEXT("16.7 m floor-undulation period: broad enough to walk as a slope"));
            Add(Archetype, TEXT("RoughnessFrequency (nominal period)"), 1.0f / 0.1f,
                TEXT("2.5 m wall-detail period: a few body lengths, not voxel-grain tunnel"));
            Add(Archetype, TEXT("CaveWarpFrequency (nominal period)"), 1.0f / 0.015f,
                TEXT("16.7 m skeleton-warp period: broad landform-scale bend"));
            Add(Archetype, TEXT("DomainWarpFrequency (nominal period)"), 1.0f / 0.03f,
                TEXT("8.3 m optional roughness-warp period"));
            Add(Archetype, TEXT("CliffSampleDist (transport)"), 2.0f,
                TEXT("0.5 m optional local slope sample distance"));
            Add(Archetype, TEXT("FloorBias"), 4.0f,
                TEXT("1 m floor stabilisation applied below room centre"));
            Add(Archetype, TEXT("DomainWarpStrength"), 0.0f,
                TEXT("0 m optional roughness-coordinate warp; disabled by default"));
            Add(Archetype, TEXT("OverhangFrequency (nominal period)"), 1.0f / 0.06f,
                TEXT("4.2 m optional shelf-shape period"));
            Add(Archetype, TEXT("ScallopFrequency (nominal period)"), 1.0f / 0.1f,
                TEXT("2.5 m optional erosion-bowl period"));
            Add(Archetype, TEXT("TerraceStepHeight (transport)"), 0.0f,
                TEXT("0 m optional step; disabled by default"));
            Add(Archetype, TEXT("LayerLineSpacing (transport)"), 0.0f,
                TEXT("0 m optional geological band period; disabled by default"));
            Add(Archetype, TEXT("LayerLineDepth (transport)"), 0.3f,
                TEXT("0.075 m optional groove depth; inactive while spacing is zero"));
            Add(Archetype, TEXT("RibbingSpacing (transport)"), 0.0f,
                TEXT("0 m optional rib period; disabled by default"));
            Add(Archetype, TEXT("RibbingDepth (transport)"), 0.4f,
                TEXT("0.1 m optional rib protrusion; inactive while spacing is zero"));
            Add(Archetype, TEXT("OverhangDepth (transport)"), 8.0f,
                TEXT("2 m optional shelf; terrain-op transport is reset when disabled"));
            AddRange(Archetype, TEXT("ArchRadius (transport)"), 6.0f, 12.0f,
                     TEXT("1.5-3 m optional arch feature"));
            AddRange(Archetype, TEXT("ColumnRadius (transport)"), 4.0f, 8.0f,
                     TEXT("1-2 m optional pillar radius"));
            AddRange(Archetype, TEXT("PitRadius (transport)"), 8.0f, 16.0f,
                     TEXT("2-4 m optional vertical opening radius"));
            Add(Archetype, TEXT("PitDepth (transport)"), 32.0f,
                TEXT("8 m optional vertical drop"));
            AddRange(Archetype, TEXT("ChimneyRadius (transport)"), 4.0f, 8.0f,
                     TEXT("1-2 m optional upward shaft radius"));
            Add(Archetype, TEXT("ChimneyHeight (transport)"), 32.0f,
                TEXT("8 m optional upward shaft height"));
            AddRange(Archetype, TEXT("DomeRadius (transport)"), 16.0f, 32.0f,
                     TEXT("4-8 m optional ceiling dome radius"));
            Add(Archetype, TEXT("PinchLength (transport)"), 24.0f,
                TEXT("6 m optional bottleneck length; default operation is disabled"));
        }

        for (const TCHAR* Archetype : { TEXT("FlatPlain"), TEXT("CrystalChamber") })
        {
            Add(Archetype, TEXT("StrateHeight"), StrateHeightVoxels,
                TEXT("8 chunks / 64 m: open vertical chamber envelope"));
            Add(Archetype, TEXT("Floor height"), StrateHeightVoxels * 0.25f,
                TEXT("16 m above the strate bottom"));
            Add(Archetype, TEXT("Ceiling height"), StrateHeightVoxels * 0.60f,
                TEXT("38.4 m above the bottom; 22.4 m clear span before roughness"));
            Add(Archetype, TEXT("FloorRoughness"), 4.0f,
                TEXT("1 m rolling floor displacement"));
            Add(Archetype, TEXT("CeilingRoughness"), 6.0f,
                TEXT("1.5 m downward formation reach"));
            Add(Archetype, TEXT("FloorRoughnessFrequency (nominal period)"), 1.0f / 0.04f,
                TEXT("6.25 m floor-detail period: broad rolling ground"));
            Add(Archetype, TEXT("CeilingRoughnessFrequency (nominal period)"), 1.0f / 0.04f,
                TEXT("6.25 m ceiling-formation period"));
            AddRange(Archetype, TEXT("ColumnRadius"), 8.0f, 16.0f,
                     TEXT("4-8 m pillar diameters; large enough to read without erasing a fight lane"));
            Add(Archetype, TEXT("ColumnSpacing"), 96.0f,
                TEXT("24 m pillar-cell spacing"));
            Add(Archetype, TEXT("BoundarySealThickness"), 4.0f,
                TEXT("1 m solid boundary"));
        }

        Add(TEXT("Common layout"), TEXT("OriginSpineRadius"), 14.0f,
            TEXT("3.5 m radius / 7 m diameter central descent spine"));
        Add(TEXT("Common layout"), TEXT("InterStrateGap"), 0.0f,
            TEXT("0 m bedrock gap by default; configured as whole 8 m chunks"));
        Add(TEXT("Common layout"), TEXT("WorldRadiusVoxels"), 0.0f,
            TEXT("0 m lateral bound: radial regions remain gated off"));

        Add(TEXT("Maze"), TEXT("StrateHeight"), StrateHeightVoxels,
            TEXT("8 chunks / 64 m: supports several 16 m maze levels"));
        Add(TEXT("Maze"), TEXT("CellSize"), 64.0f,
            TEXT("16 m lattice cells, matching ordinary fight-space scale"));
        Add(TEXT("Maze"), TEXT("CorridorRadius"), 8.0f,
            TEXT("2 m radius / 4 m circular bore: upper fight-space target"));
        Add(TEXT("Maze"), TEXT("SurfaceRoughness"), 2.0f,
            TEXT("0.5 m texture; bounded reach is 0.94 m, below the 2 m radius"));
        Add(TEXT("Maze"), TEXT("BoundarySealThickness"), 4.0f,
            TEXT("1 m solid boundary"));

        Add(TEXT("SurfaceWorld"), TEXT("StrateHeight"), StrateHeightVoxels,
            TEXT("8 chunks / 64 m: high sky-cap envelope"));
        Add(TEXT("SurfaceWorld"), TEXT("ElevationRange"), 80.0f,
            TEXT("20 m terrain relief: human-scale hills, not a miniature room"));
        Add(TEXT("SurfaceWorld"), TEXT("Base ground height"), StrateHeightVoxels * 0.25f,
            TEXT("16 m mean ground elevation above the strate bottom"));
        Add(TEXT("SurfaceWorld"), TEXT("HeightWarpStrength"), 48.0f,
            TEXT("12 m horizontal landform distortion"));
        Add(TEXT("SurfaceWorld"), TEXT("SurfaceRoughness"), 2.0f,
            TEXT("0.5 m ground detail; bounded reach is 0.94 m"));
        Add(TEXT("SurfaceWorld"), TEXT("ContinentFrequency (nominal period)"), 1.0f / 0.006f,
            TEXT("41.7 m broad landmass period"));
        Add(TEXT("SurfaceWorld"), TEXT("MountainFrequency (nominal period)"), 1.0f / 0.012f,
            TEXT("20.8 m ridge period"));
        Add(TEXT("SurfaceWorld"), TEXT("DetailFrequency (nominal period)"), 1.0f / 0.04f,
            TEXT("6.25 m fine terrain period"));
        Add(TEXT("SurfaceWorld"), TEXT("TerraceHeight"), 8.0f,
            TEXT("2 m optional step; operation remains disabled by strength 0"));
        Add(TEXT("SurfaceWorld"), TEXT("LayerLineSpacing"), 8.0f,
            TEXT("2 m optional geological band spacing"));
        Add(TEXT("SurfaceWorld"), TEXT("LayerLineDepth"), 0.0f,
            TEXT("0 m default band displacement: optional operation disabled"));
        Add(TEXT("SurfaceWorld"), TEXT("CliffSampleDist"), 2.0f,
            TEXT("0.5 m slope sample scale for optional cliff operation"));
        Add(TEXT("SurfaceWorld"), TEXT("OverhangReach"), 16.0f,
            TEXT("4 m optional shelf reach"));
        Add(TEXT("SurfaceWorld"), TEXT("OverhangHeight"), 24.0f,
            TEXT("6 m optional shelf zone"));
        Add(TEXT("SurfaceWorld"), TEXT("BeachWidth"), 12.0f,
            TEXT("3 m shore band"));
        Add(TEXT("SurfaceWorld"), TEXT("Sky cap height"), StrateHeightVoxels * 0.95f,
            TEXT("60.8 m above the strate bottom; leaves a high open sky"));
        Add(TEXT("SurfaceWorld"), TEXT("CeilingRoughness"), 6.0f,
            TEXT("1.5 m optional downward cap detail"));
        Add(TEXT("SurfaceWorld"), TEXT("CeilingUndulation"), 0.0f,
            TEXT("0 m broad cap swell; disabled by default"));
        Add(TEXT("SurfaceWorld"), TEXT("CeilingRidgeStrength"), 0.0f,
            TEXT("0 m hanging ridge height; disabled by default"));
        Add(TEXT("SurfaceWorld"), TEXT("CeilingWarpStrength"), 0.0f,
            TEXT("0 m cap-coordinate warp; disabled by default"));
        Add(TEXT("SurfaceWorld"), TEXT("BoundarySealThickness"), 4.0f,
            TEXT("1 m solid boundary"));

        Add(TEXT("SurfaceWorld"), TEXT("HeightWarpFrequency (nominal period)"), 1.0f / 0.008f,
            TEXT("31.25 m landform-warp period"));
        Add(TEXT("SurfaceWorld"), TEXT("ReliefFrequency (nominal period)"), 1.0f / 0.0015f,
            TEXT("166.7 m macro-region period"));
        Add(TEXT("SurfaceWorld"), TEXT("OverhangFrequency (nominal period)"), 1.0f / 0.02f,
            TEXT("12.5 m optional shelf-shape period"));
        Add(TEXT("SurfaceWorld"), TEXT("CeilingRoughnessFrequency (nominal period)"), 1.0f / 0.04f,
            TEXT("6.25 m optional cap-detail period"));
        Add(TEXT("SurfaceWorld"), TEXT("CeilingUndulationFrequency (nominal period)"), 1.0f / 0.004f,
            TEXT("62.5 m optional cap-swell period"));
        Add(TEXT("SurfaceWorld"), TEXT("CeilingRidgeFrequency (nominal period)"), 1.0f / 0.02f,
            TEXT("12.5 m optional hanging-ridge period"));
        Add(TEXT("SurfaceWorld"), TEXT("CeilingWarpFrequency (nominal period)"), 1.0f / 0.01f,
            TEXT("25 m optional cap-warp period"));

        Add(TEXT("VerticalShafts"), TEXT("StrateHeight"), StrateHeightVoxels,
            TEXT("8 chunks / 64 m: a full vertical traversal envelope"));
        Add(TEXT("VerticalShafts"), TEXT("ShaftSpacing"), 80.0f,
            TEXT("20 m shaft-cell spacing"));
        AddRange(TEXT("VerticalShafts"), TEXT("ShaftRadius"), 8.0f, 14.0f,
                 TEXT("4-7 m shaft diameters"));
        Add(TEXT("VerticalShafts"), TEXT("ConnectorRadius"), 6.0f,
            TEXT("1.5 m radius / 3 m horizontal connector bore"));
        Add(TEXT("VerticalShafts"), TEXT("LedgeSpacing"), 32.0f,
            TEXT("8 m vertical landing interval"));
        Add(TEXT("VerticalShafts"), TEXT("LedgeDepth"), 4.0f,
            TEXT("1 m landing intrusion"));
        Add(TEXT("VerticalShafts"), TEXT("SurfaceRoughness"), 2.0f,
            TEXT("0.5 m wall texture; bounded reach is 0.94 m"));
        Add(TEXT("VerticalShafts"), TEXT("BoundarySealThickness"), 4.0f,
            TEXT("1 m solid boundary"));

        Add(TEXT("FloatingIslands"), TEXT("StrateHeight"), StrateHeightVoxels,
            TEXT("8 chunks / 64 m: vertical sky volume"));
        Add(TEXT("FloatingIslands"), TEXT("IslandSpacing"), 112.0f,
            TEXT("28 m island-cell spacing; max 24 m diameter leaves a 2 m centre-to-edge gap"));
        AddRange(TEXT("FloatingIslands"), TEXT("IslandRadius"), 24.0f, 48.0f,
                 TEXT("12-24 m landmass diameters: room to cathedral scale"));
        Add(TEXT("FloatingIslands"), TEXT("Underside half-depth"), 48.0f * 0.60f,
            TEXT("7.2 m below the centre for the largest island; top half is 2.4 m"));
        Add(TEXT("FloatingIslands"), TEXT("SurfaceRoughness"), 3.0f,
            TEXT("0.75 m island texture; bounded reach is 1.41 m"));
        Add(TEXT("FloatingIslands"), TEXT("SDFBlendRadius"), 6.0f,
            TEXT("1.5 m island edge blend"));
        Add(TEXT("FloatingIslands"), TEXT("BoundarySealThickness"), 4.0f,
            TEXT("1 m solid boundary"));

        Add(TEXT("Common passage"), TEXT("MouthRadius"), 8.0f,
            TEXT("2 m radius / 4 m bore at each strate mouth"));
        Add(TEXT("Common passage"), TEXT("MidRadius"), 6.0f,
            TEXT("1.5 m radius / 3 m minimum bore between mouths"));
        AddRange(TEXT("Common passage"), TEXT("Reach"), 32.0f, 96.0f,
                 TEXT("8-24 m reach into each strate"));
        AddRange(TEXT("Common passage"), TEXT("Distance from spine"), 64.0f, 192.0f,
                 TEXT("16-48 m placement range"));
        Add(TEXT("Common passage"), TEXT("Wander"), 24.0f,
            TEXT("6 m sideways meander"));
        Add(TEXT("Common passage"), TEXT("VerticalWobble"), 0.0f,
            TEXT("0 m vertical wobble by default; descent remains controlled"));
        Add(TEXT("Common passage"), TEXT("SpiralRadius"), 24.0f,
            TEXT("6 m corkscrew radius"));
        Add(TEXT("Common passage"), TEXT("CascadeLedge"), 8.0f,
            TEXT("2 m ledge run per cascade step"));

        Add(TEXT("Common disturbance"), TEXT("ChasmSpacing"), 192.0f,
            TEXT("48 m chasm-cell spacing"));
        Add(TEXT("Common disturbance"), TEXT("ChasmRadius"), 24.0f,
            TEXT("6 m radius / 12 m opening when enabled"));
        Add(TEXT("Common disturbance"), TEXT("BridgeSpacing"), 128.0f,
            TEXT("32 m bridge-cell spacing"));
        Add(TEXT("Common disturbance"), TEXT("BridgeRadius"), 8.0f,
            TEXT("2 m radius / 4 m bridge width"));
        Add(TEXT("Common disturbance"), TEXT("RidgeSpacing"), 128.0f,
            TEXT("32 m ridge-cell spacing"));
        Add(TEXT("Common disturbance"), TEXT("RidgeHeight"), 32.0f,
            TEXT("8 m optional blade height"));
        Add(TEXT("Common disturbance"), TEXT("RidgeThickness"), 8.0f,
            TEXT("2 m optional blade half-width"));
        Add(TEXT("Common disturbance"), TEXT("BoundarySealThickness"), 4.0f,
            TEXT("1 m solid boundary shared with the active archetype"));

        Report += FString::Printf(
            TEXT("Audit anchors: 1 voxel=%.2f m; 1 chunk=8.00 m; 2.5 m walk corridor=10 vox; "
                 "3-4 m fight corridor=12-16 vox; 8-15 m room=32-60 vox; 20 m cathedral=80 vox.\n"),
            VoxelMetres);
        return Report;
    }

    FString VF_FormatDefaultRoughnessRatios()
    {
        const float TunnelReach = VF_RoughnessReach(2.0f);
        const float SlabFloorReach = VF_RoughnessReach(4.0f);
        const float SlabCeilingReach = VF_RoughnessReach(6.0f);
        const float MazeReach = VF_RoughnessReach(2.0f);
        const float ShaftReach = VF_RoughnessReach(2.0f);
        const float IslandReach = VF_RoughnessReach(3.0f);
        return FString::Printf(
            TEXT("Default roughness-to-feature ratios (reach = roughness * 1.25 * 1.5; "
                 "all values are dimensionless): TunnelNetwork=%.3f reach/%.1f-voxel "
                 "minimum tunnel radius (room mean %.3f); Underwater=same; FlatPlain="
                 "%.3f floor reach/mean column radius and %.3f ceiling reach/mean column radius; "
                 "CrystalChamber=same slab engine; Maze=%.3f reach/%.1f-voxel corridor radius; "
                 "SurfaceWorld=%.3f reach/80-voxel terrain relief; VerticalShafts=%.3f shaft-min "
                 "and %.3f connector; FloatingIslands=%.3f reach/min island radius "
                 "(%.3f including 6-voxel blend).\n"),
             TunnelReach / 6.0f, 6.0f, TunnelReach / 28.0f,
             SlabFloorReach / 12.0f, SlabCeilingReach / 12.0f,
             MazeReach / 8.0f, 8.0f, TunnelReach / 80.0f,
            ShaftReach / 8.0f, ShaftReach / 6.0f,
            IslandReach / 24.0f, (IslandReach + 6.0f) / 24.0f);
    }

    float VF_ShowcaseRoughnessFeatureRatio(
        const FVoxelStrateComposerCandidate& Candidate)
    {
        switch (Candidate.Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return VF_RoughnessReach(
                Candidate.ArchetypeParams.TunnelNetworkParams.SurfaceRoughness)
                / FMath::Max(
                    Candidate.ArchetypeParams.TunnelNetworkParams.TunnelMinRadius,
                    KINDA_SMALL_NUMBER);
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
        {
            const FSlabGenerationParams& P = Candidate.ArchetypeParams.SlabParams;
            const float ColumnRadius = 0.5f * (P.ColumnMinRadius + P.ColumnMaxRadius);
            return FMath::Max(
                VF_RoughnessReach(P.FloorRoughness),
                VF_RoughnessReach(P.CeilingRoughness))
                / FMath::Max(ColumnRadius, KINDA_SMALL_NUMBER);
        }
        case ECaveGeneratorType::Maze:
            return VF_RoughnessReach(Candidate.ArchetypeParams.MazeParams.SurfaceRoughness)
                / FMath::Max(Candidate.ArchetypeParams.MazeParams.CorridorRadius,
                             KINDA_SMALL_NUMBER);
        case ECaveGeneratorType::SurfaceWorld:
            return VF_RoughnessReach(Candidate.ArchetypeParams.SurfaceParams.SurfaceRoughness)
                / FMath::Max(Candidate.ArchetypeParams.SurfaceParams.ElevationRange,
                             KINDA_SMALL_NUMBER);
        case ECaveGeneratorType::VerticalShafts:
            return VF_RoughnessReach(
                Candidate.ArchetypeParams.VerticalShaftParams.SurfaceRoughness)
                / FMath::Max(
                    Candidate.ArchetypeParams.VerticalShaftParams.ShaftMinRadius,
                    KINDA_SMALL_NUMBER);
        case ECaveGeneratorType::FloatingIslands:
        {
            const FFloatingIslandParams& P = Candidate.ArchetypeParams.FloatingIslandParams;
            return (VF_RoughnessReach(P.SurfaceRoughness) + P.SDFBlendRadius)
                / FMath::Max(P.IslandMinRadius, KINDA_SMALL_NUMBER);
        }
        default:
            return 0.0f;
        }
    }

    FString VF_FormatPlayerFitSummary(
        const TArray<FShowcaseSample>& Samples,
        const TCHAR* Label)
    {
        FString Report = FString::Printf(
            TEXT("%s (player-fit fields are fitted SampleStep=1; walkable floor area and "
                 "median vertical clearance are coarse SampleStep=4 proxies)\n"
                 "archetype | player_fit_fraction_step1 | traversable_share_step1 | "
                 "fit_cells_step1 | components_step1 | largest_component_cells_step1 | "
                 "arrival_component_cells_step1 | departure_component_cells_step1 | "
                 "mouth_component_gap_voxels_step1 | restricted_law_step1 | "
                 "coarse_walkable_floor_area_m2 | coarse_median_vertical_clearance_m | "
                 "roughness-to-feature ratio\n"), Label);
        for (const ECaveGeneratorType Archetype : GShowcaseArchetypes)
        {
            const FShowcaseSample* Found = Samples.FindByPredicate(
                [Archetype](const FShowcaseSample& Sample)
                {
                    return Sample.Candidate.Archetype == Archetype;
                });
            if (Found == nullptr)
            {
                Report += FString::Printf(
                    TEXT("%s | no measured sample | no measured sample | no measured sample | "
                         "no measured sample | no measured sample | no measured sample | "
                         "no measured sample | no measured sample | no measured sample | "
                         "n/a | n/a | n/a\n"),
                    VF_GetStrateArchetypeName(Archetype));
                continue;
            }
            const FVoxelStrateMetrics& M = Found->FineMetrics;
            const FVoxelConnectivityDiagnostics& D = Found->PlayerFitLaw;
            const FString Law = M.bPlayerFitResolved
                ? VF_ShowcaseConnectivityName(Found->PlayerFitLaw.Result)
                : TEXT("unresolved");
            const float FloorAreaM2 = Found->Metrics.WalkableFloorAreaFraction
                * 128.0f * 128.0f;
            Report += FString::Printf(
                TEXT("%s | %.6f | %.6f | %lld | %d | %lld | %lld | %lld | %.3f | %s | "
                     "%.1f (%.6f footprint) | %.2f | %.3f\n"),
                VF_GetStrateArchetypeName(Archetype),
                M.bPlayerFitResolved ? M.PlayerFitFraction : 0.0f,
                M.bPlayerFitResolved ? M.TraversableComponentShare : 0.0f,
                static_cast<long long>(M.NumPlayerFitCells),
                M.NumTraversableComponents,
                static_cast<long long>(M.LargestTraversableComponentCells),
                static_cast<long long>(D.StartComponentCells),
                static_cast<long long>(D.GoalComponentCells),
                D.StartToGoalComponentDistanceCells,
                *Law, FloorAreaM2,
                Found->Metrics.WalkableFloorAreaFraction,
                Found->Metrics.MedianVerticalClearance * 0.25f,
                VF_ShowcaseRoughnessFeatureRatio(Found->Candidate));
        }
        return Report;
    }

    bool VF_RunMazeRoughnessExperiment(
        const VoxelForgeTest::FTestWorld& World,
        const FShowcaseSample& MazeSample,
        const FVoxelStrateMeasureSettings& BaseSettings,
        const FVoxelStrateFinePreviewSettings& FinePreviewSettings,
        FString& OutReport)
    {
        if (MazeSample.Candidate.Archetype != ECaveGeneratorType::Maze)
        {
            OutReport = TEXT("Part B maze roughness experiment unavailable: previous Maze roll was not found.");
            return false;
        }
        FVector ArrivalPoint = FVector::ZeroVector;
        FVector DeparturePoint = FVector::ZeroVector;
        if (!VF_GetShowcaseMouthPair(
                World, MazeSample.TargetStrateIndex, ArrivalPoint, DeparturePoint))
        {
            OutReport = TEXT("Part B maze roughness experiment unavailable: mouth pair was not found.");
            return false;
        }

        int32 TopVoxelZ = 0;
        int32 BottomVoxelZ = 0;
        if (!World.GetSlotVoxelZRange(MazeSample.TargetStrateIndex, TopVoxelZ, BottomVoxelZ))
        {
            OutReport = TEXT("Part B maze roughness experiment unavailable: Z range was not found.");
            return false;
        }

        FVoxelOpStack Stack;
        FVoxelOpContext Context;
        FVoxelStrateComposerCandidate ZeroCandidate = MazeSample.Candidate;
        ZeroCandidate.ArchetypeParams.MazeParams.SurfaceRoughness = 0.0f;
        FString BuildError;
        if (!VF_BuildShowcaseStack(
                World, ZeroCandidate, TopVoxelZ, BottomVoxelZ, Stack, Context, BuildError))
        {
            OutReport = FString::Printf(
                TEXT("Part B maze roughness experiment unavailable: %s"), *BuildError);
            return false;
        }
        Stack.PrepareChunk(Context);
        FShowcaseStackSampler Sampler(Stack);
        const FVoxelStrateMeasureSettings FineSettings = VF_MakeFinePlayerFitSettings(
            BaseSettings, FinePreviewSettings, ArrivalPoint, DeparturePoint);
        FVoxelStrateMetrics ZeroMetrics;
        const FVoxelConnectivityDiagnostics ZeroLaw =
            VF_DiagnosePlayerFitConnectivityWithSampler(
                Sampler, BottomVoxelZ, TopVoxelZ + 1, Context.EdgeSealThickness,
                ArrivalPoint, DeparturePoint, FineSettings, &ZeroMetrics);
        const FVoxelStrateArchetypeParams& OriginalParams = MazeSample.Candidate.ArchetypeParams;
        const float OriginalReach = VF_RoughnessReach(
            OriginalParams.MazeParams.SurfaceRoughness);
        OutReport = FString::Printf(
            TEXT("Part B Maze roughness=%.3f (reach %.3f voxels; corridor radius %.3f) -> 0: "
                 "PlayerFitFraction %.6f -> %.6f, minimum corridor clearance %.2f -> %.2f "
                 "voxels, restricted law %s -> %s; same target=%d and fitted step-1 ROI."),
            OriginalParams.MazeParams.SurfaceRoughness, OriginalReach,
            OriginalParams.MazeParams.CorridorRadius,
            MazeSample.FineMetrics.PlayerFitFraction, ZeroMetrics.PlayerFitFraction,
            MazeSample.FineMetrics.MinimumPlayerClearanceVoxels,
            ZeroMetrics.MinimumPlayerClearanceVoxels,
            VF_ShowcaseConnectivityName(MazeSample.PlayerFitLaw.Result),
            VF_ShowcaseConnectivityName(ZeroLaw.Result), MazeSample.TargetStrateIndex);
        return true;
    }

    bool VF_RunMazeCorridorRadiusSweep(
        const VoxelForgeTest::FTestWorld& World,
        const FShowcaseSample& MazeSample,
        const FVoxelStrateMeasureSettings& BaseSettings,
        const FVoxelStrateFinePreviewSettings& FinePreviewSettings,
        FString& OutReport)
    {
        if (MazeSample.Candidate.Archetype != ECaveGeneratorType::Maze)
        {
            OutReport = TEXT("Part A Maze corridor-radius sweep unavailable: previous Maze roll was not found.");
            return false;
        }

        FVector ArrivalPoint = FVector::ZeroVector;
        FVector DeparturePoint = FVector::ZeroVector;
        if (!VF_GetShowcaseMouthPair(
                World, MazeSample.TargetStrateIndex, ArrivalPoint, DeparturePoint))
        {
            OutReport = TEXT("Part A Maze corridor-radius sweep unavailable: mouth pair was not found.");
            return false;
        }

        int32 TopVoxelZ = 0;
        int32 BottomVoxelZ = 0;
        if (!World.GetSlotVoxelZRange(MazeSample.TargetStrateIndex, TopVoxelZ, BottomVoxelZ))
        {
            OutReport = TEXT("Part A Maze corridor-radius sweep unavailable: Z range was not found.");
            return false;
        }

        const FVoxelStrateMeasureSettings FineSettings = VF_MakeFinePlayerFitSettings(
            BaseSettings, FinePreviewSettings, ArrivalPoint, DeparturePoint);
        const float RadiusSweep[] = { 4.0f, 6.0f, 8.0f, 10.0f, 12.0f };
        OutReport = TEXT(
            "Part A Maze corridor-radius sweep (same seed/index, fitted step-1 ROI; "
            "radius voxels -> bore metres):\n");
        for (const float CorridorRadius : RadiusSweep)
        {
            FVoxelStrateComposerCandidate Candidate = MazeSample.Candidate;
            Candidate.ArchetypeParams.MazeParams.CorridorRadius = CorridorRadius;

            FVoxelOpStack Stack;
            FVoxelOpContext Context;
            FString BuildError;
            if (!VF_BuildShowcaseStack(
                    World, Candidate, TopVoxelZ, BottomVoxelZ, Stack, Context, BuildError))
            {
                OutReport += FString::Printf(
                    TEXT("  radius=%.0f bore=%.2f m: build failed (%s)\n"),
                    CorridorRadius, CorridorRadius * 2.0f * VOXEL_SIZE / 100.0f, *BuildError);
                continue;
            }

            Stack.PrepareChunk(Context);
            FShowcaseStackSampler Sampler(Stack);
            FVoxelStrateMetrics Metrics;
            const FVoxelConnectivityDiagnostics Law =
                VF_DiagnosePlayerFitConnectivityWithSampler(
                    Sampler, BottomVoxelZ, TopVoxelZ + 1, Context.EdgeSealThickness,
                    ArrivalPoint, DeparturePoint, FineSettings, &Metrics);
            OutReport += FString::Printf(
                TEXT("  radius=%.0f (%.2f m) bore=%.2f m: fit=%.6f traversable=%.6f "
                     "fit_cells=%lld min_clearance=%.2f voxels (%.2f m) law=%s\n"),
                CorridorRadius, CorridorRadius * VOXEL_SIZE / 100.0f,
                CorridorRadius * 2.0f * VOXEL_SIZE / 100.0f,
                Metrics.PlayerFitFraction, Metrics.TraversableComponentShare,
                static_cast<long long>(Metrics.NumPlayerFitCells),
                Metrics.MinimumPlayerClearanceVoxels,
                Metrics.MinimumPlayerClearanceVoxels * VOXEL_SIZE / 100.0f,
                VF_ShowcaseConnectivityName(Law.Result));
        }
        return true;
    }

    struct FIslandVerticalAudit
    {
        int32 NumIslands = 0;
        int32 NumTopTouching = 0;
        int32 NumBottomTouching = 0;
        TArray<float> GapsAbove;
        TArray<float> GapsBelow;
    };

    bool VF_AuditFloatingIslandVerticalExtents(
        const FShowcaseSample& Sample,
        int32 SourceSeed,
        FIslandVerticalAudit& OutAudit)
    {
        OutAudit = FIslandVerticalAudit();
        if (Sample.Candidate.Archetype != ECaveGeneratorType::FloatingIslands
            || !Sample.Metrics.bValid)
        {
            return false;
        }

        const FFloatingIslandParams& Params = Sample.Candidate.ArchetypeParams.FloatingIslandParams;
        const float TopWorldZ = static_cast<float>(Sample.TopVoxelZ) + 1.0f;
        const float BottomWorldZ = static_cast<float>(Sample.BottomVoxelZ);
        const float H = TopWorldZ - BottomWorldZ;
        const float Spacing = FMath::Max(Params.IslandSpacing, 1.0f);
        const float Seal = VF_ShowcaseBoundarySeal(
            Sample.Candidate.ArchetypeParams, Sample.Candidate.Archetype);
        if (!FMath::IsFinite(H) || H <= 0.0f || !FMath::IsFinite(Spacing)
            || !FMath::IsFinite(Seal) || Seal < 0.0f)
        {
            return false;
        }

        const float MinX = Sample.Metrics.SampledMinX;
        const float MaxX = Sample.Metrics.SampledMaxX;
        const float MinY = Sample.Metrics.SampledMinY;
        const float MaxY = Sample.Metrics.SampledMaxY;
        const int32 MinCellX = FMath::FloorToInt(MinX / Spacing) - 1;
        const int32 MaxCellX = FMath::CeilToInt(MaxX / Spacing) + 1;
        const int32 MinCellY = FMath::FloorToInt(MinY / Spacing) - 1;
        const int32 MaxCellY = FMath::CeilToInt(MaxY / Spacing) + 1;
        const int64 CellCount = static_cast<int64>(MaxCellX - MinCellX + 1)
            * static_cast<int64>(MaxCellY - MinCellY + 1);
        constexpr int64 MaxAuditCells = 65536;
        if (CellCount <= 0 || CellCount > MaxAuditCells)
        {
            return false;
        }

        const uint32 SeedU = static_cast<uint32>(SourceSeed) ^ 0x49736C64u; // 'Isld'
        const float MidZ = 0.5f * (TopWorldZ + BottomWorldZ);
        for (int32 CellY = MinCellY; CellY <= MaxCellY; ++CellY)
        {
            for (int32 CellX = MinCellX; CellX <= MaxCellX; ++CellX)
            {
                const uint32 Hh = VoxelHash::Cell(CellX, CellY, SeedU);
                if (VoxelHash::ToFloat01(Hh) > Params.IslandDensity)
                {
                    continue;
                }
                const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x12345678u));
                const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x9ABCDEF0u));
                const float IslandX = (CellX + 0.15f + JX * 0.7f) * Spacing;
                const float IslandY = (CellY + 0.15f + JY * 0.7f) * Spacing;
                if (IslandX < MinX || IslandX >= MaxX || IslandY < MinY || IslandY >= MaxY)
                {
                    continue;
                }

                const float Radius = FMath::Lerp(
                    Params.IslandMinRadius, Params.IslandMaxRadius,
                    VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x5A5Au)));
                const float TopHalf = Radius * 0.20f;
                const float UnderDepth = Radius * FMath::Max(Params.ThicknessRatio, 0.25f);
                const float SpreadZ = FMath::Max(
                    H * 0.5f - FMath::Max(TopHalf, UnderDepth) - Seal, 0.0f)
                    * Params.VerticalJitter;
                const float CenterZ = MidZ + VoxelHash::ToFloatSigned(
                    VoxelHash::Mix(Hh ^ 0xB17Du)) * SpreadZ;
                const float IslandTop = CenterZ + TopHalf;
                const float IslandBottom = CenterZ - UnderDepth;
                ++OutAudit.NumIslands;
                if (IslandTop >= TopWorldZ - Seal)
                {
                    ++OutAudit.NumTopTouching;
                }
                if (IslandBottom <= BottomWorldZ + Seal)
                {
                    ++OutAudit.NumBottomTouching;
                }
                OutAudit.GapsAbove.Add(FMath::Max(
                    0.0f, TopWorldZ - Seal - IslandTop));
                OutAudit.GapsBelow.Add(FMath::Max(
                    0.0f, IslandBottom - (BottomWorldZ + Seal)));
            }
        }
        return OutAudit.NumIslands > 0;
    }

    FString VF_FormatFloatingIslandReport(
        const TArray<FShowcaseSample>& Samples,
        int32 SourceSeed)
    {
        for (const FShowcaseSample& Sample : Samples)
        {
            if (Sample.Candidate.Archetype != ECaveGeneratorType::FloatingIslands)
            {
                continue;
            }
            FIslandVerticalAudit Audit;
            if (!VF_AuditFloatingIslandVerticalExtents(Sample, SourceSeed, Audit))
            {
                return TEXT("Part C FloatingIslands vertical audit unavailable for the previous candidate.");
            }
            const float TopFraction = static_cast<float>(Audit.NumTopTouching)
                / static_cast<float>(Audit.NumIslands);
            const float BottomFraction = static_cast<float>(Audit.NumBottomTouching)
                / static_cast<float>(Audit.NumIslands);
            const FFloatingIslandParams& P = Sample.Candidate.ArchetypeParams.FloatingIslandParams;
            const float H = static_cast<float>(Sample.TopVoxelZ + 1 - Sample.BottomVoxelZ);
            const float MaxVerticalHalfExtent = FMath::Max(
                0.20f * P.IslandMaxRadius,
                P.IslandMaxRadius * FMath::Max(P.ThicknessRatio, 0.25f));
            const float AvailableCenterHalfSpan = FMath::Max(
                0.0f, H * 0.5f - MaxVerticalHalfExtent
                    - VF_ShowcaseBoundarySeal(Sample.Candidate.ArchetypeParams,
                                               Sample.Candidate.Archetype));
            const float Jitter = FMath::Clamp(P.VerticalJitter, 0.0f, 1.0f);
            const float CurrentWorstNominalGap = FMath::Max(
                0.0f, AvailableCenterHalfSpan * (1.0f - Jitter));
            const int32 CurrentHeightChunks = FMath::Max(
                1, FMath::RoundToInt(H / static_cast<float>(CHUNK_SIZE)));
            constexpr float AuthoredMinRadius = 18.0f;
            constexpr float AuthoredMaxRadius = 42.0f;
            const float AuthoredMaxVerticalHalfExtent = FMath::Max(
                0.20f * AuthoredMaxRadius,
                AuthoredMaxRadius * FMath::Max(P.ThicknessRatio, 0.25f));
            const float AuthoredAvailableCenterHalfSpan = FMath::Max(
                0.0f, H * 0.5f - AuthoredMaxVerticalHalfExtent
                    - VF_ShowcaseBoundarySeal(Sample.Candidate.ArchetypeParams,
                                               Sample.Candidate.Archetype));
            const int32 RecommendedHeightChunks = FMath::Max(CurrentHeightChunks + 1, 6);
            const float RecommendedH = RecommendedHeightChunks * static_cast<float>(CHUNK_SIZE);
            const float AuthoredRecommendedAvailableCenterHalfSpan = FMath::Max(
                0.0f, RecommendedH * 0.5f - AuthoredMaxVerticalHalfExtent
                    - VF_ShowcaseBoundarySeal(Sample.Candidate.ArchetypeParams,
                                               Sample.Candidate.Archetype));
            const float AuthoredRecommendedWorstNominalGap = FMath::Max(
                0.0f, AuthoredRecommendedAvailableCenterHalfSpan * (1.0f - Jitter));
            constexpr float RecommendedNominalGap = 16.0f;
            const float ObservedRequiredHeight = 2.0f * (
                VF_ShowcaseBoundarySeal(Sample.Candidate.ArchetypeParams,
                                         Sample.Candidate.Archetype)
                + MaxVerticalHalfExtent
                + RecommendedNominalGap / FMath::Max(1.0f - Jitter, 0.01f));
            const int32 ObservedRequiredHeightChunks = FMath::Max(
                CurrentHeightChunks,
                FMath::CeilToInt(ObservedRequiredHeight / static_cast<float>(CHUNK_SIZE)));
            return FString::Printf(
                TEXT("Part C FloatingIslands structural audit (coarse XY window, %d nominal "
                     "islands; extents before surface roughness/merging): top-seal contact "
                     "%.6f (%d/%d), bottom-seal contact %.6f (%d/%d), median clear air gap "
                     "above %.3f voxels, below %.3f voxels. H=%d voxels=%d chunks, "
                     "IslandRadius=%.1f..%.1f, ThicknessRatio=%.3f, VerticalJitter=%.3f, "
                     "seal=%.1f; max-radius available centre half-span=%.3f, jittered "
                     "half-span=%.3f voxels; worst-case nominal seal gap=%.3f. "
                     "Authored radius reference is %.1f..%.1f: at current H its available "
                     "centre half-span is %.3f. Recommendation (not applied): restore/limit "
                     "IslandMinRadius/IslandMaxRadius to %.1f..%.1f, then raise "
                     "StrateHeightInChunks %d -> %d (H %.0f -> %.0f) for %.3f voxels "
                     "worst-case nominal gap; if the observed max radius is retained, height "
                     "needs at least %d chunks for a %.0f-voxel nominal gap. A ThicknessRatio "
                     "of <= 0.40 further separates authored-size plates but makes them thinner."),
                Audit.NumIslands, TopFraction, Audit.NumTopTouching, Audit.NumIslands,
                BottomFraction, Audit.NumBottomTouching, Audit.NumIslands,
                VF_MedianFloat(Audit.GapsAbove), VF_MedianFloat(Audit.GapsBelow),
                static_cast<int32>(H), static_cast<int32>(H / static_cast<float>(CHUNK_SIZE)),
                P.IslandMinRadius, P.IslandMaxRadius, P.ThicknessRatio, P.VerticalJitter,
                VF_ShowcaseBoundarySeal(Sample.Candidate.ArchetypeParams,
                                        Sample.Candidate.Archetype),
                AvailableCenterHalfSpan, AvailableCenterHalfSpan * P.VerticalJitter,
                CurrentWorstNominalGap, AuthoredMinRadius, AuthoredMaxRadius,
                AuthoredAvailableCenterHalfSpan, AuthoredMinRadius, AuthoredMaxRadius,
                CurrentHeightChunks, RecommendedHeightChunks, H, RecommendedH,
                AuthoredRecommendedWorstNominalGap, ObservedRequiredHeightChunks,
                RecommendedNominalGap);
        }
        return TEXT("Part C FloatingIslands vertical audit unavailable: previous candidate was not found.");
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
            && A.bPlayerFitResolved == B.bPlayerFitResolved
            && A.PlayerFitRefusalReason == B.PlayerFitRefusalReason
            && A.NumPlayerFitCells == B.NumPlayerFitCells
            && A.PlayerFitFraction == B.PlayerFitFraction
            && A.NumTraversableComponents == B.NumTraversableComponents
            && A.LargestTraversableComponentCells == B.LargestTraversableComponentCells
            && A.TraversableComponentShare == B.TraversableComponentShare
            && A.MinimumPlayerClearanceVoxels == B.MinimumPlayerClearanceVoxels
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
    // The shared fixture defaults to a bounded 4-chunk volume for fast density tests. The
    // owner-facing scale audit must exercise the production/default 8-chunk envelope.
    World.Build(BaseComposerSeed, 2, true, 8);
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
            VF_AddMeasuredSample(Stats[ArchetypeIndex], Sample);

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
                    VF_AddMeasuredSample(Stats[ArchetypeIndex], Sample);
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
            TEXT("Showcase search detail %s: eligible=%d measured=%d hard_gate_survivors=%d last_failure=%s"),
            VF_GetStrateArchetypeName(GShowcaseArchetypes[ArchetypeIndex]),
            Stats[ArchetypeIndex].EvaluatedCandidates,
            Stats[ArchetypeIndex].MeasuredCandidates,
            Stats[ArchetypeIndex].Survivors.Num(),
             Stats[ArchetypeIndex].LastFailure.IsEmpty()
                ? TEXT("none") : *Stats[ArchetypeIndex].LastFailure));
    }
    TArray<FShowcaseSample> FreshSurvivorSamples;
    for (const FShowcaseStats& ArchetypeStats : Stats)
    {
        if (ArchetypeStats.Best.IsSet())
        {
            FreshSurvivorSamples.Add(ArchetypeStats.Best.GetValue());
        }
    }
    AddInfo(VF_FormatPlayerFitSummary(
        FreshSurvivorSamples, TEXT("AFTER defaults: fresh hard-gate survivors")));

    TArray<FShowcaseSample> FreshDiagnosticSamples;
    FreshDiagnosticSamples.Reserve(UE_ARRAY_COUNT(GShowcaseArchetypes));
    for (const FShowcaseStats& ArchetypeStats : Stats)
    {
        if (!ArchetypeStats.BestMeasured.IsSet())
        {
            continue;
        }
        const FShowcaseSample Measured = ArchetypeStats.BestMeasured.GetValue();
        const FVoxelStrateComposerCandidate DiagnosticCandidate = Measured.Candidate;
        FShowcaseSample Diagnostic;
        FString DiagnosticFailure;
        // Re-run the selected coarse measurement with the player-fit pass enabled even when a
        // legacy gate fails. This is diagnostic only: it never enters Survivors or changes card
        // selection, but it gives every archetype an honest before/after scale row.
        VF_EvaluateShowcaseCandidate(
            World, DiagnosticCandidate, Measured.TargetStrateIndex, MeasureSettings,
            Diagnostic, &DiagnosticFailure, true);
        if (Diagnostic.Candidate.bValid && Diagnostic.Metrics.NumSampled > 0)
        {
            FreshDiagnosticSamples.Add(MoveTemp(Diagnostic));
        }
    }
    AddInfo(VF_FormatPlayerFitSummary(
        FreshDiagnosticSamples,
        TEXT("AFTER defaults: best measured candidate per archetype (diagnostic; hard gates separate)")));

    TArray<FShowcaseSample> PreviewSelections;
    PreviewSelections.Reserve(UE_ARRAY_COUNT(GShowcaseArchetypes));
    bool bHaveAllPreviewSelections = true;
    for (int32 ArchetypeIndex = 0;
         ArchetypeIndex < UE_ARRAY_COUNT(GShowcaseArchetypes);
         ++ArchetypeIndex)
    {
        if (Stats[ArchetypeIndex].Best.IsSet())
        {
            PreviewSelections.Add(Stats[ArchetypeIndex].Best.GetValue());
            continue;
        }
        const FShowcaseSample* Diagnostic = FreshDiagnosticSamples.FindByPredicate(
            [Archetype = GShowcaseArchetypes[ArchetypeIndex]](const FShowcaseSample& Sample)
            {
                return Sample.Candidate.Archetype == Archetype;
            });
        if (Diagnostic == nullptr)
        {
            bHaveAllPreviewSelections = false;
            break;
        }
        PreviewSelections.Add(*Diagnostic);
    }

    struct FPreviousShowcaseSelection
    {
        ECaveGeneratorType Archetype;
        int32 Seed;
        int32 CandidateIndex;
        int32 TargetStrateIndex;
    };
    const FPreviousShowcaseSelection PreviousSelections[] = {
        { ECaveGeneratorType::CrystalChamber, 7331, 34, VoxelForgeTest::FTestWorld::SlotFlatPlain },
        { ECaveGeneratorType::FlatPlain, 0, 4, VoxelForgeTest::FTestWorld::SlotFlatPlain },
        { ECaveGeneratorType::FloatingIslands, 0, 34, VoxelForgeTest::FTestWorld::SlotFlatPlain },
        { ECaveGeneratorType::Maze, 0, 58, VoxelForgeTest::FTestWorld::SlotFlatPlain },
        { ECaveGeneratorType::SurfaceWorld, 0, 11, VoxelForgeTest::FTestWorld::SlotSurfaceWorld },
        { ECaveGeneratorType::TunnelNetwork, 0, 61, VoxelForgeTest::FTestWorld::SlotFlatPlain },
        { ECaveGeneratorType::Underwater, 0, 46, VoxelForgeTest::FTestWorld::SlotFlatPlain },
        { ECaveGeneratorType::VerticalShafts, 0, 15, VoxelForgeTest::FTestWorld::SlotFlatPlain },
    };
    TArray<FShowcaseSample> PreviousSamples;
    PreviousSamples.Reserve(UE_ARRAY_COUNT(PreviousSelections));
    int32 NumPreviousShowcaseRejected = 0;
    for (const FPreviousShowcaseSelection& Previous : PreviousSelections)
    {
        const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
            Corpus, Previous.Seed, Previous.CandidateIndex, false);
        FShowcaseSample Sample;
        FString FailureReason;
        const bool bPassedNewGate = Candidate.bValid
            && Candidate.Archetype == Previous.Archetype
            && VF_EvaluateShowcaseCandidate(
                World, Candidate, Previous.TargetStrateIndex, MeasureSettings,
                Sample, &FailureReason, true);
        if (!bPassedNewGate)
        {
            ++NumPreviousShowcaseRejected;
        }
        if (Sample.Candidate.bValid && Sample.Metrics.bValid)
        {
            PreviousSamples.Add(Sample);
        }
        const FString FineSummary = Sample.FineMetrics.bPlayerFitResolved
            ? FString::Printf(
                TEXT("fit_step1=%.6f traversable_step1=%.6f fit_cells_step1=%lld "
                     "components_step1=%d largest_component_cells_step1=%lld "
                     "arrival_component_cells_step1=%lld departure_component_cells_step1=%lld "
                     "mouth_component_gap_step1=%.3f min_player_clearance_step1=%.2f "
                     "restricted_law_step1=%s; coarse_step4_walkable=%.6f "
                     "coarse_step4_clearance=%d voxels"),
                Sample.FineMetrics.PlayerFitFraction,
                Sample.FineMetrics.TraversableComponentShare,
                static_cast<long long>(Sample.FineMetrics.NumPlayerFitCells),
                Sample.FineMetrics.NumTraversableComponents,
                static_cast<long long>(Sample.FineMetrics.LargestTraversableComponentCells),
                static_cast<long long>(Sample.PlayerFitLaw.StartComponentCells),
                static_cast<long long>(Sample.PlayerFitLaw.GoalComponentCells),
                Sample.PlayerFitLaw.StartToGoalComponentDistanceCells,
                Sample.FineMetrics.MinimumPlayerClearanceVoxels,
                VF_ShowcaseConnectivityName(Sample.PlayerFitLaw.Result),
                Sample.Metrics.WalkableFraction,
                Sample.Metrics.MedianVerticalClearance)
            : FString::Printf(
                TEXT("fit_step1=UNRESOLVED reason=%s; coarse_step4_walkable=%.6f "
                     "coarse_step4_clearance=%d voxels"),
                Sample.FineMetrics.PlayerFitRefusalReason.IsEmpty()
                    ? TEXT("not measured") : *Sample.FineMetrics.PlayerFitRefusalReason,
                Sample.Metrics.WalkableFraction,
                Sample.Metrics.MedianVerticalClearance);
        AddInfo(FString::Printf(
            TEXT("previous showcase %s seed=%d index=%d: %s; coarse_step4_walkable=%.6f "
                 "coarse_step4_floor_area=%.6f coarse_step4_largest_surface=%.6f "
                 "coarse_step4_law=%s; %s%s"),
            VF_GetStrateArchetypeName(Previous.Archetype), Previous.Seed,
            Previous.CandidateIndex,
            bPassedNewGate ? TEXT("ACCEPTED by player-fit gate") : TEXT("REJECTED by player-fit gate"),
            Sample.Metrics.WalkableFraction,
            Sample.Metrics.WalkableFloorAreaFraction,
            Sample.Metrics.LargestWalkableSurfaceShare,
            VF_ShowcaseConnectivityName(Sample.LegacyLaw.Result),
            *FineSummary,
            (!bPassedNewGate && !FailureReason.IsEmpty())
                ? *FString::Printf(TEXT(" failure=(%s)"), *FailureReason) : TEXT("")));
    }
    AddInfo(FString::Printf(
        TEXT("Player capsule: radius %.2f voxels (%.0f cm / %.0f cm per voxel), "
             "half-height %.2f voxels (%.0f cm / %.0f cm per voxel), full height %.2f voxels; "
             "player-fit is resolved only at fine SampleStep=1."),
        FVoxelPlayerCapsuleConstants::RadiusVoxels,
        FVoxelPlayerCapsuleConstants::RadiusCentimeters,
        FVoxelPlayerCapsuleConstants::VoxelSizeCentimeters,
        FVoxelPlayerCapsuleConstants::HalfHeightVoxels,
        FVoxelPlayerCapsuleConstants::HalfHeightCentimeters,
        FVoxelPlayerCapsuleConstants::VoxelSizeCentimeters,
        FVoxelPlayerCapsuleConstants::HeightVoxels));
    AddInfo(FString::Printf(
        TEXT("Previous eight showcase candidates rejected by the new player-fit gate: %d/8."),
        NumPreviousShowcaseRejected));
    AddInfo(VF_FormatPlayerFitSummary(
        PreviousSamples, TEXT("AFTER defaults: historical candidate re-measurement")));
    AddInfo(VF_FormatWorldScaleAudit());
    AddInfo(VF_FormatDefaultRoughnessRatios());
    AddInfo(VF_FormatRoughnessRadiusReport(PreviousSamples));
    FString MazeRoughnessReport;
    if (const FShowcaseSample* MazeSample = PreviousSamples.FindByPredicate(
            [](const FShowcaseSample& Sample)
            {
                return Sample.Candidate.Archetype == ECaveGeneratorType::Maze;
            }))
    {
        VF_RunMazeRoughnessExperiment(
            World, *MazeSample, MeasureSettings, FinePreviewSettings, MazeRoughnessReport);
        FString MazeRadiusReport;
        VF_RunMazeCorridorRadiusSweep(
            World, *MazeSample, MeasureSettings, FinePreviewSettings, MazeRadiusReport);
        AddInfo(MazeRadiusReport);
    }
    else
    {
        MazeRoughnessReport = TEXT(
            "Part B Maze roughness experiment unavailable: previous Maze metrics were not measurable.");
    }
    AddInfo(MazeRoughnessReport);
    AddInfo(VF_FormatFloatingIslandReport(PreviousSamples, World.Generator->Seed));

    for (int32 ArchetypeIndex = 0;
         ArchetypeIndex < UE_ARRAY_COUNT(GShowcaseArchetypes);
         ++ArchetypeIndex)
    {
        const FShowcaseStats& ArchetypeStats = Stats[ArchetypeIndex];
        if (!ArchetypeStats.Best.IsSet())
        {
            AddInfo(FString::Printf(
                TEXT("%s: no candidate survived the unchanged legacy gates plus the new "
                     "player-fit gate in the bounded search."),
                VF_GetStrateArchetypeName(GShowcaseArchetypes[ArchetypeIndex])));
        }
    }
    if (!bHaveAllArchetypes)
    {
        AddInfo(TEXT("No complete eight-archetype hard-gate survivor set was found; the fresh "
                      "preview cards below are rejected diagnostic candidates for scale review."));
    }
    if (!bHaveAllPreviewSelections)
    {
        AddInfo(TEXT("Fresh diagnostic previews are incomplete because at least one archetype had "
                      "no measurable candidate."));
        return true;
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
        const FShowcaseSample& Selected = PreviewSelections[ArchetypeIndex];
        const FVoxelStrateComposerCandidate& Candidate = Selected.Candidate;
        const bool bDiagnosticCard = !bHaveAllArchetypes;

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
            Selected.SelectionScore, Selected.LawText, bDiagnosticCard,
            bDiagnosticCard
                ? TEXT("Diagnostic only: no complete hard-gate survivor set; this card is retained "
                       "to inspect the generated scale and exact player-fit law.")
                : TEXT(""),
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

        FVector FineArrivalPoint = FVector::ZeroVector;
        FVector FineDeparturePoint = FVector::ZeroVector;
        TestTrue(FString::Printf(TEXT("selected %s has the deterministic mouth pair for fine ROI"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 VF_GetShowcaseMouthPair(
                     World, Selected.TargetStrateIndex, FineArrivalPoint, FineDeparturePoint));
        const FVoxelStrateMeasureSettings FineMeasureSettings =
            VF_MakeFinePlayerFitSettings(
                MeasureSettings, FinePreviewSettings, FineArrivalPoint, FineDeparturePoint);
        FVoxelStrateSampleGrid FineGrid;
        const FVoxelStrateMetrics FineMetrics = VF_MeasureStrateWithSampler(
            Sampler, Selected.BottomVoxelZ, Selected.TopVoxelZ + 1, Context.EdgeSealThickness,
            FineMeasureSettings, &FineGrid);
        FVoxelStrateMetrics FinePlayerMetrics;
        VF_DiagnosePlayerFitConnectivityWithSampler(
            Sampler, Selected.BottomVoxelZ, Selected.TopVoxelZ + 1,
            Context.EdgeSealThickness, FineArrivalPoint, FineDeparturePoint,
            FineMeasureSettings, &FinePlayerMetrics);
        const auto IsInsideFineXY = [&FineGrid](const FVector& Point)
        {
            return Point.X >= FineGrid.MinX && Point.X < FineGrid.MaxX
                && Point.Y >= FineGrid.MinY && Point.Y < FineGrid.MaxY;
        };
        TestTrue(FString::Printf(TEXT("selected %s fine ROI covers both mouth XY points"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 !FineMetrics.bValid
                     || (IsInsideFineXY(FineArrivalPoint)
                         && IsInsideFineXY(FineDeparturePoint)));
        TestTrue(FString::Printf(TEXT("selected %s fine ROI stays within its cap"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 !FineMetrics.bValid || FineGrid.CellCount <= FinePreviewMaxCells);
        TestTrue(FString::Printf(TEXT("selected %s fine player-fit metrics rerun identically"),
                                 VF_GetStrateArchetypeName(Candidate.Archetype)),
                 VF_ShowcaseMetricsEqual(Selected.FineMetrics, FinePlayerMetrics));
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
        PreviewCandidate.bPlayerFitResolved = FinePlayerMetrics.bPlayerFitResolved;
        PreviewCandidate.PlayerFitRefusalReason = FinePlayerMetrics.PlayerFitRefusalReason;
        PreviewCandidate.NumPlayerFitCells = FinePlayerMetrics.NumPlayerFitCells;
        PreviewCandidate.PlayerFitFraction = FinePlayerMetrics.PlayerFitFraction;
        PreviewCandidate.NumTraversableComponents = FinePlayerMetrics.NumTraversableComponents;
        PreviewCandidate.LargestTraversableComponentCells =
            FinePlayerMetrics.LargestTraversableComponentCells;
        PreviewCandidate.TraversableComponentShare = FinePlayerMetrics.TraversableComponentShare;
        PreviewCandidate.MinimumPlayerClearanceVoxels =
            FinePlayerMetrics.MinimumPlayerClearanceVoxels;
        if (FinePlayerMetrics.bValid)
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
        PreviewCandidate.SelectionReason = bDiagnosticCard
            ? FString::Printf(
                TEXT("Diagnostic only: the complete hard-gate set was not found. Legacy law=%s; "
                     "player-fit law=%s; selected by the same score "
                     "floor_area + 0.25*clamp(clearance/%d,0,1) + 0.10*largest_surface = %.6f; "
                     "applied to existing target slot %d."),
                VF_ShowcaseConnectivityName(Selected.LegacyLaw.Result),
                VF_ShowcaseConnectivityName(Selected.PlayerFitLaw.Result),
                static_cast<int32>(ClearanceNormalizationVoxels),
                Selected.SelectionScore,
                Selected.TargetStrateIndex)
            : FString::Printf(
                TEXT("Hard gates passed: non-vacuous, largest air component >= %.2f, and "
                     "player-fit arrival → departure connectivity (fine step=1; "
                     "fit cells=%lld). Selected by legacy score "
                     "floor_area + 0.25*clamp(clearance/%d,0,1) + 0.10*largest_surface = %.6f; "
                     "floor area is the primary walking signal. Applied to existing target slot %d."),
                LargestComponentSurvivalThreshold,
                static_cast<long long>(FineMetrics.NumPlayerFitCells),
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
        const FShowcaseSample& Selected = PreviewSelections[ArchetypeIndex];
        Summaries[ArchetypeIndex].Window = PreviewCandidates[ArchetypeIndex].Window;
        Summaries[ArchetypeIndex].WindowSummary = VF_FormatShowcaseSummaryWindow(
            Stats[ArchetypeIndex]);
        Summaries[ArchetypeIndex].PlayerFitFractionSummary = FString::Printf(
            TEXT("%.6f (step=1)"), Selected.FineMetrics.PlayerFitFraction);
        Summaries[ArchetypeIndex].TraversableComponentShareSummary = FString::Printf(
            TEXT("%.6f (step=1)"), Selected.FineMetrics.TraversableComponentShare);
        AddInfo(FString::Printf(
            TEXT("%s: hard-gate survivors=%d, selected seed=%d index=%d, "
                 "coarse_step4_walkable=%.6f, coarse_step4_floor_area=%.6f, "
                 "coarse_step4_clearance=%d voxels, coarse_step4_largest_surface=%.6f, "
                 "coarse_step4_law=%s, player_fit_fraction_step1=%.6f, "
                 "traversable_component_share_step1=%.6f, player_fit_cells_step1=%lld, "
                 "components_step1=%d, largest_component_cells_step1=%lld, "
                 "arrival_component_cells_step1=%lld, departure_component_cells_step1=%lld, "
                 "mouth_component_gap_voxels_step1=%.3f, player_clearance_step1=%.2f voxels, "
                 "restricted_law_step1=%s, window=%s"),
            VF_GetStrateArchetypeName(GShowcaseArchetypes[ArchetypeIndex]),
            Stats[ArchetypeIndex].Survivors.Num(),
            Selected.Candidate.Seed, Selected.Candidate.Index,
            Selected.Metrics.WalkableFraction,
            Selected.Metrics.WalkableFloorAreaFraction,
            Selected.Metrics.MedianVerticalClearance,
            Selected.Metrics.LargestWalkableSurfaceShare,
            VF_ShowcaseConnectivityName(Selected.LegacyLaw.Result),
            Selected.FineMetrics.PlayerFitFraction,
            Selected.FineMetrics.TraversableComponentShare,
            static_cast<long long>(Selected.FineMetrics.NumPlayerFitCells),
            Selected.FineMetrics.NumTraversableComponents,
            static_cast<long long>(Selected.FineMetrics.LargestTraversableComponentCells),
            static_cast<long long>(Selected.PlayerFitLaw.StartComponentCells),
            static_cast<long long>(Selected.PlayerFitLaw.GoalComponentCells),
            Selected.PlayerFitLaw.StartToGoalComponentDistanceCells,
            Selected.FineMetrics.MinimumPlayerClearanceVoxels,
            VF_ShowcaseConnectivityName(Selected.PlayerFitLaw.Result),
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
