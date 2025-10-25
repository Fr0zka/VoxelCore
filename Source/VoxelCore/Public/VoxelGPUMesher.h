#pragma once
#include "CoreMinimal.h"

struct FMeshBuffers;

// 32-bit packed vertex layout:
// 0..5:  posX (0..63)
// 6..11: posY (0..63)
// 12..17:posZ (0..63)
// 18..20:normal (0..5)
// 21..22:AO (0..3)
// 23..30:blockID (0..255)
// 31:    reserved
struct FPackedVoxelVert
{
    uint32 Packed = 0;
};

struct FGPUMeshBuildParams
{
    // Linear block array; 0=air. Expect size SizeX*SizeY*SizeZ.
    const uint8* Voxels = nullptr;
    int32 SizeX = 0, SizeY = 0, SizeZ = 0;
    int32 XYScale = 1;      // LOD coarsening (not yet used in CS merge)
    uint8 DefaultAO = 3;    // 0..3
    float VoxelUU = 100.f;  // for CPU decode to FMeshBuffers (temp)
};

class VOXELCORE_API FVoxelGPUMesher
{
public:
    // Run compute shader; returns packed verts in OutPackedVerts (face-per-voxel for now).
    static bool BuildPackedVerts_GPU(const FGPUMeshBuildParams& Params, TArray<uint32>& OutPackedVerts);

    // TEMP: decode packed verts back to standard mesh buffers for your PMC/RMC path.
    static void DecodePackedVertsToMeshBuffers(const TArray<uint32>& Packed,
        FMeshBuffers & Out,
        float VoxelUU,
        int32 XYScale);
};
