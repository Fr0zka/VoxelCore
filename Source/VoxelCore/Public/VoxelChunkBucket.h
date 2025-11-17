// VoxelChunkBucket.h
// Spatial bucketing system for merging multiple chunks into single mesh components
// Reduces component count from 1000s → dozens for massive performance gain
//
// ARCHITECTURE:
// - World divided into BucketSize×BucketSize×BucketSize chunk grids
// - Each bucket = 1 RealtimeMeshComponent containing many chunks
// - Only rebuild affected bucket when chunk changes (not entire world)
// - Frustum culling works per-bucket (not per-chunk)
//
// EXAMPLE:
// BucketSize = 8, World has 1850 chunks:
// - Without buckets: 1850 RMC components
// - With buckets: ~30 RMC components (94% reduction!)

#pragma once

#include "CoreMinimal.h"
#include "VoxelStructs.h"

class URealtimeMeshComponent;
struct FMeshBuffers;

/**
 * Represents a spatial bucket containing multiple voxel chunks merged into one mesh.
 * Each bucket owns a single RealtimeMeshComponent and rebuilds only when chunks within change.
 */
struct VOXELCORE_API FVoxelChunkBucket
{
	/** Bucket coordinate in bucket-space (not chunk-space or world-space) */
	FIntVector BucketCoord;

	/** The single mesh component for all chunks in this bucket */
	URealtimeMeshComponent* MeshComponent = nullptr;

	/** List of chunk coordinates contained in this bucket */
	TSet<FVoxelCoord> ContainedChunks;

	/** Dirty flag - rebuild mesh when true */
	bool bNeedsRebuild = false;

	/** Last rebuild frame (for throttling) */
	int32 LastRebuildFrame = -1;

	/** World-space bounds of this bucket (for culling) */
	FBox WorldBounds = FBox(EForceInit::ForceInit);

	FVoxelChunkBucket()
		: BucketCoord(FIntVector::ZeroValue)
	{
	}

	FVoxelChunkBucket(const FIntVector& InBucketCoord)
		: BucketCoord(InBucketCoord)
	{
	}

	/** Mark this bucket as needing a mesh rebuild */
	void MarkDirty()
	{
		bNeedsRebuild = true;
	}

	/** Check if bucket is empty (no chunks) */
	bool IsEmpty() const
	{
		return ContainedChunks.Num() == 0;
	}
};

/**
 * Manages spatial buckets for chunk batching.
 * Converts chunk coordinates to bucket coordinates and handles bucket lifecycle.
 */
class VOXELCORE_API FVoxelBucketManager
{
public:
	/** Size of each bucket in chunks (e.g., 8 = 8×8×8 chunks per bucket) */
	int32 BucketSize = 8;

	/** Convert chunk coordinate to bucket coordinate */
	FIntVector ChunkToBucket(const FVoxelCoord& ChunkCoord) const
	{
		return FIntVector(
			FMath::FloorToInt(static_cast<float>(ChunkCoord.Cx) / BucketSize),
			FMath::FloorToInt(static_cast<float>(ChunkCoord.Cy) / BucketSize),
			FMath::FloorToInt(static_cast<float>(ChunkCoord.Cz) / BucketSize)
		);
	}

	/** Get world-space bounds for a bucket */
	FBox GetBucketWorldBounds(const FIntVector& BucketCoord, float ChunkWorldSizeX, float ChunkWorldSizeY, float ChunkWorldSizeZ) const
	{
		const FVector MinCorner(
			BucketCoord.X * BucketSize * ChunkWorldSizeX,
			BucketCoord.Y * BucketSize * ChunkWorldSizeY,
			BucketCoord.Z * BucketSize * ChunkWorldSizeZ
		);

		const FVector MaxCorner = MinCorner + FVector(
			BucketSize * ChunkWorldSizeX,
			BucketSize * ChunkWorldSizeY,
			BucketSize * ChunkWorldSizeZ
		);

		return FBox(MinCorner, MaxCorner);
	}

	/** Calculate optimal bucket size based on world parameters */
	static int32 CalculateOptimalBucketSize(int32 EstimatedTotalChunks)
	{
		// Target: 20-50 buckets for good balance between granularity and performance
		// Fewer buckets = better performance, but larger rebuild cost per update

		if (EstimatedTotalChunks < 500)
		{
			return 4;  // Small worlds: more granular
		}
		else if (EstimatedTotalChunks < 2000)
		{
			return 8;  // Medium worlds: balanced (recommended)
		}
		else
		{
			return 12; // Large worlds: fewer buckets
		}
	}
};
