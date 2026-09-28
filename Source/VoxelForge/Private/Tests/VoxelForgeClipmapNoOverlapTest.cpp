#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelClipmapDesiredTiles.h"

namespace
{
    using VoxelClipmapDesiredTiles::FParameters;

    struct FOverlapMeasure
    {
        uint64 PairCount = 0;
        uint64 PairwiseVolumeChunkCubed = 0;
    };

    static FOverlapMeasure MeasureOverlaps(const TArray<FVoxelTileKey>& Tiles)
    {
        TSet<FVoxelTileKey> TileSet;
        int32 MaxLevel = 0;
        for (const FVoxelTileKey& Tile : Tiles)
        {
            TileSet.Add(Tile);
            MaxLevel = FMath::Max(MaxLevel, Tile.Level);
        }

        FOverlapMeasure Result;
        for (const FVoxelTileKey& Fine : Tiles)
        {
            for (int32 CoarseLevel = Fine.Level + 1; CoarseLevel <= MaxLevel; ++CoarseLevel)
            {
                const int32 Divisor = 1 << (CoarseLevel - Fine.Level);
                const FVoxelTileKey Coarse(
                    VoxelClipmapDesiredTiles::FloorDiv(Fine.Coord, Divisor), CoarseLevel);
                if (TileSet.Contains(Coarse))
                {
                    ++Result.PairCount;
                    Result.PairwiseVolumeChunkCubed += (uint64(1) << (3 * Fine.Level));
                }
            }
        }
        return Result;
    }

    static bool IsFootprintCovered(const FVoxelTileKey& Tile,
                                   const TSet<FVoxelTileKey>& CoveringTiles,
                                   int32 MaxLevel, int32 MinChunkZ, int32 MaxChunkZ)
    {
        const int32 TileMinZ = Tile.Coord.Z * (1 << Tile.Level);
        const int32 TileMaxZ = (Tile.Coord.Z + 1) * (1 << Tile.Level) - 1;
        if (TileMaxZ < MinChunkZ || TileMinZ > MaxChunkZ)
        {
            return true;
        }

        if (CoveringTiles.Contains(Tile))
        {
            return true;
        }

        // A coarser dyadic tile covers this whole footprint when it is an ancestor.
        for (int32 ParentLevel = Tile.Level + 1; ParentLevel <= MaxLevel; ++ParentLevel)
        {
            const int32 Divisor = 1 << (ParentLevel - Tile.Level);
            const FVoxelTileKey Parent(
                VoxelClipmapDesiredTiles::FloorDiv(Tile.Coord, Divisor), ParentLevel);
            if (CoveringTiles.Contains(Parent))
            {
                return true;
            }
        }

        if (Tile.Level == 0)
        {
            return Tile.Coord.Z < MinChunkZ || Tile.Coord.Z > MaxChunkZ;
        }

        const int32 ChildLevel = Tile.Level - 1;
        const FIntVector ChildBase = Tile.Coord * 2;
        for (int32 Z = 0; Z < 2; ++Z)
        for (int32 Y = 0; Y < 2; ++Y)
        for (int32 X = 0; X < 2; ++X)
        {
            if (!IsFootprintCovered(
                    FVoxelTileKey(ChildBase + FIntVector(X, Y, Z), ChildLevel),
                    CoveringTiles, MaxLevel, MinChunkZ, MaxChunkZ))
            {
                return false;
            }
        }
        return true;
    }

    static bool CoversEveryLegacyTile(const TArray<FVoxelTileKey>& Legacy,
                                      const TArray<FVoxelTileKey>& Current,
                                      int32 MinChunkZ, int32 MaxChunkZ)
    {
        TSet<FVoxelTileKey> CurrentSet;
        int32 MaxLevel = 0;
        for (const FVoxelTileKey& Tile : Current)
        {
            CurrentSet.Add(Tile);
            MaxLevel = FMath::Max(MaxLevel, Tile.Level);
        }
        for (const FVoxelTileKey& Tile : Legacy)
        {
            if (!IsFootprintCovered(Tile, CurrentSet, MaxLevel, MinChunkZ, MaxChunkZ))
            {
                return false;
            }
        }
        return true;
    }

    // Frozen copy of the 7486c29 selector. It is the coverage oracle and keeps the regression
    // test able to prove that an exact tiling still covers every volume the old selector covered.
    static void Build7486c29(const FIntVector& Center, int32 MinChunkZ, int32 MaxChunkZ,
                             const FParameters& Parameters, TArray<FVoxelTileKey>& OutTiles)
    {
        OutTiles.Reset();
        const int32 Radius = FMath::Max(1, Parameters.ClipRadius);
        const int32 MaxLevel = FMath::Clamp(Parameters.MaxClipLevel, 0, 8);
        const int32 OuterRadius = VoxelClipmapDesiredTiles::OuterShellRadius(
            Parameters, Radius, MaxLevel);

        for (int32 Level = 0; Level <= MaxLevel; ++Level)
        {
            const int32 Pow = 1 << Level;
            const FIntVector CenterLevel = VoxelClipmapDesiredTiles::FloorDiv(Center, Pow);
            const FIntVector CenterFiner = Level > 0
                ? VoxelClipmapDesiredTiles::FloorDiv(Center, Pow >> 1)
                : FIntVector::ZeroValue;
            const int32 LevelRadius = (Level == MaxLevel) ? OuterRadius : Radius;

            int32 DeltaZMin = -LevelRadius;
            int32 DeltaZMax = LevelRadius;
            if (LevelRadius > Radius && MinChunkZ != MIN_int32)
            {
                DeltaZMin = FMath::Max(
                    DeltaZMin, VoxelClipmapDesiredTiles::FloorDiv(MinChunkZ, Pow) - CenterLevel.Z);
                DeltaZMax = FMath::Min(
                    DeltaZMax, VoxelClipmapDesiredTiles::FloorDiv(MaxChunkZ, Pow) - CenterLevel.Z);
            }

            for (int32 DeltaZ = DeltaZMin; DeltaZ <= DeltaZMax; ++DeltaZ)
            for (int32 DeltaY = -LevelRadius; DeltaY <= LevelRadius; ++DeltaY)
            for (int32 DeltaX = -LevelRadius; DeltaX <= LevelRadius; ++DeltaX)
            {
                const FIntVector Tile = CenterLevel + FIntVector(DeltaX, DeltaY, DeltaZ);
                const int32 TileMinZ = Tile.Z << Level;
                const int32 TileMaxZ = ((Tile.Z + 1) << Level) - 1;
                if (TileMaxZ < MinChunkZ || TileMinZ > MaxChunkZ)
                {
                    continue;
                }

                if (Level > 0)
                {
                    const bool bCovered =
                        (2 * Tile.X >= CenterFiner.X - Radius)
                        && (2 * Tile.X + 1 <= CenterFiner.X + Radius)
                        && (2 * Tile.Y >= CenterFiner.Y - Radius)
                        && (2 * Tile.Y + 1 <= CenterFiner.Y + Radius)
                        && (2 * Tile.Z >= CenterFiner.Z - Radius)
                        && (2 * Tile.Z + 1 <= CenterFiner.Z + Radius);
                    if (bCovered)
                    {
                        continue;
                    }
                }
                OutTiles.Emplace(Tile, Level);
            }
        }
    }

    static FString DescribeMeasure(const FOverlapMeasure& Measure)
    {
        return FString::Printf(TEXT("pairs=%llu pairwise_volume_chunk3=%llu"),
            static_cast<unsigned long long>(Measure.PairCount),
            static_cast<unsigned long long>(Measure.PairwiseVolumeChunkCubed));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeClipmapNoOverlapTest,
    "VoxelForge.Streaming.ClipmapDesiredTilesNoOverlapCoverage",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeClipmapNoOverlapTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FParameters Owner;
    Owner.ClipRadius = 3;
    Owner.MaxClipLevel = 5;
    Owner.RenderDistanceChunks = 768;

    FParameters Defaults;

    TArray<FVoxelTileKey> LegacyOwner, CurrentOwner, LegacyDefaults, CurrentDefaults;
    Build7486c29(FIntVector::ZeroValue, MIN_int32, MAX_int32, Owner, LegacyOwner);
    VoxelClipmapDesiredTiles::Build(
        FIntVector::ZeroValue, MIN_int32, MAX_int32, Owner, CurrentOwner);
    Build7486c29(FIntVector::ZeroValue, MIN_int32, MAX_int32, Defaults, LegacyDefaults);
    VoxelClipmapDesiredTiles::Build(
        FIntVector::ZeroValue, MIN_int32, MAX_int32, Defaults, CurrentDefaults);

    const FOverlapMeasure LegacyOwnerOverlap = MeasureOverlaps(LegacyOwner);
    const FOverlapMeasure CurrentOwnerOverlap = MeasureOverlaps(CurrentOwner);
    const FOverlapMeasure LegacyDefaultOverlap = MeasureOverlaps(LegacyDefaults);
    const FOverlapMeasure CurrentDefaultOverlap = MeasureOverlaps(CurrentDefaults);

    AddInfo(FString::Printf(
        TEXT("owner center=(0,0,0): before tiles=%d %s; after tiles=%d %s"),
        LegacyOwner.Num(), *DescribeMeasure(LegacyOwnerOverlap),
        CurrentOwner.Num(), *DescribeMeasure(CurrentOwnerOverlap)));
    AddInfo(FString::Printf(
        TEXT("FParameters defaults center=(0,0,0): before tiles=%d %s; after tiles=%d %s"),
        LegacyDefaults.Num(), *DescribeMeasure(LegacyDefaultOverlap),
        CurrentDefaults.Num(), *DescribeMeasure(CurrentDefaultOverlap)));

    TestTrue(TEXT("7486c29 owner overlap measurement matches the checked source geometry"),
        LegacyOwnerOverlap.PairCount == 635
            && LegacyOwnerOverlap.PairwiseVolumeChunkCubed == 594487);
    TestTrue(TEXT("7486c29 FParameters-default overlap measurement matches the checked source geometry"),
        LegacyDefaultOverlap.PairCount == 508
            && LegacyDefaultOverlap.PairwiseVolumeChunkCubed == 74295);
    TestTrue(TEXT("owner desired set has no overlapping level pairs"),
        CurrentOwnerOverlap.PairCount == 0 && CurrentOwnerOverlap.PairwiseVolumeChunkCubed == 0);
    TestTrue(TEXT("default desired set has no overlapping level pairs"),
        CurrentDefaultOverlap.PairCount == 0 && CurrentDefaultOverlap.PairwiseVolumeChunkCubed == 0);
    TestTrue(TEXT("owner coverage is identical or larger than 7486c29"),
        CoversEveryLegacyTile(LegacyOwner, CurrentOwner, MIN_int32, MAX_int32));
    TestTrue(TEXT("default coverage is identical or larger than 7486c29"),
        CoversEveryLegacyTile(LegacyDefaults, CurrentDefaults, MIN_int32, MAX_int32));

    struct FSweepSettings
    {
        int32 Radius;
        int32 MaxLevel;
        int32 RenderDistance;
    };
    static const FSweepSettings SweepSettings[] = {
        {1, 0, 0},
        {2, 2, 0},
        {3, 4, 0},
        {2, 3, 49},
        {2, 4, 87},
    };

    int32 SweptCases = 0;
    int32 OverlapCases = 0;
    int32 CoverageFailures = 0;
    uint64 TotalOverlapPairs = 0;
    FString FirstFailure;
    const int32 CenterMin = -3;
    const int32 CenterMax = 3;
    for (int32 Z = CenterMin; Z <= CenterMax; ++Z)
    for (int32 Y = CenterMin; Y <= CenterMax; ++Y)
    for (int32 X = CenterMin; X <= CenterMax; ++X)
    for (const FSweepSettings& Sweep : SweepSettings)
    {
        FParameters P;
        P.ClipRadius = Sweep.Radius;
        P.MaxClipLevel = Sweep.MaxLevel;
        P.RenderDistanceChunks = Sweep.RenderDistance;

        // Exercise the unbounded view, a tight local clamp, and a clamp crossing a tile boundary.
        for (int32 ClampCase = 0; ClampCase < 3; ++ClampCase)
        {
            int32 MinZ = MIN_int32;
            int32 MaxZ = MAX_int32;
            if (ClampCase == 1)
            {
                MinZ = Z - 2;
                MaxZ = Z + 3;
            }
            else if (ClampCase == 2)
            {
                const int32 Band = VoxelClipmapDesiredTiles::FloorDiv(Z, 1 << Sweep.MaxLevel);
                MinZ = (Band << Sweep.MaxLevel) - 1;
                MaxZ = MinZ + 6;
            }

            const FIntVector Center(X, Y, Z);
            TArray<FVoxelTileKey> Legacy, Current;
            Build7486c29(Center, MinZ, MaxZ, P, Legacy);
            VoxelClipmapDesiredTiles::Build(Center, MinZ, MaxZ, P, Current);
            ++SweptCases;

            const FOverlapMeasure CurrentOverlap = MeasureOverlaps(Current);
            if (CurrentOverlap.PairCount != 0)
            {
                ++OverlapCases;
                TotalOverlapPairs += CurrentOverlap.PairCount;
                if (FirstFailure.IsEmpty())
                {
                    FirstFailure = FString::Printf(
                        TEXT("overlap center=(%d,%d,%d) radius=%d level=%d distance=%d clamp=%d: %s"),
                        X, Y, Z, Sweep.Radius, Sweep.MaxLevel, Sweep.RenderDistance,
                        ClampCase, *DescribeMeasure(CurrentOverlap));
                }
            }
            if (!CoversEveryLegacyTile(Legacy, Current, MinZ, MaxZ))
            {
                ++CoverageFailures;
                if (FirstFailure.IsEmpty())
                {
                    FirstFailure = FString::Printf(
                        TEXT("coverage center=(%d,%d,%d) radius=%d level=%d distance=%d clamp=%d"),
                        X, Y, Z, Sweep.Radius, Sweep.MaxLevel, Sweep.RenderDistance,
                        ClampCase);
                }
            }
        }
    }

    AddInfo(FString::Printf(
        TEXT("sweep cases=%d centers=343 settings=%d clamps=3 overlap_cases=%d overlap_pairs=%llu coverage_failures=%d first=%s"),
        SweptCases, UE_ARRAY_COUNT(SweepSettings), OverlapCases,
        static_cast<unsigned long long>(TotalOverlapPairs), CoverageFailures,
        FirstFailure.IsEmpty() ? TEXT("none") : *FirstFailure));
    TestEqual(TEXT("all swept desired sets have zero overlapping pairs"), OverlapCases, 0);
    TestEqual(TEXT("all swept desired sets cover the legacy footprint"), CoverageFailures, 0);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
