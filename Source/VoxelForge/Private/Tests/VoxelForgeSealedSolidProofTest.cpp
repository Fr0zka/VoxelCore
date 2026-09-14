// Sealed-solid proof soundness and invalidation.
//
// The production shortcut is intentionally one-sided: it may return false too often, but a true
// result skips both meshing and level-0 collision generation.  This test therefore brute-forces
// every tile that the fixture presents to the proof and treats any non-solid lattice sample as a
// hard failure.  The second half deliberately disables the diff guard; a carve must then make the
// weakened proof look true while the same oracle finds an air sample.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelForgeTestFixture.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeSealedSolidProofTest,
    "VoxelForge.Determinism.SealedSolidProofSoundness",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    struct FTileSpec
    {
        FIntVector Origin = FIntVector::ZeroValue;
        int32 Step = 1;
        int32 Cells = CHUNK_SIZE;
    };

    int32 CountNonSolidSamples(const UVoxelGenerator& Generator, const FTileSpec& Tile)
    {
        const int32 Cells = FMath::Clamp(Tile.Cells, 2, CHUNK_SIZE);
        int32 NonSolid = 0;
        for (int32 Z = 0; Z <= Cells; ++Z)
        for (int32 Y = 0; Y <= Cells; ++Y)
        for (int32 X = 0; X <= Cells; ++X)
        {
            const float Density = Generator.GetDensityAt(
                static_cast<float>(Tile.Origin.X + X * Tile.Step),
                static_cast<float>(Tile.Origin.Y + Y * Tile.Step),
                static_cast<float>(Tile.Origin.Z + Z * Tile.Step));
            if (!VoxelMath::IsFinite(Density) || !(Density < 0.0f))
            {
                ++NonSolid;
            }
        }
        return NonSolid;
    }

    void SetSealThickness(UVoxelStrateDefinition& Definition, float Thickness)
    {
        Definition.GenerationParams.BoundarySealThickness = Thickness;
        Definition.SlabParams.BoundarySealThickness = Thickness;
        Definition.MazeParams.BoundarySealThickness = Thickness;
        Definition.SurfaceParams.BoundarySealThickness = Thickness;
        Definition.VerticalShaftParams.BoundarySealThickness = Thickness;
        Definition.FloatingIslandParams.BoundarySealThickness = Thickness;
    }
}

bool FVoxelForgeSealedSolidProofTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    // Forty voxels makes a complete 32-cell level-0 tile fit inside a bottom boundary seal.  The
    // production asset remains untouched; this is a transient proof-only fixture configuration.
    FTestWorld World;
    World.Build(/*Seed*/1337, /*GapChunks*/2, /*bUseOperatorStack*/false,
                /*InStrateHeightInChunks*/8);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }
    for (const TStrongObjectPtr<UVoxelStrateDefinition>& Definition : World.Definitions)
    {
        if (Definition.IsValid())
        {
            SetSealThickness(*Definition.Get(), 40.0f);
        }
    }
    World.Reinitialize();

    const UVoxelGenerator* Generator = World.Generator.Get();
    const FIntVector FarXY(4096, 4096, 0);
    TArray<FTileSpec> CandidateTiles;

    // Enumerate every gap chunk and every strate's bottom-seal tile in this fixture.  No sample
    // is selected after looking at the proof result: all candidates are offered to it first.
    for (int32 ChunkZ = World.BottomChunkZ; ChunkZ <= World.TopChunkZ; ++ChunkZ)
    {
        if (World.StrateManager->IsGapChunk(FIntVector(0, 0, ChunkZ)))
        {
            CandidateTiles.Add({FIntVector(FarXY.X, FarXY.Y, ChunkZ * CHUNK_SIZE), 1, CHUNK_SIZE});
        }
    }
    for (const FStrateSlot& Slot : World.StrateManager->GetLayout())
    {
        CandidateTiles.Add({
            FIntVector(FarXY.X, FarXY.Y, Slot.BottomChunkZ * CHUNK_SIZE),
            1, CHUNK_SIZE});
    }

    int32 NumProofSkips = 0;
    int32 NumNonSolidSamples = 0;
    int32 FirstProofTile = INDEX_NONE;
    for (int32 Index = 0; Index < CandidateTiles.Num(); ++Index)
    {
        const FTileSpec& Tile = CandidateTiles[Index];
        if (!Generator->TryProveSealedSolidTile(Tile.Origin, Tile.Step, Tile.Cells))
        {
            continue;
        }
        ++NumProofSkips;
        if (FirstProofTile == INDEX_NONE)
        {
            FirstProofTile = Index;
        }
        const int32 TileNonSolid = CountNonSolidSamples(*Generator, Tile);
        NumNonSolidSamples += TileNonSolid;
        if (TileNonSolid != 0)
        {
            AddError(FString::Printf(
                TEXT("FALSE ALL-SOLID PROOF: candidate tile (%d,%d,%d) returned true but has "
                     "%d non-solid core-lattice samples."),
                Tile.Origin.X, Tile.Origin.Y, Tile.Origin.Z, TileNonSolid));
        }
    }

    AddInfo(FString::Printf(
        TEXT("Sealed-solid proof brute force: %d candidate tiles, %d proof skips, %d non-solid "
             "core-lattice samples."),
        CandidateTiles.Num(), NumProofSkips, NumNonSolidSamples));
    TestTrue(TEXT("the fixture exercised at least one sealed-solid proof skip"),
             NumProofSkips > 0);
    TestEqual(TEXT("every skipped tile has zero non-solid core-lattice samples"),
              NumNonSolidSamples, 0);
    if (FirstProofTile == INDEX_NONE)
    {
        return false;
    }

    // A carve in the proven region is the live-edit case that must invalidate the proof.  The
    // diff layer stores the modification in the tile's chunk, so this is also the exact guard used
    // by the worker before it decides to skip meshing.
    const FTileSpec& ProofTile = CandidateTiles[FirstProofTile];
    const FVector CarveCenter(
        static_cast<float>(ProofTile.Origin.X + 16),
        static_cast<float>(ProofTile.Origin.Y + 16),
        static_cast<float>(ProofTile.Origin.Z + 16));
    FVoxelModification Carve;
    Carve.Center = CarveCenter;
    Carve.Radius = 6.0f;
    Carve.Strength = -64.0f;
    Carve.Shape = EVoxelBrushShape::Sphere;
    TestTrue(TEXT("the adversarial carve was registered"),
             World.DiffLayer->ApplyModification(Carve).Num() > 0);

    TestFalse(TEXT("a player carve kills the sealed-solid proof"),
              Generator->TryProveSealedSolidTile(
                  ProofTile.Origin, ProofTile.Step, ProofTile.Cells));

    const uint8 DiffGuardDisabled = static_cast<uint8>(
        EVoxelSealedSolidProofGuard::DiffLayer);
    TestTrue(TEXT("the deliberately weakened proof still exposes the geometric candidate"),
             Generator->TryProveSealedSolidTileForTest(
                 ProofTile.Origin, ProofTile.Step, ProofTile.Cells, DiffGuardDisabled));

    const int32 WeakenedNonSolidSamples = CountNonSolidSamples(*Generator, ProofTile);
    AddInfo(FString::Printf(
        TEXT("Weakened proof (diff guard disabled): %d non-solid core-lattice samples after the "
             "carve."),
        WeakenedNonSolidSamples));
    TestTrue(TEXT("the brute-force oracle fails the deliberately weakened guard"),
             WeakenedNonSolidSamples > 0);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
