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
    // The query and production density path use different seed identities for room graphs and
    // slabs. Keep the test's oracle explicit so a future seed change cannot silently inspect a
    // different room layout. / La requête et la densité utilisent des identités de seed différentes
    // pour les graphes de salles et les slabs ; garder l'oracle explicite évite une dérive silencieuse.
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
}

bool FVoxelForgePassageLandsInOpenSpaceTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    FTestWorld World;
    // Seed 4468 deliberately gives the supported Underwater destination a mouth near the centre
    // of a real hash room, while the old random lower reach places that same mouth well outside
    // the room's vertical extent. That makes disabling the Z aiming fail deterministically instead
    // of relying on a lucky seed.
    World.Build(/*InSeed=*/4468);
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
    int32 NumRingAirSamples = 0;
    int32 NumRingSamples = 0;
    TSet<uint8> FalseArchetypes;
    bool bAllAnswerableMouthRingsHaveAir = true;
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

        float SuggestedZ = 0.0f;
        const bool bCanAnswer = VF_SuggestOpenPointZ(
            Definition->GeneratorType,
            Definition->GenerationParams,
            Definition->SlabParams,
            QuerySeed,
            StrateTopZ,
            StrateBottomZ,
            Passage.LowerPoint.X,
            Passage.LowerPoint.Y,
            SuggestedZ);

        if (!bCanAnswer)
        {
            ++NumQueryFalse;
            FalseArchetypes.Add(static_cast<uint8>(Definition->GeneratorType));

            const bool bExpectedToBeSupported =
                Definition->GeneratorType == ECaveGeneratorType::TunnelNetwork
                || Definition->GeneratorType == ECaveGeneratorType::Underwater
                || Definition->GeneratorType == ECaveGeneratorType::FlatPlain
                || Definition->GeneratorType == ECaveGeneratorType::CrystalChamber;
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

        // Control-point endpoints are anchored (vertical wobble has a zero envelope at a mouth),
        // so the generated endpoint must be exactly the query result.
        // Les extrémités sont ancrées (la wobble verticale vaut zéro à une bouche) : l'endpoint
        // généré doit donc être exactement le résultat de la requête.
        // A mouth's sine envelope is intentionally zero; allow only the tiny float envelope
        // produced by evaluating that zero at PI, never a meaningful targeting miss.
        // L'enveloppe de bouche est intentionnellement nulle ; tolérer seulement l'infime erreur
        // flottante de son évaluation à PI, jamais un échec de visée significatif.
        if (!FMath::IsNearlyEqual(Passage.LowerPoint.Z, SuggestedZ, 0.01f))
        {
            bAllAnswerableEndpointsMatchQuery = false;
            AddError(FString::Printf(
                TEXT("Passage to strate %d (%s) missed its suggested open Z: endpoint %.9g, query %.9g."),
                Destination.StrateIndex,
                *ArchetypeName(Definition->GeneratorType),
                Passage.LowerPoint.Z,
                SuggestedZ));
        }

        // A center sample is VACUOUS: VF_ApplyPassageCarving deliberately makes the mouth air,
        // even when the destination archetype is solid there. Probe a lateral ring instead.
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
        TEXT("Passage open-space check: %d inter-strate passages, %d checked, %d/%d ring samples air, %d query-false (%d unique archetypes; %d unsupported, %d supported-but-no-point)."),
        NumInterStratePassages,
        NumChecked,
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

    return NumInterStratePassages > 0
        && NumChecked > 0
        && bAllAnswerableMouthRingsHaveAir
        && bAllAnswerableEndpointsMatchQuery;
}

#endif // WITH_DEV_AUTOMATION_TESTS
