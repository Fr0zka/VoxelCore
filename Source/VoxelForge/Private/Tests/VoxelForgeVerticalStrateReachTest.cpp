// Surface-strate streaming must keep the ground visible when the pawn is high in the same strate.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelClipmapDesiredTiles.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeVerticalStrateReachTest,
    "VoxelForge.Streaming.VerticalStrateReach",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeVerticalStrateReachTest::RunTest(const FString& Parameters)
{
    constexpr int32 StrateBottomZ = -63;
    constexpr int32 StrateTopZ = 0;
    constexpr int32 PlayerChunkZ = 0; // high above this surface strate's ground

    int32 MinChunkZ = MIN_int32;
    int32 MaxChunkZ = MAX_int32;
    VoxelClipmapDesiredTiles::ResolveVerticalBounds(
        PlayerChunkZ, 5, 5, 3, true, true,
        StrateTopZ, StrateBottomZ, MinChunkZ, MaxChunkZ);
    TestEqual(TEXT("vertical reach follows the full surface strate plus margin"), MinChunkZ, -66);
    TestEqual(TEXT("open-strate reach ends at the surface cap margin"), MaxChunkZ, 3);

    VoxelClipmapDesiredTiles::FParameters Clipmap;
    Clipmap.ClipRadius = 3;
    Clipmap.MaxClipLevel = 3;
    Clipmap.RenderDistanceChunks = 768;
    Clipmap.bFarSheetRing = true;
    Clipmap.FarSheetSpanLevels = 2;

    TArray<FVoxelTileKey> Desired;
    VoxelClipmapDesiredTiles::Build(
        FIntVector(0, 0, PlayerChunkZ), MinChunkZ, MaxChunkZ, Clipmap, Desired);

    TArray<int32> CountsByLevel;
    CountsByLevel.Init(0, 6);
    for (const FVoxelTileKey& Tile : Desired)
    {
        if (CountsByLevel.IsValidIndex(Tile.Level))
        {
            ++CountsByLevel[Tile.Level];
        }
    }

    const int32 ExpectedCounts[] = {343, 218, 178, 178, 178, 9586};
    for (int32 Level = 0; Level < static_cast<int32>(UE_ARRAY_COUNT(ExpectedCounts)); ++Level)
    {
        TestEqual(FString::Printf(TEXT("desired tile count at level %d"), Level),
                  CountsByLevel[Level], ExpectedCounts[Level]);
    }

    const auto HasTile = [&Desired](int32 Level, const FIntVector& Coord)
    {
        return Desired.ContainsByPredicate([Level, &Coord](const FVoxelTileKey& Tile)
        {
            return Tile.Level == Level && Tile.Coord == Coord;
        });
    };

    TestTrue(TEXT("level 0 tile around the high pawn is present"),
             HasTile(0, FIntVector(0, 0, 0)));
    TestTrue(TEXT("level 4 bridge tile covering the ground below the pawn is present"),
             HasTile(4, FIntVector(0, 0, -3)));
    TestTrue(TEXT("adjacent level 4 ground tile is present"),
             HasTile(4, FIntVector(1, 0, -3)));
    TestTrue(TEXT("outer level 5 sheet reaches the 768-chunk horizon"),
             HasTile(5, FIntVector(24, 0, 0)));
    TestFalse(TEXT("clamping does not request a full hidden LOD0 column"),
              HasTile(0, FIntVector(0, 0, -47)));

    for (int32 Level = 0; Level <= 5; ++Level)
    {
        TestTrue(FString::Printf(TEXT("clip level %d is non-empty"), Level),
                 CountsByLevel[Level] > 0);
    }

    AddInfo(FString::Printf(
        TEXT("center_z=%d strate=[%d..%d] view=[%d..%d] levels=[%d,%d,%d,%d,%d,%d] total=%d"),
        PlayerChunkZ, StrateBottomZ, StrateTopZ, MinChunkZ, MaxChunkZ,
        CountsByLevel[0], CountsByLevel[1], CountsByLevel[2], CountsByLevel[3],
        CountsByLevel[4], CountsByLevel[5], Desired.Num()));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
