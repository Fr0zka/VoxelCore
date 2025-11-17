// VoxelWorld_Buckets.cpp
// Chunk batching implementation - merges multiple chunks into single mesh components
// Reduces 1000s of components to dozens for massive performance gain

#include "VoxelWorld.h"
#include "VoxelChunkComponent.h"
#include "VoxelChunkBucket.h"
#include "VoxelMesher.h"
#include "VoxelSettings.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"
#include "ProceduralMeshComponent.h"
#include "RealtimeMeshCore.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelBuckets, Log, All);

// Helper macro for conditional logging based on bEnableDebugLogging
#define VOXEL_LOG(Category, Verbosity, Format, ...) \
	do { \
		const UVoxelSettings* LogSettings = Settings.GetDefaultObject(); \
		if (LogSettings && LogSettings->bEnableDebugLogging) \
		{ \
			UE_LOG(Category, Verbosity, Format, ##__VA_ARGS__); \
		} \
	} while(0)

// ============================================================================
// BUCKET MANAGEMENT
// ============================================================================

FVoxelChunkBucket* AVoxelWorld::GetOrCreateBucket(const FVoxelCoord& ChunkCoord)
{
	const UVoxelSettings* S = Settings.GetDefaultObject();
	if (!S || !S->bEnableChunkBatching)
	{
		return nullptr; // Batching disabled
	}

	// Update bucket size from settings
	BucketManager.BucketSize = FMath::Clamp(S->ChunkBucketSize, 2, 16);

	// Calculate which bucket this chunk belongs to
	const FIntVector BucketCoord = BucketManager.ChunkToBucket(ChunkCoord);

	// Try to find existing bucket
	FVoxelChunkBucket** ExistingBucket = ChunkBuckets.Find(BucketCoord);
	if (ExistingBucket && *ExistingBucket)
	{
		return *ExistingBucket;
	}

	// Create new bucket
	FVoxelChunkBucket* NewBucket = new FVoxelChunkBucket(BucketCoord);

	// Create RealtimeMeshComponent for this bucket (bForBucket=true means always visible)
	NewBucket->MeshComponent = AcquireRMC(true);

	// CRITICAL: Assign voxel material to bucket (otherwise textures won't show!)
	if (UMaterialInterface* VoxelMat = S->VoxelArrayMaterial.LoadSynchronous())
	{
		NewBucket->MeshComponent->SetMaterial(0, VoxelMat);
	}
	else
	{
		UE_LOG(LogVoxelBuckets, Error, TEXT("[Bucket] CRITICAL: VoxelArrayMaterial is NULL! Bucket will have no texture!"));
	}

	// Calculate world bounds for this bucket
	const float ChunkWorldSizeX = S->ChunkSizeX * S->VoxelWorldScale;
	const float ChunkWorldSizeY = S->ChunkSizeY * S->VoxelWorldScale;
	const float ChunkWorldSizeZ = S->ChunkSizeZ * S->VoxelWorldScale;

	NewBucket->WorldBounds = BucketManager.GetBucketWorldBounds(
		BucketCoord, ChunkWorldSizeX, ChunkWorldSizeY, ChunkWorldSizeZ);

	// Set mesh component position to bucket origin
	NewBucket->MeshComponent->SetRelativeLocation(NewBucket->WorldBounds.Min);

	// CRITICAL: Ensure bucket component is registered and visible
	if (!NewBucket->MeshComponent->IsRegistered())
	{
		UE_LOG(LogVoxelBuckets, Error, TEXT("[Bucket] ERROR: Bucket RMC not registered! This will cause invisible buckets!"));
	}

	ChunkBuckets.Add(BucketCoord, NewBucket);

	// Log bucket creation for monitoring batching system
	VOXEL_LOG(LogVoxelBuckets, Log, TEXT("Created bucket (%d,%d,%d) | Bounds: (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f)"),
		BucketCoord.X, BucketCoord.Y, BucketCoord.Z,
		NewBucket->WorldBounds.Min.X, NewBucket->WorldBounds.Min.Y, NewBucket->WorldBounds.Min.Z,
		NewBucket->WorldBounds.Max.X, NewBucket->WorldBounds.Max.Y, NewBucket->WorldBounds.Max.Z);

	return NewBucket;
}

void AVoxelWorld::MarkBucketDirty(const FVoxelCoord& ChunkCoord)
{
	const UVoxelSettings* S = Settings.GetDefaultObject();
	if (!S || !S->bEnableChunkBatching)
	{
		return;
	}

	FVoxelChunkBucket* Bucket = GetOrCreateBucket(ChunkCoord);
	if (Bucket)
	{
		Bucket->MarkDirty();
		VOXEL_LOG(LogVoxelBuckets, Log, TEXT("[Bucket] Marked bucket (%d,%d,%d) dirty due to chunk (%d,%d,%d)"),
			Bucket->BucketCoord.X, Bucket->BucketCoord.Y, Bucket->BucketCoord.Z,
			ChunkCoord.Cx, ChunkCoord.Cy, ChunkCoord.Cz);
	}
}

void AVoxelWorld::RemoveChunkFromBucket(const FVoxelCoord& ChunkCoord)
{
	const UVoxelSettings* S = Settings.GetDefaultObject();
	if (!S || !S->bEnableChunkBatching)
	{
		return;
	}

	const FIntVector BucketCoord = BucketManager.ChunkToBucket(ChunkCoord);
	FVoxelChunkBucket** BucketPtr = ChunkBuckets.Find(BucketCoord);

	if (BucketPtr && *BucketPtr)
	{
		FVoxelChunkBucket* Bucket = *BucketPtr;
		Bucket->ContainedChunks.Remove(ChunkCoord);
		Bucket->MarkDirty();

		VOXEL_LOG(LogVoxelBuckets, Log, TEXT("[Bucket] Removed chunk (%d,%d,%d) from bucket (%d,%d,%d), %d chunks remain"),
			ChunkCoord.Cx, ChunkCoord.Cy, ChunkCoord.Cz,
			BucketCoord.X, BucketCoord.Y, BucketCoord.Z,
			Bucket->ContainedChunks.Num());

		// If bucket is now empty, destroy it
		if (Bucket->IsEmpty())
		{
			// Always log bucket destruction (not gated by bEnableDebugLogging)
			UE_LOG(LogVoxelBuckets, Log, TEXT("[Bucket] Destroying empty bucket (%d,%d,%d)"),
				BucketCoord.X, BucketCoord.Y, BucketCoord.Z);

			if (Bucket->MeshComponent)
			{
				ReleaseRMC(Bucket->MeshComponent);
				Bucket->MeshComponent = nullptr;
			}

			delete Bucket;
			ChunkBuckets.Remove(BucketCoord);
		}
	}
}

// ============================================================================
// BUCKET MESH MERGING (PROOF OF CONCEPT)
// ============================================================================

void AVoxelWorld::RebuildDirtyBuckets()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_VoxelRebuildBuckets);

	const UVoxelSettings* S = Settings.GetDefaultObject();
	if (!S || !S->bEnableChunkBatching)
	{
		return;
	}

	const int32 CurrentFrame = GFrameCounter;
	int32 RebuildsThisFrame = 0;
	const int32 MaxRebuildsPerFrame = 2; // Throttle to avoid FPS spikes

	for (auto& Pair : ChunkBuckets)
	{
		FVoxelChunkBucket* Bucket = Pair.Value;
		if (!Bucket || !Bucket->bNeedsRebuild)
		{
			continue;
		}

		// Throttle: Don't rebuild same bucket too frequently
		if (CurrentFrame - Bucket->LastRebuildFrame < 10)
		{
			continue; // Wait at least 10 frames between rebuilds
		}

		// Throttle: Max rebuilds per frame
		if (RebuildsThisFrame >= MaxRebuildsPerFrame)
		{
			break; // Continue next frame
		}

		// Rebuild this bucket by merging all contained chunk meshes
		RebuildBucketMesh(Bucket);

		Bucket->bNeedsRebuild = false;
		Bucket->LastRebuildFrame = CurrentFrame;
		RebuildsThisFrame++;
	}

	if (RebuildsThisFrame > 0)
	{
		// Always log bucket rebuilds (not gated by bEnableDebugLogging)
		UE_LOG(LogVoxelBuckets, Log, TEXT("[Bucket] Rebuilt %d buckets this frame"), RebuildsThisFrame);
	}
}

void AVoxelWorld::RebuildBucketMesh(FVoxelChunkBucket* Bucket)
{
	if (!Bucket || !Bucket->MeshComponent)
	{
		UE_LOG(LogVoxelBuckets, Warning, TEXT("RebuildBucketMesh called with NULL bucket or component!"));
		return;
	}

	QUICK_SCOPE_CYCLE_COUNTER(STAT_VoxelRebuildBucketMesh);

	// Merge all chunk meshes into single bucket mesh
	FMeshBuffers MergedBuffers;
	int32 VertexOffset = 0;
	int32 ChunksMerged = 0;

	const FVector BucketOrigin = Bucket->WorldBounds.Min;
	const UVoxelSettings* S = Settings.GetDefaultObject();
	const float ChunkWorldSizeX = S->ChunkSizeX * S->VoxelWorldScale;
	const float ChunkWorldSizeY = S->ChunkSizeY * S->VoxelWorldScale;
	const float ChunkWorldSizeZ = S->ChunkSizeZ * S->VoxelWorldScale;

	// Iterate through all chunks in this bucket and merge their meshes
	int32 SkippedNotReady = 0;
	int32 SkippedNoBuffers = 0;
	int32 SkippedEmpty = 0;

	for (const FVoxelCoord& ChunkCoord : Bucket->ContainedChunks)
	{
		UVoxelChunkComponent* Chunk = ActiveChunks.FindRef(ChunkCoord);
		if (!Chunk || Chunk->State != EVoxelChunkState::Ready)
		{
			SkippedNotReady++;
			continue; // Skip chunks that aren't ready yet
		}

		// Get cached mesh data from chunk
		const FMeshBuffers* ChunkBuffers = Chunk->GetCachedBuffers();
		if (!ChunkBuffers)
		{
			SkippedNoBuffers++;
			continue; // Skip chunks without buffers
		}

		if (ChunkBuffers->Vertices.Num() == 0)
		{
			SkippedEmpty++;
			continue; // Skip empty chunks
		}

		// Calculate chunk world offset relative to bucket origin
		const FVector ChunkWorldPos(
			ChunkCoord.Cx * ChunkWorldSizeX,
			ChunkCoord.Cy * ChunkWorldSizeY,
			ChunkCoord.Cz * ChunkWorldSizeZ
		);
		const FVector ChunkOffset = ChunkWorldPos - BucketOrigin;

		// Merge vertices (translate to bucket-local space)
		const int32 ChunkVertCount = ChunkBuffers->Vertices.Num();
		MergedBuffers.Vertices.Reserve(MergedBuffers.Vertices.Num() + ChunkVertCount);
		for (const FVector& V : ChunkBuffers->Vertices)
		{
			MergedBuffers.Vertices.Add(V + ChunkOffset);
		}

		// Merge triangles (offset indices by current vertex count)
		const int32 ChunkTriCount = ChunkBuffers->Triangles.Num();
		MergedBuffers.Triangles.Reserve(MergedBuffers.Triangles.Num() + ChunkTriCount);
		for (int32 Idx : ChunkBuffers->Triangles)
		{
			MergedBuffers.Triangles.Add(Idx + VertexOffset);
		}

		// Merge normals
		if (ChunkBuffers->Normals.Num() == ChunkVertCount)
		{
			MergedBuffers.Normals.Reserve(MergedBuffers.Normals.Num() + ChunkVertCount);
			MergedBuffers.Normals.Append(ChunkBuffers->Normals);
		}

		// Merge UVs
		if (ChunkBuffers->UVs.Num() == ChunkVertCount)
		{
			MergedBuffers.UVs.Reserve(MergedBuffers.UVs.Num() + ChunkVertCount);
			MergedBuffers.UVs.Append(ChunkBuffers->UVs);
		}

		// Merge colors
		if (ChunkBuffers->Colors.Num() == ChunkVertCount)
		{
			MergedBuffers.Colors.Reserve(MergedBuffers.Colors.Num() + ChunkVertCount);
			MergedBuffers.Colors.Append(ChunkBuffers->Colors);
		}

		VertexOffset += ChunkVertCount;
		ChunksMerged++;
	}

	// Log merge statistics for monitoring bucket performance
	VOXEL_LOG(LogVoxelBuckets, Log, TEXT("Bucket (%d,%d,%d) merged: %d chunks, %d verts | Skipped: %d not ready, %d no buffers, %d empty"),
		Bucket->BucketCoord.X, Bucket->BucketCoord.Y, Bucket->BucketCoord.Z,
		ChunksMerged, MergedBuffers.Vertices.Num(),
		SkippedNotReady, SkippedNoBuffers, SkippedEmpty);

	// Apply merged mesh to RealtimeMeshComponent
	const bool bHasGeometry = (MergedBuffers.Vertices.Num() > 0 && MergedBuffers.Triangles.Num() >= 3);

	if (bHasGeometry)
	{
		URealtimeMeshSimple* MeshAsset = Bucket->MeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
		if (!MeshAsset)
		{
			return;
		}

		// Create unique keys for this bucket
		const FRealtimeMeshLODKey LOD0(0);
		const FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(
			LOD0,
			FName(*FString::Printf(TEXT("BucketGroup_%d_%d_%d"),
				Bucket->BucketCoord.X, Bucket->BucketCoord.Y, Bucket->BucketCoord.Z))
		);
		const FRealtimeMeshSectionKey SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);

		// Build stream set from merged buffers (using existing helper function)
		FRealtimeMeshStreamSet Streams;
		{
			using FIndexType = uint32;
			TRealtimeMeshBuilderLocal<FIndexType, FPackedNormal, FVector2DHalf, 1> Builder(Streams);

			const int32 NumV = MergedBuffers.Vertices.Num();
			const bool bHaveNormals = (MergedBuffers.Normals.Num() == NumV);
			const bool bHaveUV0 = (MergedBuffers.UVs.Num() == NumV);
			const bool bHaveColors = (MergedBuffers.Colors.Num() == NumV);

			Builder.EnableTangents();
			Builder.EnableTexCoords();
			Builder.EnableColors();
			Builder.EnablePolyGroups();

			// Add all vertices
			Builder.ReserveAdditionalVertices(NumV);
			for (int32 i = 0; i < NumV; ++i)
			{
				const FVector3f P = (FVector3f)MergedBuffers.Vertices[i];
				auto V = Builder.AddVertex(P);

				if (bHaveNormals)
				{
					const FVector3f N = ((FVector3f)MergedBuffers.Normals[i]).GetSafeNormal();
					const FVector3f Up = (FMath::Abs(N.Z) < 0.999f) ? FVector3f(0, 0, 1) : FVector3f(0, 1, 0);
					const FVector3f T = (Up ^ N).GetSafeNormal();
					V.SetNormalAndTangent(N, T);
				}

				V.SetColor(bHaveColors ? MergedBuffers.Colors[i] : FColor::White);

				if (bHaveUV0)
				{
					V.SetTexCoord((FVector2f)MergedBuffers.UVs[i]);
				}
			}

			// Add all triangles
			const int32 NumI = MergedBuffers.Triangles.Num();
			Builder.ReserveAdditionalTriangles(NumI / 3);
			for (int32 t = 0; t < NumI; t += 3)
			{
				Builder.AddTriangle(
					(FIndexType)MergedBuffers.Triangles[t + 0],
					(FIndexType)MergedBuffers.Triangles[t + 1],
					(FIndexType)MergedBuffers.Triangles[t + 2],
					0); // PolyGroup
			}
		}

		// Remove old group and create new one
		MeshAsset->RemoveSectionGroup(GroupKey)
			.Next([MeshAsset, GroupKey, SectionKey, Streams = MoveTemp(Streams), Bucket, ChunksMerged, NumVerts = MergedBuffers.Vertices.Num()]
				(ERealtimeMeshProxyUpdateStatus /*RemoveStatus*/) mutable
			{
				MeshAsset->CreateSectionGroup(GroupKey, MoveTemp(Streams))
					.Next([MeshAsset, SectionKey, Bucket, ChunksMerged, NumVerts](ERealtimeMeshProxyUpdateStatus Status)
					{
						if (Status == ERealtimeMeshProxyUpdateStatus::NoUpdate)
						{
							FRealtimeMeshSectionConfig SectionConfig(0); // Material slot 0
							SectionConfig.bIsVisible = true;
							MeshAsset->UpdateSectionConfig(SectionKey, SectionConfig, false); // No collision on buckets (too expensive)
						}

						// Always log successful bucket rebuilds (not gated by bEnableDebugLogging)
						UE_LOG(LogVoxelBuckets, Log, TEXT("[Bucket] Rebuilt bucket (%d,%d,%d): %d chunks merged, %d vertices"),
							Bucket->BucketCoord.X, Bucket->BucketCoord.Y, Bucket->BucketCoord.Z,
							ChunksMerged, NumVerts);
					});
			});
	}
	else
	{
		// No geometry - clear mesh
		if (URealtimeMesh* Mesh = Bucket->MeshComponent->GetRealtimeMesh())
		{
			Mesh->Reset();
			VOXEL_LOG(LogVoxelBuckets, Log, TEXT("[Bucket] Cleared empty bucket (%d,%d,%d)"),
				Bucket->BucketCoord.X, Bucket->BucketCoord.Y, Bucket->BucketCoord.Z);
		}
	}
}
