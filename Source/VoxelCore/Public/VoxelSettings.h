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

    // DEPRECATED: Use ViewDistanceChunksUp/Down for asymmetric vertical loading (better performance)
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "0"))
    int32 ViewDistanceChunksZ = 6;

    // === Asymmetric Vertical Radii (Performance Optimization) ===
    // Separate control for chunks ABOVE vs BELOW player.
    // Recommended: High value UP (air chunks are cheap), low value DOWN (solid chunks expensive)
    // Example: Up=10 (sky/clouds), Down=5 (underground caves/stone)
    // Set to 0 to use symmetric ViewDistanceChunksZ instead.
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming|Asymmetric Vertical",
        meta = (ClampMin = "0", ClampMax = "32"))
    int32 ViewDistanceChunksUp = 0;  // 0 = use ViewDistanceChunksZ

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming|Asymmetric Vertical",
        meta = (ClampMin = "0", ClampMax = "32"))
    int32 ViewDistanceChunksDown = 0;  // 0 = use ViewDistanceChunksZ

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

    // Only enable collision on solid/ground chunks (massive performance boost).
    // Air chunks and sky chunks never need collision.
    // Underground stone chunks need collision, air caves do too.
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming|Performance")
    bool bCollisionOnlyForSolidChunks = true;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "0"))
    int32 AORadiusChunks = 3;

    // === Frustum-Based Generation (Massive Performance Boost) ===
    // Only generate chunks within player's view cone instead of full 360° sphere.
    // PERFORMANCE IMPACT: Reduces chunk count by 50-70% (e.g., 6,270 → ~2,000 chunks)
    // Recommended: Enable for large view distances (ViewDistanceChunks > 16)
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming|Frustum Culling")
    bool bUseFrustumBasedGeneration = false;

    // Horizontal field of view for chunk generation (in degrees).
    // Larger = more chunks ahead of player, smaller = narrower cone.
    // Recommended: 120° (player FOV ~90° + 30° margin for turning)
    // 180° = half-sphere (behind player culled), 360° = full sphere (frustum disabled)
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming|Frustum Culling",
        meta = (ClampMin = "30.0", ClampMax = "360.0", EditCondition = "bUseFrustumBasedGeneration"))
    float FrustumHorizontalFOV = 120.0f;

    // Also apply frustum culling vertically (cull chunks above/below camera view).
    // Recommended: false (vertical culling less useful, player often looks up/down)
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming|Frustum Culling",
        meta = (EditCondition = "bUseFrustumBasedGeneration"))
    bool bFrustumCullVertical = false;

    // Vertical field of view for chunk generation (in degrees). Only used if bFrustumCullVertical=true.
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming|Frustum Culling",
        meta = (ClampMin = "30.0", ClampMax = "180.0", EditCondition = "bUseFrustumBasedGeneration && bFrustumCullVertical"))
    float FrustumVerticalFOV = 100.0f;

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

    // Use GPU compute shaders for mesh generation (experimental).
    // Requires compute shader support. Disabled by default for stability.
    // Enable only if CPU meshing is a bottleneck.
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|GPU Meshing")
    bool bUseGPUMesherForLOD0 = false;

    // Use true greedy meshing algorithm on GPU (reduces triangles by 60-90%).
    // Only applies when bUseGPUMesherForLOD0 is enabled.
    // True = proper greedy quad merging (slower GPU, fewer triangles, better rendering).
    // False = naive face culling (faster GPU, more triangles, worse rendering).
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|GPU Meshing",
        meta = (EditCondition = "bUseGPUMesherForLOD0", EditConditionHides))
    bool bUseGPUGreedyMeshing = true;

    // === Rendering backend ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Rendering")
    bool bUseRuntimeMeshComponent = false;

    // === Performance: concurrency caps ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxConcurrentGenerationTasks = 4;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxConcurrentMeshingTasks = 2;

    // Maximum GPU generation jobs in flight (waiting for readback).
    // Higher values = more GPU utilization but higher memory and potential saturation.
    // Lower values = more conservative, prevents GPU queue buildup.
    // Recommended: 8-16 for balanced performance.
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxGPUGenerationJobsInFlight = 16;

    // === Performance: Frame Budgets ===
    // Maximum number of NEW chunk components to spawn per frame (CPU/memory budget).
    // Controls how many UVoxelChunkComponent objects are created per frame.
    // Lower values = smoother FPS during loading, higher values = faster chunk spawn with hitches.
    // Recommended: 4-6 for smooth 60fps, 8-12 for faster loading, 16+ for aggressive loading.
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance|Frame Budgets",
        meta = (ClampMin = "1", ClampMax = "256"))
    int32 MaxChunksSpawnPerFrame = 4;

    // === Performance: mesh component pool ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance")
    bool bEnableMeshComponentPooling = true;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "0", ClampMax = "1024"))
    int32 MeshComponentPoolPrewarm = 0;

    // === Performance: Chunk Batching (Mesh Merging) ===
    /**
     * Enable chunk mesh batching (merges multiple chunks into single mesh components).
     * MASSIVE performance boost: reduces 1000s of components → dozens.
     * Benefits:
     * - Reduces draw calls by 90-95% (e.g., 1850 → 30 components)
     * - Lower transform/bounds overhead
     * - Better frustum culling (per-bucket instead of per-chunk)
     * Recommended: true for large worlds (>500 chunks).
     * Trade-off: Slightly higher rebuild cost when chunks change (throttled to 2 per frame).
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance")
    bool bEnableChunkBatching = false;

    /**
     * Number of chunks per bucket dimension (e.g., 8 = 8×8×8 = 512 chunks per bucket).
     * Smaller = more granular updates but more buckets.
     * Larger = fewer buckets but larger rebuild cost per update.
     * Recommended values:
     * - 4: Small worlds (<500 chunks) - more granular updates
     * - 8: Medium worlds (500-2000 chunks) - balanced (default)
     * - 12: Large worlds (>2000 chunks) - fewer buckets
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance",
        meta = (ClampMin = "2", ClampMax = "16", EditCondition = "bEnableChunkBatching"))
    int32 ChunkBucketSize = 8;

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
     * Maximum GPU mesh readbacks to process per frame (GPU→CPU transfer budget).
     * This is SEPARATE from MaxChunksSpawnPerFrame (which controls component spawning).
     *
     * Controls frame-time budget for processing completed GPU meshing results.
     * Lower values = smoother FPS, higher values = faster GPU mesh completion.
     *
     * Recommended values:
     * - 0: Unlimited (best for editor, prevents queue backup and deadlock)
     * - 10-20: Smooth 144 FPS in shipped games with minimal hitches
     * - 30-50: Balanced performance for 60 FPS
     * - 100+: Maximum throughput, may cause frame drops
     *
     * IMPORTANT: Use 0 (unlimited) in editor to prevent GPU queue backup.
     * In shipping builds, consider 10-20 for smooth frame times.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance|Frame Budgets",
        meta = (ClampMin = "0", ClampMax = "500"))
    int32 MaxGPUMeshReadbacksPerFrame = 0;

    /**
     * Maximum bucket mesh rebuilds per frame (when chunk batching enabled).
     * Bucket rebuilds merge multiple chunk meshes into single batched mesh.
     * Lower values = smoother FPS, higher values = faster visibility after chunk changes.
     *
     * Recommended values:
     * - 2-4: Smooth 120+ FPS (mid-range hardware: 3700X + RTX 3070)
     * - 8-12: Balanced 60 FPS (high-end hardware)
     * - 16+: Maximum throughput (may cause hitches on lower-end hardware)
     *
     * Only applies when bEnableChunkBatching = true.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance|Frame Budgets",
        meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxBucketRebuildsPerFrame = 4;

    /**
     * Minimum frames between bucket rebuilds (throttle to prevent rebuild spam).
     * Higher values = less CPU overhead but slower updates when chunks change.
     * Lower values = faster updates but more CPU work.
     *
     * Recommended: 2-3 frames for responsive updates without overhead.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance|Frame Budgets",
        meta = (ClampMin = "0", ClampMax = "60"))
    int32 BucketRebuildThrottleFrames = 3;

    /**
     * Maximum chunks to check per frame during update loop.
     * Prevents FPS drops when iterating 10,000+ chunks (40uu voxels at large view distances).
     * Chunks are sorted by priority, so closest chunks always checked first.
     *
     * Lower values = smoother FPS but slower to process all chunks.
     * Higher values = faster processing but potential frame drops.
     *
     * Recommended values:
     * - 256-512: Smooth 120 FPS (mid-range hardware)
     * - 1024-2048: Balanced 60 FPS
     * - 4096+: Maximum throughput (high-end hardware only)
     *
     * NOTE: Already-correct chunks are skipped "for free" without counting toward limit.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance|Frame Budgets",
        meta = (ClampMin = "64", ClampMax = "8192"))
    int32 MaxChunksToCheckPerFrame = 512;

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

    // === Debug & Logging ===
    /**
     * Enable ALL debug logging throughout the entire voxel codebase.
     * When enabled, shows detailed logs for:
     * - Chunk generation and meshing
     * - Bucket creation and rebuilding
     * - GPU operations
     * - Streaming and LOD transitions
     * - Performance warnings and stats
     *
     * WARNING: Enabling this will generate LOTS of console output and may impact performance.
     * Recommended: Enable only when debugging specific issues.
     */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Debug")
    bool bEnableDebugLogging = false;

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
