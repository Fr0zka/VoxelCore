#include "VoxelSettings.h"

// ============================================================================
// SETTINGS VALIDATION
// ============================================================================
// Validates voxel world configuration parameters on load and property change.
// Provides warnings for unusual values and clamps critical parameters.
// ============================================================================

#if WITH_EDITOR
void UVoxelSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Validate and warn about configuration issues
	ValidateSettings();
}
#endif

void UVoxelSettings::PostLoad()
{
	Super::PostLoad();

	// Validate settings on load
	ValidateSettings();
}

void UVoxelSettings::ValidateSettings()
{
	// === CHUNK SIZE VALIDATION ===
	// Chunk sizes should be reasonable powers of 2 or multiples thereof
	if (ChunkSizeX < 4 || ChunkSizeX > 64)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] ChunkSizeX=%d is outside recommended range [4-64]. May cause performance issues."), ChunkSizeX);
	}
	if (ChunkSizeY < 4 || ChunkSizeY > 64)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] ChunkSizeY=%d is outside recommended range [4-64]. May cause performance issues."), ChunkSizeY);
	}
	if (ChunkSizeZ < 16 || ChunkSizeZ > 128)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] ChunkSizeZ=%d is outside recommended range [16-128]. May cause performance issues."), ChunkSizeZ);
	}

	// === VOXEL WORLD SCALE VALIDATION ===
	if (VoxelWorldScale <= 0.0f)
	{
		UE_LOG(LogTemp, Error, TEXT("[VoxelSettings] VoxelWorldScale must be positive! Clamping to 100.0"));
		VoxelWorldScale = 100.0f;
	}
	if (VoxelWorldScale < 10.0f || VoxelWorldScale > 500.0f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] VoxelWorldScale=%f is unusual. Recommended range: [50-200]"), VoxelWorldScale);
	}

	// === VIEW DISTANCE VALIDATION ===
	if (ViewDistanceChunks < 1)
	{
		UE_LOG(LogTemp, Error, TEXT("[VoxelSettings] ViewDistanceChunks must be >= 1. Clamping to 1."));
		ViewDistanceChunks = 1;
	}
	if (ViewDistanceChunks > 32)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] ViewDistanceChunks=%d is very large. May cause memory issues."), ViewDistanceChunks);
	}

	// === LOD CONFIGURATION VALIDATION ===
	// Allow LOD0-only configuration for testing (set LOD1_Radius = LOD2_Radius = 0)
	// Only validate ordering if multiple LOD levels are actually in use
	if (LOD1_Radius > 0 && LOD0_Radius > LOD1_Radius)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] LOD0_Radius (%d) > LOD1_Radius (%d). Typically LOD0 should be inner ring. Swapping values."), LOD0_Radius, LOD1_Radius);
		Swap(LOD0_Radius, LOD1_Radius);
	}
	if (LOD2_Radius > 0 && LOD1_Radius > LOD2_Radius)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] LOD1_Radius (%d) > LOD2_Radius (%d). Typically LOD1 should be middle ring. Swapping values."), LOD1_Radius, LOD2_Radius);
		Swap(LOD1_Radius, LOD2_Radius);
	}

	// === CONCURRENCY VALIDATION ===
	if (MaxConcurrentGenerationTasks < 1)
	{
		UE_LOG(LogTemp, Error, TEXT("[VoxelSettings] MaxConcurrentGenerationTasks must be >= 1. Setting to 1."));
		MaxConcurrentGenerationTasks = 1;
	}
	if (MaxConcurrentMeshingTasks < 1)
	{
		UE_LOG(LogTemp, Error, TEXT("[VoxelSettings] MaxConcurrentMeshingTasks must be >= 1. Setting to 1."));
		MaxConcurrentMeshingTasks = 1;
	}
	if (MaxConcurrentGenerationTasks + MaxConcurrentMeshingTasks > 16)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] Total concurrent tasks (%d) is high. May saturate CPU cores."),
			MaxConcurrentGenerationTasks + MaxConcurrentMeshingTasks);
	}

	// === GPU MESHER VALIDATION ===
	if (bUseGPUMesherForLOD0)
	{
		if (GPUMesherTileSize != 8)
		{
			UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] GPUMesherTileSize is fixed to 8. Your value of %d will be ignored."), GPUMesherTileSize);
		}

		if (bGPUAggressiveCulling)
		{
			UE_LOG(LogTemp, Error, TEXT("[VoxelSettings] bGPUAggressiveCulling=true creates WALLS between chunks! Set to false for proper seamless meshing."));
		}
	}

	// === NOISE SCALE VALIDATION ===
	if (NoiseScale <= 0.0f)
	{
		UE_LOG(LogTemp, Error, TEXT("[VoxelSettings] NoiseScale must be positive! Setting to 32.0"));
		NoiseScale = 32.0f;
	}
	if (NoiseScale < 8.0f || NoiseScale > 256.0f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] NoiseScale=%f may produce unusual terrain. Recommended: [16-128]"), NoiseScale);
	}

	// === MEMORY SETTINGS VALIDATION ===
	if (MaxGPUVertexBufferSize < 1024)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] MaxGPUVertexBufferSize=%d is very small. May cause mesh clipping."), MaxGPUVertexBufferSize);
	}

	// === LOD SCALE VALIDATION ===
	if (LOD1_ScaleXY < 1 || LOD1_ScaleXY > 8)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelSettings] LOD1_ScaleXY=%d is outside recommended range [1-4]."), LOD1_ScaleXY);
	}

	// === DEPRECATED FEATURE WARNING ===
	if (bLiveNoisePreview)
	{
		UE_LOG(LogTemp, Error, TEXT("[VoxelSettings] bLiveNoisePreview is EXTREMELY EXPENSIVE (unloads all chunks every tick). Disable for normal use!"));
	}
}
