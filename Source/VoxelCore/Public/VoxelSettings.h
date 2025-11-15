#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "VoxelSettings.generated.h"

UCLASS(Blueprintable, BlueprintType, Config = Game)
class VOXELCORE_API UVoxelSettings : public UObject
{
    GENERATED_BODY()

public:
    // === Scale & base chunk shape ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Scale")
    float VoxelWorldScale = 100.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Chunk")
    int32 ChunkSizeX = 16;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Chunk")
    int32 ChunkSizeY = 16;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Chunk")
    int32 ChunkSizeZ = 64;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming")
    int32 ViewDistanceChunks = 6;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "0"))
    int32 ViewDistanceChunksZ = 6;

    // === LOD rings ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD", meta = (ClampMin = "1"))
    int32 LOD0_Radius = 3;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD", meta = (ClampMin = "0"))
    int32 LOD1_Radius = 8;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD")
    int32 LOD2_Radius = 12;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD", meta = (ClampMin = "1"))
    int32 LOD1_ScaleXY = 2;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD", meta = (ClampMin = "2", ClampMax = "128"))
    int32 LOD2_Tessellation = 16;

    // === Streaming policy ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming")
    bool bDiskShapedLoading = true;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "0"))
    int32 CollisionViewDistance = 2;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "0"))
    int32 AORadiusChunks = 3;

    // === Generation parameters ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Generation")
    int32 Seed = 1337;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Generation", meta = (ClampMin = "1.0"))
    float NoiseScale = 32.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Generation")
    float NoiseAmplitude = 10.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Generation")
    int32 BaseHeight = 20;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Generation")
    int32 WaterLevel = 18;

    // === Performance: Generation Optimizations ===
    // Maximum depth (in voxels) from macro surface to generate caves.
    // Caves deeper than this are skipped (major performance gain).
    // Recommended: 128-256 for good performance, 512+ for deep cave systems.
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "0", ClampMax = "1024"))
    int32 MaxCaveDepth = 200;

    // Use GPU compute shaders for terrain generation (10-50x faster than CPU).
    // Requires compute shader support. Falls back to CPU if unavailable.
    // EXPERIMENTAL: Enable for massive performance gains, disable if issues occur.
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance")
    bool bUseGPUGeneration = false;

    // === Rendering backend ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Rendering")
    bool bUseRuntimeMeshComponent = false;

    // === Performance: concurrency caps ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxConcurrentGenerationTasks = 4;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxConcurrentMeshingTasks = 2;

    // === Performance: spawn budget ===
    // Maximum number of new chunks we are allowed to spawn per frame.
    // Lower values = smoother FPS during load, higher values = faster chunk spawn.
    // Recommended: 4-6 for smooth 60fps, 8-12 for faster loading with some hitches.
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "256"))
    int32 MaxChunksSpawnPerFrame = 4;

    // === Performance: mesh component pool ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance")
    bool bEnableMeshComponentPooling = true;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "0", ClampMax = "1024"))
    int32 MeshComponentPoolPrewarm = 0;

    // === LOD2 macro-tiles ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD2 MacroTiles")
    bool bUseLOD2MacroTiles = true;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD2 MacroTiles", meta = (ClampMin = "2", ClampMax = "64"))
    int32 LOD2_MacroTileSize = 8;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD2 MacroTiles", meta = (ClampMin = "1", ClampMax = "64"))
    int32 LOD2_SampleXY = 2;

    // ===================================================================
    // GPU MESHER OPTIMIZATION OPTIONS (NEW)
    // ===================================================================

    /**
     * Maximum GPU mesh readbacks to process per frame.
     *
     * Controls frame-time budget for GPU meshing results. Lower values = smoother FPS,
     * higher values = faster chunk loading but potential frame spikes.
     *
     * Recommended values:
     * - 10-20: Smooth 144 FPS with minimal hitches
     * - 30-50: Balanced performance
     * - 100+: Maximum throughput, may cause frame drops
     *
     * Set to 0 for unlimited (process all ready readbacks).
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|GPU Meshing",
        meta = (ClampMin = "0", ClampMax = "500"))
    int32 MaxGPUMeshReadbacksPerFrame = 20;

    /**
     * Enable GPU mesher for LOD0 (experimental, optional).
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|GPU Meshing")
    bool bUseGPUMesherForLOD0 = false;

    /**
     * GPU mesher tile size. Larger = more shared memory usage but better occupancy.
     * FIXED: Currently hardcoded to 8 in the compute shader and cannot be changed.
     * This setting is kept for future parameterization but currently has no effect.
     * WARNING: Changing this value will log a warning but will not affect GPU meshing.
     */
    UPROPERTY(VisibleAnywhere, Config, BlueprintReadOnly, Category = "Voxel|GPU Meshing",
        meta = (EditCondition = "false"))
    int32 GPUMesherTileSize = 8;

    /**
     * [DEPRECATED - DO NOT ENABLE] Use aggressive GPU culling (ignores neighbor borders).
     * WARNING: Enabling this creates visible WALLS between chunks!
     * This setting should remain FALSE for proper seamless meshing.
     * When false: GPU properly culls faces between chunks using neighbor border data.
     * When true: GPU treats chunk boundaries as solid walls (broken behavior).
     * This setting exists only for debugging and should be removed in future versions.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|GPU Meshing|Advanced",
        meta = (EditCondition = "bUseGPUMesherForLOD0", DisplayName = "[BROKEN] Aggressive Culling (Creates Walls)"))
    bool bGPUAggressiveCulling = false;

    /**
     * Enable asynchronous GPU mesh readback (reduces CPU stalls).
     * WARNING: Slightly increases latency but massively improves throughput.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|GPU Meshing",
        meta = (EditCondition = "bUseGPUMesherForLOD0"))
    bool bAsyncGPUReadback = true;

    /**
     * Fallback to CPU mesher if GPU fails (useful for debugging).
     * Disable this in shipping builds for better error detection.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|GPU Meshing",
        meta = (EditCondition = "bUseGPUMesherForLOD0"))
    bool bFallbackToCPUOnGPUFailure = true;

    /**
     * EXPERIMENTAL: Use binary greedy mesher on CPU (faster but less stable).
     * Only used when GPU mesher is disabled or fails.
     * DEFAULT: false (use stable greedy mesher).
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Rendering")
    // Default ON: project binary greedy mesher is faster here than the normal mesher.
    bool bUseBinaryGreedyMesher = true;

    /**
     * Use naive face culler (no greedy meshing optimization).
     * This generates one quad per visible face - useful for debugging mesh generation issues.
     * WARNING: Produces MUCH higher vertex/triangle counts. Only use for debugging.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Rendering")
    bool bUseNaiveMesher = false;

    // ===================================================================
    // PROFILING & DIAGNOSTICS (NEW)
    // ===================================================================

    /**
     * Log detailed GPU meshing performance stats to console.
     * Shows: GPU time, readback time, vertex count, etc.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Profiling")
    bool bLogGPUMeshingStats = false;

    /**
     * Log warnings when GPU mesh generation takes longer than this (ms).
     * Set to 0 to disable. Useful for detecting performance bottlenecks.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Profiling",
        meta = (ClampMin = "0.0", ClampMax = "100.0"))
    float GPUMeshWarningThresholdMS = 5.0f;

    /**
     * Dump first N GPU mesh outputs to log for debugging.
     * Shows packed vertex data. Set to 0 to disable.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Profiling",
        meta = (ClampMin = "0", ClampMax = "10"))
    int32 DebugDumpFirstNMeshes = 0;

    // ===================================================================
    // MEMORY OPTIMIZATION (NEW)
    // ===================================================================

    /**
     * Maximum GPU vertex buffer size (vertices). Prevents OOM on huge chunks.
     * GPU allocates this much per chunk. Reduce if you have GPU memory issues.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Memory")
    int32 MaxGPUVertexBufferSize = 1048576; // 1M verts = ~4MB

    /**
     * Aggressively free mesh buffers after upload to GPU.
     * Reduces RAM but prevents collision-only reapply optimization.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Memory")
    bool bAggressivelyFreeMeshBuffers = false;

    /**
     * Pool size for GPU readback resources (reduces allocation overhead).
     * Higher = more memory but less stutter. 0 = no pooling.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Memory",
        meta = (ClampMin = "0", ClampMax = "64"))
    int32 GPUReadbackPoolSize = 8;

    // near other UPROPERTY
    UPROPERTY(EditAnywhere, Config, Category = "Voxel|Materials")
    TSoftObjectPtr<class UVoxelMaterialSet> MaterialSet;

    UPROPERTY(EditAnywhere, Config, Category = "Voxel|Materials")
    TSoftObjectPtr<class UVoxelBlockTable> BlockTable;

    UPROPERTY(EditAnywhere, Config, Category = "Voxel|Generation")
    TSoftObjectPtr<class UVoxelBiomeTable> BiomeTable;
    UPROPERTY(EditAnywhere, Config, Category = "Voxel|Materials")
    TSoftObjectPtr<class UMaterialInterface> VoxelArrayMaterial;

    UPROPERTY(EditAnywhere, Config, Category = "Voxel|Generation")
    TSoftObjectPtr<class UVoxelNoiseProfile> NoiseProfile;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Debug")
    bool bVisualizeNoiseFields = false;

    /** Which field to debug: 0=Temp, 1=Moist, 2=Height, 3=Biome, 4=TempMoist2D */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Debug")
    int32 NoiseDebugMode = 0;

    /** If true, remeshes all chunks every tick for live preview (dev only) */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Debug")
    bool bLiveNoisePreview = false;

    // Validation functions
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
    virtual void PostLoad() override;
    void ValidateSettings();
};
