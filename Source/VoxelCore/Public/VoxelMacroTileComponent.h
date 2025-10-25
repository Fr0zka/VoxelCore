#pragma once

#include "CoreMinimal.h"
#include "VoxelChunkComponent.h"
#include "VoxelMacroTileComponent.generated.h"

// A macro-tile merges an MxM grid of LOD2 chunks into ONE heightfield mesh.
// It overrides generation to build a single large height map and meshing to emit a single mesh.
UCLASS()
class VOXELCORE_API UVoxelMacroTileComponent : public UVoxelChunkComponent
{
    GENERATED_BODY()

public:
    // Tile index in macro-tiles (NOT chunk coords). We store it in ChunkCoord.Cx/Cy for simplicity.
    void InitializeMacroTile(
        FIntPoint InTileCoord,
        const UVoxelSettings* InSettings,
        AVoxelWorld* InWorld);

    // Overrides
    virtual void DoGeneration() override;
    virtual void DoMeshing(bool bSeamRemesh) override;

private:
    // Dimensions in chunks per side
    int32 MacroSize = 8;

    // Sample step in voxels; matches seams with LOD1 by default
    int32 SampleXY = 2;

    // Computed once at init
    int32 MacroSamplesX = 0;
    int32 MacroSamplesY = 0;

    // World-space origin (bottom-left of the macro tile)
    FVector WorldOrigin = FVector::ZeroVector;

    // Helpers
    void ComputeWorldOrigin();
};
