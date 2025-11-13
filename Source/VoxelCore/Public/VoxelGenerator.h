#pragma once

#include "CoreMinimal.h"
#include "VoxelStructs.h"
#include <VoxelNoise.h>
#include "VoxelGenerator.generated.h"


/** Thread-safe copy of settings */
USTRUCT()
struct FChunkGenParams
{
    GENERATED_BODY()

    int32 SizeX = 16;
    int32 SizeY = 16;
    int32 SizeZ = 64;

    int32 Seed = 1337;
    float NoiseScale = 32.f;
    float NoiseAmplitude = 10.f;
    int32 BaseHeight = 20;
    int32 WaterLevel = 18;
    TWeakObjectPtr<class UVoxelMaterialSet> MaterialSet;
    TWeakObjectPtr<class UVoxelBlockTable>  BlockTable;
    TWeakObjectPtr<class UVoxelBiomeTable>  BiomeTable;
    const UVoxelNoiseProfile* NoiseProfile = nullptr;

};

UCLASS()
class VOXELCORE_API UVoxelGenerator : public UObject
{
    GENERATED_BODY()

public:
    static FChunkGenParams MakeParamsFromSettings(const class UVoxelSettings* Settings);

    /** LOD0/LOD1 voxel volume (XY coarsened by LODScaleXY, Z unchanged). */
    static void GenerateChunkLOD(
        const FVoxelCoord& Coord,
        const FChunkGenParams& P,
        int32 LODScaleXY, // 1 for LOD0, 2/3/... for LOD1
        TArray<EVoxelBlockID>& OutData,
        FIntVector& OutSize /* SizeX',SizeY',SizeZ */);

    /**
     * LOD0/LOD1 voxel categories (XY coarsened by LODScaleXY, Z unchanged).
     * Instead of returning full block IDs this fills a compact bitset with
     * 2‑bit categories: 0 = air, 1 = semi‑solid (liquids/foliage), 2 = solid.
     * Caves are carved out using 3D noise as described in the Voxel Tools
     * documentation【884953187607796†L153-L205】, where a squared 2D noise field is
     * modulated by a parabola along Z and combined with a low‑frequency
     * vertical noise to form path‑like tunnels.  The coordinate and LOD
     * scale determine the world position and resolution.
     */
    static void GenerateChunkLOD_Categories(
        const FVoxelCoord& Coord,
        const FChunkGenParams& P,
        int32 LODScaleXY,
        FCategoryBitset& OutCats);

    /** LOD2 heightfield: returns ground height [0..SizeZ-1] per (x,y) at given XY scale. */
    static void GenerateHeightmap(
        const FVoxelCoord& Coord,
        const FChunkGenParams& P,
        int32 LODScaleXY,
        TArray<int32>& OutHeights,
        FIntPoint& OutSizeXY /* SizeX',SizeY' */);

    static int32 SampleHeightWorld(float WX, float WY);

    static void GenerateBiomeGrid2D(
        const FVoxelCoord& Coord,
        const FChunkGenParams& P,
        int32 LODScaleXY,
        FBiomeGrid2D& OutGrid);


private:
    static FORCEINLINE int32 Idx(int32 x, int32 y, int32 z, int32 SX, int32 SY)
    {
        return x + y * SX + z * SX * SY;
    }
};
