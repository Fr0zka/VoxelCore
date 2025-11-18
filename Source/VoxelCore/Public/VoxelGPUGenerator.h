// VoxelGPUGenerator.h
// GPU-based voxel terrain generation using compute shaders
// Provides massive performance gains by offloading generation to GPU

#pragma once

#include "CoreMinimal.h"
#include "VoxelStructs.h"
#include "VoxelBiome.h"
#include "RHIGPUReadback.h"

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
 * 2. Poll for readback completion each frame (non-blocking)
 * 3. Callback when data ready - proceed to meshing
 */
class VOXELCORE_API FVoxelGPUGenerator
{
public:
    /**
     * Generate voxel chunk categories AND block IDs using GPU compute shader.
     * OPTION 3: Now generates both category and block ID data in parallel on GPU
     *
     * @param Coord - Chunk coordinate
     * @param SizeX/Y/Z - Chunk dimensions (LOD-scaled + halo)
     * @param BaseSizeX/Y/Z - Base chunk size (unscaled, for world coordinate calculation)
     * @param LODScaleXY - LOD scale factor (1 for LOD0, 2+ for LOD1)
     * @param Seed - World seed for noise generation
     * @param BaseHeight - Base terrain height
     * @param WaterLevel - Water level cutoff
     * @param MaxCaveDepth - Maximum cave depth (performance optimization)
     * @param BiomeTable - Biome definitions (terrain params + block types)
     * @param NoiseProfile - Climate noise parameters
     * @param OnComplete - Callback when generation complete (receives category + blockID data)
     */
    static void GenerateChunkGPU(
        const FVoxelCoord& Coord,
        int32 SizeX, int32 SizeY, int32 SizeZ,
        int32 BaseSizeX, int32 BaseSizeY, int32 BaseSizeZ,
        int32 LODScaleXY,
        int32 Seed,
        int32 BaseHeight,
        int32 WaterLevel,
        int32 MaxCaveDepth,
        const class UVoxelBiomeTable* BiomeTable,
        const class UVoxelNoiseProfile* NoiseProfile,
        TFunction<void(TArray<uint8>&&, TArray<EVoxelBlockID>&&)> OnComplete);

    /**
     * Check if GPU generation is available on this platform.
     * Falls back to CPU if compute shaders not supported.
     */
    static bool IsGPUGenerationAvailable();

    /**
     * Tick GPU generation jobs (poll for readback completion).
     * Call this every frame from VoxelWorld to process pending GPU jobs.
     */
    static void TickGPUGenerationJobs();

    /**
     * Get number of pending GPU generation jobs (for profiling/stats).
     * Jobs are waiting for GPU readback to complete.
     */
    static int32 GetPendingJobCount();

private:
    struct FGPUBiomeData
    {
        // Climate ranges for biome selection
        float TempMin;
        float TempMax;
        float MoistMin;
        float MoistMax;

        // Height parameters
        float HeightAmplitude;
        float HeightFrequency;
        int32 HeightOctaves;
        float HeightLacunarity;
        float HeightGain;

        // Mountain parameters
        float MountainAmplitude;
        float MountainFrequency;
        float MountainThreshold;
        float MountainSharpness;

        // 3D features
        float OverhangAmplitude;
        float OverhangFrequency;
        float WarpAmplitude;
        float WarpFrequency;
        float IslandAmplitude;
        float IslandFrequency;
        float IslandThreshold;
        float IslandBandCenterZ;
        float IslandBandHalfThickness;

        // Cave parameters
        float CaveDensity;
        float CaveFrequency2D;
        int32 CaveOctaves2D;
        float CaveLacunarity2D;
        float CaveGain2D;
        float CaveFrequency3D;
        int32 CaveOctaves3D;

        // OPTION 3: Block ID Generation
        // Surface block (grass, sand, snow, etc.) - MUST match shader layout
        uint32 SurfaceBlock;

        // Subsurface layers (up to 8 layers: dirt, stone, etc.)
        // Each layer: lower 16 bits = BlockID, upper 16 bits = Thickness
        uint32 SubsurfaceLayers[8];
        int32 SubsurfaceLayerCount;

        // Padding to match shader alignment (if needed)
        uint32 _Padding[1];
    };
    /**
     * GPU generation job - tracks pending async readback.
     * OPTION 3: Now tracks BOTH category and blockID readbacks
     */
    struct FGPUGenerationJob
    {
        TUniquePtr<FRHIGPUBufferReadback> CategoryReadback;
        TUniquePtr<FRHIGPUBufferReadback> BlockIDReadback;  // OPTION 3: Added
        int32 CategoryBufferSizeBytes = 0;
        int32 BlockIDBufferSizeBytes = 0;  // OPTION 3: Added
        TFunction<void(TArray<uint8>&&, TArray<EVoxelBlockID>&&)> OnComplete;  // OPTION 3: Updated signature
    };

    // Pending GPU generation jobs (polled each frame)
    static TArray<TSharedPtr<FGPUGenerationJob, ESPMode::ThreadSafe>> PendingJobs;
    static FCriticalSection JobsMutex;

    /**
     * Internal implementation - dispatches compute shader on render thread.
     */
    static void DispatchGenerationShader_RenderThread(
        const FVoxelCoord& Coord,
        int32 SizeX, int32 SizeY, int32 SizeZ,
        int32 BaseSizeX, int32 BaseSizeY, int32 BaseSizeZ,
        int32 LODScaleXY,
        int32 Seed,
        int32 BaseHeight,
        int32 WaterLevel,
        int32 MaxCaveDepth,
        float TempBaseFreq, int32 TempOctaves, float TempLacunarity, float TempGain, float TempWarpStrength, int32 TempSeedOffset,
        float MoistBaseFreq, int32 MoistOctaves, float MoistLacunarity, float MoistGain, float MoistWarpStrength, int32 MoistSeedOffset,
        const TArray<FGPUBiomeData>& BiomeDataArray,
        TFunction<void(TArray<uint8>&&, TArray<EVoxelBlockID>&&)> OnComplete);  // OPTION 3: Updated signature
};

