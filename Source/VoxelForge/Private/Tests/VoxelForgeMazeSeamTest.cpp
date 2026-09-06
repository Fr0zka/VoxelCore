// VoxelForgeMazeSeamTest.cpp
// The Maze parent rule must be independent of which neighbouring chunk was evaluated first.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelCaveMorphology.h"
#include "VoxelForgeTestFixture.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeMazeSeamTest,
    "VoxelForge.Generation.MazeSeamFreedom",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    void AddMazeBoundaryProbes(const FMazeGenerationParams& Params, int32 BottomVoxelZ,
                               int32 TopVoxelZ, TArray<FVector>& OutProbes)
    {
        const float CellSize = FMath::Max(Params.CellSize, 1.0f);
        const int32 MidCellZ = FMath::FloorToInt(
            0.5f * (Params.StrateTopWorldZ + Params.StrateBottomWorldZ) / CellSize);
        const float MidZ = (MidCellZ + 0.5f) * CellSize;
        if (MidZ <= BottomVoxelZ || MidZ >= TopVoxelZ)
        {
            return;
        }

        const int32 CellCoordinates[] = { -5, -2, -1, 0, 1, 2, 5 };
        const float Offsets[] = { -0.25f, 0.0f, 0.25f };
        for (const int32 CellX : CellCoordinates)
        {
            for (const float Offset : Offsets)
            {
                OutProbes.Add(FVector(CellX * CellSize + Offset, 7.25f, MidZ));
                OutProbes.Add(FVector(7.25f, CellX * CellSize + Offset, MidZ));
            }
        }
        for (const int32 CellZ : CellCoordinates)
        {
            for (const float Offset : Offsets)
            {
                const float Z = CellZ * CellSize + Offset;
                if (Z > BottomVoxelZ && Z < TopVoxelZ)
                {
                    OutProbes.Add(FVector(7.25f, -11.5f, Z));
                }
            }
        }
    }

    void WarmNeighbouringChunks(const VoxelForgeTest::FTestWorld& World,
                                const FVector& Probe, int32 Variant)
    {
        const float Delta = static_cast<float>(CHUNK_SIZE * 3);
        const FVector Offsets[] = {
            FVector(Variant == 0 ? -Delta : Delta, Variant == 1 ? -Delta : Delta, 0.0f),
            FVector(Variant == 2 ? Delta : -Delta, Variant == 3 ? Delta : -Delta, 0.0f),
            FVector(Variant == 4 ? Delta : -Delta, Variant == 5 ? -Delta : Delta, 0.0f),
        };
        for (const FVector& Offset : Offsets)
        {
            World.Generator->GetDensityAt(
                Probe.X + Offset.X, Probe.Y + Offset.Y, Probe.Z + Offset.Z);
        }
    }

    int32 CountContextMismatches(const VoxelForgeTest::FTestWorld& World,
                                 const TArray<FVector>& Probes, int32& OutFirstProbe,
                                 float& OutFirstBefore, float& OutFirstAfter)
    {
        int32 Mismatches = 0;
        OutFirstProbe = INDEX_NONE;
        OutFirstBefore = 0.0f;
        OutFirstAfter = 0.0f;
        for (int32 Index = 0; Index < Probes.Num(); ++Index)
        {
            const FVector& Probe = Probes[Index];
            WarmNeighbouringChunks(World, Probe, Index % 6);
            const float Before = World.Generator->GetDensityAt(Probe.X, Probe.Y, Probe.Z);
            WarmNeighbouringChunks(World, Probe, (Index + 3) % 6);
            const float After = World.Generator->GetDensityAt(Probe.X, Probe.Y, Probe.Z);
            if (!VoxelForgeTest::BitEqual(Before, After))
            {
                if (OutFirstProbe == INDEX_NONE)
                {
                    OutFirstProbe = Index;
                    OutFirstBefore = Before;
                    OutFirstAfter = After;
                }
                ++Mismatches;
            }
        }
        return Mismatches;
    }
}

bool FVoxelForgeMazeSeamTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    FTestWorld LegacyWorld;
    LegacyWorld.Build(7331, 2, false, 4);
    FTestWorld StackWorld;
    StackWorld.Build(7331, 2, true, 4);
    if (!LegacyWorld.IsValid() || !StackWorld.IsValid())
    {
        AddError(LegacyWorld.IsValid() ? StackWorld.WhyInvalid() : LegacyWorld.WhyInvalid());
        return false;
    }

    int32 LegacyTop = 0, LegacyBottom = 0;
    int32 StackTop = 0, StackBottom = 0;
    if (!LegacyWorld.GetSlotVoxelZRange(FTestWorld::SlotMaze, LegacyTop, LegacyBottom)
        || !StackWorld.GetSlotVoxelZRange(FTestWorld::SlotMaze, StackTop, StackBottom))
    {
        AddError(TEXT("The seam fixture has no Maze slot."));
        return false;
    }
    const int32 LegacyMidChunkZ = ((LegacyTop + LegacyBottom) / 2) / CHUNK_SIZE;
    const int32 StackMidChunkZ = ((StackTop + StackBottom) / 2) / CHUNK_SIZE;
    const FMazeGenerationParams LegacyParams = LegacyWorld.StrateManager->GetMazeParamsForChunk(
        FIntVector(0, 0, LegacyMidChunkZ));
    const FMazeGenerationParams StackParams = StackWorld.StrateManager->GetMazeParamsForChunk(
        FIntVector(0, 0, StackMidChunkZ));

    TArray<FVector> LegacyProbes;
    TArray<FVector> StackProbes;
    AddMazeBoundaryProbes(LegacyParams, LegacyBottom, LegacyTop, LegacyProbes);
    AddMazeBoundaryProbes(StackParams, StackBottom, StackTop, StackProbes);
    TestEqual(TEXT("legacy and operator seam tests have the same probe count"),
              LegacyProbes.Num(), StackProbes.Num());

    int32 LegacyFirst = INDEX_NONE, StackFirst = INDEX_NONE;
    float LegacyBefore = 0.0f, LegacyAfter = 0.0f;
    float StackBefore = 0.0f, StackAfter = 0.0f;
    const int32 LegacyMismatches = CountContextMismatches(
        LegacyWorld, LegacyProbes, LegacyFirst, LegacyBefore, LegacyAfter);
    const int32 StackMismatches = CountContextMismatches(
        StackWorld, StackProbes, StackFirst, StackBefore, StackAfter);

    TestEqual(TEXT("legacy Maze has no chunk-context seam mismatches"), LegacyMismatches, 0);
    TestEqual(TEXT("operator-stack Maze has no chunk-context seam mismatches"), StackMismatches, 0);
    TestTrue(TEXT("Maze seam test sampled cell-boundary probes"), LegacyProbes.Num() > 0);

    if (LegacyFirst != INDEX_NONE)
    {
        AddError(FString::Printf(TEXT("Legacy Maze seam mismatch at probe %d: %.9g -> %.9g"),
                                 LegacyFirst, LegacyBefore, LegacyAfter));
    }
    if (StackFirst != INDEX_NONE)
    {
        AddError(FString::Printf(TEXT("Operator Maze seam mismatch at probe %d: %.9g -> %.9g"),
                                 StackFirst, StackBefore, StackAfter));
    }

    AddInfo(FString::Printf(
        TEXT("Maze seam freedom: %d boundary probes, legacy mismatches=%d, operator mismatches=%d; "
             "local window=2x2x2 child nodes ({-1,0}^3), no wide collect."),
        LegacyProbes.Num(), LegacyMismatches, StackMismatches));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
