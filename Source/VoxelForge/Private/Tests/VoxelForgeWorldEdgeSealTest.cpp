// VoxelForgeWorldEdgeSealTest.cpp
// Falsifiable coverage for the bounded-world XY edge seal.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"
#include "VoxelDensityPrimitives.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeWorldEdgeSealTest,
    "VoxelForge.Generation.WorldEdgeSeal",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr float TestWorldRadius = 256.0f;
    constexpr float TestEdgeThickness = 64.0f;
    constexpr float RimJumpThreshold = 1.0f;

    void SetEdgeSettings(VoxelForgeTest::FTestWorld& World, float Radius)
    {
        World.Settings->WorldRadiusVoxels = Radius;
        World.Settings->EdgeSealThickness = TestEdgeThickness;
        World.Generator->InitializeSettings(World.Settings.Get());
    }

    void ConfigureNearRimPassages(VoxelForgeTest::FTestWorld& World)
    {
        for (TStrongObjectPtr<UVoxelStrateDefinition>& DefPtr : World.Definitions)
        {
            if (!DefPtr.IsValid()) continue;
            FStratePassageConfig& Cfg = DefPtr->PassageConfig;
            Cfg.Connections = 1;
            Cfg.Style = EVoxelPassageStyle::Straight;
            Cfg.MouthRadius = 6.0f;
            Cfg.MidRadius = 6.0f;
            Cfg.ReachMin = 40.0f;
            Cfg.ReachMax = 40.0f;
            Cfg.DistanceMin = 240.0f;
            Cfg.DistanceMax = 240.0f;
            Cfg.Wander = 0.0f;
            Cfg.Segments = 4;
            Cfg.VerticalWobble = 0.0f;
        }
        World.Reinitialize();
    }

    bool IsSolidMC(const UVoxelGenerator& Generator, const FVector& Point)
    {
        return Generator.GetDensityAt(Point.X, Point.Y, Point.Z) < 0.0f;
    }
}

bool FVoxelForgeWorldEdgeSealTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    int32 NumFailures = 0;
    FString FailureKinds;
    auto Fail = [&](const TCHAR* Kind)
    {
        ++NumFailures;
        if (!FailureKinds.IsEmpty()) FailureKinds += TEXT(", ");
        FailureKinds += Kind;
    };

    // The ordinary fixture deliberately uses the requested default, so the other tests exercise
    // the setting without having to know about the new post.
    FTestWorld Bounded;
    Bounded.Build(/*Seed*/1337, /*GapChunks*/2);
    FTestWorld Unbounded;
    Unbounded.Build(/*Seed*/1337, /*GapChunks*/2);

    if (!Bounded.IsValid() || !Unbounded.IsValid())
    {
        AddError(!Bounded.IsValid() ? Bounded.WhyInvalid() : Unbounded.WhyInvalid());
        return false;
    }

    SetEdgeSettings(Bounded, TestWorldRadius);
    SetEdgeSettings(Unbounded, 0.0f);

    //==========================================================================
    // POSITIVE CONTROL — far outside the radius, across several Z strata.
    //==========================================================================
    const FVector FarXY[] = {
        FVector(384.0f, 0.0f, 0.0f),
        FVector(-384.0f, 0.0f, 0.0f),
        FVector(0.0f, 384.0f, 0.0f),
        FVector(0.0f, -384.0f, 0.0f),
        FVector(320.0f, 320.0f, 0.0f),
    };
    TArray<float> StrateZ;
    for (int32 Slot = 0; Slot < 8; ++Slot)
    {
        int32 TopZ = 0, BottomZ = 0;
        if (Bounded.GetSlotVoxelZRange(Slot, TopZ, BottomZ))
        {
            StrateZ.Add((float)((TopZ + BottomZ) / 2));
        }
    }
    StrateZ.Add(Bounded.MidVoxelZ());

    int32 NumPositiveSamples = 0;
    for (const FVector& XY : FarXY)
    {
        for (const float Z : StrateZ)
        {
            const FVector P(XY.X, XY.Y, Z);
            ++NumPositiveSamples;
            if (!IsSolidMC(*Bounded.Generator, P)) Fail(TEXT("positive"));
        }
    }

    //==========================================================================
    // NEGATIVE CONTROL — the interior must be bit-identical to radius == 0.
    //==========================================================================
    const FVector InteriorXY[] = {
        FVector(0.0f, 0.0f, 0.0f),
        FVector(64.0f, -80.0f, 0.0f),
        FVector(128.0f, 0.0f, 0.0f),
        FVector(0.0f, -128.0f, 0.0f),
        FVector(100.0f, 100.0f, 0.0f),
    };
    int32 NumInteriorComparisons = 0;
    for (const FVector& XY : InteriorXY)
    {
        for (const float Z : StrateZ)
        {
            const FVector P(XY.X, XY.Y, Z);
            const float BoundedD = Bounded.Generator->GetDensityAt(P.X, P.Y, P.Z);
            const float UnboundedD = Unbounded.Generator->GetDensityAt(P.X, P.Y, P.Z);
            ++NumInteriorComparisons;
            if (!BitEqual(BoundedD, UnboundedD)) Fail(TEXT("interior-not-no-op"));
        }
    }

    //==========================================================================
    // RIM SMOOTHNESS — measure solidness (the negative MC density magnitude), not its sign.
    //==========================================================================
    float MaxAdjacentSolidnessJump = 0.0f;
    int32 NumRimDirections = 0;
    const FVector RimDirections[] = {
        FVector(1.0f, 0.0f, 0.0f),
        FVector(0.0f, 1.0f, 0.0f),
        FVector(-1.0f, 0.0f, 0.0f),
        FVector(0.0f, -1.0f, 0.0f),
        FVector(0.70710678f, 0.70710678f, 0.0f),
    };
    for (const FVector& Dir : RimDirections)
    {
        ++NumRimDirections;
        float PreviousSolidness = 0.0f;
        bool bHasPrevious = false;
        for (int32 R = (int32)(TestWorldRadius - TestEdgeThickness - 4.0f);
             R <= (int32)(TestWorldRadius + 4.0f); ++R)
        {
            float InternalDensity = 0.0f;
            VF_ApplyXYEdgeSeal(InternalDensity, Dir.X * (float)R, Dir.Y * (float)R,
                               TestWorldRadius, TestEdgeThickness, 8.0f);
            const float Solidness = InternalDensity;
            if (bHasPrevious)
            {
                MaxAdjacentSolidnessJump = FMath::Max(
                    MaxAdjacentSolidnessJump, FMath::Abs(Solidness - PreviousSolidness));
                if (Solidness + KINDA_SMALL_NUMBER < PreviousSolidness)
                    Fail(TEXT("rim-not-monotonic"));
            }
            PreviousSolidness = Solidness;
            bHasPrevious = true;
        }
    }
    if (MaxAdjacentSolidnessJump > RimJumpThreshold) Fail(TEXT("rim-discontinuity"));

    // Explicitly verify the true no-op contract, including the MC-facing wrapper.
    float NoOpDensity = 3.25f;
    const float NoOpBefore = NoOpDensity;
    VF_ApplyXYEdgeSealMC(NoOpDensity, 100000.0f, -100000.0f,
                         0.0f, TestEdgeThickness, 8.0f);
    if (!BitEqual(NoOpDensity, NoOpBefore)) Fail(TEXT("radius-zero-not-no-op"));

    //==========================================================================
    // CLASSIFYBOX — force boxes in the ramp and beyond the radius, then brute-force every voxel.
    //==========================================================================
    int32 TopShaftZ = 0, BottomShaftZ = 0;
    if (!Bounded.GetSlotVoxelZRange(FTestWorld::SlotVerticalShafts, TopShaftZ, BottomShaftZ))
    {
        Fail(TEXT("missing-shaft-slot"));
    }

    int32 NumBoxesProved = 0;
    int64 NumVoxelsChecked = 0;
    if (TopShaftZ > BottomShaftZ)
    {
        const float BoxZ0 = (float)(BottomShaftZ + 8);
        const float BoxZ1 = (float)FMath::Min(BottomShaftZ + 10, TopShaftZ - 8);
        const FBox ProvedBoxes[] = {
            // Ramp boxes: closest radius is 240, safely past the 1-voxel proof margin.
            FBox(FVector(240.0f, -2.0f, BoxZ0), FVector(244.0f, 2.0f, BoxZ1)),
            FBox(FVector(-244.0f, -2.0f, BoxZ0), FVector(-240.0f, 2.0f, BoxZ1)),
            FBox(FVector(-2.0f, 240.0f, BoxZ0), FVector(2.0f, 244.0f, BoxZ1)),
            FBox(FVector(170.0f, 170.0f, BoxZ0), FVector(174.0f, 174.0f, BoxZ1)),
            // Forced-solid boxes beyond the world radius.
            FBox(FVector(320.0f, -2.0f, BoxZ0), FVector(324.0f, 2.0f, BoxZ1)),
            FBox(FVector(-324.0f, -2.0f, BoxZ0), FVector(-320.0f, 2.0f, BoxZ1)),
            FBox(FVector(-2.0f, 320.0f, BoxZ0), FVector(2.0f, 324.0f, BoxZ1)),
            FBox(FVector(226.0f, 226.0f, BoxZ0), FVector(230.0f, 230.0f, BoxZ1)),
        };

        const int32 MidChunkZ = FMath::FloorToInt((BoxZ0 + BoxZ1) * 0.5f / CHUNK_SIZE);
        const FVerticalShaftParams ShaftParams = Bounded.StrateManager->GetVerticalShaftParamsForChunk(
            FIntVector(0, 0, MidChunkZ));
        FVoxelOpStack Stack;
        VoxelDensityOps::BuildVerticalShaftStack(
            Stack, ShaftParams, Bounded.Settings->Seed,
            Bounded.Generator->OriginSpineRadius, Bounded.StrateManager.Get());

        FVoxelOpContext Ctx;
        Ctx.ChunkCoord = FIntVector(0, 0, MidChunkZ);
        Ctx.Seed = (uint32)Bounded.Settings->Seed;
        Ctx.LayoutVersion = Bounded.StrateManager->GetLayoutVersion();
        Ctx.StrateTopWorldZ = ShaftParams.StrateTopWorldZ;
        Ctx.StrateBottomWorldZ = ShaftParams.StrateBottomWorldZ;
        Ctx.WorldRadiusVoxels = TestWorldRadius;
        Ctx.EdgeSealThickness = TestEdgeThickness;
        Stack.PrepareChunk(Ctx);

        for (const FBox& Box : ProvedBoxes)
        {
            if (Stack.ClassifyBox(Box, Ctx) != EVoxelTileClass::AllSolid)
            {
                Fail(TEXT("classifybox-missed-forced-band"));
                continue;
            }

            ++NumBoxesProved;
            const int32 X0 = FMath::CeilToInt(Box.Min.X);
            const int32 X1 = FMath::FloorToInt(Box.Max.X);
            const int32 Y0 = FMath::CeilToInt(Box.Min.Y);
            const int32 Y1 = FMath::FloorToInt(Box.Max.Y);
            const int32 Z0 = FMath::CeilToInt(Box.Min.Z);
            const int32 Z1 = FMath::FloorToInt(Box.Max.Z);
            for (int32 Z = Z0; Z <= Z1; ++Z)
            for (int32 Y = Y0; Y <= Y1; ++Y)
            for (int32 X = X0; X <= X1; ++X)
            {
                ++NumVoxelsChecked;
                if (!IsSolidMC(*Bounded.Generator, FVector((float)X, (float)Y, (float)Z)))
                    Fail(TEXT("classifybox-false-allsolid"));
            }
        }
    }

    // The T1.d path must be able to skip an edge tile without knowing which Z category it hits.
    const EVoxelTileClass EdgeTile = Bounded.Generator->ClassifyTile(
        FIntVector(320, 0, BottomShaftZ + 8), 1, 8);
    int32 NumTilesProved = (EdgeTile == EVoxelTileClass::AllSolid) ? 1 : 0;
    if (EdgeTile != EVoxelTileClass::AllSolid) Fail(TEXT("classifytile-missed-edge"));

    //==========================================================================
    // PASSAGE ORDER — a near-rim passage carve is followed by the edge force.
    //==========================================================================
    float NearRimInternalDensity = 8.0f;
    VF_ApplyPassageCarving(NearRimInternalDensity, 0.0f, 8.0f, TestEdgeThickness);
    const float PassageOnlyDensity = NearRimInternalDensity;
    VF_ApplyXYEdgeSeal(NearRimInternalDensity, TestWorldRadius - 16.0f, 0.0f,
                       TestWorldRadius, TestEdgeThickness, 8.0f);
    int32 NumNearRimPassageChecks = 1;
    if (!(PassageOnlyDensity < 0.0f && NearRimInternalDensity > 0.0f))
        Fail(TEXT("passage-breaches-rim"));

    // Try the same construction through the real deterministic passage generator. It is diagnostic
    // coverage only: the synthetic helper case above is the hard assertion even if landing-site
    // adjustment moves every generated mouth out of this small test rim.
    FTestWorld GeneratedPassageWorld;
    GeneratedPassageWorld.Build(/*Seed*/1337, /*GapChunks*/2);
    auto CheckGeneratedPassages = [&](FTestWorld& World, const TCHAR* FailureKind)
    {
        if (!World.IsValid())
        {
            Fail(FailureKind);
            return;
        }
        ConfigureNearRimPassages(World);
        SetEdgeSettings(World, TestWorldRadius);
        for (const FVoxelPassage& Passage : World.StrateManager->GetPassages())
        {
            TArray<FVector> Candidates = Passage.ControlPoints;
            Candidates.Add(Passage.UpperPoint);
            Candidates.Add(Passage.LowerPoint);
            for (const FVector& P : Candidates)
            {
                if (P.X * P.X + P.Y * P.Y <= FMath::Square(TestWorldRadius - TestEdgeThickness)) continue;
                ++NumNearRimPassageChecks;
                if (!IsSolidMC(*World.Generator, P)) Fail(FailureKind);
                break;
            }
        }
    };
    CheckGeneratedPassages(GeneratedPassageWorld, TEXT("generated-passage-breaches-rim"));

    // One sorted summary block; individual samples intentionally never log.
    AddInfo(FString::Printf(
        TEXT("WORLD_EDGE_SEAL SUMMARY\n")
        TEXT("  classify_box_proved=%d\n")
        TEXT("  classify_tile_proved=%d\n")
        TEXT("  interior_comparisons=%d\n")
        TEXT("  near_rim_passage_checks=%d\n")
        TEXT("  positive_sample_points=%d\n")
        TEXT("  rim_directions=%d\n")
        TEXT("  rim_max_adjacent_solidness_jump=%.6f (threshold=%.6f)\n")
        TEXT("  voxels_checked=%lld\n")
        TEXT("  failures=%d (%s)"),
        NumBoxesProved, NumTilesProved, NumInteriorComparisons, NumNearRimPassageChecks,
        NumPositiveSamples, NumRimDirections, MaxAdjacentSolidnessJump, RimJumpThreshold,
        (long long)NumVoxelsChecked, NumFailures, *FailureKinds));

    if (NumFailures > 0)
    {
        AddError(FString::Printf(TEXT("WorldEdgeSeal aggregate checks failed: %d (%s)"),
                                 NumFailures, *FailureKinds));
    }
    return NumFailures == 0;
}

#endif // WITH_DEV_AUTOMATION_TESTS
