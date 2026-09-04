// Tier 4a — corpus spread, deterministic parameter roll, and candidate measurement.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelSettings.h"
#include "VoxelStrateComposer.h"
#include "VoxelStrateMeasure.h"
#include "VoxelTerrainOpDefinition.h"

namespace
{
    constexpr int32 NumCandidates = 64;
    constexpr float LargestComponentSurvivalThreshold = 0.50f;
    constexpr int32 BoxChecksPerCandidate = 40;

    const TCHAR* VF_FieldKindName(EVoxelStrateFieldKind Kind)
    {
        switch (Kind)
        {
        case EVoxelStrateFieldKind::Continuous: return TEXT("float");
        case EVoxelStrateFieldKind::Integer:    return TEXT("int");
        case EVoxelStrateFieldKind::Boolean:    return TEXT("bool");
        case EVoxelStrateFieldKind::Enum:       return TEXT("enum");
        }
        return TEXT("unknown");
    }

    FString VF_FormatSpreadValue(double Value)
    {
        return FString::Printf(TEXT("%.9g"), Value);
    }

    FString VF_FormatClamp(const FVoxelStrateFieldSpread& Spread)
    {
        if (!Spread.bHasClampMin && !Spread.bHasClampMax)
        {
            return TEXT("-");
        }
        if (Spread.bHasClampMin && Spread.bHasClampMax)
        {
            return FString::Printf(TEXT("[%s,%s]"),
                                    *VF_FormatSpreadValue(Spread.ClampMin),
                                    *VF_FormatSpreadValue(Spread.ClampMax));
        }
        if (Spread.bHasClampMin)
        {
            return FString::Printf(TEXT("[%s,+inf)"),
                                    *VF_FormatSpreadValue(Spread.ClampMin));
        }
        return FString::Printf(TEXT("(-inf,%s]"),
                                *VF_FormatSpreadValue(Spread.ClampMax));
    }

    bool VF_IsProjectPackagePath(const FString& PackagePath)
    {
        return PackagePath == TEXT("/Game")
            || PackagePath.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase);
    }

    bool VF_IsIgnoredSavedCopy(const FString& PackagePath)
    {
        return PackagePath.Contains(TEXT("/Saved/Autosaves"), ESearchCase::IgnoreCase)
            || PackagePath.Contains(TEXT("/Saved/Cooked"), ESearchCase::IgnoreCase);
    }

    TArray<FString> VF_GetProjectAssetPaths(UClass* AssetClass)
    {
        TArray<FString> Paths;
        if (AssetClass == nullptr)
        {
            return Paths;
        }

        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Assets;
        AssetRegistryModule.Get().GetAssetsByClass(AssetClass->GetClassPathName(), Assets, true);

        TSet<FString> SeenPaths;
        for (const FAssetData& Asset : Assets)
        {
            const FString PackagePath = Asset.PackageName.ToString();
            const FString ObjectPath = Asset.GetObjectPathString();
            if (!VF_IsProjectPackagePath(PackagePath)
                || VF_IsIgnoredSavedCopy(PackagePath)
                || VF_IsIgnoredSavedCopy(ObjectPath)
                || SeenPaths.Contains(ObjectPath))
            {
                continue;
            }
            SeenPaths.Add(ObjectPath);
            Paths.Add(ObjectPath);
        }
        Paths.Sort();
        return Paths;
    }

    FString VF_FormatSettingsAudit(const UVoxelSettings* Settings)
    {
        TArray<FString> Paths;
        if (Settings != nullptr)
        {
            TSet<FString> UniquePaths;
            for (const TPair<int32, TSoftObjectPtr<UVoxelStrateDefinition>>& Pair : Settings->FixedStrates)
            {
                const FString Path = Pair.Value.ToSoftObjectPath().ToString();
                if (!Path.IsEmpty())
                {
                    UniquePaths.Add(Path);
                }
            }
            for (const TSoftObjectPtr<UVoxelStrateDefinition>& Reference : Settings->StratePool)
            {
                const FString Path = Reference.ToSoftObjectPath().ToString();
                if (!Path.IsEmpty())
                {
                    UniquePaths.Add(Path);
                }
            }
            for (const FString& Path : UniquePaths)
            {
                Paths.Add(Path);
            }
            Paths.Sort();
        }

        FString Result = FString::Printf(TEXT("Settings corpus audit: %d unique fixed/pool paths"),
                                          Paths.Num());
        for (const FString& Path : Paths)
        {
            Result += FString::Printf(TEXT(" [%s]"), *Path);
        }
        Result += TEXT("; these settings references are not the corpus source.");
        return Result;
    }

    bool VF_RollRuntimeZIsZero(const FVoxelStrateRollInfo& Roll)
    {
        switch (Roll.Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return Roll.ArchetypeParams.TunnelNetworkParams.StrateTopWorldZ == 0.0f
                && Roll.ArchetypeParams.TunnelNetworkParams.StrateBottomWorldZ == 0.0f;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return Roll.ArchetypeParams.SlabParams.StrateTopWorldZ == 0.0f
                && Roll.ArchetypeParams.SlabParams.StrateBottomWorldZ == 0.0f;
        case ECaveGeneratorType::Maze:
            return Roll.ArchetypeParams.MazeParams.StrateTopWorldZ == 0.0f
                && Roll.ArchetypeParams.MazeParams.StrateBottomWorldZ == 0.0f;
        case ECaveGeneratorType::SurfaceWorld:
            return Roll.ArchetypeParams.SurfaceParams.StrateTopWorldZ == 0.0f
                && Roll.ArchetypeParams.SurfaceParams.StrateBottomWorldZ == 0.0f;
        case ECaveGeneratorType::VerticalShafts:
            return Roll.ArchetypeParams.VerticalShaftParams.StrateTopWorldZ == 0.0f
                && Roll.ArchetypeParams.VerticalShaftParams.StrateBottomWorldZ == 0.0f;
        case ECaveGeneratorType::FloatingIslands:
            return Roll.ArchetypeParams.FloatingIslandParams.StrateTopWorldZ == 0.0f
                && Roll.ArchetypeParams.FloatingIslandParams.StrateBottomWorldZ == 0.0f;
        default:
            return false;
        }
    }

    bool VF_IsComposerDefault(const FVoxelStrateCorpusEntry& Entry)
    {
        return Entry.SourcePath.StartsWith(TEXT("/VoxelForge/ComposerDefaults/"),
                                           ESearchCase::IgnoreCase);
    }

    FString VF_FormatParents(const FVoxelStrateCorpus& Corpus,
                             const FVoxelStrateRollInfo& Roll)
    {
        FString Result;
        for (int32 ParentSlot = 0; ParentSlot < Roll.ParentEntryIndices.Num(); ++ParentSlot)
        {
            if (ParentSlot > 0)
            {
                Result += TEXT("+");
            }
            const int32 EntryIndex = Roll.ParentEntryIndices[ParentSlot];
            const FVoxelStrateCorpusEntry* Entry = Corpus.GetEntries().IsValidIndex(EntryIndex)
                ? &Corpus.GetEntries()[EntryIndex] : nullptr;
            if (Entry == nullptr)
            {
                Result += TEXT("(invalid)");
            }
            else
            {
                Result += Entry->SourceName;
                Result += FString::Printf(TEXT("@%.3f"), Roll.ParentWeights[ParentSlot]);
            }
        }
        return Result;
    }

    FString VF_ConnectivityName(EVoxelConnectivityResult Result)
    {
        switch (Result)
        {
        case EVoxelConnectivityResult::Connected:                  return TEXT("Connected");
        case EVoxelConnectivityResult::NotConnectedAtThisResolution:return TEXT("NotConnected");
        case EVoxelConnectivityResult::StartCellSolid:             return TEXT("StartCellSolid");
        case EVoxelConnectivityResult::GoalCellSolid:              return TEXT("GoalCellSolid");
        case EVoxelConnectivityResult::OutOfWindow:                return TEXT("OutOfWindow");
        case EVoxelConnectivityResult::CoarseLiedBudgetExhausted:  return TEXT("CoarseLiedBudget");
        }
        return TEXT("Unknown");
    }

    int32 VF_FloorDiv(int32 A, int32 B)
    {
        const int32 Q = A / B;
        const int32 R = A % B;
        return (R != 0 && (R < 0) != (B < 0)) ? Q - 1 : Q;
    }

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

    /**
     * Exercise the production ClassifyTile box verdict with a rolled candidate, then brute-force
     * the exact mesher lattice. A false AllSolid/AllAir result is the §6.2 hole this pass is meant
     * to expose. The report is aggregate: there is no per-sample logging.
     */
    FBoxVerdictReport VF_CheckRolledBoxVerdicts(
        const VoxelForgeTest::FTestWorld& World, int32 CandidateIndex, int32 StrateIndex)
    {
        FBoxVerdictReport Report;
        const UVoxelGenerator* Generator = World.Generator.Get();
        if (Generator == nullptr || !World.StrateManager->GetLayout().IsValidIndex(StrateIndex))
        {
            Report.FirstViolation = TEXT("fixture generator or target layout slot was invalid");
            return Report;
        }

        int32 TopVoxelZ = 0;
        int32 BottomVoxelZ = 0;
        if (!World.GetSlotVoxelZRange(StrateIndex, TopVoxelZ, BottomVoxelZ))
        {
            Report.FirstViolation = TEXT("target slot had no voxel Z range");
            return Report;
        }

        constexpr int32 Step = 1;
        constexpr int32 Cells = 8;
        constexpr int32 Extent = Step * Cells;
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
                FVector((float)(Origin.X - Step), (float)(Origin.Y - Step), (float)(Origin.Z - Step)),
                FVector((float)(Origin.X + (Cells + 1) * Step),
                        (float)(Origin.Y + (Cells + 1) * Step),
                        (float)(Origin.Z + (Cells + 1) * Step)));
            (void)Box; // The production API receives the equivalent origin/step/cell box.

            const EVoxelTileClass Verdict = Generator->ClassifyTile(Origin, Step, Cells);
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

            const int32 GridDim = Cells + 1;
            const bool bClaimsSolid = Verdict == EVoxelTileClass::AllSolid;
            bool bBoxBad = false;
            for (int32 GZ = -1; GZ <= GridDim; ++GZ)
            for (int32 GY = -1; GY <= GridDim; ++GY)
            for (int32 GX = -1; GX <= GridDim; ++GX)
            {
                const float X = (float)(Origin.X + GX * Step);
                const float Y = (float)(Origin.Y + GY * Step);
                const float Z = (float)(Origin.Z + GZ * Step);
                const float Density = Generator->GetDensityAt(X, Y, Z);
                ++Report.CheckedVoxels;

                // MC convention: negative = solid; zero belongs to the air side, exactly as the
                // existing ClassifyTile soundness test defines it.
                const bool bAgrees = bClaimsSolid ? (Density < 0.0f) : (Density >= 0.0f);
                if (!bAgrees)
                {
                    ++Report.Violations;
                    bBoxBad = true;
                    if (Report.FirstViolation.IsEmpty())
                    {
                        Report.FirstViolation = FString::Printf(
                            TEXT("candidate %d box %d origin (%d,%d,%d) said %s but density at "
                                 "(%.0f,%.0f,%.0f) was %.9g"),
                            CandidateIndex, BoxIndex, Origin.X, Origin.Y, Origin.Z,
                            bClaimsSolid ? TEXT("AllSolid") : TEXT("AllAir"),
                            X, Y, Z, Density);
                    }
                }
            }
            (void)bBoxBad;
        }
        return Report;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeComposerParameterRollTest,
    "VoxelForge.Composer.ParameterRoll",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeComposerParameterRollTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;
    (void)Parameters;

    const double TestStartSeconds = FPlatformTime::Seconds();
    const FString SettingsPath = TEXT("/Game/VoxelForge/DA_Settings.DA_Settings");
    UVoxelSettings* AuthoredSettings = LoadObject<UVoxelSettings>(nullptr, *SettingsPath);
    if (AuthoredSettings == nullptr)
    {
        AddError(FString::Printf(
            TEXT("Could not load the hand-authored VoxelSettings asset at %s; the corpus was not "
                 "constructed and no invented candidates may be substituted."), *SettingsPath));
        return false;
    }

    FVoxelStrateCorpus Corpus;
    FString CorpusReport;
    const double CorpusStartSeconds = FPlatformTime::Seconds();
    const bool bCorpusLoaded = Corpus.LoadFromAssetRegistry(CorpusReport);
    const double CorpusSeconds = FPlatformTime::Seconds() - CorpusStartSeconds;
    AddInfo(FString::Printf(TEXT("%s Load time %.3fs; contents hash 0x%08x; clamp metadata at "
                                 "build=%s, promised at cooked runtime=%s."),
                            *CorpusReport, CorpusSeconds, Corpus.GetContentsHash(),
                            Corpus.AreClampMetadataAvailableAtBuild() ? TEXT("yes") : TEXT("no"),
                            Corpus.AreClampMetadataAvailableAtRuntime() ? TEXT("yes") : TEXT("no")));
    AddInfo(VF_FormatSettingsAudit(AuthoredSettings));

    const TArray<FString> ProjectStrateAssets =
        VF_GetProjectAssetPaths(UVoxelStrateDefinition::StaticClass());
    const TArray<FString> ProjectTerrainOpAssets =
        VF_GetProjectAssetPaths(UVoxelTerrainOpDefinition::StaticClass());
    int32 NumProjectCorpusEntries = 0;
    int32 NumDefaultCorpusEntries = 0;
    FString CorpusMembership = TEXT("CORPUS MEMBERSHIP\n");
    for (const FVoxelStrateCorpusEntry& Entry : Corpus.GetEntries())
    {
        if (VF_IsComposerDefault(Entry))
        {
            ++NumDefaultCorpusEntries;
        }
        else
        {
            ++NumProjectCorpusEntries;
        }
        CorpusMembership += FString::Printf(TEXT("%s | %s | %s\n"),
                                             VF_IsComposerDefault(Entry) ? TEXT("default") : TEXT("project"),
                                             VF_GetStrateArchetypeName(Entry.Archetype),
                                             *Entry.SourcePath);
    }
    AddInfo(FString::Printf(
        TEXT("Corpus membership: %d project strate assets discovered, %d project vectors loaded, "
             "%d defaults, %d total members. Terrain-op Asset Registry count (not rolled in this "
             "task): %d."),
        ProjectStrateAssets.Num(), NumProjectCorpusEntries, NumDefaultCorpusEntries, Corpus.Num(),
        ProjectTerrainOpAssets.Num()));
    AddInfo(CorpusMembership);
    for (const ECaveGeneratorType Archetype : {
        ECaveGeneratorType::TunnelNetwork,
        ECaveGeneratorType::FlatPlain,
        ECaveGeneratorType::CrystalChamber,
        ECaveGeneratorType::Maze,
        ECaveGeneratorType::SurfaceWorld,
        ECaveGeneratorType::VerticalShafts,
        ECaveGeneratorType::FloatingIslands,
        ECaveGeneratorType::Underwater})
    {
        int32 NumDefaultsForArchetype = 0;
        for (const FVoxelStrateCorpusEntry& Entry : Corpus.GetEntries())
        {
            if (Entry.Archetype == Archetype && VF_IsComposerDefault(Entry))
            {
                ++NumDefaultsForArchetype;
            }
        }
        TestEqual(FString::Printf(TEXT("one hand-tuned default for %s"),
                                  VF_GetStrateArchetypeName(Archetype)),
                  NumDefaultsForArchetype, 1);
        TestTrue(FString::Printf(TEXT("%s has a same-archetype corpus group"),
                                 VF_GetStrateArchetypeName(Archetype)),
                 Corpus.NumForArchetype(Archetype) > 0);
    }
    TestEqual(TEXT("all project strate assets plus eight defaults are corpus members"),
              Corpus.Num(), ProjectStrateAssets.Num() + 8);

    FString SpreadTable = TEXT(
        "CORPUS SPREAD (population stddev; jitter range = ±15% of max-min; clamps are safety only)\n"
        "archetype | struct | field | kind | excluded | reflected | samples | min | max | mean | stddev | clamp\n");
    for (const FVoxelStrateFieldSpread& Spread : Corpus.GetFieldSpreads())
    {
        SpreadTable += FString::Printf(
            TEXT("%s | %s | %s | %s | %s | %s | %d | %s | %s | %s | %s | %s\n"),
            VF_GetStrateArchetypeName(Spread.Archetype),
            *Spread.ParamStructName,
            *Spread.FieldName,
            VF_FieldKindName(Spread.Kind),
            Spread.bExcluded ? TEXT("yes") : TEXT("no"),
            Spread.bReflected ? TEXT("yes") : TEXT("no"),
            Spread.SampleCount,
            *VF_FormatSpreadValue(Spread.Min),
            *VF_FormatSpreadValue(Spread.Max),
            *VF_FormatSpreadValue(Spread.Mean),
            *VF_FormatSpreadValue(Spread.StdDev),
            *VF_FormatClamp(Spread));
    }
    AddInfo(SpreadTable);

    FString ExclusionTable = TEXT("EXPLICIT COMPOSER EXCLUSIONS\nfield | reason\n");
    for (const FVoxelStrateFieldExclusion& Exclusion : FVoxelStrateCorpus::GetNonTunableFields())
    {
        ExclusionTable += FString::Printf(TEXT("%s | %s\n"),
                                           *Exclusion.FieldName, *Exclusion.Reason);
    }
    AddInfo(ExclusionTable);
    AddInfo(TEXT(
        "Bool policy: bTunnelsFlowTowardOrigin is inherited from the dominant weighted parent; "
        "int/enum SNAP fields use the native family blend and are not jittered. Parent selection "
        "is weight-1.0 (equal) because UVoxelStrateDefinition has no corpus-weight field; parents "
        "are selected only within one exact archetype group. FStrateGenerationParams terrain-op "
        "transport fields remain excluded; the sibling families' authored fields are rolled."));

    TestTrue(TEXT("the reflected parameter schema is covered or explicitly excluded"),
             Corpus.IsSchemaValid());
    TestTrue(TEXT("hand-authored corpus loaded at least one usable vector"), bCorpusLoaded);
    if (!bCorpusLoaded || !Corpus.IsValid())
    {
        AddError(FString::Printf(
            TEXT("Cannot run the 64-candidate measurement: %s"),
            Corpus.IsSchemaValid() ? TEXT("the Asset Registry exposed no usable strate vectors")
                                   : *Corpus.GetSchemaError()));
        return false;
    }

    FTestWorld World;
    World.Build(1337, 2, true);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }
    TestTrue(TEXT("WorldRadiusVoxels remains the required default 0"),
             World.Settings->WorldRadiusVoxels == 0.0f);

    constexpr int32 CandidateStrateIndex = FTestWorld::SlotFlatPlain;
    if (!World.Definitions.IsValidIndex(CandidateStrateIndex))
    {
        AddError(TEXT("The fixture has no candidate slot for ParameterRoll."));
        return false;
    }

    FVoxelStrateMeasureSettings MeasureSettings;
    MeasureSettings.SampleStep = 4;
    MeasureSettings.RadiusInVoxels = 256;
    MeasureSettings.CenterXY = FVector2D::ZeroVector;
    MeasureSettings.MaxCells = 8000000;
    MeasureSettings.MaxRouteRetries = 16;
    MeasureSettings.HeadroomCells = 2;
    MeasureSettings.InteriorMarginVoxels = -1;

    FString CandidateTable = TEXT(
        "candidate | archetype | parents | air | largest share | walkable | feature scale | arrival->departure\n");
    int32 NumSurvivors = 0;
    int32 NumNonVacuous = 0;
    int32 NumLargestEnough = 0;
    int32 NumLawPasses = 0;
    int32 NumRollFailures = 0;
    int32 TotalBoxMixed = 0;
    int32 TotalBoxAllSolid = 0;
    int32 TotalBoxAllAir = 0;
    int32 TotalBoxProved = 0;
    int64 TotalBoxCheckedVoxels = 0;
    int32 TotalBoxViolations = 0;
    TArray<int32> BoxViolationCandidates;
    FString FirstBoxViolation;

    const double RollAndMeasureStartSeconds = FPlatformTime::Seconds();
    for (int32 CandidateIndex = 0; CandidateIndex < NumCandidates; ++CandidateIndex)
    {
        const FVoxelStrateRollInfo Roll = VF_RollStrateParamsDetailed(
            Corpus, AuthoredSettings->Seed, CandidateIndex);
        TestTrue(FString::Printf(TEXT("candidate %d roll is valid"), CandidateIndex), Roll.bValid);

        FString ParentText = VF_FormatParents(Corpus, Roll);
        FString AirText = TEXT("invalid");
        FString LargestText = TEXT("invalid");
        FString WalkableText = TEXT("invalid");
        FString FeatureText = TEXT("invalid");
        FString LawText = TEXT("not measured");

        if (!Roll.bValid)
        {
            ++NumRollFailures;
            CandidateTable += FString::Printf(TEXT("%d | invalid | %s | invalid | invalid | invalid | invalid | %s\n"),
                                               CandidateIndex, *ParentText, *Roll.FailureReason);
            continue;
        }

        bool bParentsMatchArchetype = true;
        for (const int32 ParentIndex : Roll.ParentEntryIndices)
        {
            if (!Corpus.GetEntries().IsValidIndex(ParentIndex)
                || Corpus.GetEntries()[ParentIndex].Archetype != Roll.Archetype)
            {
                bParentsMatchArchetype = false;
                break;
            }
        }
        TestTrue(FString::Printf(TEXT("candidate %d parents stay within %s"),
                                 CandidateIndex, VF_GetStrateArchetypeName(Roll.Archetype)),
                 bParentsMatchArchetype);

        const FVoxelStrateRollInfo RepeatRoll = VF_RollStrateParamsDetailed(
            Corpus, AuthoredSettings->Seed, CandidateIndex);
        TestTrue(FString::Printf(TEXT("candidate %d is bit-identical on deterministic reroll"),
                                 CandidateIndex),
                 RepeatRoll.bValid && RepeatRoll.Archetype == Roll.Archetype
                     && VF_AreStrateArchetypeParamsBitIdentical(
                         RepeatRoll.ArchetypeParams, Roll.ArchetypeParams, Roll.Archetype)
                     && RepeatRoll.ParentEntryIndices == Roll.ParentEntryIndices
                     && RepeatRoll.ParentWeights == Roll.ParentWeights);

        // Put the invented vector into a transient fixture definition. This is an offline test
        // world only; the runtime manager/generation code and authored assets are untouched.
        UVoxelStrateDefinition* CandidateDefinition =
            World.Definitions[CandidateStrateIndex].Get();
        CandidateDefinition->GeneratorType = Roll.Archetype;
        CandidateDefinition->GenerationParams = Roll.ArchetypeParams.TunnelNetworkParams;
        CandidateDefinition->SlabParams = Roll.ArchetypeParams.SlabParams;
        CandidateDefinition->MazeParams = Roll.ArchetypeParams.MazeParams;
        CandidateDefinition->SurfaceParams = Roll.ArchetypeParams.SurfaceParams;
        CandidateDefinition->VerticalShaftParams = Roll.ArchetypeParams.VerticalShaftParams;
        CandidateDefinition->FloatingIslandParams = Roll.ArchetypeParams.FloatingIslandParams;
        CandidateDefinition->bUseOperatorStack = true;
        CandidateDefinition->TransitionType = EVoxelStrateTransition::Hard;
        World.Reinitialize();

        TestTrue(FString::Printf(TEXT("candidate %d keeps runtime Z out of the rolled vector"), CandidateIndex),
                 VF_RollRuntimeZIsZero(Roll));

        const FVoxelStrateMetrics Metrics = VF_MeasureStrate(
            *World.Generator, *World.StrateManager, CandidateStrateIndex, MeasureSettings);

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

        FVoxelConnectivityDiagnostics Law;
        if (ArrivalCount == 1 && DepartureCount == 1)
        {
            Law = VF_DiagnoseConnectivity(
                *World.Generator, *World.StrateManager, CandidateStrateIndex,
                ArrivalPoint, DeparturePoint, MeasureSettings);
            LawText = VF_ConnectivityName(Law.Result);
            if (Law.bStartSnapped || Law.bGoalSnapped)
            {
                LawText += TEXT(" (snapped)");
            }
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

        const FBoxVerdictReport BoxReport = VF_CheckRolledBoxVerdicts(
            World, CandidateIndex, CandidateStrateIndex);
        TotalBoxMixed += BoxReport.Mixed;
        TotalBoxAllSolid += BoxReport.AllSolid;
        TotalBoxAllAir += BoxReport.AllAir;
        TotalBoxProved += BoxReport.Proved;
        TotalBoxCheckedVoxels += BoxReport.CheckedVoxels;
        TotalBoxViolations += BoxReport.Violations;
        if (BoxReport.Violations > 0)
        {
            BoxViolationCandidates.Add(CandidateIndex);
            if (FirstBoxViolation.IsEmpty())
            {
                FirstBoxViolation = BoxReport.FirstViolation;
            }
        }

        CandidateTable += FString::Printf(
            TEXT("%d | %s | %s | %s | %s | %s | %s | %s%s\n"),
            CandidateIndex,
            VF_GetStrateArchetypeName(Roll.Archetype),
            *ParentText,
            *AirText,
            *LargestText,
            *WalkableText,
            *FeatureText,
            *LawText,
            Metrics.bValid ? TEXT("") : *FString::Printf(TEXT(" [%s]"), *Metrics.RefusalReason));

        // Keep the fact that this is a test fixture explicit and check the invariant after every
        // rebuild, so a future composer cannot accidentally start assigning the global world radius.
        TestTrue(FString::Printf(TEXT("candidate %d leaves WorldRadiusVoxels at 0"), CandidateIndex),
                 World.Settings->WorldRadiusVoxels == 0.0f);
    }
    const double RollAndMeasureSeconds = FPlatformTime::Seconds() - RollAndMeasureStartSeconds;

    AddInfo(CandidateTable);
    AddInfo(FString::Printf(
        TEXT("Survival: %d/%d (%.1f%%). Criteria: non-vacuous (valid, sampled, air>0, solid>0), "
             "largest component share >= %.2f, and exact unsnapped arrival->departure law "
             "Result=Connected. Intermediate counts: non-vacuous %d, largest threshold %d, law "
             "pass %d, roll failures %d. Roll+measure time %.3fs."),
        NumSurvivors, NumCandidates, 100.0f * (float)NumSurvivors / (float)NumCandidates,
        LargestComponentSurvivalThreshold, NumNonVacuous, NumLargestEnough, NumLawPasses,
        NumRollFailures, RollAndMeasureSeconds));

    AddInfo(FString::Printf(
        TEXT("Rolled §6.2 box verdict check: mixed=%d, AllSolid=%d, AllAir=%d, proved=%d, "
             "brute-force lattice samples=%lld, violations=%d; violating candidates=%s. "
             "A zero-proved run is reported as vacuous rather than silently treated as sound."),
        TotalBoxMixed, TotalBoxAllSolid, TotalBoxAllAir, TotalBoxProved,
        TotalBoxCheckedVoxels, TotalBoxViolations,
        BoxViolationCandidates.Num() == 0 ? TEXT("none") : TEXT("see first violation below")));
    if (!BoxViolationCandidates.IsEmpty())
    {
        AddError(FString::Printf(
            TEXT("ROLLED BOX-VERDICT VIOLATION: %d candidate(s) produced a false AllSolid/AllAir "
                 "claim; first candidate index %d. This closes the test red because §6.2 bounds "
                 "were not sound for the rolled parameter space. First violation: %s"),
            BoxViolationCandidates.Num(), BoxViolationCandidates[0], *FirstBoxViolation));
    }
    if (TotalBoxProved == 0)
    {
        AddWarning(TEXT(
            "VACUOUS BOX CHECK: no rolled candidate emitted an AllSolid/AllAir verdict over the "
            "40-box scan. This is not evidence of soundness; widen the scan or improve the bound "
            "before using it as a §6.2 result."));
    }

    TestEqual(TEXT("exactly 64 candidate rows were requested"), NumCandidates, 64);
    TestEqual(TEXT("rolled production box verdicts contain no false uniform claim"),
              TotalBoxViolations, 0);

    const double TestSeconds = FPlatformTime::Seconds() - TestStartSeconds;
    AddInfo(FString::Printf(TEXT("ParameterRoll total runtime %.3fs (corpus %.3fs, candidate "
                                 "roll/measurement %.3fs)."),
                            TestSeconds, CorpusSeconds, RollAndMeasureSeconds));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
