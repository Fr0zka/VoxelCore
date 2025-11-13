// VoxelGPUGenerator.h
// GPU-based voxel terrain generation using compute shaders
// Provides massive performance gains by offloading generation to GPU

#pragma once

#include "CoreMinimal.h"
#include "VoxelStructs.h"
#include "VoxelBiome.h"

/**
 * GPU Generator - Executes voxel terrain generation on GPU using compute shaders.
 *
 * This provides 10-50x performance improvement over CPU generation by leveraging
 * GPU's massive parallelism (thousands of threads vs dozens).
 *
 * Features:
 * - Fully parallel voxel generation (each voxel computed independently)
 * - Async GPU readback (non-blocking on game thread)
 * - Identical results to CPU path (same noise algorithms)
 * - Graceful fallback to CPU if GPU unavailable
 *
 * Workflow:
 * 1. Dispatch compute shader to generate voxel categories on GPU
 * 2. Async readback results to CPU (continues in background)
 * 3. Callback when data ready - proceed to meshing
 */
class VOXELCORE_API FVoxelGPUGenerator
{
public:
    /**
     * Generate voxel chunk categories using GPU compute shader.
     *
     * @param Coord - Chunk coordinate
     * @param SizeX/Y/Z - Chunk dimensions
     * @param LODScaleXY - LOD scale factor (1 for LOD0, 2+ for LOD1)
     * @param Seed - World seed for noise generation
     * @param BaseHeight - Base terrain height
     * @param WaterLevel - Water level cutoff
     * @param MaxCaveDepth - Maximum cave depth (performance optimization)
     * @param BiomeParams - Biome-specific terrain parameters
     * @param OnComplete - Callback when generation complete (receives category data)
     */
    static void GenerateChunkGPU(
        const FVoxelCoord& Coord,
        int32 SizeX, int32 SizeY, int32 SizeZ,
        int32 LODScaleXY,
        int32 Seed,
        int32 BaseHeight,
        int32 WaterLevel,
        int32 MaxCaveDepth,
        const FBiomeTerrainParams& BiomeParams,
        TFunction<void(TArray<uint8>&&)> OnComplete);

    /**
     * Check if GPU generation is available on this platform.
     * Falls back to CPU if compute shaders not supported.
     */
    static bool IsGPUGenerationAvailable();

private:
    /**
     * Internal implementation - dispatches compute shader on render thread.
     */
    static void DispatchGenerationShader_RenderThread(
        const FVoxelCoord& Coord,
        int32 SizeX, int32 SizeY, int32 SizeZ,
        int32 LODScaleXY,
        int32 Seed,
        int32 BaseHeight,
        int32 WaterLevel,
        int32 MaxCaveDepth,
        const FBiomeTerrainParams& BiomeParams,
        TFunction<void(TArray<uint8>&&)> OnComplete);
};
