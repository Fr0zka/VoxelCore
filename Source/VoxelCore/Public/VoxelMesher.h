#pragma once

#include "CoreMinimal.h"
#include "VoxelStructs.h"
#include "VoxelWorld.h"
#include "UObject/NoExportTypes.h"
#include "VoxelMesher.generated.h"

class UProceduralMeshComponent;

#if WITH_RUNTIME_MESHCOMPONENT
class URuntimeMeshComponent;
#endif

/** Border slices sampled from neighbors (1-voxel halo). */
USTRUCT()
struct FChunkNeighbors
{
    GENERATED_BODY()

    int32 SizeX = 0, SizeY = 0, SizeZ = 0;

    TArray<EVoxelBlockID> XNeg; bool bHasXNeg = false;
    TArray<EVoxelBlockID> XPos; bool bHasXPos = false;
    TArray<EVoxelBlockID> YNeg; bool bHasYNeg = false;
    TArray<EVoxelBlockID> YPos; bool bHasYPos = false;
    TArray<EVoxelBlockID> ZNeg; bool bHasZNeg = false;
    TArray<EVoxelBlockID> ZPos; bool bHasZPos = false;
};

USTRUCT()
struct FMeshBuffers
{
    GENERATED_BODY()

    TArray<FVector>       Vertices;
    TArray<int32>         Triangles;
    TArray<FVector2D>     UVs;
    TArray<FLinearColor>  Colors;
    TArray<FVector>       Normals;
};
//_______________________________________________________________________________________________//
    // --- Compact per-chunk voxel storage (no air persisted) ---
struct FCompactVoxelData
{
    // Occupancy: 1 bit = solid, 0 = air
    TArray<uint64> Occupancy;    // NumWords = ceil(SizeX*SizeY*SizeZ / 64)
    // Packed IDs for solid voxels only (uint16 is enough for large palettes)
    TArray<uint16> Ids;

    // Optional prefix rank cache per 64-bit word for O(1) rank1 lookups
    TArray<uint32> Prefix64;     // Prefix64[w] = #set-bits in Occupancy[0..w-1]

    int32 SizeX = 0, SizeY = 0, SizeZ = 0;  // in LOD-local voxel units
    int32 XYScale = 1;                      // world XY scale factor for this LOD
};
UCLASS()
class VOXELCORE_API UVoxelMesher : public UObject
{
    GENERATED_BODY()

public:
    /** Greedy mesher for voxel volume (off-thread). Anisotropic scale: XY scaled by XYScale, Z stays 1×. */
    static void BuildGreedyMesh(
        const TArray<EVoxelBlockID>& Voxels,
        const FIntVector& Size,              // logical size of Voxels
        const FChunkNeighbors* Neighbors,    // optional 1-voxel halo
        float VoxelUU,                       // base UU per voxel (LOD0)
        int32 XYScale,                       // 1 for LOD0; 2/3/... for LOD1
        bool bUseAO,
        FMeshBuffers& OutBuffers);

    static void BuildGreedyMesh_FaceMask(
        const TArray<EVoxelBlockID>& Voxels,
        const FIntVector& Size,
        const FChunkNeighbors* Nbh,
        float VoxelUU,
        int32 XYScale,
        bool bUseAO,
        FMeshBuffers& Out);

    static void BuildGreedyMesh_FastLOD1(
        const TArray<EVoxelBlockID>& Voxels,
        const FIntVector& Size,              // in LOD1 voxel units
        const FChunkNeighbors* Nbh,          // may be null
        float VoxelUU,
        int32 XYScale,
        FMeshBuffers& Out);


    /** Heightfield impostor (surface only), using (CellsX+1)*(CellsY+1) samples. */
    static void BuildHeightfieldMesh(
        const TArray<int32>& Heights,        // [SamplesX * SamplesY]
        int32 SamplesX,
        int32 SamplesY,
        int32 ChunkSizeX,                    // original LOD0 chunk X size
        int32 ChunkSizeY,                    // original LOD0 chunk Y size
        int32 XYScale,                       // LOD2 XY scale (>= LOD1)
        float VoxelUU,                       // base UU per voxel (LOD0)
        FMeshBuffers& OutBuffers);

    static void BuildHeightfieldMesh_Grid(const TArray<int32>& Heights, int32 SamplesX, int32 SamplesY, int32 ChunkSizeX, int32 ChunkSizeY, int32 XYScale, float VoxelUU, const TArray<int32>& SharedIB, FMeshBuffers& Out);

    /** Apply to ProceduralMeshComponent on GameThread. */
    static void ApplyToPMC(
        UProceduralMeshComponent* PMC,
        const FMeshBuffers& Bufs,
        bool bCreateCollision);

    // NEW: GPU path for LOD1 (fallback to CPU if disabled/failed)
    static bool BuildGreedyMesh_GPU(const TArray<EVoxelBlockID>& Voxels,
        FIntVector Size,
        int32 XYScale,
        float VoxelUU,
        FMeshBuffers& Out);

    // Build greedy mesh directly from compact bitset+IDs (LOD1 fast path, no AO)
    static void BuildGreedyMesh_FromBitset(
        const FCompactVoxelData& C,
        float VoxelUU,
        FMeshBuffers& Out);

    // Recompute Prefix64 from Occupancy
    static void RebuildPrefix64(FCompactVoxelData& C);

    // Fast rank1 using Prefix64
    static int32 Rank1_Prefix(const FCompactVoxelData& C, int32 linearIdx);

    static int32 Idx3D(int32 x, int32 y, int32 z, int32 SX, int32 SY);


    // LOD0 greedy mesher from bitset + IDs, with optional AO and neighbor halos
    static void BuildGreedyMesh_FromBitset_AO(
        const FCompactVoxelData& C,
        const FChunkNeighbors* NeighOrNull,   // may be null
        float VoxelUU,
        bool  bUseAO,                         // AO only for LOD0
        FMeshBuffers& Out);









    //_______________________________________________________________________________________________//


   static void ApplyToPMC_Create(UProceduralMeshComponent* PMC, int32 SectionIndex, const FMeshBuffers& B, bool bCreateCollision);
   static void ApplyToPMC_Update(UProceduralMeshComponent* PMC, int32 SectionIndex, const FMeshBuffers& B);

#if WITH_RUNTIME_MESHCOMPONENT
    /** Apply to RuntimeMeshComponent on GameThread (if plugin present). */
    static void ApplyToRMC(
        URuntimeMeshComponent* RMC,
        const FMeshBuffers& Bufs,
        bool bCreateCollision);
#endif
};
struct FNeighborSolidMasks
{
    // Sizes of the current chunk (for indexing)
    int32 SizeX = 0, SizeY = 0, SizeZ = 0;

    // Bit order: row-major in the face's local 2D coordinates:
    // XNeg/XPos: (y,z) space -> bits = SizeY * SizeZ
    // YNeg/YPos: (x,z) space -> bits = SizeX * SizeZ
    // ZNeg/ZPos: (x,y) space -> bits = SizeX * SizeY
    TBitArray<> XNeg; bool bHasXNeg = false;
    TBitArray<> XPos; bool bHasXPos = false;
    TBitArray<> YNeg; bool bHasYNeg = false;
    TBitArray<> YPos; bool bHasYPos = false;
    TBitArray<> ZNeg; bool bHasZNeg = false;
    TBitArray<> ZPos; bool bHasZPos = false;

    void Reset(int32 InSX, int32 InSY, int32 InSZ)
    {
        SizeX = InSX; SizeY = InSY; SizeZ = InSZ;
        XNeg = TBitArray<>(false, SizeY * SizeZ); bHasXNeg = false;
        XPos = TBitArray<>(false, SizeY * SizeZ); bHasXPos = false;
        YNeg = TBitArray<>(false, SizeX * SizeZ); bHasYNeg = false;
        YPos = TBitArray<>(false, SizeX * SizeZ); bHasYPos = false;
        ZNeg = TBitArray<>(false, SizeX * SizeY); bHasZNeg = false;
        ZPos = TBitArray<>(false, SizeX * SizeY); bHasZPos = false;
    }
};


