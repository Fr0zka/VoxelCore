// VoxelForgePassageOpenSpaceTest.cpp
// Vérifie que les bouches basses des passages ciblées tombent dans l'air de leur strate destination.
// Verifies that targeted lower passage mouths land in air in their destination strate.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelCaveMorphology.h"
#include "VoxelDensityPrimitives.h"
#include "VoxelForgeTestFixture.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgePassageLandsInOpenSpaceTest,
    "VoxelForge.Determinism.PassageLandsInOpenSpace",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    // The query and production density path use different seed identities for room graphs versus
    // the world-seeded lattice/grid/blob sources. Keep the test's oracle explicit so a future seed
    // change cannot silently inspect a different layout. / La requête et la densité utilisent des
    // identités de seed différentes pour les graphes de salles et les sources monde ; garder
    // l'oracle explicite évite une dérive silencieuse.
    int32 OpenPointSeedFor(
        const UVoxelStrateDefinition& Definition,
        const FStrateSlot& Destination,
        int32 WorldSeed)
    {
        const bool bUsesCaveRooms =
            Definition.GeneratorType == ECaveGeneratorType::TunnelNetwork
            || Definition.GeneratorType == ECaveGeneratorType::Underwater;
        return bUsesCaveRooms
            ? static_cast<int32>(VoxelCaveMorphology::MakeStrateSeed(
                static_cast<uint32>(WorldSeed), Destination.StrateIndex))
            : WorldSeed;
    }

    FString ArchetypeName(ECaveGeneratorType Archetype)
    {
        if (const UEnum* ArchetypeEnum = StaticEnum<ECaveGeneratorType>())
        {
            return ArchetypeEnum->GetNameStringByValue(static_cast<int64>(Archetype));
        }
        return FString::Printf(TEXT("Value_%d"), static_cast<int32>(Archetype));
    }

    float MaxLateralSnapFor(const UVoxelStrateDefinition& Definition)
    {
        switch (Definition.GeneratorType)
        {
        case ECaveGeneratorType::Maze:
            return FMath::Max(Definition.MazeParams.CellSize, 1.0f);
        case ECaveGeneratorType::VerticalShafts:
            return FMath::Max(Definition.VerticalShaftParams.ShaftSpacing, 1.0f);
        case ECaveGeneratorType::FloatingIslands:
            return FMath::Max(Definition.FloatingIslandParams.IslandSpacing, 1.0f);
        default:
            return 0.0f;
        }
    }
}

bool FVoxelForgePassageLandsInOpenSpaceTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    FTestWorld World;
    // Seed 11791 deliberately puts the passage mouths over a Maze corridor, a VerticalShafts
    // shaft, and a FloatingIslands blob. SurfaceWorld remains a deliberate query refusal because
    // its production height can be biome/context-selected by the manager.
    World.Build(/*InSeed=*/11791);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    const TArray<FStrateSlot>& Layout = World.StrateManager->GetLayout();
    const TArray<FVoxelPassage>& Passages = World.StrateManager->GetPassages();
    const int32 WorldSeed = World.Settings->Seed;

    int32 NumInterStratePassages = 0;
    int32 NumChecked = 0;
    int32 NumQueryFalse = 0;
    int32 NumUnsupported = 0;
    int32 NumSupportedWithoutPoint = 0;
    int32 NumFootingChecked = 0;
    int32 NumRingAirSamples = 0;
    int32 NumRingSamples = 0;
    TSet<uint8> FalseArchetypes;
    TSet<uint8> AnsweredArchetypes;
    bool bAllAnswerableMouthRingsHaveAir = true;
    bool bAllAnswerableFootingsAreValid = true;
    bool bAllAnswerableEndpointsMatchQuery = true;

    for (const FVoxelPassage& Passage : Passages)
    {
        // The surface entry is a same-strate shaft and is not an inter-strate destination query.
        // Le puits de surface relie la même strate ; ce n'est pas un passage inter-strates à tester.
        if (Passage.UpperStrateIndex == Passage.LowerStrateIndex)
        {
            continue;
        }

        ++NumInterStratePassages;
        if (!Layout.IsValidIndex(Passage.LowerStrateIndex))
        {
            AddError(FString::Printf(
                TEXT("Passage lower endpoint references invalid destination strate index %d."),
                Passage.LowerStrateIndex));
            continue;
        }

        const FStrateSlot& Destination = Layout[Passage.LowerStrateIndex];
        const UVoxelStrateDefinition* Definition = Destination.Definition;
        if (!Definition)
        {
            AddError(FString::Printf(
                TEXT("Passage destination strate %d has no definition."),
                Destination.StrateIndex));
            continue;
        }

        if (!Layout.IsValidIndex(Passage.UpperStrateIndex))
        {
            AddError(FString::Printf(
                TEXT("Passage to strate %d references invalid source strate index %d."),
                Destination.StrateIndex,
                Passage.UpperStrateIndex));
            continue;
        }

        const FStrateSlot& Source = Layout[Passage.UpperStrateIndex];
        const UVoxelStrateDefinition* SourceDefinition = Source.Definition;
        if (!SourceDefinition)
        {
            AddError(FString::Printf(
                TEXT("Passage source strate %d has no definition."),
                Source.StrateIndex));
            continue;
        }

        const FStratePassageConfig& PassageConfig = SourceDefinition->PassageConfig;
        if (!FMath::IsFinite(PassageConfig.MouthRadius) || PassageConfig.MouthRadius <= 0.0f)
        {
            AddError(FString::Printf(
                TEXT("Passage to strate %d (%s) has invalid mouth radius %.9g."),
                Destination.StrateIndex,
                *ArchetypeName(Definition->GeneratorType),
                PassageConfig.MouthRadius));
            continue;
        }

        const float StrateTopZ = (float)(Destination.TopChunkZ + 1) * CHUNK_SIZE;
        const float StrateBottomZ = (float)Destination.BottomChunkZ * CHUNK_SIZE;
        const int32 QuerySeed = OpenPointSeedFor(*Definition, Destination, WorldSeed);
        const FVector DesiredPoint = Passage.RequestedLowerPoint;
        const float MaxLateralSnap = MaxLateralSnapFor(*Definition);

        FVector SuggestedPoint = FVector::ZeroVector;
        const bool bCanAnswer = VF_SuggestLandingPoint(
            Definition->GeneratorType,
            Definition->GenerationParams,
            Definition->SlabParams,
            Definition->MazeParams,
            Definition->VerticalShaftParams,
            Definition->FloatingIslandParams,
            QuerySeed,
            StrateTopZ,
            StrateBottomZ,
            DesiredPoint.X,
            DesiredPoint.Y,
            MaxLateralSnap,
            SuggestedPoint);

        if (!bCanAnswer)
        {
            ++NumQueryFalse;
            FalseArchetypes.Add(static_cast<uint8>(Definition->GeneratorType));

            const bool bExpectedToBeSupported =
                Definition->GeneratorType == ECaveGeneratorType::TunnelNetwork
                || Definition->GeneratorType == ECaveGeneratorType::Underwater
                || Definition->GeneratorType == ECaveGeneratorType::FlatPlain
                || Definition->GeneratorType == ECaveGeneratorType::CrystalChamber
                || Definition->GeneratorType == ECaveGeneratorType::Maze
                || Definition->GeneratorType == ECaveGeneratorType::VerticalShafts
                || Definition->GeneratorType == ECaveGeneratorType::FloatingIslands;
            if (bExpectedToBeSupported)
            {
                ++NumSupportedWithoutPoint;
            }
            else
            {
                ++NumUnsupported;
            }
            continue;
        }

        ++NumChecked;
        AnsweredArchetypes.Add(static_cast<uint8>(Definition->GeneratorType));

        // Control-point endpoints are pinned to the complete query result, including any lateral
        // snap. A mismatch here means GeneratePassages used stale XY or stale Z downstream.
        if (!FMath::IsNearlyEqual(Passage.LowerPoint.X, SuggestedPoint.X, 0.01f)
            || !FMath::IsNearlyEqual(Passage.LowerPoint.Y, SuggestedPoint.Y, 0.01f)
            || !FMath::IsNearlyEqual(Passage.LowerPoint.Z, SuggestedPoint.Z, 0.01f))
        {
            bAllAnswerableEndpointsMatchQuery = false;
            AddError(FString::Printf(
                TEXT("Passage to strate %d (%s) missed its suggested landing point: endpoint (%.9g, %.9g, %.9g), query (%.9g, %.9g, %.9g)."),
                Destination.StrateIndex,
                *ArchetypeName(Definition->GeneratorType),
                Passage.LowerPoint.X,
                Passage.LowerPoint.Y,
                Passage.LowerPoint.Z,
                SuggestedPoint.X,
                SuggestedPoint.Y,
                SuggestedPoint.Z));
        }

        const float SnapDX = Passage.LowerPoint.X - DesiredPoint.X;
        const float SnapDY = Passage.LowerPoint.Y - DesiredPoint.Y;
        const float SnapDistance = FMath::Sqrt(SnapDX * SnapDX + SnapDY * SnapDY);
        if (!FMath::IsFinite(SnapDistance) || SnapDistance > MaxLateralSnap + 0.01f)
        {
            bAllAnswerableEndpointsMatchQuery = false;
            AddError(FString::Printf(
                TEXT("Passage to strate %d (%s) exceeded its lateral snap bound: %.6f > %.6f from requested (%.3f, %.3f)."),
                Destination.StrateIndex,
                *ArchetypeName(Definition->GeneratorType),
                SnapDistance,
                MaxLateralSnap,
                DesiredPoint.X,
                DesiredPoint.Y));
        }

        // A center sample is VACUOUS: VF_ApplyPassageCarving deliberately makes the mouth air,
        // even when the destination archetype is solid there. Thin lattice/shaft/blob sources
        // need a vertical footing probe against the source with the manager detached; their
        // open circumference is not represented by the generic ring below.
        const bool bNeedsFootingProbe =
            Definition->GeneratorType == ECaveGeneratorType::Maze
            || Definition->GeneratorType == ECaveGeneratorType::VerticalShafts
            || Definition->GeneratorType == ECaveGeneratorType::FloatingIslands;
        if (bNeedsFootingProbe)
        {
            ++NumFootingChecked;

            // The live world's passage modifier is intentionally absent here. The endpoint is
            // already known to be passage-carved, so this second generator checks the archetype
            // source itself: air at the landing and solid immediately below it.
            TStrongObjectPtr<UVoxelGenerator> SourceOnlyGenerator(
                NewObject<UVoxelGenerator>(GetTransientPackage(), NAME_None, RF_Transient));
            SourceOnlyGenerator->InitializeSettings(World.Settings.Get());

            float LandingDensity = 0.0f;
            float FootingDensity = 0.0f;
            switch (Definition->GeneratorType)
            {
            case ECaveGeneratorType::Maze:
                {
                    FMazeGenerationParams P = Definition->MazeParams;
                    P.StrateTopWorldZ = StrateTopZ;
                    P.StrateBottomWorldZ = StrateBottomZ;
                    LandingDensity = SourceOnlyGenerator->GetMazeDensity(
                        SuggestedPoint.X, SuggestedPoint.Y, SuggestedPoint.Z, P);
                    const float FloorProbeZ = SuggestedPoint.Z
                        - FMath::Max(P.CorridorRadius, 0.5f)
                        - P.SurfaceRoughness * VOXEL_NOISE_SCALE - 3.0f;
                    FootingDensity = SourceOnlyGenerator->GetMazeDensity(
                        SuggestedPoint.X, SuggestedPoint.Y, FloorProbeZ, P);
                    break;
                }

            case ECaveGeneratorType::VerticalShafts:
                {
                    FVerticalShaftParams P = Definition->VerticalShaftParams;
                    P.StrateTopWorldZ = StrateTopZ;
                    P.StrateBottomWorldZ = StrateBottomZ;
                    LandingDensity = SourceOnlyGenerator->GetVerticalShaftDensity(
                        SuggestedPoint.X, SuggestedPoint.Y, SuggestedPoint.Z, P);
                    const float FloorProbeZ = StrateBottomZ + P.BoundarySealThickness * 0.5f;
                    FootingDensity = SourceOnlyGenerator->GetVerticalShaftDensity(
                        SuggestedPoint.X, SuggestedPoint.Y, FloorProbeZ, P);
                    break;
                }

            case ECaveGeneratorType::FloatingIslands:
                {
                    FFloatingIslandParams P = Definition->FloatingIslandParams;
                    P.StrateTopWorldZ = StrateTopZ;
                    P.StrateBottomWorldZ = StrateBottomZ;
                    LandingDensity = SourceOnlyGenerator->GetFloatingIslandDensity(
                        SuggestedPoint.X, SuggestedPoint.Y, SuggestedPoint.Z, P);
                    FootingDensity = SourceOnlyGenerator->GetFloatingIslandDensity(
                        SuggestedPoint.X, SuggestedPoint.Y, SuggestedPoint.Z - 1.0f, P);
                    break;
                }

            default:
                break;
            }

            if (!FMath::IsFinite(LandingDensity) || !FMath::IsFinite(FootingDensity)
                || LandingDensity < 0.0f || FootingDensity >= 0.0f)
            {
                bAllAnswerableFootingsAreValid = false;
                AddError(FString::Printf(
                    TEXT("Passage to strate %d (%s) did not bracket source footing: landing density %.9g, below density %.9g at (%.3f, %.3f, %.3f)."),
                    Destination.StrateIndex,
                    *ArchetypeName(Definition->GeneratorType),
                    LandingDensity,
                    FootingDensity,
                    Passage.LowerPoint.X,
                    Passage.LowerPoint.Y,
                    SuggestedPoint.Z));
            }
            continue;
        }

        // A center sample is VACUOUS: VF_ApplyPassageCarving deliberately makes the mouth air,
        // even when the destination archetype is solid there. Probe a lateral ring for the
        // room/slab sources, where an open circumference is a meaningful footprint.
        // The ring is Cfg.MouthRadius + two complete PassageBlend bands (4 voxels each): the
        // first clears the passage's carve/blend reach, and the second is a safety margin beyond
        // EvaluateModifierSDF's 3-voxel junction smoothing. At this radius the passage itself
        // cannot supply the air being counted; a nearby destination room/void must do so.
        // Require 8 of 16 samples (50%): that is a meaningful open circumference for a mouth,
        // while a mouth in bedrock has zero air once its own tube is outside the ring. This is a
        // connectivity PROXY, not proof that the mouth is connected to the room: a disconnected
        // pocket can pass it. A real proof needs a flood fill (Tier 2).
        // Un échantillon au centre serait VACU : le carve structurel force la bouche à l'air.
        // L'anneau latéral doit donc trouver de l'air dans la strate de destination elle-même.
        constexpr int32 RingSampleCount = 16;
        constexpr int32 MinimumAirSamples = 8;
        const float RingRadius = PassageConfig.MouthRadius
            + 2.0f * VoxelDensityReach::PassageBlend;

        int32 AirSamples = 0;
        for (int32 RingSample = 0; RingSample < RingSampleCount; ++RingSample)
        {
            const float Angle = 2.0f * PI * (float)RingSample / (float)RingSampleCount;
            const float SampleX = Passage.LowerPoint.X + FMath::Cos(Angle) * RingRadius;
            const float SampleY = Passage.LowerPoint.Y + FMath::Sin(Angle) * RingRadius;
            const float Density = World.Generator->GetDensityAt(
                SampleX,
                SampleY,
                Passage.LowerPoint.Z);

            ++NumRingSamples;
            if (Density >= 0.0f)
            {
                ++AirSamples;
                ++NumRingAirSamples;
            }
        }

        if (AirSamples < MinimumAirSamples)
        {
            bAllAnswerableMouthRingsHaveAir = false;
            AddError(FString::Printf(
                TEXT("Passage to strate %d (%s) has too little destination air around its mouth: %d/%d ring samples at radius %.3f are air; mouth (%.3f, %.3f, %.3f)."),
                Destination.StrateIndex,
                *ArchetypeName(Definition->GeneratorType),
                AirSamples,
                RingSampleCount,
                RingRadius,
                Passage.LowerPoint.X,
                Passage.LowerPoint.Y,
                Passage.LowerPoint.Z));
        }
    }

    AddInfo(FString::Printf(
        TEXT("Passage open-space check: %d inter-strate passages, %d checked, %d footing checks, %d/%d room/slab ring samples air, %d query-false (%d unique archetypes; %d unsupported, %d supported-but-no-point)."),
        NumInterStratePassages,
        NumChecked,
        NumFootingChecked,
        NumRingAirSamples,
        NumRingSamples,
        NumQueryFalse,
        FalseArchetypes.Num(),
        NumUnsupported,
        NumSupportedWithoutPoint));

    // A test that inspected nothing is not evidence of the invariant. Fail loudly in both cases.
    // Un test qui n'a rien inspecté ne prouve pas l'invariant : échouer explicitement dans les deux cas.
    if (NumInterStratePassages == 0)
    {
        AddError(TEXT("VACUOUS: the fixture generated zero inter-strate passages."));
    }
    if (NumChecked == 0)
    {
        AddError(TEXT("VACUOUS: zero answerable passage mouths were checked; the passage invariant was not exercised."));
    }
    if (NumFootingChecked == 0)
    {
        AddError(TEXT("VACUOUS: no Maze/VerticalShafts/FloatingIslands footing was checked."));
    }

    const bool bAllNewFootingArchetypesAnswered =
        AnsweredArchetypes.Contains(static_cast<uint8>(ECaveGeneratorType::Maze))
        && AnsweredArchetypes.Contains(static_cast<uint8>(ECaveGeneratorType::VerticalShafts))
        && AnsweredArchetypes.Contains(static_cast<uint8>(ECaveGeneratorType::FloatingIslands));
    if (!bAllNewFootingArchetypesAnswered)
    {
        AddError(TEXT("The fixture did not exercise all three source-footing queries newly covered by Tier 1."));
    }

    // ⚠️ MESURE, PAS CONTRAT. Quels archétypes répondent DANS CE FIXTURE dépend de l'endroit où ses
    // 7 passages tombent : une requête supportée a le DROIT de décliner quand le site le plus proche
    // dépasse son budget latéral. C'est le comportement voulu — un refus honnête vaut mieux qu'une
    // réponse fausse et confiante. Assertion supprimée le 2026-08-30 : elle affirmait une propriété
    // de l'ÉCHANTILLON, pas du code, et échouait sur une CORRECTION (le budget salle passant de 0 à
    // RoomSpacing a rendu un atterrissage hors-salle honnêtement refusé).
    //
    // MEASUREMENT, NOT CONTRACT. Which archetypes answer IN THIS FIXTURE depends on where its 7
    // passages happen to fall: a supported query is ALLOWED to decline when the nearest site exceeds
    // its lateral budget. That is the intended behaviour — an honest refusal beats a confident wrong
    // answer. This assertion was removed 2026-08-30: it asserted a property of the SAMPLE rather
    // than of the code, and it failed on a FIX (raising the room budget from 0 to RoomSpacing turned
    // a previously-wrong outside-the-room landing into an honest refusal).
    //
    // The real risks remain hard failures: the vacuity guards below (zero passages, zero checks,
    // no footing exercised) and the per-mouth ring/footing assertions above. Same discipline the
    // codebase applies to box-verdict PROVED counts: read the count as a measurement, assert only
    // that nothing it reports is WRONG.
    TArray<FString> SupportedButUnexercised;
    const TPair<ECaveGeneratorType, const TCHAR*> PreviouslyCovered[] = {
        { ECaveGeneratorType::TunnelNetwork,  TEXT("TunnelNetwork")  },
        { ECaveGeneratorType::Underwater,     TEXT("Underwater")     },
        { ECaveGeneratorType::FlatPlain,      TEXT("FlatPlain")      },
        { ECaveGeneratorType::CrystalChamber, TEXT("CrystalChamber") },
    };
    for (const TPair<ECaveGeneratorType, const TCHAR*>& Entry : PreviouslyCovered)
    {
        if (!AnsweredArchetypes.Contains(static_cast<uint8>(Entry.Key)))
        {
            SupportedButUnexercised.Add(FString(Entry.Value));
        }
    }
    if (SupportedButUnexercised.Num() > 0)
    {
        AddInfo(FString::Printf(
            TEXT("Supported but unexercised at this fixture seed (declined on lateral budget, not a ")
            TEXT("failure): %s."),
            *FString::Join(SupportedButUnexercised, TEXT(", "))));
    }

    const bool bSurfaceIntentionallyRefused =
        FalseArchetypes.Contains(static_cast<uint8>(ECaveGeneratorType::SurfaceWorld));
    if (!bSurfaceIntentionallyRefused)
    {
        AddError(TEXT("SurfaceWorld unexpectedly answered a pure footing query; keep it refused until its biome/context contract is explicit."));
    }

    return NumInterStratePassages > 0
        && NumChecked > 0
        && NumFootingChecked > 0
        && bAllNewFootingArchetypesAnswered
        // (fixture coverage of the room/slab archetypes is reported, not asserted — see above)
        && bSurfaceIntentionallyRefused
        && bAllAnswerableFootingsAreValid
        && bAllAnswerableMouthRingsHaveAir
        && bAllAnswerableEndpointsMatchQuery;
}

#endif // WITH_DEV_AUTOMATION_TESTS
