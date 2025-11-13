#pragma once
#include "CoreMinimal.h"
#include "Templates/Function.h"

struct FMeshBuffers;

// 32-bit packed vertex layout (7-bit coords, no AO):
// 0..6:  posX (0..127)
// 7..13: posY (0..127)
// 14..20:posZ (0..127)
// 21..23:normal (0..5)
// 24..31:blockID (0..255)
struct FPackedVoxelVert
{
    uint32 Packed = 0;
};

struct FGPUMeshBuildParams
{
    const uint8* Voxels = nullptr;
    int32 SizeX = 0, SizeY = 0, SizeZ = 0;
    int32 XYScale = 1;
    uint8 DefaultAO = 3;
    float VoxelUU = 100.f;

    // NEW: Neighbor data for seamless edges
    const uint8* NeighborXN = nullptr;
    const uint8* NeighborXP = nullptr;
    const uint8* NeighborYN = nullptr;
    const uint8* NeighborYP = nullptr;

    bool bHasNeighborXN = false;
    bool bHasNeighborXP = false;
    bool bHasNeighborYN = false;
    bool bHasNeighborYP = false;

    // Added Z‑axis neighbors for 3D chunk support.  When bHasNeighborZN is true,
    // NeighborZN points to an array of SizeX*SizeY category values corresponding
    // to the topmost XY slice of the chunk immediately below this one.  When
    // bHasNeighborZP is true, NeighborZP points to an array of SizeX*SizeY
    // category values for the bottommost XY slice of the chunk immediately
    // above this one.  These buffers are used by the GPU greedy mesher to
    // correctly cull faces between vertically adjacent chunks.
    const uint8* NeighborZN = nullptr;
    const uint8* NeighborZP = nullptr;
    bool bHasNeighborZN = false;
    bool bHasNeighborZP = false;

    int32 MaxOutputVerts = INT32_MAX;
    bool bAggressiveCulling = false;
};

class VOXELCORE_API FVoxelGPUMesher
{
public:
    static bool BuildPackedVerts_GPU(const FGPUMeshBuildParams& Params, TArray<uint32>& OutPackedVerts);
    static bool BuildPackedVerts_GPU_Async(const FGPUMeshBuildParams& Params, TFunction<void(bool bSuccess, TArray<uint32>&& Packed)> Completion);
    static void PumpAsyncReadbacks();
    static bool HasPendingAsyncReadbacks();
    static void DecodePackedVertsToMeshBuffers(const TArray<uint32>& Packed,
        FMeshBuffers& Out,
        float VoxelUU,
        int32 XYScale,
        int32 SizeX,
        int32 SizeY,
        int32 SizeZ);
};
