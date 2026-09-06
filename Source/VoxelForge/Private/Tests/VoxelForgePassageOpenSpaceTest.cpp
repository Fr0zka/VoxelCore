// VoxelForgePassageOpenSpaceTest.cpp
// Vérifie que les bouches basses des passages ciblées tombent dans l'air de leur strate destination.
// Verifies that targeted lower passage mouths land in air in their destination strate.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
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
            // A 0.6 shaft density does not guarantee an occupied immediate cell. The production
            // query therefore permits a bounded two-cell local search and still returns the
            // selected site's deterministic feature core/ledge-side pose.
            return FMath::Max(2.0f * Definition.VerticalShaftParams.ShaftSpacing, 1.0f);
        case ECaveGeneratorType::FloatingIslands:
            return FMath::Max(Definition.FloatingIslandParams.IslandSpacing, 1.0f);
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return FMath::Max(Definition.GenerationParams.RoomSpacing, 1.0f);
        default:
            return 0.0f;
        }
    }

    float BoundarySealFor(const UVoxelStrateDefinition& Definition)
    {
        switch (Definition.GeneratorType)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return Definition.GenerationParams.BoundarySealThickness;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return Definition.SlabParams.BoundarySealThickness;
        case ECaveGeneratorType::Maze:
            return Definition.MazeParams.BoundarySealThickness;
        case ECaveGeneratorType::SurfaceWorld:
            return Definition.SurfaceParams.BoundarySealThickness;
        case ECaveGeneratorType::VerticalShafts:
            return Definition.VerticalShaftParams.BoundarySealThickness;
        case ECaveGeneratorType::FloatingIslands:
            return Definition.FloatingIslandParams.BoundarySealThickness;
        default:
            return 0.0f;
        }
    }
}

bool FVoxelForgePassageLandsInOpenSpaceTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    FTestWorld World;
    // Seed 12345 deliberately puts the passage mouths over a Maze corridor, a VerticalShafts
    // shaft, and a FloatingIslands blob. SurfaceWorld remains a deliberate query refusal because
    // its production height can be biome/context-selected by the manager. This fixture seed was
    // repinned after the Maze topology/radius change; it is coverage selection, not generation
    // tuning.
    World.Build(/*InSeed=*/12345);
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
    int32 NumUpperChecked = 0;
    int32 NumUpperQueryFalse = 0;
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

    int32 NumLandingEnds = 0;
    int32 NumLandingFloorsPassed = 0;
    int32 NumLandingFloorFailures = 0;
    int32 NumLandingNetworkJoins = 0;
    int32 NumLandingSealFailures = 0;
    int32 NumLandingFloorBoxes = 0;
    int32 NumLandingBoxProofs = 0;
    int32 NumLandingBoxAllSolid = 0;
    int32 NumLandingBoxAllAir = 0;
    int32 NumLandingBoxViolations = 0;
    int32 NumLandingSourceFit = 0;
    int32 NumLandingConnectorSlopesPassed = 0;
    int32 NumLandingConnectorSlopeFailures = 0;
    float WorstLandingFloorGradient = 0.0f;
    float WorstLandingConnectorGradient = 0.0f;
    FString FirstLandingFailure;
    FString LandingSlopeReport;
    constexpr float LandingWalkableAngleDegrees = 44.0f;
    const float LandingWalkableSlope = FMath::Tan(
        FMath::DegreesToRadians(LandingWalkableAngleDegrees));

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

        // The upper mouth has its own source query too. This is deliberately evaluated from the
        // upper strate's definition, seed, bounds, and requested XY; it must never inherit the
        // lower mouth's answer or become an implicit passage chain.
        const float SourceTopZ = (float)(Source.TopChunkZ + 1) * CHUNK_SIZE;
        const float SourceBottomZ = (float)Source.BottomChunkZ * CHUNK_SIZE;
        const int32 SourceQuerySeed = OpenPointSeedFor(
            *SourceDefinition, Source, WorldSeed);
        const float SourceMaxLateralSnap = MaxLateralSnapFor(*SourceDefinition);
        FVector SuggestedUpperPoint = FVector::ZeroVector;
        const bool bCanAnswerUpper = VF_SuggestLandingPoint(
            SourceDefinition->GeneratorType,
            SourceDefinition->GenerationParams,
            SourceDefinition->SlabParams,
            SourceDefinition->MazeParams,
            SourceDefinition->VerticalShaftParams,
            SourceDefinition->FloatingIslandParams,
            SourceQuerySeed,
            SourceTopZ,
            SourceBottomZ,
            Passage.RequestedUpperPoint.X,
            Passage.RequestedUpperPoint.Y,
            SourceMaxLateralSnap,
            SuggestedUpperPoint);
        if (!bCanAnswerUpper)
        {
            ++NumUpperQueryFalse;
            FalseArchetypes.Add(static_cast<uint8>(SourceDefinition->GeneratorType));
        }
        else
        {
            ++NumUpperChecked;
            const float UpperSnapDX = Passage.UpperPoint.X
                - Passage.RequestedUpperPoint.X;
            const float UpperSnapDY = Passage.UpperPoint.Y
                - Passage.RequestedUpperPoint.Y;
            const float UpperSnapDistance = FMath::Sqrt(
                UpperSnapDX * UpperSnapDX + UpperSnapDY * UpperSnapDY);
            if (!FMath::IsNearlyEqual(Passage.UpperPoint.X, SuggestedUpperPoint.X, 0.01f)
                || !FMath::IsNearlyEqual(Passage.UpperPoint.Y, SuggestedUpperPoint.Y, 0.01f)
                || !FMath::IsNearlyEqual(Passage.UpperPoint.Z, SuggestedUpperPoint.Z, 0.01f))
            {
                bAllAnswerableEndpointsMatchQuery = false;
                AddError(FString::Printf(
                    TEXT("Passage from strate %d (%s) missed its upper player-fit landing point: endpoint (%.9g, %.9g, %.9g), query (%.9g, %.9g, %.9g)."),
                    Source.StrateIndex,
                    *ArchetypeName(SourceDefinition->GeneratorType),
                    Passage.UpperPoint.X,
                    Passage.UpperPoint.Y,
                    Passage.UpperPoint.Z,
                    SuggestedUpperPoint.X,
                    SuggestedUpperPoint.Y,
                    SuggestedUpperPoint.Z));
            }
            if (!FMath::IsFinite(UpperSnapDistance)
                || UpperSnapDistance > SourceMaxLateralSnap + 0.01f)
            {
                bAllAnswerableEndpointsMatchQuery = false;
                AddError(FString::Printf(
                    TEXT("Passage from strate %d (%s) exceeded its upper lateral snap bound: %.6f > %.6f from requested (%.3f, %.3f)."),
                    Source.StrateIndex,
                    *ArchetypeName(SourceDefinition->GeneratorType),
                    UpperSnapDistance,
                    SourceMaxLateralSnap,
                    Passage.RequestedUpperPoint.X,
                    Passage.RequestedUpperPoint.Y));
            }
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

        // Standing endpoints remain pinned to the complete query result, including any lateral
        // snap. The tube endpoint is now the landing door, so this intentionally compares the
        // public standing anchor rather than the first/last control point.
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
                        - P.SurfaceRoughness * VOXEL_NOISE_SCALE * 1.5f - 3.0f;
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

    //==========================================================================
    // LANDING GEOMETRY — the mouth is now measured as a room, not as a tube.
    //==========================================================================
    // Use the final production density, including the legacy structural post and any
    // disturbance backstop. Five points on every floor give a small numerical walkability
    // stencil; the floor writer itself is a plane, but the test must inspect the resulting field.
    for (const FVoxelPassage& Passage : Passages)
    {
        if (Passage.UpperStrateIndex == Passage.LowerStrateIndex) continue;

        const FVoxelPassageLanding* Landings[] = {
            &Passage.UpperLanding, &Passage.LowerLanding };
        const int32 StrateIndices[] = {
            Passage.UpperStrateIndex, Passage.LowerStrateIndex };
        for (int32 EndIndex = 0; EndIndex < 2; ++EndIndex)
        {
            ++NumLandingEnds;
            const FVoxelPassageLanding& Landing = *Landings[EndIndex];
            const int32 StrateIndex = StrateIndices[EndIndex];
            const UVoxelStrateDefinition* Definition = Layout.IsValidIndex(StrateIndex)
                ? Layout[StrateIndex].Definition : nullptr;

            auto RecordFailure = [&](const FString& Message)
            {
                if (FirstLandingFailure.IsEmpty()) FirstLandingFailure = Message;
            };

            if (Definition == nullptr
                || Landing.HalfWidth <= 0.0f
                || Landing.CeilingZ <= Landing.FloorZ
                || !FMath::IsFinite(Landing.FloorZ)
                || !FMath::IsFinite(Landing.CeilingZ)
                || !FMath::IsFinite(Landing.HalfWidth))
            {
                ++NumLandingFloorFailures;
                RecordFailure(FString::Printf(
                    TEXT("invalid landing descriptor at passage strate end %d/%d"),
                    Passage.UpperStrateIndex, Passage.LowerStrateIndex));
                continue;
            }

            const float StrateTopZ = (float)(Layout[StrateIndex].TopChunkZ + 1) * CHUNK_SIZE;
            const float StrateBottomZ = (float)Layout[StrateIndex].BottomChunkZ * CHUNK_SIZE;
            const float Seal = BoundarySealFor(*Definition);
            if (!(Landing.FloorZ > StrateBottomZ + Seal)
                || !(Landing.CeilingZ < StrateTopZ - Seal))
            {
                ++NumLandingSealFailures;
                RecordFailure(FString::Printf(
                    TEXT("landing at strate %d reaches its seal (floor %.3f, ceiling %.3f, interior %.3f..%.3f)"),
                    StrateIndex, Landing.FloorZ, Landing.CeilingZ,
                    StrateBottomZ + Seal, StrateTopZ - Seal));
            }

            if ((Landing.StandingPoint - (EndIndex == 0
                    ? Passage.UpperPoint : Passage.LowerPoint)).SizeSquared() > 0.0001f)
            {
                ++NumLandingFloorFailures;
                RecordFailure(FString::Printf(
                    TEXT("landing standing anchor does not match passage %s point at strate %d"),
                    EndIndex == 0 ? TEXT("upper") : TEXT("lower"), StrateIndex));
            }

            const bool bWidthAndHeadroom = Landing.HalfWidth * 2.0f >= 12.0f - 0.01f
                && Landing.CeilingZ - Landing.FloorZ >= 12.0f - 0.01f;
            const bool bDoorTangent = FMath::IsNearlyEqual(
                Landing.DoorPoint.Z - Landing.FloorZ,
                Passage.Radius >= 0.0f ? FMath::Max(
                    Passage.ControlRadii.Num() > 0
                        ? Passage.ControlRadii[0] : Passage.Radius,
                    1.0f) : 1.0f,
                0.01f);
            if (!bWidthAndHeadroom || !bDoorTangent)
            {
                ++NumLandingFloorFailures;
                RecordFailure(FString::Printf(
                    TEXT("landing at strate %d failed body/door arithmetic (width %.3f, height %.3f, door rise %.3f)"),
                    StrateIndex, Landing.HalfWidth * 2.0f,
                    Landing.CeilingZ - Landing.FloorZ,
                    Landing.DoorPoint.Z - Landing.FloorZ));
            }

            const FVector FloorSamples[] = {
                Landing.StandingPoint,
                Landing.StandingPoint + FVector(2.0f, 0.0f, 0.0f),
                Landing.StandingPoint + FVector(-2.0f, 0.0f, 0.0f),
                Landing.StandingPoint + FVector(0.0f, 2.0f, 0.0f),
                Landing.StandingPoint + FVector(0.0f, -2.0f, 0.0f) };
            float SurfaceHeights[UE_ARRAY_COUNT(FloorSamples)] = {};
            bool bFloorPassed = true;
            float LandingGradient = -1.0f;
            for (int32 SampleIndex = 0; SampleIndex < UE_ARRAY_COUNT(FloorSamples); ++SampleIndex)
            {
                const FVector& Sample = FloorSamples[SampleIndex];
                const float Below = World.Generator->GetDensityAt(
                    Sample.X, Sample.Y, Landing.FloorZ - 1.0f);
                const float Above = World.Generator->GetDensityAt(
                    Sample.X, Sample.Y, Landing.FloorZ + 1.0f);
                const float Denominator = Above - Below;
                if (!FMath::IsFinite(Below) || !FMath::IsFinite(Above)
                    || !(Below < 0.0f) || !(Above >= 0.0f)
                    || !(Denominator > 0.0f))
                {
                    bFloorPassed = false;
                    continue;
                }
                SurfaceHeights[SampleIndex] = Landing.FloorZ - 1.0f
                    + FMath::Clamp(-Below / Denominator, 0.0f, 1.0f) * 2.0f;
            }

            if (bFloorPassed)
            {
                float MaxGradient = 0.0f;
                for (int32 SampleIndex = 1; SampleIndex < UE_ARRAY_COUNT(FloorSamples); ++SampleIndex)
                {
                    MaxGradient = FMath::Max(MaxGradient,
                        FMath::Abs(SurfaceHeights[SampleIndex] - SurfaceHeights[0]) / 2.0f);
                }
                LandingGradient = MaxGradient;
                WorstLandingFloorGradient = FMath::Max(WorstLandingFloorGradient, MaxGradient);
                const float NormalZ = 1.0f / FMath::Sqrt(1.0f + MaxGradient * MaxGradient);
                bFloorPassed = NormalZ + KINDA_SMALL_NUMBER >= FMath::Cos(
                    FMath::DegreesToRadians(LandingWalkableAngleDegrees));
            }

            float ConnectorGradient = -1.0f;
            bool bConnectorSlopePassed = false;
            if (Landing.bHasNetworkConnector
                && FMath::IsFinite(Landing.ConnectorStart.X)
                && FMath::IsFinite(Landing.ConnectorStart.Y)
                && FMath::IsFinite(Landing.ConnectorEnd.X)
                && FMath::IsFinite(Landing.ConnectorEnd.Y)
                && (!Landing.bHasConnectorBend
                    || (FMath::IsFinite(Landing.ConnectorControl.X)
                        && FMath::IsFinite(Landing.ConnectorControl.Y)))
                && FMath::IsFinite(Landing.RootFloorZ)
                && FMath::IsFinite(Landing.FloorZ))
            {
                const FVector2D ConnectorStart(
                    Landing.ConnectorStart.X, Landing.ConnectorStart.Y);
                const FVector2D ConnectorEnd(
                    Landing.ConnectorEnd.X, Landing.ConnectorEnd.Y);
                const float DirectLength = (ConnectorEnd - ConnectorStart).Size();
                if (Landing.bHasConnectorBend)
                {
                    const FVector2D ConnectorControl(
                        Landing.ConnectorControl.X, Landing.ConnectorControl.Y);
                    const float FinalLegLength = (ConnectorEnd - ConnectorControl).Size();
                    const float RampLength = FMath::Max(
                        FinalLegLength - (Landing.RootSpineRadius > 0.0f ? 2.25f : 0.0f),
                        KINDA_SMALL_NUMBER);
                    ConnectorGradient = FMath::Abs(
                        Landing.RootFloorZ - Landing.FloorZ) / RampLength;
                }
                else
                {
                    const float RampLength = FMath::Max(
                        DirectLength - FMath::Max(Landing.HalfWidth - 1.0f, 0.0f)
                            - (Landing.RootSpineRadius > 0.0f ? 2.25f : 0.0f),
                        KINDA_SMALL_NUMBER);
                    ConnectorGradient = FMath::Abs(
                        Landing.RootFloorZ - Landing.FloorZ) / RampLength;
                }
                bConnectorSlopePassed = FMath::IsFinite(ConnectorGradient)
                    && ConnectorGradient <= LandingWalkableSlope + KINDA_SMALL_NUMBER;
            }

            if (bConnectorSlopePassed)
            {
                ++NumLandingConnectorSlopesPassed;
                WorstLandingConnectorGradient = FMath::Max(
                    WorstLandingConnectorGradient, ConnectorGradient);
            }
            else
            {
                ++NumLandingConnectorSlopeFailures;
                RecordFailure(FString::Printf(
                    TEXT("landing at strate %d has connector slope %.6f above the %.6f walkable limit"),
                    StrateIndex, ConnectorGradient, LandingWalkableSlope));
            }

            const bool bLandingSlopePassed = bFloorPassed && bConnectorSlopePassed;
            if (bLandingSlopePassed)
            {
                ++NumLandingFloorsPassed;
            }
            else
            {
                ++NumLandingFloorFailures;
                RecordFailure(FString::Printf(
                    TEXT("landing at strate %d failed its final-density flat-floor slope bracket"),
                    StrateIndex));
            }

            if (Landing.bSourcePlayerFit)
            {
                ++NumLandingSourceFit;
            }
            if (Landing.bHasNetworkConnector)
            {
                ++NumLandingNetworkJoins;
            }
            else
            {
                RecordFailure(FString::Printf(
                    TEXT("landing at strate %d has neither a source fit nor a deterministic network connector"),
                    StrateIndex));
            }

            // Exercise the same box shortcut that can otherwise erase a support slab. A uniform
            // verdict is allowed only if every exact ClassifyTile lattice sample agrees with the
            // final density; a floor-bearing landing should normally force Mixed via the
            // bidirectional passage effect. This is one aggregate audit, not per-sample logging.
            const FIntVector LandingBoxOrigin(
                FMath::FloorToInt(Landing.StandingPoint.X) - 4,
                FMath::FloorToInt(Landing.StandingPoint.Y) - 4,
                FMath::FloorToInt(Landing.FloorZ) - 3);
            constexpr int32 LandingBoxCells = 8;
            constexpr int32 LandingBoxStep = 1;
            const EVoxelTileClass LandingBoxVerdict = World.Generator->ClassifyTile(
                LandingBoxOrigin, LandingBoxStep, LandingBoxCells);
            ++NumLandingFloorBoxes;
            if (LandingBoxVerdict != EVoxelTileClass::Mixed)
            {
                ++NumLandingBoxProofs;
                if (LandingBoxVerdict == EVoxelTileClass::AllSolid) ++NumLandingBoxAllSolid;
                if (LandingBoxVerdict == EVoxelTileClass::AllAir) ++NumLandingBoxAllAir;

                bool bBoxAgrees = true;
                const bool bClaimsSolid = LandingBoxVerdict == EVoxelTileClass::AllSolid;
                for (int32 GZ = -1; GZ <= LandingBoxCells + 1 && bBoxAgrees; ++GZ)
                {
                    for (int32 GY = -1; GY <= LandingBoxCells + 1 && bBoxAgrees; ++GY)
                    {
                        for (int32 GX = -1; GX <= LandingBoxCells + 1; ++GX)
                        {
                            const float Density = World.Generator->GetDensityAt(
                                (float)(LandingBoxOrigin.X + GX * LandingBoxStep),
                                (float)(LandingBoxOrigin.Y + GY * LandingBoxStep),
                                (float)(LandingBoxOrigin.Z + GZ * LandingBoxStep));
                            const bool bMatches = bClaimsSolid
                                ? Density < 0.0f : Density >= 0.0f;
                            if (!FMath::IsFinite(Density) || !bMatches)
                            {
                                bBoxAgrees = false;
                                break;
                            }
                        }
                    }
                }
                if (!bBoxAgrees)
                {
                    ++NumLandingBoxViolations;
                    if (NumLandingBoxViolations == 1)
                    {
                        AddError(FString::Printf(
                            TEXT("Landing box verdict %d was not sound at (%d,%d,%d): the uniform %s verdict disagreed with final density."),
                            NumLandingBoxProofs,
                            LandingBoxOrigin.X,
                            LandingBoxOrigin.Y,
                            LandingBoxOrigin.Z,
                            bClaimsSolid ? TEXT("AllSolid") : TEXT("AllAir")));
                    }
                }
            }

            const FString LandingArchetype = ArchetypeName(Definition->GeneratorType);
            LandingSlopeReport += FString::Printf(
                TEXT("%s[%d:%s]=floor:%s(%.6f),connector:%s(%.6f)%s "),
                *LandingArchetype,
                StrateIndex,
                EndIndex == 0 ? TEXT("upper") : TEXT("lower"),
                bFloorPassed ? TEXT("PASS") : TEXT("FAIL"),
                LandingGradient,
                bConnectorSlopePassed ? TEXT("PASS") : TEXT("FAIL"),
                ConnectorGradient,
                Landing.bHasConnectorBend ? TEXT("[bend]") : TEXT(""));
        }
    }

    // The passage modifier is called once per density sample, but shortlist construction is
    // keyed by (manager, layout version, chunk) in thread_local storage. Measure the hot hit and
    // the deliberately alternating two-chunk rebuild separately so the report exposes the cache
    // contract without adding a log line for any individual sample.
    const FVoxelPassage* TimingPassage = nullptr;
    for (const FVoxelPassage& Passage : Passages)
    {
        if (Passage.UpperStrateIndex != Passage.LowerStrateIndex)
        {
            TimingPassage = &Passage;
            break;
        }
    }
    if (TimingPassage != nullptr)
    {
        const FVector TimingPoint = TimingPassage->UpperLanding.StandingPoint
            + FVector(0.0f, 0.0f, 2.0f);
        constexpr int32 HotCalls = 8192;
        constexpr int32 RebuildCalls = 96;
        volatile float TimingSink = 0.0f;
        double StartSeconds = FPlatformTime::Seconds();
        for (int32 Call = 0; Call < HotCalls; ++Call)
        {
            float Density = 8.0f;
            World.StrateManager->ApplyPassageModifier(
                Density, TimingPoint.X, TimingPoint.Y, TimingPoint.Z, 8.0f, 0.0f);
            TimingSink += Density;
        }
        const double HotSeconds = FPlatformTime::Seconds() - StartSeconds;

        StartSeconds = FPlatformTime::Seconds();
        for (int32 Call = 0; Call < RebuildCalls; ++Call)
        {
            const FVector Point = TimingPoint
                + ((Call & 1) != 0 ? FVector((float)CHUNK_SIZE, 0.0f, 0.0f)
                                   : FVector::ZeroVector);
            float Density = 8.0f;
            World.StrateManager->ApplyPassageModifier(
                Density, Point.X, Point.Y, Point.Z, 8.0f, 0.0f);
            TimingSink += Density;
        }
        const double RebuildSeconds = FPlatformTime::Seconds() - StartSeconds;
        AddInfo(FString::Printf(
            TEXT("Passage TLS cache timing: rebuild %.3f us/call over %d alternating chunks; hot %.3f us/call over %d same-chunk calls (sink %.3f)."),
            RebuildSeconds * 1000000.0 / (double)RebuildCalls,
            RebuildCalls,
            HotSeconds * 1000000.0 / (double)HotCalls,
            HotCalls,
            (float)TimingSink));
    }

    AddInfo(FString::Printf(
        TEXT("Passage player-fit check: %d inter-strate passages, upper %d checked (%d false), lower %d checked (%d footing checks), %d/%d room/slab ring samples air, lower %d query-false (%d unique archetypes; %d unsupported, %d supported-but-no-point)."),
        NumInterStratePassages,
        NumUpperChecked,
        NumUpperQueryFalse,
        NumChecked,
        NumFootingChecked,
        NumRingAirSamples,
        NumRingSamples,
        NumQueryFalse,
        FalseArchetypes.Num(),
        NumUnsupported,
        NumSupportedWithoutPoint));
    AddInfo(FString::Printf(
        TEXT("Landing geometry: %d/%d ends passed the final-density floor test (worst room gradient %.6f), connector slopes %d/%d (worst %.6f; walkable limit %.6f), source-fit anchors %d/%d, explicit root-network joins %d/%d, seal violations %d, geometry/floor failures %d."),
        NumLandingFloorsPassed,
        NumLandingEnds,
        WorstLandingFloorGradient,
        NumLandingConnectorSlopesPassed,
        NumLandingEnds,
        WorstLandingConnectorGradient,
        LandingWalkableSlope,
        NumLandingSourceFit,
        NumLandingEnds,
        NumLandingNetworkJoins,
        NumLandingEnds,
        NumLandingSealFailures,
        NumLandingFloorFailures));
    if (!LandingSlopeReport.IsEmpty())
    {
        AddInfo(FString::Printf(TEXT("Landing slope tests per end: %s"), *LandingSlopeReport));
    }
    AddInfo(FString::Printf(
        TEXT("Landing floor box verdicts: %d boxes probed, %d uniform proofs (AllSolid %d, AllAir %d), %d violations."),
        NumLandingFloorBoxes,
        NumLandingBoxProofs,
        NumLandingBoxAllSolid,
        NumLandingBoxAllAir,
        NumLandingBoxViolations));
    if (!FirstLandingFailure.IsEmpty())
    {
        AddError(FString::Printf(TEXT("First landing geometry failure: %s"), *FirstLandingFailure));
    }

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
    if (NumUpperChecked == 0)
    {
        AddError(TEXT("VACUOUS: zero answerable upper passage mouths were checked; independent upper-mouth aiming was not exercised."));
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
        && NumLandingEnds == NumInterStratePassages * 2
        && NumLandingFloorsPassed == NumLandingEnds
        && NumLandingConnectorSlopesPassed == NumLandingEnds
        && NumLandingConnectorSlopeFailures == 0
        && NumLandingNetworkJoins == NumLandingEnds
        && NumLandingSealFailures == 0
        && NumLandingFloorFailures == 0
        && NumLandingBoxViolations == 0
        && NumChecked > 0
        && NumUpperChecked > 0
        && NumFootingChecked > 0
        && bAllNewFootingArchetypesAnswered
        // (fixture coverage of the room/slab archetypes is reported, not asserted — see above)
        && bSurfaceIntentionallyRefused
        && bAllAnswerableFootingsAreValid
        && bAllAnswerableMouthRingsHaveAir
        && bAllAnswerableEndpointsMatchQuery;
}

#endif // WITH_DEV_AUTOMATION_TESTS
