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

    FTestWorld World;
    World.Build(7331, 2, 4);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    int32 Top = 0, Bottom = 0;
    if (!World.GetSlotVoxelZRange(FTestWorld::SlotMaze, Top, Bottom))
    {
        AddError(TEXT("The seam fixture has no Maze slot."));
        return false;
    }
    const int32 MidChunkZ = ((Top + Bottom) / 2) / CHUNK_SIZE;
    const FMazeGenerationParams Params = World.StrateManager->GetMazeParamsForChunk(
        FIntVector(0, 0, MidChunkZ));

    TArray<FVector> Probes;
    AddMazeBoundaryProbes(Params, Bottom, Top, Probes);

    int32 First = INDEX_NONE;
    float Before = 0.0f, After = 0.0f;
    const int32 Mismatches = CountContextMismatches(World, Probes, First, Before, After);

    TestEqual(TEXT("Maze has no chunk-context seam mismatches"), Mismatches, 0);
    TestTrue(TEXT("Maze seam test sampled cell-boundary probes"), Probes.Num() > 0);

    if (First != INDEX_NONE)
    {
        AddError(FString::Printf(TEXT("Maze seam mismatch at probe %d: %.9g -> %.9g"),
                                 First, Before, After));
    }

    AddInfo(FString::Printf(
        TEXT("Maze seam freedom: %d boundary probes, mismatches=%d; "
             "local window=2x2x2 child nodes ({-1,0}^3), no wide collect."),
        Probes.Num(), Mismatches));
    return true;
}

// The cave-warp optimization is deliberately tile/LOD agnostic: its three channel inputs are
// world coordinates only.  Exercise the production density path nevertheless, because a cache
// window or an LOD TLS leak could still make an otherwise pure warp look like a seam.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeCaveWarpTileLodSeamTest,
    "VoxelForge.Generation.CaveWarpTileLodSeam",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeCaveWarpTileLodSeamTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    FTestWorld World;
    World.Build(1337, 2, 4);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    int32 TunnelTopZ = 0, TunnelBottomZ = 0;
    if (!World.GetSlotVoxelZRange(FTestWorld::SlotTunnelNetwork, TunnelTopZ, TunnelBottomZ))
    {
        AddError(TEXT("The seam fixture has no TunnelNetwork slot."));
        return false;
    }

    const float TunnelMidZ = 0.5f * (static_cast<float>(TunnelTopZ)
        + static_cast<float>(TunnelBottomZ));
    const int32 ProbeZMid = FMath::Clamp(
        FMath::RoundToInt(TunnelMidZ), TunnelBottomZ + 1, TunnelTopZ - 1);

    const int32 PreviousStep = VoxelGenLOD::GetThreadSampleStep();
    FIntVector PreviousOrigin = FIntVector::ZeroValue;
    int32 PreviousCells = 0;
    int32 PreviousContextStep = PreviousStep;
    VoxelGenLOD::GetThreadTileCacheWindow(
        PreviousOrigin, PreviousContextStep, PreviousCells);

    int32 PairMismatches = 0;
    int32 LodMismatches = 0;
    int32 NumPairSamples = 0;
    FString FirstMismatch;
    auto Compare = [&](const TCHAR* Label, const FVector& Point, float A, float B,
                       int32& Counter)
    {
        if (!BitEqual(A, B))
        {
            ++Counter;
            if (FirstMismatch.IsEmpty())
            {
                FirstMismatch = FString::Printf(
                    TEXT("%s at (%.0f, %.0f, %.0f): %.9g [0x%08X] vs %.9g [0x%08X]"),
                    Label, Point.X, Point.Y, Point.Z,
                    A, *reinterpret_cast<const uint32*>(&A),
                    B, *reinterpret_cast<const uint32*>(&B));
            }
        }
    };

    auto Sample = [&](int32 Step, const FIntVector& TileOrigin, const FVector& Point)
    {
        VoxelGenLOD::SetThreadSampleStep(Step);
        VoxelGenLOD::SetThreadTileContext(TileOrigin, CHUNK_SIZE);
        return World.Generator->GetDensityAt(Point.X, Point.Y, Point.Z);
    };

    // Common points are deliberately on the shared face of adjacent production tiles.  The
    // LOD1 pair is 64 voxels wide (Step=2, Cells=32); its boundary is also a LOD0 tile boundary.
    const int32 CommonOffsets[] = { 4, 12, 20, 28 };
    const int32 ZOffsets[] = { -8, 0, 8 };
    const int32 LodSteps[] = { 1, 2 };
    for (const int32 Step : LodSteps)
    {
        const int32 Extent = CHUNK_SIZE * Step;
        const int32 ZOrigin = FMath::FloorToInt(
            TunnelMidZ / static_cast<float>(Extent)) * Extent;
        const int32 L0Boundary = Extent == CHUNK_SIZE ? CHUNK_SIZE : Extent;
        const FIntVector XLeftOrigin(
            L0Boundary - Extent, 0, ZOrigin);
        const FIntVector XRightOrigin(
            L0Boundary, 0, ZOrigin);
        const FIntVector YLowerOrigin(
            0, L0Boundary - Extent, ZOrigin);
        const FIntVector YUpperOrigin(
            0, L0Boundary, ZOrigin);

        for (const int32 Offset : CommonOffsets)
        {
            for (const int32 ZOffset : ZOffsets)
            {
                const float ProbeZ = FMath::Clamp(
                    static_cast<float>(ProbeZMid + ZOffset),
                    static_cast<float>(TunnelBottomZ + 1),
                    static_cast<float>(TunnelTopZ - 1));

                const FVector XPoint(static_cast<float>(L0Boundary),
                                     static_cast<float>(Offset), ProbeZ);
                const float XLeft = Sample(Step, XLeftOrigin, XPoint);
                const float XRight = Sample(Step, XRightOrigin, XPoint);
                Compare(Step == 1 ? TEXT("LOD0 X tile pair") : TEXT("LOD1 X tile pair"),
                        XPoint, XLeft, XRight, PairMismatches);

                const FVector YPoint(static_cast<float>(Offset),
                                     static_cast<float>(L0Boundary), ProbeZ);
                const float YLower = Sample(Step, YLowerOrigin, YPoint);
                const float YUpper = Sample(Step, YUpperOrigin, YPoint);
                Compare(Step == 1 ? TEXT("LOD0 Y tile pair") : TEXT("LOD1 Y tile pair"),
                        YPoint, YLower, YUpper, PairMismatches);
                NumPairSamples += 2;
            }
        }
    }

    // Compare the same world points through the two LOD contexts.  The default LODOctaveDrop is
    // zero, so LOD changes sampling density, not the value of a shared world sample.
    const int32 CrossLodBoundary = CHUNK_SIZE * 2;
    const int32 L0ZOrigin = FMath::FloorToInt(
        TunnelMidZ / static_cast<float>(CHUNK_SIZE)) * CHUNK_SIZE;
    const int32 L1ZOrigin = FMath::FloorToInt(
        TunnelMidZ / static_cast<float>(CrossLodBoundary)) * CrossLodBoundary;
    for (const int32 Offset : CommonOffsets)
    {
        for (const int32 ZOffset : ZOffsets)
        {
            const float ProbeZ = FMath::Clamp(
                static_cast<float>(ProbeZMid + ZOffset),
                static_cast<float>(TunnelBottomZ + 1),
                static_cast<float>(TunnelTopZ - 1));

            const FVector XPoint(static_cast<float>(CrossLodBoundary),
                                 static_cast<float>(Offset), ProbeZ);
            const float L0X = Sample(
                1, FIntVector(CHUNK_SIZE, 0, L0ZOrigin), XPoint);
            const float L1X = Sample(
                2, FIntVector(0, 0, L1ZOrigin), XPoint);
            Compare(TEXT("same X world point across LOD0/LOD1"),
                    XPoint, L0X, L1X, LodMismatches);

            const FVector YPoint(static_cast<float>(Offset),
                                 static_cast<float>(CrossLodBoundary), ProbeZ);
            const float L0Y = Sample(
                1, FIntVector(0, CHUNK_SIZE, L0ZOrigin), YPoint);
            const float L1Y = Sample(
                2, FIntVector(0, 0, L1ZOrigin), YPoint);
            Compare(TEXT("same Y world point across LOD0/LOD1"),
                    YPoint, L0Y, L1Y, LodMismatches);
        }
    }

    VoxelGenLOD::SetThreadSampleStep(PreviousContextStep);
    VoxelGenLOD::SetThreadTileContext(PreviousOrigin, PreviousCells);

    TestTrue(TEXT("sampled shared boundary points"), NumPairSamples > 0);
    TestEqual(TEXT("LOD0 and LOD1 neighbouring tile boundaries are bit-identical"),
              PairMismatches, 0);
    TestEqual(TEXT("same world boundary points are bit-identical across LOD0 and LOD1"),
              LodMismatches, 0);
    if (!FirstMismatch.IsEmpty())
    {
        AddError(FirstMismatch);
    }
    AddInfo(FString::Printf(
        TEXT("Cave warp tile/LOD seam proof: %d same-LOD pair samples, pair mismatches=%d, "
             "cross-LOD mismatches=%d; shared points sampled from X and Y boundaries."),
        NumPairSamples, PairMismatches, LodMismatches));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
