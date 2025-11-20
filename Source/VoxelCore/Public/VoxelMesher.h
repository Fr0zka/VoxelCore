#pragma once

#include "CoreMinimal.h"
#include "VoxelStructs.h"
#include "UObject/NoExportTypes.h"
#include "VoxelMesher.generated.h"

class UProceduralMeshComponent;

class URealtimeMeshComponent;

class UVoxelSettings;

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

    /**
     * Compute a hash of all neighbor border data. Used to detect if neighbors
     * have changed and seam remeshing is actually necessary.
     * OPTIMIZATION: Avoids unnecessary seam remeshes when neighbors haven't changed.
     */
    uint32 ComputeHash() const
    {
        uint32 Hash = 0;
        Hash = HashCombine(Hash, ::GetTypeHash(SizeX));
        Hash = HashCombine(Hash, ::GetTypeHash(SizeY));
        Hash = HashCombine(Hash, ::GetTypeHash(SizeZ));

        // Hash each border array if it exists
        if (bHasXNeg) for (const EVoxelBlockID& B : XNeg) Hash = HashCombine(Hash, (uint32)B);
        if (bHasXPos) for (const EVoxelBlockID& B : XPos) Hash = HashCombine(Hash, (uint32)B);
        if (bHasYNeg) for (const EVoxelBlockID& B : YNeg) Hash = HashCombine(Hash, (uint32)B);
        if (bHasYPos) for (const EVoxelBlockID& B : YPos) Hash = HashCombine(Hash, (uint32)B);
        if (bHasZNeg) for (const EVoxelBlockID& B : ZNeg) Hash = HashCombine(Hash, (uint32)B);
        if (bHasZPos) for (const EVoxelBlockID& B : ZPos) Hash = HashCombine(Hash, (uint32)B);

        return Hash;
    }
};

USTRUCT()
struct FMeshBuffers
{
    GENERATED_BODY()
    TArray<FVector>    Vertices;
    TArray<int32>      Triangles;     // keep int32; PMC requires it
    TArray<FVector2D>  UVs;
    TArray<FColor>     Colors;        // packed 8-bit RGBA
    TArray<FVector>    Normals;

    void Reset()
    {
        Vertices.Reset(); Triangles.Reset(); UVs.Reset(); Colors.Reset(); Normals.Reset();
    }
    void Shrink()
    {
        Vertices.Shrink(); Triangles.Shrink(); UVs.Shrink(); Colors.Shrink(); Normals.Shrink();
    }
};

UCLASS()
class VOXELCORE_API UVoxelMesher : public UObject
{
    GENERATED_BODY()

public:
    /** Greedy mesher for voxel volume (off-thread). Anisotropic scale: XY scaled by XYScale, Z stays 1x. */
    static void BuildGreedyMesh(
        const TArray<EVoxelBlockID>& Voxels,
        const FIntVector& Size,              // logical size of Voxels
        const FChunkNeighbors* Neighbors,    // optional 1-voxel halo
        float VoxelUU,                       // base UU per voxel (LOD0)
        int32 XYScale,                       // 1 for LOD0; 2/3/... for LOD1
        bool bUseAO,
        const class UVoxelBlockTable* BlockTable,
        FMeshBuffers& OutBuffers,
        const FVoxelLightData* LightData = nullptr);  // Optional voxel lighting

    /**
     * Binary greedy mesher for voxel volumes.  This variant precomputes
     * run‑lengths in each slice and uses a flood‑fill style algorithm to
     * generate larger quads with fewer per‑voxel checks.  It separates faces
     * by block category (semi‑solid vs solid) and will generate quads only
     * within each category.  It can provide a modest speedup over the
     * standard greedy mesher on dense chunks.
     *
     * OPTIMIZATION: Now accepts pre-computed Cats array to eliminate conversion overhead.
     */
    static void BuildBinaryGreedyMesh(
        const TArray<uint8>& Cats,               // Pre-computed categories (0=air,1=semi,2=solid)
        const TArray<EVoxelBlockID>& Voxels,
        const FIntVector& Size,
        const FChunkNeighbors* Neighbors,
        float VoxelUU,
        int32 XYScale,
        bool bUseAO,
        const class UVoxelBlockTable* BlockTable,
        FMeshBuffers& OutBuffers,
        const FVoxelLightData* LightData = nullptr);  // Optional voxel lighting

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

    // New overload to pass categories directly (0=air,1=semi,2=solid)
    static void BuildBinaryGreedyMesh_Cats(
        const TArray<uint8>& Cats,                 // 0=air,1=semi,2=solid
        const TArray<EVoxelBlockID>& Voxels,       // full IDs for layer lookups
        const FIntVector& Size,
        const FChunkNeighbors* Nbh,
        float VoxelUU,
        int32 XYScale,
        bool bUseAO,
        const class UVoxelBlockTable* BlockTable,  // face→layer map
        FMeshBuffers& Out);

    /**
     * TRUE binary greedy mesher: Uses bitwise operations to process 64 voxels at once.
     * This is 10-50x faster than the standard mesher for face-finding.
     *
     * Algorithm:
     * - Loads rows of 64 voxels as uint64
     * - Uses bitwise XOR/AND to find all faces in one operation
     * - Uses CTZ (count trailing zeros) to find runs
     * - Merges faces greedily using bit manipulation
     *
     * Performance: Processes 64 voxels per comparison instead of 1.
     * Expected speedup: 10-50x for face-finding, 3-10x overall meshing time.
     */
    static void BuildTrueBinaryGreedyMesh(
        const TArray<uint8>& Cats,                 // 0=air,1=semi,2=solid
        const TArray<EVoxelBlockID>& Voxels,       // full IDs for layer lookups
        const FIntVector& Size,
        const FChunkNeighbors* Nbh,
        float VoxelUU,
        int32 XYScale,
        bool bUseAO,
        const class UVoxelBlockTable* BlockTable,
        FMeshBuffers& Out);

    /**
     * Naive mesher: generates one quad per visible voxel face (no greedy merging).
     * Useful for debugging mesh generation issues. Produces high vertex/triangle counts.
     */
    static void BuildNaiveMesh(
        const TArray<EVoxelBlockID>& Voxels,
        const FIntVector& Size,
        const FChunkNeighbors* Neighbors,
        float VoxelUU,
        int32 XYScale,
        bool bUseAO,
        const class UVoxelBlockTable* BlockTable,
        FMeshBuffers& OutBuffers);

    /** Apply to ProceduralMeshComponent on GameThread. */
    static void ApplyToPMC(
        UProceduralMeshComponent* PMC,
        const FMeshBuffers& Bufs,
        bool bCreateCollision);

    static void ApplyToRMC(URealtimeMeshComponent* RMC, const FMeshBuffers& Bufs, bool bCreateCollision);

    // NEW: GPU path for LOD0 and LOD1 (fallback to CPU if disabled/failed)
    static bool BuildGreedyMesh_GPU(
        const TArray<EVoxelBlockID>& Voxels,
        const FIntVector& Size,
        const FChunkNeighbors* Neighbors,
        int32 XYScale,
        float VoxelUU,
        FMeshBuffers& Out,
        const UVoxelSettings* Settings = nullptr);

    static bool BuildGreedyMesh_GPU_Async(
        const TArray<EVoxelBlockID>& Voxels,
        const FIntVector& Size,
        const FChunkNeighbors* Neighbors,
        int32 XYScale,
        float VoxelUU,
        TFunction<void(bool bSuccess, TArray<uint32>&& Packed, int32 SizeX, int32 SizeY, int32 SizeZ, int32 XYScaleParam, float VoxelUUParam)> Completion,
        const UVoxelSettings* Settings = nullptr);


};
