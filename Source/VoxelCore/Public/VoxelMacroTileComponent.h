#pragma once

#include "CoreMinimal.h"
#include "VoxelChunkComponent.h"
#include "VoxelMacroTileComponent.generated.h"

/**
 * UVoxelMacroTileComponent: LOD2 heightfield impostor for distant terrain.
 *
 * **Purpose:**
 * Macro-tiles merge an MxM grid of LOD2 chunks into a single heightfield mesh,
 * dramatically reducing draw calls and mesh complexity for distant terrain.
 *
 * **Key Features:**
 * - Single heightfield mesh covering MacroSize × MacroSize chunks
 * - Downsampled terrain (SampleXY voxel spacing, typically 2)
 * - No collision (visual-only representation)
 * - Seamless edge matching with LOD1 chunks
 * - Async generation and meshing via world scheduler
 *
 * **Coordinate System:**
 * - Tile coordinates stored in ChunkCoord.Cx/Cy for reuse of world helpers
 * - World origin computed from tile coords (bottom-left corner)
 * - Mesh vertices in world space (component placed at WorldOrigin)
 *
 * **Generation Pipeline:**
 * 1. InitializeMacroTile: Setup size, sampling, mesh component
 * 2. DoGeneration: Sample heightmap across entire macro-tile area
 * 3. DoMeshing: Build single heightfield mesh from sampled heights
 */
UCLASS()
class VOXELCORE_API UVoxelMacroTileComponent : public UVoxelChunkComponent
{
	GENERATED_BODY()

public:
	/**
	 * Initialize macro-tile at given tile coordinates.
	 * Sets up sampling grid, mesh component, and schedules generation.
	 *
	 * @param InTileCoord Tile index in macro-tile space (NOT chunk coordinates)
	 * @param InSettings Voxel world settings
	 * @param InWorld Owner world for scheduling
	 */
	void InitializeMacroTile(
		FIntPoint InTileCoord,
		const UVoxelSettings* InSettings,
		AVoxelWorld* InWorld);

	/**
	 * Generate heightmap for entire macro-tile area.
	 * Samples terrain height at SampleXY spacing across MacroSize chunks.
	 * Runs async on task graph, then schedules meshing.
	 */
	virtual void DoGeneration() override;

	/**
	 * Build heightfield mesh from sampled heights.
	 * Creates single quad mesh covering entire macro-tile area.
	 * Runs async on task graph.
	 *
	 * @param bSeamRemesh Unused for macro-tiles (no seam remeshing needed)
	 */
	virtual void DoMeshing(bool bSeamRemesh) override;

private:
	// Dimensions in chunks per side (default: 8×8 chunks per macro-tile)
	int32 MacroSize = 8;

	// Sample step in voxels (default: 2, matches LOD1 downsampling)
	int32 SampleXY = 2;

	// Computed heightmap grid dimensions
	int32 MacroSamplesX = 0;
	int32 MacroSamplesY = 0;

	// World-space origin (bottom-left corner of macro-tile)
	FVector WorldOrigin = FVector::ZeroVector;

	// Helper to compute world origin from tile coordinates
	void ComputeWorldOrigin();
};
