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
    float VoxelWorldScale = 100.0f; // UU per LOD0 voxel

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Chunk")
    int32 ChunkSizeX = 16;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Chunk")
    int32 ChunkSizeY = 16;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Chunk")
    int32 ChunkSizeZ = 64;

    // Legacy fallback if LOD2_Radius <= 0
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming")
    int32 ViewDistanceChunks = 6;

    // === LOD rings ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD", meta = (ClampMin = "1"))
    int32 LOD0_Radius = 3;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD", meta = (ClampMin = "1"))
    int32 LOD1_Radius = 8;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD")
    int32 LOD2_Radius = 12;

    // Coarsening factor for LOD1 in XY (Z stays 1×)
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD", meta = (ClampMin = "1"))
    int32 LOD1_ScaleXY = 2;

    // Heightfield tessellation for LOD2
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD", meta = (ClampMin = "2", ClampMax = "128"))
    int32 LOD2_Tessellation = 16;

    // === Streaming policy ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxMeshAppliesPerTick = 2;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming")
    bool bDiskShapedLoading = true;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "0"))
    int32 CollisionViewDistance = 2;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "0"))
    int32 AORadiusChunks = 3;

    // === Directional streaming (currently unused, kept for future) ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Directional")
    bool bDirectionalStreaming = false;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Directional", meta = (ClampMin = "1.0", ClampMax = "179.0"))
    float ForwardHalfAngleDeg = 80.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Directional", meta = (ClampMin = "0.0", ClampMax = "90.0"))
    float RearHalfAngleDeg = 25.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Directional")
    bool bUseControllerForward = true;

    // === Generation parameters (example) ===
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

    // === Rendering backend ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Rendering")
    bool bUseRuntimeMeshComponent = false;

    // === Performance: concurrency caps ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxConcurrentGenerationTasks = 4;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxConcurrentMeshingTasks = 2;

    // === Performance: mesh component pool ===
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance")
    bool bEnableMeshComponentPooling = true;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Performance", meta = (ClampMin = "0", ClampMax = "1024"))
    int32 MeshComponentPoolPrewarm = 0;

    // === LOD2 macro-tiles (NEW) ===
    // Merge far-ring LOD2 into one heightfield mesh per macro-tile
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD2 MacroTiles")
    bool bUseLOD2MacroTiles = true;

    // Size in *chunks* per side (e.g., 8 = 8×8 chunks merged)
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD2 MacroTiles", meta = (ClampMin = "2", ClampMax = "64"))
    int32 LOD2_MacroTileSize = 8;

    // XY downsample step in voxels for LOD2 heightfield sampling; match LOD1 scale by default
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|LOD2 MacroTiles", meta = (ClampMin = "1", ClampMax = "64"))
    int32 LOD2_SampleXY = 2;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Voxel|Rendering")
    bool bUseGPUMesherForLOD1 = true;

    // Conservative caps so spikes recover fast
    UPROPERTY(EditAnywhere, Category = "Voxel|Performance")
    int32 MaxGenHardCap = 8;

    UPROPERTY(EditAnywhere, Category = "Voxel|Performance")
    int32 MaxMeshHardCap = 8;

    // Target frame time window and allocation
    UPROPERTY(EditAnywhere, Category = "Voxel|Performance")
    float TargetFrameMs = 16.0f;

    UPROPERTY(EditAnywhere, Category = "Voxel|Performance")
    float BackoffFrameMs = 18.0f;

    UPROPERTY(EditAnywhere, Category = "Voxel|Performance")
    float BudgetForGen = 0.25f;   // 25% of frame for gen
    UPROPERTY(EditAnywhere, Category = "Voxel|Performance")
    float BudgetForMesh = 0.25f;  // 25% of frame for mesh

    UPROPERTY(EditAnywhere, Category = "Voxel|Streaming")
    float FreezeSpeedThreshold = 1200.0f; // UU/s

    UPROPERTY(EditAnywhere, Category = "Voxel|Streaming")
    int32 FreezeOuterLodRings = 1; // freeze last N rings
};
